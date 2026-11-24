// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
// A guest function this file calls (docs/design/aot-recompiler.md 2.4): the plain call where the guest's code runs natively (Apple), or where a unit test supplies it
// as a C function (DM_TEST_GUEST_C); on the x86-64 engine a guest call on the calling thread (aot.h DM_GUEST_CALL: nested inside the host call it is answering)
// A string the guest is handed (getenv's answers) is guest memory (aot.h, "Host memory the guest sees"): strdup on Apple, the garena's copy here
#if DM_NATIVE_EXEC || defined(DM_TEST_GUEST_C)
#define GUEST_CALL(f, ...) f(__VA_ARGS__)
#define GUEST_STRDUP(s) strdup(s)
#else
#include "aot.h"
#define GUEST_CALL DM_GUEST_CALL
#define GUEST_STRDUP(s) dm_garena_strdup(s)
#endif
#include <stdatomic.h>

// ---------- Side table ----------
// Guest object -> host state, linear probing with backward-shift deletion: no deleted markers, so every key sits once in the run of
// occupied slots from its home slot, and an empty slot ends every search (t_objmap). Keys are guest pointers, never NULL.
#define OM_SIZE (1u << 18)
#define OM_MASK (OM_SIZE - 1)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & OM_MASK; }
// The key's slot, or the empty slot that ends its run. Called with g_om_lock held
static unsigned om_find(const void *k) {
    unsigned i = om_h(k);
    while (g_om[i].k && g_om[i].k != k) i = (i + 1) & OM_MASK;
    return i;
}
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = g_om[om_find(k)].v;   // an empty slot holds NULL
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    g_om[i].k = k; g_om[i].v = v;
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    void *r = g_om[i].v;
    if (g_om[i].k) {
        // Close the hole: each later entry of the run whose home slot is not after the hole (cyclically) moves back into it
        for (unsigned j = (i + 1) & OM_MASK; g_om[j].k; j = (j + 1) & OM_MASK)
            if (((j - om_h(g_om[j].k)) & OM_MASK) >= ((j - i) & OM_MASK)) { g_om[i] = g_om[j]; i = j; }
        g_om[i].k = NULL; g_om[i].v = NULL;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) { return plat_now_ns(); }
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the host's start the guest clock is virtual time, moved only by guest events, not
// following the wall clock, so that frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof). The CPU's work
// takes no time, and a frame's time passes where a Switch's does, while the game waits:
//   · when no game thread can run, the last one to block moves it to the earliest guest-clock deadline (hle_thread.c det_idle, through det_jump below) and the threads due wake.
//     The display's pace is such a deadline: NVN's window acquire waits on the guest clock until its texture is free (hle_nvn.c, dm_detclock_display_wait), and a wait on vi's vsync
//     event lasts until the next vsync time, origin + k × step (hle_thread.c dm_detclock_vsync_event);
//   · a present of a graphics layer that does not wait for the display that way (the guest Vulkan and OpenGL layers) moves it to the later of itself and the previous present's
//     value plus one step (dm_guest_frame_tick); an NVN present moves nothing (dm_detclock_present_paced).
//     Presents alone used to move it, and a thread that presents only after a thread that sleeps on the guest clock never presented again: Mario vs. Donkey Kong stopped at
//     frame 1 (docs/reference/threads.md, "A game stops at its first frame under DM_DETCLOCK"). Then every present moved it a step, also an NVN present on the thread that presents
//     while the main thread works, whose span of work then read a whole frame (that entry's Note).
// The clock starts at DET_ORIGIN_NS, before the guest's first reading (hle_thread.c's svc_thread_main_init asks for it before nnosInitialize), and every run reads the same values
// from there. It used to take over at the first present from the wall clock, so what the guest read before (Mario vs. Donkey Kong seeds its random generators from the tick at
// start) and the phase of every timer it started then (its vibration thread's 5 ms timer) differed from run to run, and so did every later value against the tick
// (docs/reference/threads.md, "Two runs under DM_DETCLOCK part from the boot").
// It moves neither at a present nor by a jump while frozen.
#define DET_ORIGIN_NS 60000000000ull   // a minute after the console's start, on a whole tick (19.2 MHz: 1,152,000,000 ticks)
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_gdet_last, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
static _Atomic int g_gdet_paced;   // the next present's NVN interval, whose acquire waits for the display on the guest clock (0 = a present of another layer)
void dm_detclock_present_paced(int itv) { atomic_store(&g_gdet_paced, itv > 0 ? itv : 1); }
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static uint64_t det_jump(uint64_t to);
static _Atomic int g_gdet_cfg = -1;   // DM_DETCLOCK read: 1 on, 0 off, -1 not yet
static pthread_once_t g_gdet_once = PTHREAD_ONCE_INIT;
static void detclock_init(void) {   // the first use of the guest clock (not under hle_thread.c's lock: dm_detclock_set_jump takes it)
    const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0;
    if (g_gdet_step) {
        atomic_store(&g_gdet, DET_ORIGIN_NS);
        dm_detclock_set_jump(det_jump, DET_ORIGIN_NS, g_gdet_step); atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: the guest clock is virtual time from %.0f s: NVN's acquire waits for the display on it and vi's vsync event comes every %llu ns, another layer's present "
            "moves it to at least the previous present's value plus that step, and when no game thread can run it jumps to the earliest guest-clock deadline",
            DET_ORIGIN_NS / 1e9, (unsigned long long)g_gdet_step);
    }
    atomic_store(&g_gdet_cfg, g_gdet_step > 0);
}
static int detclock_cfg(void) {
    int c = atomic_load_explicit(&g_gdet_cfg, memory_order_acquire);
    if (c < 0) { pthread_once(&g_gdet_once, detclock_init); c = atomic_load(&g_gdet_cfg); }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) {
    if (!atomic_load(&g_gdet_on) && (atomic_load_explicit(&g_gdet_cfg, memory_order_relaxed) >= 0 || !detclock_cfg())) return wall_guest_ns();   // off: one flag more once read
    if (atomic_load_explicit(&dm_detclock_tracing, memory_order_relaxed)) dm_detclock_trace_read();   // DM_DETCLOCK_TRACE: a thread's first reading after host code let it go
    return atomic_load(&g_gdet);
}
int guest_clock_pinned(void) { return atomic_load(&g_gdet_on) || atomic_load(&g_gfz); }
uint64_t guest_ns_at_host_time(uint64_t host_ns) {
    if (atomic_load(&g_gdet_on)) return atomic_load(&g_gdet);
    if (atomic_load(&g_gfz)) return atomic_load(&g_gfrozen);
    uint64_t lag = atomic_load(&g_glag);
    return host_ns >= lag ? host_ns - lag : 0;
}
// The system counter on that clock: 19.2 MHz on the Switch, 31.25 MHz (32 ns a tick) on the Switch 2, whose SDK reads cntpct_el0 and converts with that rate
// (nn::os::GetSystemTickFrequency answers 0x1dcd650, sdk 20.5.19 Ounce). Read at 19.2 MHz, every tick span a Switch 2 game measured came out 1.63 times too short, and the
// same clock's GPU timestamps 1.63 times too late: Donkey Kong Bananza's frame-rate controller took its frame for seconds long and held it at 20 fps (present interval 3)
uint64_t guest_ticks(void) { return dm_sdk_ounce() ? guest_ns() / 32 : guest_ns() * 12 / 625; }
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
// The idle jump (hle_thread.c det_idle, holding its lock): the clock moves to `to` when that is later, unless it is frozen; returns the clock after it.
// The jumps and the frames longer than a step since the last line are logged every 300 presents
static _Atomic uint64_t g_gdet_jumps, g_gdet_jumped;
static uint64_t g_gdet_long, g_gdet_longest;   // the presenting thread's
static uint64_t det_jump(uint64_t to) {
    uint64_t c = atomic_load(&g_gdet);
    if (atomic_load(&g_gfz)) return c;
    while (c < to) if (atomic_compare_exchange_weak(&g_gdet, &c, to)) { atomic_fetch_add(&g_gdet_jumps, 1); atomic_fetch_add(&g_gdet_jumped, to - c); return to; }
    return c;
}
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    uint64_t n = atomic_fetch_add(&g_guest_frame, 1) + 1;
    if (!detclock_cfg()) return;
    int paced = atomic_exchange(&g_gdet_paced, 0);
    if (!atomic_load(&g_gdet_last)) atomic_store(&g_gdet_last, atomic_load(&g_gdet));   // the first present moves nothing: the previous present's value from here on
    else if (!atomic_load(&g_gfz)) {
        uint64_t last = atomic_load(&g_gdet_last), to = last + g_gdet_step, c = atomic_load(&g_gdet);
        if (!paced) { while (c < to && !atomic_compare_exchange_weak(&g_gdet, &c, to)) {} if (c < to) c = to; }
        atomic_store(&g_gdet_last, c);
        uint64_t frame = (uint64_t)(paced ? paced : 1) * g_gdet_step;   // the frame its present interval asks for
        if (c - last > frame) { g_gdet_long++; if (c - last > g_gdet_longest) g_gdet_longest = c - last; }
    }
    if (n % 300 == 0) {
        LOG("DM_DETCLOCK: present %llu: %llu idle jumps since present %llu (%.3f ms in all), %llu frames longer than their present interval (the longest %.3f ms)", (unsigned long long)n,
            (unsigned long long)atomic_exchange(&g_gdet_jumps, 0), (unsigned long long)(n - 300), atomic_exchange(&g_gdet_jumped, 0) / 1e6, (unsigned long long)g_gdet_long, g_gdet_longest / 1e6);
        g_gdet_long = g_gdet_longest = 0;
    }
    dm_detclock_presented(n, paced);   // hle_thread.c: the threads now due wake; the thread that presents is recorded
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up, 3 NVN prepare step, 4 NVN pipeline archive init):
// freeze if any holds, release only when all are lifted -- otherwise "switch to background and back while the keyboard is up" or "archive init finishes inside the prepare step"
// would release the clock early
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
// A suspended application does not run: Horizon suspends the whole process out of focus, iOS suspends a background app. The host freezes the guest clock in the background but its
// guest threads go on until they block, so a guest reading the clock twice there saw no time pass. Super Mario Galaxy 2's frame pacing (main+0x5926a8) took the interval between two
// frames as its period, got 0, and the next frame's loop that steps by it (main+0x9abd0) never ended. The same stall on the render thread during the host's first-submission prepare
// step or pipeline archive open (reasons 3 and 4: seconds on a slow disk or cold prebuild) let Vanillaware's main thread skip hundreds of frames and advance into texture teardown
// before frame 1 finished (13 Sentinels: Aegis Rim, SIGSEGV at main+0x309128 / +0x309e28). Guest reads of the system counter (the cntpct gate, svc GetSystemTick) wait while any
// GFZ_WAIT_MASK reason holds; host work (dm_host_work: inside a submit or present, which the graphics suspend waits for) reads the frozen clock, as do the other reasons
enum { GFZ_WAIT_MASK = (1u << 1) | (1u << 3) | (1u << 4) };
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER; static _Atomic int g_gfz_wait;
// Each read also counts on the reading thread: a run of yields whose passes read the clock is a wait for a time, which the yield back-off sleeps from its start (hle_thread.c thr_yield)
__thread uint64_t dm_clock_reads;
void guest_clock_read_wait(void) {
    dm_clock_reads++;
    if (!atomic_load(&g_gfz_wait) || atomic_load(&dm_host_work)) return;
    pthread_mutex_lock(&g_gfz_mu);
    while (g_gfz_reasons & GFZ_WAIT_MASK) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    int wait = (g_gfz_reasons & GFZ_WAIT_MASK) != 0;
    atomic_store(&g_gfz_wait, wait);
    if (!wait) pthread_cond_broadcast(&g_gfz_cv);   // after the clock is running again: a released read sees it continue
    int running = !on && !g_gfz_reasons;
    pthread_mutex_unlock(&g_gfz_mu);
    if (running) dm_guest_clock_advanced();   // DM_DETCLOCK: game threads that all blocked while the clock was frozen jump now (hle_thread.c; nothing without the switch)
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)plat_random32() << 32) | plat_random32();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { plat_random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
// The SDK's own getentropy (Unicorn Overlord, SDK 16.2.4: std::random_device at nnMain) fills through nn::crypto::GenerateCryptographicallyRandomBytes, which opens the csrng service;
// Pokémon Legends: Z-A (SDK 19) calls it first thing in nnMain (main+0x1734b4)
// (InitializeCsrng → sm::Initialize, the game aborted there). The call goes through the sdk's PLT, so the takeover gives it the same stream as getentropy above
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// ---------- getenv: the game profile's game_env ----------
// Horizon gives a program no environment, and the SDK's C library starts with an empty environ. Runtimes that take their settings from the environment then run on defaults sized for the
// Switch's 64 GB alias region: Absolum's .NET GC (NativeAOT) reserves 5 × its heap limit as its region range, 3.75 GB, unless DOTNET_GCRegionRange says otherwise.
// The profile's game_env (KEY=VALUE, separated by spaces; DM_GAME_ENV overrides it, also from Documents/env.txt) answers getenv for those names; any other name goes to the SDK's own getenv
static const char **g_env_kv;   // "KEY=VALUE" strings, NULL-terminated
static void game_env_init(void) {
    const char *e = getenv("DM_GAME_ENV"); if (!e) e = DM_GAME_ENV;
    int n = 0; for (const char *p = e; *p; p++) n += *p == '=';
    if (!(g_env_kv = calloc((size_t)n + 1, sizeof *g_env_kv))) die("game_env allocation failed");
    char *s = GUEST_STRDUP(e); int k = 0;   // the values getenv hands the guest point into it
    for (char *t = strtok(s, " "); t && k < n; t = strtok(NULL, " ")) if (strchr(t, '=')) g_env_kv[k++] = t;
    if (k) LOG("game environment (%s): %s", getenv("DM_GAME_ENV") ? "DM_GAME_ENV" : "game profile game_env", e);
    else free(s);   // no accepted entry points into the copy
}
static const char *const *game_env(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, game_env_init); return g_env_kv; }
HLE_FORCE("getenv", "the game profile's game_env answers its names; the SDK's own getenv (an empty environ on Horizon) the rest", char *, g_getenv, const char *name) {
    static char *(*orig)(const char *);
    size_t n = name ? strlen(name) : 0;
    for (const char *const *p = game_env(); n && *p; p++) if (!strncmp(*p, name, n) && (*p)[n] == '=') return (char *)*p + n + 1;
    if (!orig && !(orig = guest_sym("getenv"))) return NULL;
    return GUEST_CALL(orig, name);
}

// ---------- nn::mem::StandardAllocator::Allocate(size): Vanillaware's large blocks aligned to 64 KB (profile key alloc_big_align) ----------
// 13 Sentinels 1.10's own malloc (main 4b632573…, main+0x310550) is StandardAllocator::Allocate(size). It gets the storage of a GPU memory pool from malloc rounded only to 4 KB (main+0x3073b8),
//   then cuts three ring heaps out of the pool at 64 KB alignment relative to the GPU address (main+0x30c118), and the pool size is exactly their sum (main+0x307adc). On the Switch a pool's GPU address is aligned to a big page;
//   the host's GPU address is the CPU address, which is only 4 KB aligned, so up to 60 KB is lost at the start, the third ring heap (16 MB, used by the main thread's allocations while a command buffer is recorded) does not fit and keeps base 0,
//   and the first allocation while recording writes through a null pointer (main+0x2fa9a0, when the prologue enters its first battle).
// Unicorn Overlord 1.05 (main c841ffe2…), the same engine: operator new (main+0x5a55d4) takes the calling thread's ring heap (bump allocator main+0x5b8504: cursor +0x20 starts at the base) and one kept base 0.
//   Past the title screen the first allocation from it returned 0, which operator new's null check skipped, and the next returned 0x200; its constructor (main+0x5a3224 via main+0x4c754) wrote to 0x268.
//   With the profile key, allocations of 64 KB or more are aligned to 64 KB (Allocate(size, 0x10000)), which puts the pool storage on a 64 KB boundary. Other games forward unchanged
static void *(*g_sa_alloc)(void *, size_t), *(*g_sa_alloc_al)(void *, size_t, size_t);
static int g_sa_big_align;
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;
static void sa_init(void) {
    module_t *ms = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    g_sa_alloc = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEm") : NULL;
    g_sa_alloc_al = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
    if (!g_sa_alloc || !g_sa_alloc_al) die("StandardAllocator::Allocate not found in the SDK");
    g_sa_big_align = DM_GAME_ALLOC_BIG_ALIGN;
    if (g_sa_big_align) LOG("StandardAllocator::Allocate: allocations ≥ 64 KB are 64 KB aligned (game profile alloc_big_align: memory pool storage, see hle_os.c)");
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEm", "passed straight to the SDK; with the profile's alloc_big_align (13 Sentinels, Unicorn Overlord) allocations ≥ 64 KB are 64 KB aligned (memory pool start)", void *, h_mem_SAAllocate, void *self, size_t size) {
    pthread_once(&g_sa_once, sa_init);
    return g_sa_big_align && size >= 0x10000 ? GUEST_CALL(g_sa_alloc_al, self, size, 0x10000) : GUEST_CALL(g_sa_alloc, self, size);
}
