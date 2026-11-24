// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <fcntl.h>
#include <errno.h>
#if DM_NATIVE_EXEC   // Apple's module mapping (map_image, the jit layout, the startup span); the x86-64 engine maps the modules in its arena
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include "hle.h"
#include "../gfx/nvn/nvn_fstep.h"   // the state page's target and tick unit (FSTEP_UNIT_OFF, nvn_fstep_target_put)
#if TARGET_OS_IPHONE
#include <mach/mach.h>
#endif

module_t *g_mod;
int g_nmod;
// Stable module addresses live in g_mod; lookup order follows activation, including unload/reload.
static module_t **g_loaded; static int g_nloaded, g_mod_capacity;
static pthread_rwlock_t g_modules_mu = PTHREAD_RWLOCK_INITIALIZER;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);
#if !DM_NATIVE_EXEC
// The x86-64 execution engine (aot.h): module images one to one in its arena below 2^39 (design 5.1, 5.2), never executable (3.2: their code runs from the
// pack's library), and guest functions the host calls entered through dm_guest_call (2.4)
void *dm_guest_alloc(size_t n);
uint8_t *dm_aot_image_map(const uint8_t *bytes, size_t len, size_t bss, uint64_t ro_end, size_t *mapped);
struct dm_guest_state;
struct dm_guest_state *dm_aot_self_state(void);
uint64_t dm_guest_call(struct dm_guest_state *st, uint64_t addr, int nargs, ...);
// What an import of a module the engine mapped binds to: a host function's guest-visible slot, an unimplemented import's stub slot (aot_arena.c, design 2.4)
uint64_t dm_aot_bind(const module_t *m, const char *name, uint64_t v);
_Noreturn void dm_aot_pack_refused(const char *err);   // aot.h: a pack a repack cures, refused at the start: its [RED] lines and the exit status DM_AOT_REPACK
int dm_aot_write_module_bindings(const char *dir, const module_t *m, const uint8_t sha256[32], const char *pack);   // aot.h: nxhost-run --bindings
#if defined(_WIN32)   // aot.h's DM_AOT_PACK_SUFFIX (this file declares what it needs of the engine instead of including aot.h: its unit test t_nro.c links no engine)
#define AOT_PACK_SUFFIX ".aot.dll"
#else
#define AOT_PACK_SUFFIX ".aot.so"
#endif
#endif

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    default: return g_keygate;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("cannot create the host key");
    LOG("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", (unsigned long)g_key18, (unsigned long)g_keytls, (unsigned long)g_keytlr, (unsigned long)g_keygate);
}

// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
#if DM_NATIVE_EXEC
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD layout mismatch");
    pthread_setspecific(g_key18, 0);
#endif   // the rewritten code reads its thread slots through Apple's TSD (tpidrro_el0); the x86-64 execution engine (Phase 2) keeps the guest's thread registers itself
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    int64_t bss_end = mod0 + (int64_t)*(const int32_t *)(m->base + mod0 + 12);
    if (bss_end > 0 && (uint64_t)bss_end <= m->key_page_off)
        m->guest_size = ((uint64_t)bss_end + 0xfff) & ~0xfffull;
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_FINI: m->fini = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("[RED] DM_SYMCHECK: %s in %s hash %p ≠ linear %p", name, m->name, r, l); } }
    return r;
}
static void *loaded_sym(const char *name) {   // caller holds g_modules_mu, or is doing startup relocation before guest threads run
    for (int i = 0; i < g_nloaded; i++) { void *p = module_sym(g_loaded[i], name); if (p) return p; }
    return NULL;
}
void *guest_sym(const char *name) {
    pthread_rwlock_rdlock(&g_modules_mu); void *p = loaded_sym(name); pthread_rwlock_unlock(&g_modules_mu);
    return p;
}
void *guest_module_sym(uintptr_t base, const char *name) {
    void *symbol = NULL;
    pthread_rwlock_rdlock(&g_modules_mu);
    for (int i = 0; i < g_nloaded; ++i) {
        module_t *m = g_loaded[i];
        if ((uintptr_t)m->base == base) { symbol = module_sym(m, name); break; }
    }
    pthread_rwlock_unlock(&g_modules_mu);
    return symbol;
}
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// Whether main imports a symbol whose name starts with prefix (an undefined dynamic symbol: one main calls but another module defines), for behaviour that follows what the game uses
// The NROs count too, loaded or not: main may be only a boot stub that loads the game's code as an NRO (Mario Tennis Fever), and a Source engine game's modules are NROs
int dm_game_imports(const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].nro || !strcmp(g_mod[i].name, "main")) {
        module_t *m = &g_mod[i];
        for (unsigned k = 1; k < m->nsym; k++) if (!m->symtab[k].st_shndx && !strncmp(m->strtab + m->symtab[k].st_name, prefix, n)) return 1;
    }
    return 0;
}

// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_;
// Switch 2 SDKs name a variant first: "NintendoSdk_nnSdk-Generic_20_5_19", Donkey Kong Bananza).
// Behaviour that follows the SDK version is decided by it (the NVN API version, the shared-font layout), not by the game profile. 0 = no sdk module or no version string
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;   // read-only data, data and bss
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7) {
            if (p - b < 11 || strncasecmp(p - 11, "NintendoSDK", 11)) continue;
            const char *v = p + 7;
            while (v < e && ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z'))) v++;   // a variant name ("Generic") and its '_'
            if (v > p + 7 && v < e && *v == '_') v++;
            if (v < e && *v >= '0' && *v <= '9') { major = atoi(v); LOG("sdk: NintendoSDK nnSdk-%.20s (major %d)", p + 7, major); break; }
        }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module; SDK-dependent behaviour takes the newer SDKs' side");
    return major;
}

// Whether the sdk module was built for the Switch 2 ("Ounce"): its file system access log line names the build spec ("FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }";
// Switch 1 SDKs say "spec: NX"). The Switch 2's NVN (NVN2) reports another API version (hle_nvn.c nvn_api)
int dm_sdk_ounce(void) {
    static int ounce = -1;
    if (ounce >= 0) return ounce;
    ounce = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;
        if (memmem(b, (size_t)(e - b), "spec: Ounce", 11)) ounce = 1;
    }
    if (ounce) LOG("sdk: built for the Switch 2 (spec: Ounce)");
    return ounce;
}

// The constants NVN2's own nvnDeviceGetInteger answers: the driver is compiled into the game's sdk, and its API and GPU code versions move with the SDK (20.5.19: API 231.30, GPU code minor 17;
// 21.4.4: 231.39 and 20). Games check them: Splatoon Raiders requires minor >= 39 and otherwise creates no graphics system (main+0x31b86b0). The driver's switch over the items loads w0 with the
// major first, then jumps through a byte table: mov w0, #231 / adr / adrp + add (the table) / ldrb / add, lsl #2 / br, with cmp wN, #<last item> before it. A case that is a constant is mov w0, #v; b.
// Returns 1 with *out set when item has such a case; 0 for a case that reads something else (most items go through the driver's table lookup or read the GPU's description), or no such switch
int dm_sdk_nvn2_item(unsigned item, int *out) {
    static const uint8_t *table; static const uint32_t *cases; static unsigned last; static int found = -1;
    if (found < 0) {
        found = 0;
        for (int i = 0; i < g_nmod && !found; i++) if (!strcmp(g_mod[i].name, "sdk")) {
            const uint32_t *t = (const uint32_t *)g_mod[i].base; size_t n = g_mod[i].ro_off / 4;
            for (size_t p = 8; p + 9 < n && !found; p++) if (t[p] == 0x52801CE0) {   // mov w0, #231
                const uint8_t *tb = NULL; const uint32_t *adr = NULL; int lim = -1, ldrb = 0, br = 0;
                for (size_t q = p - 8; q <= p + 8; q++) {
                    uint32_t w = t[q];
                    if ((w & 0xFFC0001F) == 0x7100001F) lim = (w >> 10) & 0xFFF;   // cmp wN, #imm
                    else if ((w & 0x9F000000) == 0x90000000 && (t[q + 1] & 0xFFC00000) == 0x91000000) {   // adrp + add
                        int64_t pg = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (pg & 0x100000) pg -= 0x200000;
                        tb = (const uint8_t *)(((uintptr_t)&t[q] & ~(uintptr_t)0xFFF) + (uintptr_t)(pg * 4096) + ((t[q + 1] >> 10) & 0xFFF));
                    } else if ((w & 0x9F000000) == 0x10000000) {   // adr
                        int64_t off = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (off & 0x100000) off -= 0x200000;
                        adr = (const uint32_t *)((const char *)&t[q] + off);
                    } else if ((w & 0xFFE0FC00) == 0x38606800) ldrb = 1;   // ldrb wK, [xT, xI]
                    else if ((w & 0xFFFFFC1F) == 0xD61F0000) br = 1;       // br
                }
                if (!tb || !adr || lim <= 1 || !ldrb || !br) continue;
                const char *ro = (const char *)g_mod[i].base + g_mod[i].ro_off, *ro_end = (const char *)g_mod[i].base + g_mod[i].key_page_off;
                if ((const char *)tb < ro || (const char *)tb + lim >= ro_end) continue;
                table = tb; cases = adr; last = (unsigned)lim; found = 1;
            }
        }
        if (dm_sdk_ounce() && !found) LOG("[RED] sdk: NVN2's nvnDeviceGetInteger switch not found; the device answers the newest versions known");
    }
    if (!found || item > last) return 0;
    const uint32_t *c = cases + table[item];
    if ((c[0] & 0xFFE0001F) != 0x52800000 || (c[1] & 0xFC000000) != 0x14000000) return 0;   // mov w0, #imm16; b
    *out = (int)((c[0] >> 5) & 0xFFFF);
    return 1;
}

// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    void *p = loaded_sym(name); if (p) return (uint64_t)p;
    return resolve_import(m, name);
}

static const char k_language_list[] = "_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi";
static const char k_language_count[] = "_ZN2nn8settings29GetAvailableLanguageCodeCountEv";

// The public list/count exports in these Ounce SDKs only read the SDK's own table. Check the SDK revision and both exports
// together so an unknown or incomplete SDK keeps both settings-service fallbacks. See docs/reference/services.md.
static int sdk_has_local_language_list(void) {
    static const char prefix[] = "FS_ACCESS: { sdk_version: ";
    static const char *const reviewed[] = {
        "20.5.19, spec: Ounce }", "20.5.25, spec: Ounce }", "21.4.0, spec: Ounce }",
        "21.4.1, spec: Ounce }", "22.2.8, spec: Ounce }", "22.2.10, spec: Ounce }",
    };
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (strcmp(m->name, "sdk") || m->ro_off >= m->data_off || m->data_off > m->size ||
            !module_sym(m, k_language_list) || !module_sym(m, k_language_count)) continue;
        const uint8_t *p = m->base + m->ro_off, *end = m->base + m->data_off;
        while ((p = memmem(p, (size_t)(end - p), prefix, sizeof prefix - 1))) {
            p += sizeof prefix - 1;
            for (size_t j = 0; j < sizeof reviewed / sizeof *reviewed; j++) {
                size_t len = strlen(reviewed[j]);
                if ((size_t)(end - p) >= len && !memcmp(p, reviewed[j], len)) return 1;
            }
        }
    }
    return 0;
}

// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
PLAT_TABLE_DEFINE(swforce)   // plat_table_swforce
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static unsigned *g_fidx, g_fmask;   // name hash over the table: force_lookup runs for every named relocation, and each lookup used to strcmp its way through all 722 entries (the swhle table's scan is hashed the same way, hle.c)
static const unsigned k_force_disabled = UINT_MAX;
static const char *force_entry_label(const hle_force_ent *e) {
    if (e->to) return e->to;
    const char *s = e->fn ? plat_addr_symbol(e->fn, NULL) : NULL;   // dladdr's symbol name (plat.h)
    return s ? s : "?";
}
static int force_build(int abort_on_error) {
    size_t sz = 0; g_force = (const hle_force_ent *)plat_table_swforce(&sz);
    if (g_force && sz % sizeof *g_force) {
        if (abort_on_error) die("forced takeover section size %zu is not a multiple of entry size %zu", sz, sizeof *g_force);
        LOG("forced takeover section size %zu is not a multiple of entry size %zu", sz, sizeof *g_force);
        g_force = NULL; g_nforce = 0;
        return 1;
    }
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    unsigned cap = 16; while (cap < g_nforce * 2) cap <<= 1;
    g_fidx = calloc(cap, sizeof *g_fidx); g_fmask = cap - 1;
    if (!g_force_hits || !g_fidx) die("forced takeover index allocation failed");
    int native_language_list = sdk_has_local_language_list();
    int bad = 0;
    for (unsigned i = 0; i < g_nforce; i++) {   // insertion doubles as the duplicate check (a wrong list must still fail loudly, at once)
        if (!g_force[i].name || !g_force[i].why || (!g_force[i].fn == !g_force[i].to)) {
            if (abort_on_error) die("malformed forced takeover entry %u (%s)", i, g_force[i].name ? g_force[i].name : "(null)");
            LOG("malformed forced takeover entry %u (%s)", i, g_force[i].name ? g_force[i].name : "(null)");
            bad++; continue;
        }
        if (native_language_list && (!strcmp(g_force[i].name, k_language_list) || !strcmp(g_force[i].name, k_language_count)))
            g_force_hits[i] = k_force_disabled;
        for (unsigned j = sym_hash(g_force[i].name) & g_fmask;; j = (j + 1) & g_fmask) {
            if (!g_fidx[j]) { g_fidx[j] = i + 1; break; }
            if (!strcmp(g_force[g_fidx[j] - 1].name, g_force[i].name)) {
                const hle_force_ent *prev = &g_force[g_fidx[j] - 1];
                if (abort_on_error)
                    die("duplicate entry in the forced takeover list: %s (%s and %s)", g_force[i].name, force_entry_label(prev), force_entry_label(&g_force[i]));
                LOG("duplicate entry in the forced takeover list: %s (%s and %s)", g_force[i].name, force_entry_label(prev), force_entry_label(&g_force[i]));
                bad++; break;
            }
        }
    }
    for (unsigned i = 0; i < g_nforce; i++)   // an alias binds to the guest's definition: a target the host takes over would bypass that take-over
        if (g_force[i].name && !g_force[i].fn && g_force[i].to)
            for (unsigned j = sym_hash(g_force[i].to) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask)
                if (g_force_hits[g_fidx[j] - 1] != k_force_disabled && !strcmp(g_force[g_fidx[j] - 1].name, g_force[i].to)) {
                    if (abort_on_error) die("forced alias %s: its target %s is itself taken over", g_force[i].name, g_force[i].to);
                    LOG("forced alias %s: its target %s is itself taken over", g_force[i].name, g_force[i].to);
                    bad++; break;
                }
    return bad;
}
static void force_init(void) { (void)force_build(1); }
// Build-time self-check (`dmhost --check-tables`): validate the linked __DATA,swforce registrations without game files or a GPU, reporting every error before returning non-zero.
int dm_check_tables(void) {
    int bad = force_build(0);
    if (!g_nforce) { LOG("forced takeover list is empty"); bad++; }
    free(g_fidx); g_fidx = NULL; g_fmask = 0;
    free(g_force_hits); g_force_hits = NULL;
    g_force = NULL; g_nforce = 0;
    return bad != 0;
}
static void *force_lookup(const char *name) {
    if (!g_fmask) return NULL;
    for (unsigned j = sym_hash(name) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask) {
        unsigned i = g_fidx[j] - 1;
        if (!strcmp(g_force[i].name, name)) {
            if (g_force_hits[i] == k_force_disabled) return NULL;   // let normal guest symbol resolution bind it
            if (g_force[i].to) {   // a guest alias (hle.h HLE_FORCE_ALIAS); the caller holds g_modules_mu or is relocating at startup, as for resolve_sym
                void *p = loaded_sym(g_force[i].to);
                if (!g_force_hits[i]++) {
                    if (!p) LOG("[RED] forced alias %s: no loaded module defines %s; bound without the alias", name, g_force[i].to);
                    else if (g_flag.imports) LOG("force %s -> %s  (%s)", name, g_force[i].to, g_force[i].why);
                }
                void *hlecount_wrap(const char *, const char *, void *); return p ? hlecount_wrap("ALIAS", name, p) : NULL;
            }
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // A counting stub when DM_HLECOUNT is set
        }
    }
    return NULL;
}
#if !DM_NATIVE_EXEC
// nxhost-run --takeovers (main_x86.c): the names of the forced take-over list, one per line after a '#' line, in the table's order: what the launcher (dm/launcher
// launcher_model.cpp host_takeovers) compares with the PLT bindings a pack assumed (its <module>.bind.tsv), to tell before a start that this host would refuse it
void loader_print_takeovers(FILE *f) {
    size_t sz; const hle_force_ent *t = (const hle_force_ent *)plat_table_swforce(&sz);
    const unsigned n = t ? (unsigned)(sz / sizeof *t) : 0;
    fprintf(f, "# nxhost-run take-overs: %u names this host binds to its own functions, whatever a module defines (loader.c bind_sym, HLE_FORCE)\n", n);
    for (unsigned i = 0; i < n; i++) fprintf(f, "%s\n", t[i].name);
}
#endif
// Generic host: the list serves several games, so entries a game does not reference are only noted; with DM_FORCE_STRICT=1 it still fails loudly (for checking a mistyped list)
static void force_check(void) {
    unsigned unused = 0, disabled = 0; int strict = getenv("DM_FORCE_STRICT") != NULL;
    for (unsigned i = 0; i < g_nforce; i++)
        if (g_force_hits[i] == k_force_disabled) disabled++;
        else if (!g_force_hits[i]) { if (strict || g_flag.imports) LOG("forced takeover entry with no reference at all: %s", g_force[i].name); unused++; }
    if (unused && strict) die("forced takeover list is wrong");
    LOG("force: %u entries hit, %u not referenced by this game", g_nforce - unused - disabled, unused);
}

// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *s, uint8_t *out, size_t n) {
    for (size_t k = 0; k < n; k++) {
        int hi = hex_nibble(s[2 * k]), lo = hex_nibble(s[2 * k + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[k] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[48]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %47s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // may be negative (patch area before the base)
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "BACK_OFF")) m->back_off = v;
        else if (!strcmp(key, "BACK_LEN")) m->back_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "FPS_STATE_OFF")) m->fps_state_off = v;
        else if (!strcmp(key, "RENDER_SCALE")) m->render_scale_max = (uint32_t)v;
        else if (!strcmp(key, "MOVIE_NATIVE_COLOR_KEYS")) m->movie_native_color_keys = v == 1;
        else if (!strcmp(key, "STEP_MAX")) m->step_max = (uint32_t)v;
        else if (!strcmp(key, "FRAME_STEP_120")) m->fstep_t120 = v != 0;
        else if (!strcmp(key, "UE_MALLOC_VT")) m->ue_malloc_vt = v;
        else if (!strcmp(key, "UE_MALLOC_SLOT")) m->ue_malloc_slot = (unsigned)v;
        else if (!strcmp(key, "FPS_TARGET_PTR")) m->fps_target_ptr = v;
        else if (!strcmp(key, "FPS_TARGET_FIELD")) m->fps_target_field = (uint32_t)v;
        else if (!strcmp(key, "MONOLITH_INTERVAL")) m->monolith_interval = v;
        else if (!strcmp(key, "ALIAS_CUT")) {
            char ob[40], nb[40]; size_t n;
            if (m->n_alias_cut == (int)(sizeof m->alias_cut / sizeof *m->alias_cut) || sscanf(line, "# %*s %*s %39s %39s", ob, nb) != 2 || (n = strlen(ob)) != strlen(nb) || !n || n % 2 || n > 32)
                die("%s: bad ALIAS_CUT row: %s", path, line);
            m->alias_cut[m->n_alias_cut].off = v; m->alias_cut[m->n_alias_cut].len = (uint8_t)(n / 2);
            if (!parse_hex_bytes(ob, m->alias_cut[m->n_alias_cut].orig, n / 2) || !parse_hex_bytes(nb, m->alias_cut[m->n_alias_cut].cut, n / 2))
                die("%s: bad ALIAS_CUT hex: %s", path, line);
            m->n_alias_cut++;
        }
        else if (!strcmp(key, "NATIVE_FPS_BLOCKED")) m->native_fps_blocked = v != 0;
        else if (!strcmp(key, "UE_FIXED_RATE_PTR")) m->ue_fixed_ptr = v;
        else if (!strcmp(key, "UE_FIXED_RATE_FIELD")) m->ue_fixed_field = (uint32_t)v;
        else if (!strcmp(key, "UE_FIXED_RATE_FLAG")) m->ue_fixed_flag = (uint32_t)v;
        else if (!strcmp(key, "UNITY_TARGET_FPS")) m->unity_target_fps = v;
        else if (!strcmp(key, "UNITY_QS_PTR")) m->unity_qs_ptr = v;
        else if (!strcmp(key, "UNITY_QS_IDX")) m->unity_qs_idx = (uint32_t)v;
        else if (!strcmp(key, "UNITY_QS_ARR")) m->unity_qs_arr = (uint32_t)v;
        else if (!strcmp(key, "UNITY_QS_SIZE")) m->unity_qs_size = (uint32_t)v;
        else if (!strcmp(key, "UNITY_QS_VSYNC")) m->unity_qs_vsync = (uint32_t)v;
        else if (!strncmp(key, "UE_CVAR:", 8)) {
            if (m->n_ue_cvar == (int)(sizeof m->ue_cvar / sizeof *m->ue_cvar) || strlen(key + 8) >= sizeof m->ue_cvar[0].name) die("%s: too many Unreal console variables, or too long a name: %s", path, key);
            strcpy(m->ue_cvar[m->n_ue_cvar].name, key + 8); m->ue_cvar[m->n_ue_cvar++].off = v;
        }
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < KEY_N; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1)
        die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
    if ((m->back_off || m->back_len) && (m->patch_off >= 0 || !m->back_len || m->back_off % 0x4000 || m->back_len % 0x4000 || m->back_off < m->key_page_off + m->key_page_size
                                         || m->back_off + m->back_len != m->image_size))
        die("%s: back patch area, but BACK_OFF / BACK_LEN missing or wrong", path);
    // map_image seals [0, text_end rounded up to 16 KB) read + execute (and gen_embed.py puts it in __TEXT on iOS): data below that would be read-only and SIGBUS in relocate()
    // (FINAL FANTASY VIII Remastered's Math.nro and Platform.nro). patch_nso.py moves ro and data up (mod_layout.py); stop here on a copy rewritten before that
    if (m->data_off < ((m->text_end + 0x3FFF) & ~0x3FFFull))
        die("%s: data at %#llx shares a 16 KB page with code (text ends at %#llx): rewrite the module again with dm/tools/patch_nso.py", m->name, (unsigned long long)m->data_off, (unsigned long long)m->text_end);
}

// Frame step state (the page after the key page, read-write; layout in patch_nso.py FRAME_STEP): the rewriter's stubs step the game by the host's whole vsyncs capped at this
// target, 0 = the game's own interval logic. The profile's fps_target, overridden by DM_FPS_TARGET (0, 30 or 60; 40 for a stub that takes ticks of 1/120 s, FRAME_STEP_120);
// the graphics layer decides the steps and paces presents (hle_nvn.c fstep_tick)
uint8_t *dm_fps_state;
int dm_fps_step_max;   // the largest step the stubs give the game (STEP_MAX in its fixups table; 0 = FSTEP_MAX): the frame step holds frames and pays back within it (hle_nvn.c)
static const char *g_fps_mod;   // the module holding the state page (for the log)
static float g_render_scale_max = 1.0f;   // the render-scale stub's largest scale (RENDER_SCALE in its fixups table; 1 for a stub that only lowers the game's size)
static int g_fstep_t120;                  // the frame step stub takes ticks of 1/120 s (the FRAME_STEP_120 capability): the page's unit (+0x38) is 120, and 40 fps is taken
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    dm_fps_state = m->base + m->fps_state_off; g_fps_mod = m->name; dm_fps_step_max = (int)m->step_max;
    if (m->render_scale_max > 1) g_render_scale_max = (float)m->render_scale_max;
    g_fstep_t120 = m->fstep_t120;
}
// The settings in the state page, written when the game starts (main.c, after host loading): on iOS the loading runs at app launch, before the launcher has set DM_FPS_TARGET and
// DM_RENDER_SCALE (Launcher.swift), and nothing reads the page before the game runs
void dm_fps_state_apply(void) {
    if (!dm_fps_state) return;
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    // The step's unit, before the target: 120 (ticks of 1/120 s) for a stub that takes them, fixed for the run (nvn_fstep.h FSTEP_UNIT_OFF); 0 = vsyncs of 1/60 s
    uint32_t unit = g_fstep_t120 ? 120 : 0; __atomic_store_n((uint32_t *)(dm_fps_state + FSTEP_UNIT_OFF), unit, __ATOMIC_RELAXED);
    if (t == 40 && !g_fstep_t120) { LOG("[RED] DM_FPS_TARGET=40: this build's frame step takes whole vsyncs of 1/60 s only (no FRAME_STEP_120 stub), so 25 ms frames cannot be stepped; using 30"); t = 30; }
    if (t != 0 && t != 30 && t != 40 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30%s or 60; using the game's own frame step", e, g_fstep_t120 ? ", 40" : ""); t = 0; }
    nvn_fstep_target_put(dm_fps_state, (float)t);
    // Render scale of the 3D views (the stubs at Tears of the Kingdom's main+0xd5cc0, Breath of the Wild's main+0x1266880, Hades II's render size, Ocarina of Time's view-size
    // update): 1.0 keeps the game's own size and dynamic resolution; another factor fixes the size at that fraction (DM_RENDER_SCALE, 0.5 to the stub's largest scale: 1.0, or
    // above for a stub that also raises it, Hades II's 3.0). Independent of the frame step: written at target 0 too
    const char *rs = getenv("DM_RENDER_SCALE"); float sc = 1.0f, hi = g_render_scale_max;
    if (rs) { char *end; sc = strtof(rs, &end); if (end == rs || *end || !(sc >= 0.5f && sc <= hi)) { LOG("[RED] DM_RENDER_SCALE=%s: must be 0.5 to %.1f; keeping the game's size", rs, hi); sc = 1.0f; } else LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", g_fps_mod, sc); }
    memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    // The game's own dynamic resolution, for builds whose stubs read it (the DYNAMIC_RESOLUTION capability of a frame step stub: Ocarina of Time's level function): 1 lets it
    // run, 0 (the default) holds full resolution. Builds without such a stub ignore the field
    const char *dr = getenv("DM_DYNAMIC_RESOLUTION"); uint32_t dyn = dr && !strcmp(dr, "1");
    if (dr && strcmp(dr, "0") && strcmp(dr, "1")) LOG("[RED] DM_DYNAMIC_RESOLUTION=%s: must be 0 or 1; keeping full resolution", dr);
    memcpy(dm_fps_state + 0x40, &dyn, sizeof dyn);
    // Screen shape: DM_SCREEN_ASPECT, the screen's width / height between 1.3 and 1.77, makes the builds with screen-shape stubs
    // (Breath of the Wild 1.9.0's patches.py) render that shape: the output keeps its width (1920) and takes the height that gives the ratio, a multiple of 48 so the
    // 2/3-size layout space (1280 wide) stays whole: 1.43 → 1920x1344 and 1280x896. State +0x48 u32 output height, +0x4c u32 layout height, +0x50 its float; 0 = 16:9
    const char *as = getenv("DM_SCREEN_ASPECT");
    if (as && *as && strcmp(as, "0") && strcmp(as, "auto")) {   // 0: 16:9; auto: the iOS front end resolves it to the screen's shape (a Mac window has none)
        char *end; float a = strtof(as, &end);
        if (end == as || *end || !(a >= 1.3f && a < 1.77f)) LOG("[RED] DM_SCREEN_ASPECT=%s: a width / height from 1.3 to 1.77; keeping 16:9", as);
        else {
            uint32_t oh = (uint32_t)(1920.0f / a / 48.0f + 0.5f) * 48, lh = oh * 2 / 3; float lf = (float)lh;
            memcpy(dm_fps_state + 0x48, &oh, 4); memcpy(dm_fps_state + 0x4c, &lh, 4); memcpy(dm_fps_state + 0x50, &lf, 4);
#if defined(__APPLE__)
            extern void mtl_present_screen(uint32_t w, uint32_t h); mtl_present_screen(1920, oh);   // the picture's shape for a texture that follows it (nvn_mtl.m present_shape)
#else
            LOG("[RED] DM_SCREEN_ASPECT: the stubs render the taller shape, but this platform's presenter keeps 16:9 for a texture that follows the screen (the Metal backend's mtl_present_screen has no Vulkan counterpart yet)");
#endif
            const char *ms = getenv("DM_SCREEN_ASPECT_SITES"); uint32_t mask = ms ? (uint32_t)strtoul(ms, NULL, 0) : 0xffffffffu;   // the stubs of the game's own conversions, one bit each (+0x54)
            memcpy(dm_fps_state + 0x54, &mask, 4);
            if (ms) LOG("%s: screen shape: conversion sites %#x (DM_SCREEN_ASPECT_SITES)", g_fps_mod, mask);
            // The field of view: views of the screen's ratio (the scene, the HUD's camera) keep 16:9's sides and show more above and below (patches.py's sead::PerspectiveProjection stubs)
            float r = 1920.0f / (float)oh, k = (16.0f / 9.0f) / r, tol = 0.01f;
            const char *fo = getenv("DM_SCREEN_FOV"); if (fo && !strcmp(fo, "off")) tol = -1.0f;   // trials: the scene keeps the game's vertical angle (no view matches); the HUD camera still widens
            memcpy(dm_fps_state + 0x58, &r, 4); memcpy(dm_fps_state + 0x5c, &k, 4); memcpy(dm_fps_state + 0x60, &tol, 4);
            float wide = 16.0f / 9.0f, wtol = 0.001f, far = 10000.0f;   // the HUD camera's projection: 16:9, near 1, far 10000 (+0x74 / +0x78 / +0x7c)
            memcpy(dm_fps_state + 0x74, &wide, 4); memcpy(dm_fps_state + 0x78, &wtol, 4); memcpy(dm_fps_state + 0x7c, &far, 4);
            // The HUD at the taller edges (patches.py duo_anchor.c, +0x80..+0x98): the shift (half the layout height over 360), the stretch of full-screen panes, the edge rule's
            // distance, the listed corner widgets' distance, a full-screen pane's width and height in the 1280x720 space, and the tallest one still stretched (a menu's
            // background is about 720 tall, the weapon wheel's gradient 1280; the map's tiles are left alone anyway)
            // (Mac, a Master Mode save: an edge distance of 280 takes the menus' title bars (286 to 320), the map's frame pieces (284 to 360) and the bottom guides (-318) along
            // as whole rows; 330 left the title bars and the rupee count in the middle while the hearts moved. Text moves with its widget since the rule runs where global
            // matrices are computed. Only panes as wide and as tall as the screen are stretched: a width of 0 stretched the inventory's half-width side panel past its top
            // and bottom bars). DM_SCREEN_HUD=<edge>,<listed>,<full-screen width>,<full-screen height>[,<tallest>]
            // overrides them for trials
            float anc[7] = {(float)lh / 2 - 360.0f, (float)lh / 720.0f, 280.0f, 100.0f, 1270.0f, 710.0f, 1300.0f};
            const char *hv = getenv("DM_SCREEN_HUD");
            if (hv && sscanf(hv, "%f,%f,%f,%f,%f", &anc[2], &anc[3], &anc[4], &anc[5], &anc[6]) < 4) LOG("[RED] DM_SCREEN_HUD=%s: <edge>,<listed>,<full width>,<full height>[,<tallest>]; keeping the rest", hv);
            memcpy(dm_fps_state + 0x80, anc, sizeof anc);
            LOG("%s: screen shape: HUD moves out by %.0f (edge %.0f, listed widgets %.0f), panes %.0fx%.0f or larger stretched %.3f", g_fps_mod, anc[0], anc[2], anc[3], anc[4], anc[5], anc[1]);
            LOG("%s: screen shape %.3f: output 1920x%u, layout 1280x%u (DM_SCREEN_ASPECT; only builds with screen-shape stubs follow it)", g_fps_mod, a, oh, lh);
        }
    }
    char cap[32]; snprintf(cap, sizeof cap, ", capped at %d fps", t);
    LOG("%s: frame step %s%s (%s)", g_fps_mod, !t ? "as the game sets it" : g_fstep_t120 ? "in ticks of 1/120 s decided by the host" : "in whole vsyncs decided by the host", t ? cap : "", e ? "DM_FPS_TARGET" : "game profile fps_target");
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: key page out of range", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
    for (int k = 0; k < KEY_N; k++) if (!page[m->key_idx[k]]) die("%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", m->name, k);
    if (plat_vm_protect(m->base + m->key_page_off, m->key_page_size, PLAT_PROT_READ)) die("%s key page mprotect", m->name);
    if (m->fps_state_off) install_fps_state(m);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, (long long)m->patch_off, (unsigned long)g_key18, (unsigned long)g_keytls, (unsigned long)g_keytlr, (unsigned long)g_keygate);
}

// DT_RELR (from SDK 20; all three modules of The Legend of Zelda: Breath of the Wild 1.9.0 have it): a compact table of relative relocations with the addend in the slot (*slot += base).
// Each 8-byte entry: even = a new start offset (relocate it, the pointer moves to the next slot); odd = a bitmap whose bits 1-63 stand for the 63 slots after the pointer, which then moves on by 63 slots.
// Without it main's GOT and vtables hold module offsets (main's RELA keeps only symbol relocations)
static unsigned relr_apply(module_t *m, uint64_t relr, uint64_t relrsz, uint64_t delta) {
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += delta; n++; continue; }
        if (!where) die("%s: DT_RELR bitmap entry with no start before it", m->name);
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += delta; n++; }
        where += 63;
    }
    return n;
}

// The address a symbol relocation of module m binds to, as Horizon's rtld would with the modules loaded now (forced take-over, then the loaded modules in load order, then HLE / stub).
// *other: the module's own definition was taken over by an earlier module's; *weak0: an undefined weak symbol nothing defines, bound to 0
static uint64_t bind_sym(module_t *m, const Elf64_Sym *s, int *other, int *weak0) {
    uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
    *other = *weak0 = 0;
    // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
    // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
    // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
    if (!v && s->st_shndx) {
        uint64_t self = (uint64_t)m->base + s->st_value;
        v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
        if (!v) v = self;
        *other = v != self;
    }
    // An undefined weak symbol that no module defines and the host does not implement is 0, as Horizon's rtld binds it: the code tests its address before calling it.
    // Octopath Traveler 0's main imports the thread_local init function _ZTHN13LowLevelTasks5FTask10ActiveTaskE weakly; a named stub made it look defined and the game called it
    const char *nm = m->strtab + s->st_name;
    *weak0 = !v && !s->st_shndx && ELF64_ST_BIND(s->st_info) == STB_WEAK && !loaded_sym(nm) && !hle_implements(nm);
    if (!v && !*weak0) v = resolve_sym(m, nm);
#if !DM_NATIVE_EXEC
    v = dm_aot_bind(m, nm, v);   // x86-64 engine: a host function or stub as its guest-visible address, so a compiled blr through the GOT reaches it through the dispatcher
#endif
    return v;
}

typedef struct { const Elf64_Rela *tab[2]; uint64_t n[2]; uint64_t relr, relrsz; } relocs_t;
static relocs_t relocs_of(const module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0; relocs_t r = {0};
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: r.relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: r.relrsz = d->d_un.d_val; break;
    }
    r.tab[0] = (const Elf64_Rela *)(m->base + rela); r.tab[1] = (const Elf64_Rela *)(m->base + jmprel);
    r.n[0] = relasz / sizeof(Elf64_Rela); r.n[1] = pltsz / sizeof(Elf64_Rela);
    return r;
}

static void relocate(module_t *m) {
    relocs_t rl = relocs_of(m);
    int cnt[3] = {0}, nint = 0, nweak0 = 0;
    unsigned nrelr = rl.relr ? relr_apply(m, rl.relr, rl.relrsz, (uint64_t)m->base) : 0;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si]; int other, weak0;
            uint64_t v = bind_sym(m, s, &other, &weak0);
            if (other && nint++ < 8) LOG("%s: %s bound to an earlier module's definition (%s)", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            if (weak0 && nweak0++ < 8) LOG("%s: undefined weak %s bound to 0", m->name, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    if (!m->nro || m->loads <= 1)
        LOG("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m->name, cnt[0], nrelr, cnt[1], cnt[2], nint ? " (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: %d bound to an earlier module's definition in all", m->name, nint);
}

#if !DM_NATIVE_EXEC
// The PLT slots of module m the loader bound to m's own functions (aot.h, "The loader's bindings"): each JUMP_SLOT relocation of a symbol m defines whose slot holds
// base + the symbol's value now, as bind_sym left it (the forced take-over, an earlier module's definition and the host's HLE decide). fn gets the slot's and the
// function's module offsets and the name, in the table's order; returns how many
size_t loader_self_bindings(const module_t *m, void (*fn)(uint64_t slot, uint64_t target, const char *name, void *arg), void *arg) {
    relocs_t rl = relocs_of((module_t *)m);
    size_t n = 0;
    for (uint64_t i = 0; i < rl.n[1]; i++) {
        const Elf64_Rela *r = &rl.tab[1][i];
        const Elf64_Sym *s = &m->symtab[ELF64_R_SYM(r->r_info)];
        if (ELF64_R_TYPE(r->r_info) != R_AARCH64_JUMP_SLOT || !s->st_shndx || r->r_offset > m->size - 8) continue;
        uint64_t now;
        memcpy(&now, m->base + r->r_offset, 8);
        if (now != (uint64_t)(uintptr_t)m->base + s->st_value) continue;
        fn(r->r_offset, s->st_value, m->strtab + s->st_name, arg);
        n++;
    }
    return n;
}
#endif

// ---- NRO modules (nn::ro, hle_ro.c) ----
// A game may load modules at run time: nn::ro::LoadModule hands over an NRO image it read from its files (Super Smash Bros. Ultimate: the fighters' code, 98 NROs in data.arc, whose
// exports main imports: 1,045 lua2cpp::create_agent_* functions). iOS runs no code loaded at run time, so the NROs are rewritten offline like the other modules (dm/tools/nro_collect.py,
// patch_nso.py) and listed in <patched dir>/nro.tsv with their SHA-256. All are mapped at start (code read-only + execute from then on); none takes part in symbol lookups until the game
// loads it. A load restores the NRO's data and bss as rewritten (after the first), relocates it against the modules loaded then, in load order (Horizon's rtld does the same), and binds the
// symbol relocations of the modules loaded before it that it defines and nothing loaded defined (Horizon: BindVariables, and functions bound lazily at their first call): main's lua2cpp
// imports are stubs until then. Only data is written, so the same works with code embedded in the iOS executable. An unload binds every relocation that pointed into the NRO again
static void map_image(module_t *m, const char *dir);
static uint8_t g_refused[16][32]; static int g_nrefused;   // NROs the pack refused (online SDKs, nro.tsv rows named "refused")
int nro_refused(const uint8_t sha256[32]) {
    for (int i = 0; i < g_nrefused && i < 16; i++) if (!memcmp(g_refused[i], sha256, 32)) return 1;
    return 0;
}
static FILE *open_nros(const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/nro.tsv", dir);
    const char *off = getenv("DM_NRO");
    if (off && !atoi(off)) { LOG("NRO: loading off (DM_NRO=0): every LoadModule fails"); return NULL; }
    return fopen(path, "r");
}
// Final Fantasy X/X-2 registers 1,432 NROs, exceeding the old 128-slot array before nnMain. Size both tables from the actual manifest before any module pointer escapes;
// never realloc them, since nn::ro handles and the activation list retain module_t pointers. Refused SDK rows need no slot.
static void allocate_modules(int n, FILE *nros) {
    char line[512], name[16];
    if (n <= 0) die("no modules to load");
    if (nros) {
        while (fgets(line, sizeof line, nros)) {
            if (line[0] == '#' || sscanf(line, "%15s", name) != 1 || !strcmp(name, "refused")) continue;
            if (n == INT_MAX) die("too many modules");
            n++;
        }
        if (ferror(nros) || fseek(nros, 0, SEEK_SET)) die("cannot read NRO manifest");
    }
    g_mod_capacity = n;
    g_mod = calloc((size_t)n, sizeof *g_mod); g_loaded = calloc((size_t)n, sizeof *g_loaded);
    if (!g_mod || !g_loaded) die("cannot allocate %d module slots", n);
}
static void preload_nros(const char *dir, FILE *f) {
    char path[1024], line[512]; snprintf(path, sizeof path, "%s/nro.tsv", dir);
    if (!f) return;
    int n0 = g_nmod; size_t data = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[16], hex[65]; unsigned size;
        if (sscanf(line, "%15s %64s %u", name, hex, &size) != 3 || strlen(hex) != 64) die("%s: bad row %s", path, line);
        if (!strcmp(name, "refused")) {   // an online SDK the pack left out (dm/tools/nro_collect.py ONLINE_SDKS): LoadModule refuses its hash
            if (g_nrefused < (int)(sizeof g_refused / sizeof *g_refused)) for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &g_refused[g_nrefused][k]);
            g_nrefused++; continue;
        }
        if (g_nmod >= g_mod_capacity) die("%s: NRO manifest grew while loading", path);
        module_t *m = &g_mod[g_nmod];
        strlcpy(m->name, name, sizeof m->name); m->nro = 1; m->nro_size = size;
        for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &m->sha256[k]);
        read_layout(m, dir); map_image(m, dir); parse_dynamic(m); register_gates(m, dir);
        // The NRO header gives initialized data separately from bss. Keep only those bytes: bss is known zero before the first load, even for FFX's 925 MB static heap.
        m->pristine_len = *(const uint32_t *)(m->base + 0x34);
        if (*(const uint32_t *)(m->base + 0x30) != m->data_off || m->pristine_len > m->key_page_off - m->data_off) die("%s: NRO data layout mismatch", m->name);
        data += m->pristine_len;
        g_nmod++;
    }
    fclose(f);
    LOG("NRO: %d modules mapped (%s), loaded when the game asks; initialized data %.1f MB, snapshotted on first load", g_nmod - n0, path, data / 1048576.0);
}

module_t *nro_find(const uint8_t sha256[32]) {
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].nro && !memcmp(g_mod[i].sha256, sha256, 32)) return &g_mod[i];
    return NULL;
}

// Bind the symbol relocations of module k again: those whose symbol nm defines (load) or whose slot points into nm (unload). Returns how many changed
static unsigned rebind(module_t *k, module_t *nm, int load) {
    relocs_t rl = relocs_of(k); unsigned n = 0;
    uint64_t lo = (uint64_t)nm->base, hi = lo + nm->size;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            unsigned type = ELF64_R_TYPE(r->r_info);
            if (type == R_AARCH64_RELATIVE) continue;
            const Elf64_Sym *s = &k->symtab[ELF64_R_SYM(r->r_info)];
            uint64_t *where = (uint64_t *)(k->base + r->r_offset), add = type == R_AARCH64_JUMP_SLOT ? 0 : (uint64_t)r->r_addend, cur = *where - add;
            if (load ? s->st_shndx || !module_sym(nm, k->strtab + s->st_name) : !(cur >= lo && cur < hi)) continue;
            int other, weak0; uint64_t v = bind_sym(k, s, &other, &weak0);
            if (v != cur) { *where = v + add; n++; }
        }
    return n;
}

// Every load of an NRO, linked or alone, starts from its data as rewritten and a zero bss. A module already loaded is not loaded again: resetting its data under a live
// user (a second CreateJitEnvironment for a plugin whose first environment is not destroyed, a LoadModule of a plugin's image) would pull its state from under it. The first load snapshots the initialized data (only those bytes: no guest code can modify
// an inactive NRO before it, and copying FFX/X-2's 1,432 modules eagerly committed 5.8 GB of mostly unused bss); a reload restores it and zeroes the bss. Call with g_modules_mu held
static void nro_reset_data(module_t *m) {
    if (m->loaded) die("NRO %s: loaded again while it is loaded%s", m->name, m->alone ? " alone (a JIT plugin whose environment was not destroyed)" : "");
    if (!m->pristine) {
        if (!(m->pristine = malloc(m->pristine_len ? m->pristine_len : 1))) die("%s: pristine data copy", m->name);
        memcpy(m->pristine, m->base + m->data_off, m->pristine_len);
    }
    if (m->loads) {
        memcpy(m->base + m->data_off, m->pristine, m->pristine_len);
        memset(m->base + m->data_off + m->pristine_len, 0, m->key_page_off - m->data_off - m->pristine_len);
    }
    m->loads++; m->loaded = 1;
}

void nro_load(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    nro_reset_data(m); g_loaded[g_nloaded++] = m;
    relocate(m);
    unsigned n = 0;
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].loaded && !g_mod[i].alone && &g_mod[i] != m) n += rebind(&g_mod[i], m, 1);   // a module loaded alone binds nothing outside itself
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s loaded (load %u, base %p): %u imports of the modules loaded before bound to it", m->name, m->loads, m->base, n);
}

// A JIT plugin (hle_jit.c) runs in the jit service's own process on Horizon, beside the game but not linked with it: it is relocated against itself only (its own definitions,
// undefined weak symbols 0; Super Mario 64's EmuSystemJITPlugin.nro has only those and relative relocations) and never takes part in the game's symbol lookups
void nro_load_alone(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    nro_reset_data(m); m->alone = 1;
    relocs_t rl = relocs_of(m);
    unsigned nrel = rl.relr ? relr_apply(m, rl.relr, rl.relrsz, (uint64_t)m->base) : 0, nsym = 0;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; nrel++; continue; }
            const Elf64_Sym *s = &m->symtab[ELF64_R_SYM(r->r_info)];
            if (!s->st_shndx && ELF64_ST_BIND(s->st_info) != STB_WEAK) die("%s: %s is not defined in the module, and a module loaded alone binds nothing outside itself", m->name, m->strtab + s->st_name);
            uint64_t v = s->st_shndx ? (uint64_t)m->base + s->st_value : 0;
            *where = type == R_AARCH64_JUMP_SLOT ? v : v + r->r_addend; nsym++;
        }
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s loaded alone (load %u, base %p): %u relative relocations, %u to its own or undefined weak symbols", m->name, m->loads, m->base, nrel, nsym);
}

// Its fini has run (the caller's). Deactivated as nro_unload deactivates a linked one, under the same locks and with the same check of stacks borrowing its data; nothing to
// unbind, as no other module was bound to it
void nro_unload_alone(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    if (!m->alone) die("NRO %s: unloaded alone but not loaded alone", m->name);
    if (!svc_nro_deactivate(m)) die("NRO %s: cannot unload while a mapped stack borrows its data", m->name);
    m->alone = 0;
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s unloaded (it was loaded alone)", m->name);
}

void nro_unload(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    if (!svc_nro_deactivate(m)) die("NRO %s: cannot unload while a mapped stack borrows its data", m->name);
    for (int i = 0; i < g_nloaded; i++) if (g_loaded[i] == m) { memmove(g_loaded + i, g_loaded + i + 1, (size_t)(--g_nloaded - i) * sizeof *g_loaded); break; }
    unsigned n = 0;
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].loaded && !g_mod[i].alone) n += rebind(&g_mod[i], m, 0);
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s unloaded: %u imports that pointed into it bound again", m->name, n);
}

// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
            case DT_RELR: relr = d->d_un.d_ptr; break; case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
        // DT_RELR slots: subtracting the base makes them address-independent (done on the copy, reusing relr_apply's walk)
        if (relr) { uint8_t *b = m->base; m->base = cp; relr_apply(m, relr, relrsz, -(uint64_t)b); m->base = b; }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        plat_sha256_ctx c; plat_sha256_init(&c); plat_sha256_update(&c, cp, m->size);
        for (int t = 0; t < 2; t++)                                // Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                plat_sha256_update(&c, &tail, 8);
            }
        uint8_t d[32]; plat_sha256_final(&c, d); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s size %#zx relocation slots %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

// ---- A served nn::jit image (hle_jit.c, dm/tools/jit_capture.py) ----
// A host that runs no code made at run time (iOS; the Mac with DM_JIT_SERVE=<dir>) serves the JIT plugin's output from an image built on the Mac. Its code reaches the modules
// with PC-relative addresses, so the modules are mapped at the image's canonical layout: one reservation holding the code memory at 0 and every module at the offset jit.tsv gives
// it (mod rows), the distances the image was built for. Modules the layout does not name are mapped as before
static uint8_t *g_jit_base;
#if DM_NATIVE_EXEC   // the layout's own state, which only native execution reads (below)
static uint64_t g_jit_span;
static struct { char name[16]; uint64_t off, size, pre; } g_jit_mod[256]; static int g_njit_mod;
#endif
// Where the image is: DM_JIT_SERVE; on iOS also the folder "jit" beside the RomFS folder (the app's jit/, staged with the RomFS). NULL: the plugin runs (the Mac's default)
const char *dm_jit_serve_dir(void) {
    static int init; static char dir[1024];
    if (!init) {
        init = 1; const char *e = getenv("DM_JIT_SERVE");
        if (e && *e) strlcpy(dir, e, sizeof dir);
#if TARGET_OS_IPHONE
        else if (getenv("DM_ROMFS")) {
            snprintf(dir, sizeof dir, "%s", getenv("DM_ROMFS")); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
            strlcat(dir, "/jit", sizeof dir);
            char t[1100]; snprintf(t, sizeof t, "%s/jit.tsv", dir); if (access(t, R_OK)) dir[0] = 0;
        }
#endif
    }
    return dir[0] ? dir : NULL;
}
uint8_t *dm_jit_layout_base(void) { return g_jit_base; }
#if DM_NATIVE_EXEC   // the layout places natively run modules at the image's distances; the x86-64 engine maps its modules in the arena and lays out none (its base stays NULL)
static void jit_layout_init(void) {
    const char *d = dm_jit_serve_dir(); if (!d) return;
    char path[1100], line[512]; snprintf(path, sizeof path, "%s/jit.tsv", d);
    FILE *f = fopen(path, "r"); if (!f) die("DM_JIT_SERVE: no %s", path);
    unsigned long long rx = 0, ro = 0, img = 0;
    while (fgets(line, sizeof line, f)) {
        char nm[32]; unsigned long long off, size, pre;
        if (sscanf(line, "layout	%llx	%llx	%llx", &rx, &ro, &img) == 3) continue;
        if (sscanf(line, "mod	%31s	%llx	%llx	%llx", nm, &off, &size, &pre) == 4 && g_njit_mod < 256) {
            strlcpy(g_jit_mod[g_njit_mod].name, nm, 16); g_jit_mod[g_njit_mod].off = off; g_jit_mod[g_njit_mod].size = size; g_jit_mod[g_njit_mod].pre = pre; g_njit_mod++;
            if (off + size > g_jit_span) g_jit_span = off + size;
        }
        if (!strncmp(line, "req	", 4)) break;
    }
    fclose(f);
    if (!rx || !g_njit_mod) die("%s: no layout", path);
#if TARGET_OS_IPHONE
    vm_address_t a = 0;
    if (vm_allocate(mach_task_self(), &a, g_jit_span, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("jit layout: cannot reserve %#llx bytes", (unsigned long long)g_jit_span);
    g_jit_base = (uint8_t *)a;
#else
    g_jit_base = mmap(NULL, g_jit_span, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_jit_base == MAP_FAILED) die("jit layout: cannot reserve %#llx bytes", (unsigned long long)g_jit_span);
#endif
    LOG("jit layout (%s): code memory and %d modules in %#llx bytes at %p", path, g_njit_mod, (unsigned long long)g_jit_span, g_jit_base);
}
// The place of module m in the layout (its base), NULL when the layout does not name it. Its size and front patch area must be those the image was built with
static uint8_t *jit_slot(module_t *m, uint64_t size, uint64_t pre) {
    for (int i = 0; g_jit_base && i < g_njit_mod; i++) if (!strcmp(g_jit_mod[i].name, m->name)) {
        if (g_jit_mod[i].size != size || g_jit_mod[i].pre != pre) die("jit layout: %s is %#llx + %#llx bytes here, %#llx + %#llx in the image: rebuild the image (dm/tools/jit_capture.py)", m->name,
            (unsigned long long)size, (unsigned long long)pre, (unsigned long long)g_jit_mod[i].size, (unsigned long long)g_jit_mod[i].pre);
        return g_jit_base + g_jit_mod[i].off;
    }
    return NULL;
}
#endif


#if TARGET_OS_IPHONE
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx (%#lx bytes) failed: %d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s permissions after vm_remap %#x, missing %#x", m->name, cur, need);
}
// The image the module sections are read from (front.h dm_set_game_image_header). getsectiondata takes any loaded image's header and applies its slide
#include "front.h"
static const struct mach_header_64 *g_game_image = &_mh_execute_header;
void dm_set_game_image_header(const void *mh) { g_game_image = mh; }
#define IMAGE_MH g_game_image
// The NRO modules (loader.c "NRO modules") are packed instead of getting sections of their own: a Mach-O file has at most 255 sections, and Super Smash Bros. Ultimate's 98 NROs would
// take 294. gen_embed.py puts their pieces one after another, each 16K aligned, in __TEXT,__dm_nro_x0 / __DM_NRO_W,__img / __TEXT,__dm_nro_x1, and __DATA,__dm_nro_tab lists them:
// 32 bytes of name, then the offset and length of its x0, W and x1 pieces in their sections (u64 each). A module name never holds an underscore, so these names meet no module's
typedef struct { char name[32]; uint64_t off[3][2]; } nro_ent;
// DM_EMBED_TAG (main.m, an app holding several programs of one game): the running program's sections are tagged (gen_embed.py --tag: __dm<tag><name>x0, …, __dm<tag>_nro_x0)
static const char *embed_tag(void) { const char *t = getenv("DM_EMBED_TAG"); return t ? t : ""; }
static int nro_pieces(const char *name, const uint8_t **x0, unsigned long *z0, const uint8_t **w, unsigned long *zw, const uint8_t **x1, unsigned long *z1) {
    unsigned long zt, s0, sw, s1;
    char st[17], s0n[17], swn[17], s1n[17]; const char *g = embed_tag();
    snprintf(st, sizeof st, "__dm%s_nro_tab", g); snprintf(s0n, sizeof s0n, "__dm%s_nro_x0", g); snprintf(swn, sizeof swn, "__DM%s_NRO_W", g); snprintf(s1n, sizeof s1n, "__dm%s_nro_x1", g);
    const nro_ent *t = (const nro_ent *)getsectiondata(IMAGE_MH, "__DATA", st, &zt);
    const uint8_t *b0 = getsectiondata(IMAGE_MH, "__TEXT", s0n, &s0), *bw = getsectiondata(IMAGE_MH, swn, "__img", &sw), *b1 = getsectiondata(IMAGE_MH, "__TEXT", s1n, &s1);
    if (!t || !b0 || !bw || !b1) return 0;
    for (unsigned long i = 0; i < zt / sizeof *t; i++) if (!strncmp(t[i].name, name, sizeof t[i].name)) {
        *x0 = b0 + t[i].off[0][0]; *z0 = t[i].off[0][1]; *w = bw + t[i].off[1][0]; *zw = t[i].off[1][1]; *x1 = b1 + t[i].off[2][0]; *z1 = t[i].off[2][1];
        return 1;
    }
    return 0;
}
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; const char *g = embed_tag();
    snprintf(sx0, sizeof sx0, "__dm%s%sx0", g, m->name); snprintf(sx1, sizeof sx1, "__dm%s%sx1", g, m->name); snprintf(dm, sizeof dm, "__DM%s%sW", g, m->name);
    unsigned long z0 = 0, zw = 0, z1 = 0;
    const uint8_t *x0 = NULL, *w = NULL, *x1 = NULL;
    if (m->nro) { if (!nro_pieces(m->name, &x0, &z0, &w, &zw, &x1, &z1)) die("NRO %s: not in the embedded NRO table (__DATA,__dm_nro_tab)", m->name); }
    else { x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0); w = getsectiondata(IMAGE_MH, dm, "__img", &zw); x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1); }
    if (!x0 || !w || !x1) die("embedded sections %s / %s / %s missing", sx0, dm, sx1);
    // The back patch area (patch_nso.py TWO) is a fourth section, __dm<name>x2; NROs never have one (nro_pieces holds three pieces)
    unsigned long z2 = 0; const uint8_t *x2 = NULL;
    if (m->back_len) {
        char sx2[17]; snprintf(sx2, sizeof sx2, "__dm%s%sx2", g, m->name);
        if (m->nro || !(x2 = getsectiondata(IMAGE_MH, "__TEXT", sx2, &z2)) || z2 != m->back_len || ((uintptr_t)x2 & 0x3FFF)) die("embedded back patch area %s missing or wrong", sx2);
    }
    // Patch area before the base (patch_off < 0): W ends at the end of data (FILE_IMAGE_LEN); bss and the key page are zero pages from vm_allocate and are not in the executable
    int front = m->patch_off < 0;
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off) || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s embedded section layout mismatch (x0 %#lx w %#lx x1 %#lx, PATCH_OFF %#llx)", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = front ? m->image_size : m->patch_off + z1;
    uint64_t pre = front ? (uint64_t)-m->patch_off : 0;
    if (front) m->patch_len = z1;
    vm_address_t base = 0; uint8_t *slot = jit_slot(m, m->size, pre);
    if (slot) base = (vm_address_t)slot - pre;   // inside the jit layout's reservation
    else if (vm_allocate(mach_task_self(), &base, pre + m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s failed to reserve %#llx bytes of address space", m->name, (unsigned long long)(pre + m->size));
    base += pre;
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    // A module with nothing to rewrite has an empty patch area (87 of Super Smash Bros. Ultimate's 98 NROs: no x18, SVC or TLS register use); vm_remap refuses 0 bytes
    // (KERN_INVALID_ARGUMENT), which stopped the app at launch on an iPhone at the first such NRO (found in PR #94)
    if (z1) remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (z2) remap_at(m, base + m->back_off, x2, z2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
static void mod_span_init(int n) { (void)n; }
uint8_t *dm_mod_rtld_slot(size_t *page_out) { (void)page_out; return NULL; }
#else
#if !DM_NATIVE_EXEC
// The x86-64 engine maps its modules in the arena (aot_arena.c) and reserves no startup span, so there is no slot for a stand-in rtld (hle_exl.c, Mac only)
uint8_t *dm_mod_rtld_slot(size_t *page_out) { (void)page_out; return NULL; }
#else
static uint8_t *g_mod_span;
static size_t g_mod_span_page;

uint8_t *dm_mod_rtld_slot(size_t *page_out) {
    if (!g_mod_span) return NULL;
    if (page_out) *page_out = g_mod_span_page;
    return g_mod_span;
}

// Reserve one contiguous virtual span for startup NSOs not placed by jit_slot: 3 pages at the bottom for an optional
// stand-in rtld (hle_exl.c), then each module in load order (main, subsdk*, mod plugins, sdk) separated by a PROT_NONE
// guard page. Guarantees Horizon's ascending address order (rtld < main < subsdks < sdk) even when small plugins would
// otherwise land in an mmap gap below main, and keeps FAR modules' front patch areas strictly above the rtld slot.
static void mod_span_init(int n) {
    if (n <= 0) return;
    long pg_raw = sysconf(_SC_PAGESIZE);
    size_t pg = pg_raw >= 0x4000 ? (size_t)pg_raw : 0x4000;
    size_t *offs = calloc((size_t)n, sizeof *offs), *lens = calloc((size_t)n, sizeof *lens);
    if (!offs || !lens) { free(offs); free(lens); return; }
    size_t total = 4 * pg;   // [0, 3*pg): optional stand-in rtld; [3*pg, 4*pg): guard page
    int count = 0;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        uint64_t pre = m->patch_off < 0 ? (uint64_t)-m->patch_off : 0, img;
        if (m->patch_off < 0) img = m->image_size;
        else {
            char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", m->dir, m->name);
            struct stat st;
            if (stat(path, &st) || st.st_size <= 0) continue;
            img = ((uint64_t)st.st_size + 0x3FFFull) & ~0x3FFFull;
        }
        if (jit_slot(m, img, pre)) continue;
        size_t span = (size_t)((pre + img + (uint64_t)pg - 1) & ~((uint64_t)pg - 1));
        offs[i] = total; lens[i] = span;
        total += span + pg;   // one guard page after each module
        count++;
    }
    if (count) {
        uint8_t *base = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base != MAP_FAILED) {
            g_mod_span = base; g_mod_span_page = pg;
            for (int i = 0; i < n; i++) if (lens[i]) { g_mod[i].span_slot = base + offs[i]; g_mod[i].span_len = lens[i]; }
        }
    }
    free(offs); free(lens);
}
#endif

static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// Rewriter cuts marked ALIAS_CUT only exist to fit an iPhone's small alias region (mem_alias). On the Mac, whose alias region is at least the Switch's 64 GB (DM_SWITCH_ALIAS),
// restore their original bytes while text is still writable so the game keeps its native reservation layout (docs/reference/memory.md). A smaller DM_ALIAS_SIZE (such as
// build_ipa_from_nsp.py's heap check reproducing an iPhone layout) or DM_KEEP_ALIAS_CUTS=1 (region_check measuring the iPhone binary's reservations) keeps the cuts in place.
static void undo_alias_cuts(module_t *m) {
    if (!m->n_alias_cut) return;
    const char *keep = getenv("DM_KEEP_ALIAS_CUTS");
    if (keep && *keep && strcmp(keep, "0") != 0) {
        LOG("%s: %d cuts for a small alias region kept (DM_KEEP_ALIAS_CUTS=%s)", m->name, m->n_alias_cut, keep);
        return;
    }
    size_t a = dm_alias_size();
    if (a < DM_SWITCH_ALIAS) {
        LOG("%s: %d cuts for a small alias region kept (alias region %#zx, needs %#llx to restore)", m->name, m->n_alias_cut, a, (unsigned long long)DM_SWITCH_ALIAS);
        return;
    }
    for (int i = 0; i < m->n_alias_cut; i++) {
        uint8_t *p = m->base + m->alias_cut[i].off;
        if (m->alias_cut[i].off > m->text_end || m->alias_cut[i].len > m->text_end - m->alias_cut[i].off ||
            memcmp(p, m->alias_cut[i].cut, m->alias_cut[i].len) != 0)
            die("%s: the cut at %#llx does not hold the bytes its ALIAS_CUT row names", m->name, (unsigned long long)m->alias_cut[i].off);
    }
    for (int i = 0; i < m->n_alias_cut; i++) memcpy(m->base + m->alias_cut[i].off, m->alias_cut[i].orig, m->alias_cut[i].len);
    LOG("%s: %d cuts for a small alias region undone (alias region %#zx)", m->name, m->n_alias_cut, a);
}

// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE)
        // With a back patch area (patch_nso.py TWO) the file ends with it, BACK_LEN bytes, copied to BACK_OFF in the image
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len, bl = m->back_len;
        if (sz < fl + bl) die("%s is shorter than FILE_IMAGE_LEN %#llx + BACK_LEN %#llx", path, (unsigned long long)fl, (unsigned long long)bl);
        m->size = m->image_size; m->patch_len = (sz - bl - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
#if DM_NATIVE_EXEC
        uint8_t *slot = jit_slot(m, m->size, pre);
        if (!slot && m->span_slot && m->span_len >= pre + m->size) slot = m->span_slot + pre;
        uint8_t *a = mmap(slot ? slot - pre : NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (slot ? MAP_FIXED : 0), -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
#else
        uint8_t *a = dm_guest_alloc(pre + m->size);   // x86-64 engine: in the arena
        if (!a) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
#endif
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - bl - fl); if (bl) memcpy(m->base + m->back_off, raw + sz - bl, bl);
        free(raw); install_key_page(m); undo_alias_cuts(m);
#if DM_NATIVE_EXEC
        if (bl && mprotect(m->base + m->back_off, bl, PROT_READ | PROT_EXEC)) die("%s mprotect back patch", m->name);
#else
        if (bl && plat_vm_protect(m->base + m->back_off, bl, PLAT_PROT_READ)) die("%s mprotect back patch", m->name);   // x86-64 engine: readable, never executable (design 3.2)
#endif
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
        if (plat_vm_protect(m->base, rx, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect text", m->name);
        if (plat_vm_protect(a, m->patch_len, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect patch", m->name);
#else
        if (plat_vm_protect(m->base, rx, PLAT_PROT_READ)) die("%s mprotect text", m->name);   // x86-64 engine: readable, never executable (design 3.2)
        if (plat_vm_protect(a, m->patch_len, PLAT_PROT_READ)) die("%s mprotect patch", m->name);
#endif
        if (pre > m->patch_len && plat_vm_protect(a + m->patch_len, pre - m->patch_len, PLAT_PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
    uint8_t *slot = jit_slot(m, m->size, 0);
    if (!slot && m->span_slot && m->span_len >= m->size) slot = m->span_slot;
    m->base = mmap(slot, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (slot ? MAP_FIXED : 0), -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
#else
    m->base = dm_guest_alloc(m->size);   // x86-64 engine: in the arena
    if (!m->base) die("%s mmap", m->name);
#endif
    memcpy(m->base, raw, sz); free(raw); install_key_page(m); undo_alias_cuts(m);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
    if (plat_vm_protect(m->base, rx, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect text", m->name);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (plat_vm_protect(m->base + po, m->size - po, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect patch", m->name);
#else
    if (plat_vm_protect(m->base, rx, PLAT_PROT_READ)) die("%s mprotect text", m->name);   // x86-64 engine: readable, never executable (design 3.2)
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (plat_vm_protect(m->base + po, m->size - po, PLAT_PROT_READ)) die("%s mprotect patch", m->name);
#endif
}
#endif

static char g_module_dir[1024];   // the folder the modules were loaded from (dm_module_dir)
const char *dm_module_dir(void) { return g_module_dir; }

#if DM_NATIVE_EXEC   // the x86-64 engine's load_modules takes the recompiler's input instead (below, "The game-module path")
void load_modules(const char *dir, const char **names, int n) {
    strlcpy(g_module_dir, dir, sizeof g_module_dir);
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    FILE *nros = open_nros(dir);
    allocate_modules(n, nros);
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        // "<folder>/<name>": a module from another folder (a mod's code plugin), else one of the module folder
        const char *sl = strrchr(names[i], '/');
        if (sl) { char *d = strndup(names[i], (size_t)(sl - names[i])); if (!d) die("module folder"); m->dir = d; strlcpy(m->name, sl + 1, sizeof m->name); }
        else { m->dir = g_module_dir; strlcpy(m->name, names[i], sizeof m->name); }
        read_layout(m, m->dir);
    }
    create_keys();
    jit_layout_init();
    mod_span_init(n);
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, m->dir); tm += now_ns() - t;   // On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, m->dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx%s%s", m->name, m->base, m->size, m->dir != g_module_dir ? " from " : "", m->dir != g_module_dir ? m->dir : "");
        m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    preload_nros(dir, nros);   // mapped now, relocated when the game loads them (hle_ro.c)
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}
#endif

// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT. The order is reverse of the load list:
// modules later in the list are the ones the earlier ones depend on, so the sdk module's DT_INIT, which constructs the
// libc++ globals (std::cout and friends), runs before main's static initializers, which already write to them. Forward
// order crashed Little Busters! Converted Edition at its first log line (std::ostream::write on an unconstructed cout).
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = g_nmod - 1; i >= 0; i--) {
        module_t *m = &g_mod[i];
        if (!m->init || m->nro) continue;   // an NRO's init runs when the game loads it (hle_ro.c)
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
#if DM_NATIVE_EXEC
        ((void (*)(void))(m->base + m->init))();
#else
        dm_guest_call(dm_aot_self_state(), (uint64_t)(uintptr_t)(m->base + m->init), 0);   // x86-64 engine: guest code from the host (design 2.4)
#endif
        dm_exl_sync_all();   // flush any persistent MapProcessMemory views before earlier modules' (e.g. main's) DT_INIT runs
    }
}
#if !DM_NATIVE_EXEC

// ---- The x86-64 execution engine's module images ----
// A module image as it is: an NRO file (a game's modules come through load_modules below, from their manifests), whose code runs from the pack the caller loads
// for it (aot_arena.c dm_aot_pack_load). Mapped one to one in the arena at a base the engine chooses (the pack's code is base-relative: 3.3), [0, ro_off)
// readable and never executable, the rest writable, bss zero; its dynamic section parsed; loaded (in the symbol lookups) and relocated and bound as
// load_modules relocates every module (relocations are data: 9.4). Modules added this way share g_mod with load_modules, at a fixed capacity, since
// module_t pointers never move
#define AOT_MAX_MODULES 64
// The image one to one in the arena (aot_arena.c dm_aot_image_map: [0, ro_off) readable and never executable, the rest readable and writable, bss zero) and the module's
// layout fields as load_modules' mapping sets them on Apple. key_page_off is the end of the mapping, where Apple's key page would begin: the end of the module's data and
// bss for dm_sdk_major's search and QueryMemory's read-only runs (none here)
static void aot_place(module_t *m, const uint8_t *bytes, size_t len, size_t bss, uint64_t text_end, uint64_t ro_off, uint64_t data_off) {
    size_t mapped = 0;
    m->base = dm_aot_image_map(bytes, len, bss, ro_off, &mapped);
    m->size = mapped; m->text_end = text_end; m->ro_off = ro_off; m->data_off = data_off; m->image_size = len; m->file_image_len = len;
    m->key_page_off = mapped; m->key_page_size = 0; m->patch_off = 0;
}
module_t *loader_add_image(const char *name, const uint8_t *bytes, size_t len, size_t bss, uint64_t text_end, uint64_t ro_off, uint64_t data_off) {
    pthread_rwlock_wrlock(&g_modules_mu);
    if (!g_mod) {
        g_mod_capacity = AOT_MAX_MODULES;
        g_mod = calloc((size_t)g_mod_capacity, sizeof *g_mod); g_loaded = calloc((size_t)g_mod_capacity, sizeof *g_loaded);
        if (!g_mod || !g_loaded) die("cannot allocate %d module slots", g_mod_capacity);
    }
    if (g_nmod >= g_mod_capacity) die("%s: no room for another module (%d)", name, g_mod_capacity);
    module_t *m = &g_mod[g_nmod];
    strlcpy(m->name, name, sizeof m->name);
    aot_place(m, bytes, len, bss, text_end, ro_off, data_off);
    parse_dynamic(m);
    m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    if (!g_force_hits) force_init();
    relocate(m);
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("%s: base %p size %#zx (image %#zx, bss %#zx; x86-64 engine)", name, (void *)m->base, m->size, len, bss);
    return m;
}

// hle.c's named stub for name, as resolve_import makes one for an import nothing implements (&unimpl_stubs[k], or the import table's function when it has the name),
// for a host function handed to the guest at run time (aot_arena.c dm_guest_fnptr): under the module lock, which every other stub allocation after the start holds too
// (an NRO's relocation). name must live as long as the process
uint64_t loader_named_stub(const char *name) {
    pthread_rwlock_wrlock(&g_modules_mu);
    const uint64_t v = resolve_import(NULL, name);
    pthread_rwlock_unlock(&g_modules_mu);
    return v;
}

// ---- The game-module path of the x86-64 execution engine (docs/design/aot-recompiler.md 1.1, 1.2, 9.4) ----
// load_modules' folder holds the recompiler's input, as it holds the rewritten modules on Apple (dm/tools/aot/README.md, "A game module in the host"). Per module:
//   <name>.aot.tsv    the manifest patch_nso.py --aot wrote: the module, the NSO build ID and the SHA-256 of the image; layout, aux, data, obs, spin, cut and oracle rows
//                     (patch_nso.py, "The recompiler's input"; nxaot reads the same text: dm/tools/aot/src/module/manifest.cpp)
//   <name>.image.bin  the behaviour image: the NSO's segments as the rewriter decompressed them (it reads the LZ4 of the NSO, as for Apple's <name>.patched.bin: the host decompresses
//                     nothing), with the cuts and the frame step's branches, none of Apple's rewrites
//   <name>.aot.so     the pack `nxaot build` made from the two (.aot.dll on Windows; aot.h DM_AOT_PACK_SUFFIX)
// The image is checked against its manifest (size, layout, SHA-256: refused with both hashes) and mapped one to one in the arena as loader_add_image maps one; the build ID
// goes to module_t.build_id, as fixups.tsv's on Apple, for the host's per-build behaviour (dm_main_build); the aux row places the frame step's state page; the obs and spin rows
// go into hle.c's gate table at their guest addresses, which on Apple holds the gate blocks' return addresses (dm_aot_gate dispatches them: aot_run.c). Then every module is
// relocated and bound (dm_aot_bind: host functions and unimplemented imports at their guest-visible slots), and its pack loaded with the image's identity (aot_arena.c)
int dm_aot_load_module_pack(module_t *m, const char *dir, const uint8_t sha256[32], uint64_t image_size, uint64_t text_end, uint64_t bss, char *err, size_t errn);
// aot.h's (this file is compiled into unit tests that include it without the engine's headers): the manifest's gate rows against the pack's gate sites
#ifndef DM_AOT_GATE_ROW
#define DM_AOT_GATE_ROW
typedef struct dm_aot_gate_row { uint64_t offset, arg; uint32_t kind; } dm_aot_gate_row;
#endif
enum { AOT_GATE_OBS = 1, AOT_GATE_SPIN = 2 };   // include/nxaot/aot_module.h NXAOT_GATE_OBS, NXAOT_GATE_SPIN
int dm_aot_pack_check_gates(const module_t *m, const dm_aot_gate_row *rows, int n, int *unserved, char *err, size_t errn);
void dm_aot_gate_rows(const module_t *m, const dm_aot_gate_row *rows, int n);
typedef struct { uint64_t off, param, word; uint32_t num; } aot_obs_t;
typedef struct { uint64_t head, edge; } aot_edge_t;
typedef struct {
    char module[64], build_id[72], sha256[65];
    uint64_t text_end, ro_off, data_off, bss, image_size, aux_off, aux_len, fps_state;
    int layout, aux, rows[4];   // rows: data, cut, oracle (T, J and W), comments
    aot_obs_t *obs; int nobs;
    aot_edge_t *edge; int nedge, nspin;
    uint8_t sha[32];            // the image's, computed
} aot_manifest_t;

static uint8_t *aot_read(const char *path, size_t *n) {   // a whole file, NUL-terminated; NULL when it cannot be read
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *p = NULL; size_t cap = 0, len = 0, got;
    do {
        if (len + 65536 + 1 > cap) { cap = (len + 65536 + 1) * 2; uint8_t *q = realloc(p, cap); if (!q) { free(p); fclose(f); return NULL; } p = q; }
        got = fread(p + len, 1, 65536, f); len += got;
    } while (got);
    int bad = ferror(f); fclose(f);
    if (bad) { free(p); return NULL; }
    p[len] = 0; *n = len;
    return p;
}
static int aot_hex(const char *s, uint64_t *v) {   // hex with or without 0x, as nxaot's reader takes it
    char *e;
    if (!*s || *s == '-' || *s == '+' || *s == ' ') return 0;
    errno = 0; *v = strtoull(s, &e, 16);
    return !*e && !errno;
}
static int aot_dec(const char *s, uint64_t *v) {
    char *e;
    if (*s < '0' || *s > '9') return 0;
    errno = 0; *v = strtoull(s, &e, 10);
    return !*e && !errno;
}
static int aot_hexbytes(const char *s) { size_t n = strlen(s); if (!n || n % 2) return 0; for (; *s; s++) if (!strchr("0123456789abcdefABCDEF", *s)) return 0; return 1; }

// The manifest's rows into t, every row checked as nxaot's reader checks it; 0, or 1 with the reason in err
static int aot_parse(char *text, aot_manifest_t *t, const char *path, char *err, size_t errn) {
    int line = 0, header = 0;
    for (char *s = text, *next; s && *s; s = next) {
        next = strchr(s, '\n');
        if (next) *next++ = 0;
        line++;
        size_t ln = strlen(s);
        if (ln && s[ln - 1] == '\r') s[--ln] = 0;
        if (!ln) continue;
        char *f[16]; int nf = 0;
        f[nf++] = s;
        for (char *p = s; *p && nf < 16; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        uint64_t a = 0, b = 0, c = 0, d = 0;
        if (!header) {
            if (nf != 4 || strcmp(f[0], "# nxaot-manifest 1") || !*f[1] || strlen(f[1]) >= sizeof t->module || !*f[2] || strlen(f[2]) >= sizeof t->build_id ||
                strlen(f[3]) != 64 || !aot_hexbytes(f[3]) || !aot_hexbytes(f[2])) {
                snprintf(err, errn, "%s line %d: not a manifest of version 1 ('# nxaot-manifest 1', the module, the build ID in hex, the SHA-256 of the image)", path, line); return 1;
            }
            snprintf(t->module, sizeof t->module, "%s", f[1]); snprintf(t->build_id, sizeof t->build_id, "%s", f[2]); snprintf(t->sha256, sizeof t->sha256, "%s", f[3]);
            header = 1; continue;
        }
        const char *k = f[0], *bad = NULL;
        if (k[0] == '#') { t->rows[3]++; continue; }
        if (!strcmp(k, "layout")) {
            if (nf != 11 || strcmp(f[1], "text_end") || strcmp(f[3], "ro_off") || strcmp(f[5], "data_off") || strcmp(f[7], "bss") || strcmp(f[9], "image_size") ||
                !aot_hex(f[2], &t->text_end) || !aot_hex(f[4], &t->ro_off) || !aot_hex(f[6], &t->data_off) || !aot_hex(f[8], &t->bss) || !aot_hex(f[10], &t->image_size))
                bad = "layout: text_end <hex> ro_off <hex> data_off <hex> bss <hex> image_size <hex>";
            else if (t->layout++) bad = "a second layout row";
            else if (!(t->text_end <= t->ro_off && t->ro_off <= t->data_off && t->data_off <= t->image_size)) bad = "layout: text_end <= ro_off <= data_off <= image_size does not hold";
        } else if (!strcmp(k, "aux")) {
            if (nf != 5 || strcmp(f[3], "fps_state") || !aot_hex(f[1], &t->aux_off) || !aot_hex(f[2], &t->aux_len) || !aot_hex(f[4], &t->fps_state)) bad = "aux: <offset> <length> fps_state <offset>";
            else if (t->aux++) bad = "a second aux row";
        } else if (!strcmp(k, "data") || !strcmp(k, "oracle-W")) {
            if (nf != 3 || !aot_hex(f[1], &a) || !aot_hex(f[2], &b) || b < a) bad = "data / oracle-W: <start> <end> with start <= end";
            else t->rows[k[0] == 'd' ? 0 : 2]++;
        } else if (!strcmp(k, "obs")) {
            if (nf != 5 || !aot_hex(f[1], &a) || !aot_dec(f[2], &b) || b > 0xFF || !aot_hex(f[3], &c) || !aot_hex(f[4], &d) || d > 0xFFFFFFFFu)
                bad = "obs: <offset> <number (decimal, below 256)> <parameter> <original instruction word>";
            else {
                aot_obs_t *o = realloc(t->obs, (size_t)(t->nobs + 1) * sizeof *o);
                if (!o) die("aot: no memory for a manifest");
                t->obs = o; t->obs[t->nobs++] = (aot_obs_t){a, c, d, (uint32_t)b};
            }
        } else if (!strcmp(k, "spin") || !strcmp(k, "oracle-T")) {
            if (nf != 3 || !aot_hex(f[1], &a)) bad = "spin / oracle-T: <offset> <offset>[,<offset>...]";
            for (char *p = nf == 3 ? f[2] : NULL, *q; !bad && p; p = q) {
                q = strchr(p, ',');
                if (q) *q++ = 0;
                if (!aot_hex(p, &b)) { bad = "spin / oracle-T: <offset> <offset>[,<offset>...]"; break; }
                if (k[0] == 's') {
                    aot_edge_t *e = realloc(t->edge, (size_t)(t->nedge + 1) * sizeof *e);
                    if (!e) die("aot: no memory for a manifest");
                    t->edge = e; t->edge[t->nedge++] = (aot_edge_t){a, b};
                }
            }
            if (!bad) { if (k[0] == 's') t->nspin++; else t->rows[2]++; }
        } else if (!strcmp(k, "cut")) {
            if (nf < 5 || !aot_hex(f[1], &a) || !aot_hexbytes(f[2]) || !aot_hexbytes(f[3]) || strlen(f[2]) != strlen(f[3])) bad = "cut: <offset> <old hex> <new hex> <reason>";
            else t->rows[1]++;
        } else if (!strcmp(k, "oracle-J")) {
            if (nf != 4 || !aot_hex(f[1], &a) || !aot_hex(f[2], &b) || !aot_dec(f[3], &c) || (c != 1 && c != 2)) bad = "oracle-J: <br offset> <base> <width: 1 or 2>";
            else t->rows[2]++;
        } else bad = "an unknown row kind";
        if (bad) { snprintf(err, errn, "%s line %d: %s", path, line, bad); return 1; }
    }
    if (!header) { snprintf(err, errn, "%s: empty, not a manifest", path); return 1; }
    if (!t->layout) { snprintf(err, errn, "%s: no layout row", path); return 1; }
    return 0;
}

static void aot_hexstr(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

// Module m's manifest and image from dir: checked, mapped, its gates registered, its dynamic section parsed. 0, or 1 with the reason in err
static int aot_map_module(module_t *m, const char *dir, aot_manifest_t *t, char *err, size_t errn) {
    char path[1024], ipath[1024];
    snprintf(path, sizeof path, "%s/%s.aot.tsv", dir, m->name);
    snprintf(ipath, sizeof ipath, "%s/%s.image.bin", dir, m->name);
    size_t n = 0, len = 0;
    char *text = (char *)aot_read(path, &n);
    if (!text) { snprintf(err, errn, "%s: cannot read it (errno %d): rewrite the module with dm/tools/patch_nso.py <NSO> <out> %s --aot %s", path, errno, m->name, dir); return 1; }
    int bad = aot_parse(text, t, path, err, errn);
    free(text);
    if (bad) return 1;
    if (strcmp(t->module, m->name)) { snprintf(err, errn, "%s is the manifest of module %s, not of %s", path, t->module, m->name); return 1; }
    uint8_t *img = aot_read(ipath, &len);
    if (!img) { snprintf(err, errn, "%s: cannot read the image %s names (errno %d)", ipath, path, errno); return 1; }
    char have[65], want[65];
    plat_sha256(img, len, t->sha);
    aot_hexstr(t->sha, 32, have);
    snprintf(want, sizeof want, "%s", t->sha256);
    for (char *c = want; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
    const uint64_t end = t->image_size + t->bss;
    const char *why = NULL;
    if (len != t->image_size) why = "its size is not the layout's image_size";
    else if (strcmp(have, want)) why = "its SHA-256 is not the manifest's";
    else if (t->text_end & 3 || !t->text_end) why = "the layout's text_end is no whole number of instructions";
    else if (t->aux && (t->fps_state < t->data_off || t->fps_state + 0x40 > end || t->aux_off < t->data_off || t->aux_len > t->image_size || t->aux_off > t->image_size - t->aux_len))
        why = "its aux row lies outside the image's data";
    for (int i = 0; !why && i < t->nobs; i++) {
        uint32_t w;
        if (t->obs[i].off >= t->text_end || t->obs[i].off & 3 || !t->obs[i].num) { why = "an obs row names no instruction of the text, or number 0"; break; }
        memcpy(&w, img + t->obs[i].off, 4);
        if (w != t->obs[i].word) { why = "an obs row's original word is not the image's (the gate's instruction must be the original one)"; break; }
    }
    for (int i = 0; !why && i < t->nedge; i++)
        if (t->edge[i].head >= t->text_end || t->edge[i].edge >= t->text_end || (t->edge[i].head | t->edge[i].edge) & 3) why = "a spin row names no instruction of the text";
    if (why) {
        snprintf(err, errn, "%s does not match %s (%s): image %zu bytes, SHA-256 %s; manifest image_size %llu, SHA-256 %s. Rewrite the module again with patch_nso.py --aot",
                 ipath, path, why, len, have, (unsigned long long)t->image_size, want);
        free(img); return 1;
    }
    aot_place(m, img, len, (size_t)t->bss, t->text_end, t->ro_off, t->data_off);
    free(img);
    snprintf(m->build_id, sizeof m->build_id, "%.32s", t->build_id);   // the first 16 bytes, as fixups.tsv's BUILD_ID on Apple
    if (t->aux) {   // the frame step's state page (install_fps_state on Apple), and its stubs' region, which the JIT fallback takes as code (aot_run.c jit_code_ranges)
        m->fps_state_off = t->fps_state; dm_fps_state = m->base + t->fps_state; g_fps_mod = m->name;
        m->aux_off = t->aux_off; m->aux_len = t->aux_len;
    }
    parse_dynamic(m);
    for (int i = 0; i < t->nobs; i++) gate_register((uint64_t)(uintptr_t)m->base + t->obs[i].off, "obs", (int)(t->obs[i].num | t->obs[i].param << 8), t->obs[i].off);
    for (int i = 0; i < t->nedge; i++) gate_register((uint64_t)(uintptr_t)m->base + t->edge[i].edge, "spin", 0, t->edge[i].head);
    // The gates are the host's observers and the spin waits' back-off (Apple's gates.tsv): the pack's code calls them (nxaot_rt_gate, design 1.1) at the sites its
    // descriptor lists, which load_modules checks against these rows once the pack is loaded (aot_arena.c dm_aot_pack_check_gates)
    LOG("%s: base %p size %#zx (behaviour image %#zx, bss %#llx; build %s; %d data blocks, %d cuts, %d oracle rows%s; x86-64 engine)", m->name, (void *)m->base, m->size, len,
        (unsigned long long)t->bss, m->build_id, t->rows[0], t->rows[1], t->rows[2], t->aux ? ", frame step state page" : "");
    return 0;
}

// ---- NROs on the game-module path (nn::ro, hle_ro.c) ----
// The folder's nro.tsv has Apple's rows (name, SHA-256, size; "refused" rows for the online SDKs the pack leaves out: nro_refused). The NRO itself is the image the game
// hands ro::LoadModule (as Horizon makes the game's buffer the module): at its first load it is mapped one to one in the arena from those bytes, as loader_add_image maps
// an image, and its pack <dir>/<name>.aot.so loaded with their identity (nxaot build <name>.nro -o <object> --shared <dir>/<name>.aot.so: the pack of the NRO file,
// whose SHA-256 is the one nro.tsv lists). From then on it is an NRO module as on Apple: nro_load relocates it and binds the earlier modules' imports, nro_unload undoes
// that, a later load restores its data. g_mod has room for every row (allocate_modules counts them), so module pointers never move
typedef struct { char name[16]; uint8_t sha[32]; uint32_t size; int no_pack; } aot_nro_t;   // no_pack: its pack failed to load once (reported then; later loads fail at once)
static aot_nro_t *g_aot_nro; static int g_aot_nnro;
static char *g_aot_dir;   // load_modules' folder: the packs of the NROs lie there
static void aot_read_nros(const char *dir, FILE *f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[16], hex[65]; unsigned size;
        if (sscanf(line, "%15s %64s %u", name, hex, &size) != 3 || strlen(hex) != 64) die("[RED] aot: %s/nro.tsv: bad row %s", dir, line);
        uint8_t sha[32];
        for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &sha[k]);
        if (!strcmp(name, "refused")) {   // an online SDK the pack left out (dm/tools/nro_collect.py ONLINE_SDKS): LoadModule refuses its hash
            if (g_nrefused < (int)(sizeof g_refused / sizeof *g_refused)) memcpy(g_refused[g_nrefused], sha, 32);
            g_nrefused++; continue;
        }
        aot_nro_t *r = realloc(g_aot_nro, (size_t)(g_aot_nnro + 1) * sizeof *r);
        if (!r) die("aot: no memory for nro.tsv");
        g_aot_nro = r;
        r[g_aot_nnro] = (aot_nro_t){0};   // realloc leaves the new row as the heap had it: no_pack read as garbage refused the NRO's first load with no reason given
        snprintf(r[g_aot_nnro].name, sizeof r[g_aot_nnro].name, "%s", name); memcpy(r[g_aot_nnro].sha, sha, 32); r[g_aot_nnro].size = size;
        g_aot_nnro++;
    }
    fclose(f);
    LOG("aot: %s/nro.tsv lists %d NROs (mapped from the game's image at their first ro::LoadModule, each with its pack <name>" AOT_PACK_SUFFIX "), %d refused", dir, g_aot_nnro,
        g_nrefused);
}

module_t *loader_nro_map(const uint8_t *image, uint32_t n, const uint8_t sha[32], char *err, size_t errn) {
    char hx[65];
    aot_hexstr(sha, 32, hx);
    aot_nro_t *row = NULL;
    for (int i = 0; i < g_aot_nnro; i++) if (!memcmp(g_aot_nro[i].sha, sha, 32)) { row = &g_aot_nro[i]; break; }
    if (!row) {
        snprintf(err, errn, "the NRO with SHA-256 %s (%u bytes) has no row in %s/nro.tsv, so it has no pack: list it there (<name> <SHA-256> <size>) and pack it beside it "
                 "(nxaot build <name>.nro -o <object> --shared %s/<name>" AOT_PACK_SUFFIX ")", hx, n, g_aot_dir ? g_aot_dir : "the modules' folder", g_aot_dir ? g_aot_dir : "<folder>");
        return NULL;
    }
    if (row->no_pack) { snprintf(err, errn, "%s (SHA-256 %s): its pack did not load (the reason is above)", row->name, hx); return NULL; }
    uint32_t hdr[8];   // the NRO header from +0x18: size, flags, text, read-only data and data (offset, size), bss size
    if (n < 0x80 || memcmp(image + 0x10, "NRO0", 4)) { snprintf(err, errn, "%s (SHA-256 %s): not an NRO", row->name, hx); return NULL; }
    memcpy(hdr, image + 0x18, sizeof hdr);
    const uint64_t text_end = hdr[3], ro_off = hdr[4], data_off = hdr[6], data_size = hdr[7];
    uint32_t bss; memcpy(&bss, image + 0x38, 4);
    if (hdr[2] || text_end > ro_off || ro_off > data_off || data_off + data_size > n || hdr[0] != n) {
        snprintf(err, errn, "%s (SHA-256 %s): the NRO header's segments do not fit its %u bytes", row->name, hx, n); return NULL;
    }
    pthread_rwlock_wrlock(&g_modules_mu);
    if (g_nmod >= g_mod_capacity) die("[RED] aot: no module slot for NRO %s (%d)", row->name, g_mod_capacity);
    module_t *m = &g_mod[g_nmod];   // filled here, counted (g_nmod) once its pack loaded
    memset(m, 0, sizeof *m);
    strlcpy(m->name, row->name, sizeof m->name);
    m->nro = 1; m->nro_size = n; memcpy(m->sha256, sha, 32);
    aot_place(m, image, n, bss, text_end, ro_off, data_off);
    parse_dynamic(m);
    m->pristine_len = data_size;   // initialized data (nro_load keeps a copy at the first load, restores it at the next)
    pthread_rwlock_unlock(&g_modules_mu);
    if (dm_aot_load_module_pack(m, g_aot_dir, sha, n, text_end, bss, err, errn)) {   // the slot stays unused; the image stays mapped, once (no_pack)
        row->no_pack = 1;
        free(m->symh); memset(m, 0, sizeof *m);
        return NULL;
    }
    pthread_rwlock_wrlock(&g_modules_mu);
    g_nmod++;
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s mapped from the game's image at %p (%u bytes, bss %#x; x86-64 engine)", m->name, (void *)m->base, n, bss);
    return m;
}

// nxhost-run --bindings <folder> (main_x86.c): load_modules writes the loader's bindings and ends the process after the relocation (aot.h dm_aot_write_module_bindings)
static const char *g_bindings_to;
void loader_bindings_only(const char *dir) { g_bindings_to = dir; }

void load_modules(const char *dir, const char **names, int n) {
    const uint64_t t0 = now_ns();
    char err[1200];
    strlcpy(g_module_dir, dir, sizeof g_module_dir);   // dm_module_dir, as Apple's load_modules sets it (the cheat engine's main.changed.tsv)
    FILE *nros = open_nros(dir);
    allocate_modules(n, nros);   // room for the NROs nro.tsv lists too
    g_aot_dir = strdup(dir);
    aot_manifest_t *man = calloc((size_t)n, sizeof *man);
    if (!man) die("aot: no memory for %d manifests", n);
    for (int i = 0; i < n; i++) {   // every module mapped and parsed first, then relocated (cross-module symbols must all be in place), as on Apple
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        m->dir = g_module_dir;   // every module from the one folder: a mod's code plugin from another folder is Apple's (hle_exl.c, Mac only)
        if (aot_map_module(m, dir, &man[i], err, sizeof err)) die("[RED] aot: %s", err);
        m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    }
    force_init();
    for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]);
    force_check();
    if (g_bindings_to) {   // nxhost-run --bindings: the bindings this start made, written, and nothing loaded or run
        int bad = 0;
        for (int i = 0; i < n; i++) bad += dm_aot_write_module_bindings(g_bindings_to, &g_mod[i], man[i].sha, "none (nxhost-run --bindings: the loader's bindings before any pack)");
        LOG("aot: the loader's bindings of %d modules from %s written to %s in %.1f ms (nxhost-run --bindings)%s", n, dir, g_bindings_to, (now_ns() - t0) / 1e6,
            bad ? "; [RED] a file could not be written" : "");
        fflush(NULL);
        _Exit(bad ? 1 : 0);
    }
    if (nros) aot_read_nros(dir, nros);   // mapped when the game loads them (hle_ro.c, loader_nro_map)
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        if (dm_aot_load_module_pack(m, dir, man[i].sha, man[i].image_size, man[i].text_end, man[i].bss, err, sizeof err)) dm_aot_pack_refused(err);
        // The manifest's gates against the sites the pack's code calls (design 1.1): a pack of another manifest is refused; a row it does not serve is a [RED] line
        dm_aot_gate_row *rows = calloc((size_t)(man[i].nobs + man[i].nedge) + 1, sizeof *rows);
        if (!rows) die("aot: no memory for %s's gate rows", m->name);
        int nrows = 0, unserved = 0;
        for (int k = 0; k < man[i].nobs; k++) rows[nrows++] = (dm_aot_gate_row){man[i].obs[k].off, man[i].obs[k].num | man[i].obs[k].param << 8, AOT_GATE_OBS};
        for (int k = 0; k < man[i].nedge; k++) rows[nrows++] = (dm_aot_gate_row){man[i].edge[k].edge, man[i].edge[k].head, AOT_GATE_SPIN};
        if (dm_aot_pack_check_gates(m, rows, nrows, &unserved, err, sizeof err)) dm_aot_pack_refused(err);
        dm_aot_gate_rows(m, rows, nrows);   // the JIT fallback's gate sites: every row, so it serves those no compiled code holds when it reaches them
        if (nrows) LOG("aot: %s: %d gates (%d observation gates, %d spin wait back-edges) called by its pack, %d not", m->name, nrows - unserved, man[i].nobs, man[i].nedge, unserved);
        free(rows);
        free(man[i].obs); free(man[i].edge);
    }
    free(man);
    { void hlecount_seal(void); hlecount_seal(); }
    LOG("aot: %d modules from %s mapped, relocated and their packs loaded in %.1f ms", n, dir, (now_ns() - t0) / 1e6);
}
#endif
