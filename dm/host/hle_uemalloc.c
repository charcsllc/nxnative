// Unreal Engine's allocator on a host malloc zone. Unreal Engine 4 / 5's FMallocBinned3 reserves 37 GB of address space for its pools (74 of 512 MB), which an iPhone cannot give (about
// 5 GB without the extended virtual addressing entitlement), and its pools are too uneven to shrink. The rewriter finds it in any main without a build ID (dm/tools/ue_malloc.py: its name
// string, getter, vtable, constructor and the constructor's one call), cuts the call, puts observation gate OBS_UE_MALLOC on it and names the vtable and its slot layout in the fixups table
// (UE_MALLOC_VT, UE_MALLOC_SLOT). At the gate the host puts this vtable in the object instead (x0 there: made by GCreateMalloc, or a bss object in nninitStartup). Every FMemory allocation
// reaches it through the object (FPlatformMemory::BaseAllocator returns it, and the thread-local storage allocator calls its Malloc / Free slots), and the guest's own trivial functions stay
// in the slots that do nothing. The slot of the name getter (GetDescriptiveName) names the layout:
// Unreal Engine 5 (Octopath Traveler 0; the getter at slot 22: the engine's FMalloc interface, measured from the vtable and the thread-local callbacks): 0-1 destructors, 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 QuantizeSize, 12 GetAllocationSize, 13 Trim, 14 SetupTLSCachesOnCurrentThread,
// 15 ClearAndDisableTLSCachesOnCurrentThread, 16 InitializeStatsMetadata, 17 UpdateStats, 18 GetAllocatorStats, 19 DumpAllocatorStats, 20 IsInternallyThreadSafe, 21 ValidateHeap,
// 22 GetDescriptiveName, 23-25 OnMallocInitialized / OnPreFork / OnPostFork (the thread-local callbacks main+0x12e80 / main+0x12ec0 call +0x30 and +0x50).
// Unreal Engine 4.26 and 4.27 (DRAGON QUEST VII Reimagined, Pikmin 4, OCTOPATH TRAVELER II; the getter at slot 19) have one FExec slot and no fork callbacks: 0-1 destructors, 2 Exec, 3 Malloc, 4 TryMalloc, 5 Realloc, 6 TryRealloc, 7 Free, 8 QuantizeSize,
// 9 GetAllocationSize, 10 Trim, 11 SetupTLSCachesOnCurrentThread, 12 ClearAndDisableTLSCachesOnCurrentThread, 13 InitializeStatsMetadata, 14 UpdateStats, 15 GetAllocatorStats,
// 16 DumpAllocatorStats, 17 IsInternallyThreadSafe, 18 ValidateHeap, 19 GetDescriptiveName, 20 OnMallocInitialized (main+0x5e4c0e8: TryMalloc / TryRealloc call +0x18 / +0x28, GCreateMalloc
// calls +0x88 and +0xa0; Free is main+0x10, which reads the pool base as GetAllocationSize does). OCTOPATH TRAVELER II's vtable has no OnMallocInitialized, so the 21st word copied
// is the next vtable's offset to top, which nothing calls
// Unreal Engine 4.24 (DRAGON BALL Z: KAKAROT, getter at slot 17) has 18 slots. Its interface predates TryMalloc and TryRealloc.
// Malloc 3 takes x1/w2, Realloc 4 takes x1/x2/w3, Free 5 uses x1, QuantizeSize 6 takes x1/w2, and GetAllocationSize 7 stores 64 bits at [x2].
// Trim, TLS cache setup and teardown, and DumpAllocatorStats move to slots 8, 9, 10, and 14. The two words after the getter are the next vtable's header.
// Unreal Engine 5.6 (FINAL FANTASY RESONANCE; the getter at slot 26) adds MallocZeroed / TryMallocZeroed after Free and two TLS cache calls after the setup, and two cached-memory queries at the end: 0-1 destructors,
// 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 MallocZeroed, 12 TryMallocZeroed, 13 QuantizeSize, 14 GetAllocationSize, 15 Trim, 16 SetupTLSCachesOnCurrentThread,
// 17 MarkTLSCachesAsUsedOnCurrentThread, 18 MarkTLSCachesAsUnusedOnCurrentThread, 19 ClearAndDisableTLSCachesOnCurrentThread, 20 InitializeStatsMetadata, 21 UpdateStats, 22 GetAllocatorStats,
// 23 DumpAllocatorStats, 24 IsInternallyThreadSafe, 25 ValidateHeap, 26 GetDescriptiveName, 27-29 OnMallocInitialized / OnPreFork / OnPostFork, 30-31 the free cached memory sizes (main+0x67df800;
// the base FMalloc vtable main+0x67dff48 has 0 for the three pure slots 6 / 8 / 10, its 7 / 9 call +0x30 / +0x40, and its 11 and 12 are one function: +0x30, then memset). The zeroed slots
// keep the guest's function, which calls Malloc through the vtable
// Unreal Engine 3 (Batman: Arkham Knight, with a backported binned allocator) takes UE3's 32-bit sizes (DWORD Count / Alignment) and has no TryMalloc / TryRealloc: 0-1 destructors, 2 Exec,
// 3 QuantizeSize, 4 Malloc, 5 Realloc, 6 Free (call sites by vtable offset: 4 / 5 / 6 at 8090 / 8423 / 20308; malloc main+0xb2e3d0 calls +0x20 with alignment 16), 13 the TLS cache flush,
// 14 IsInternallyThreadSafe (1), 20 GetAllocationSize (stores a 32-bit size), 21 the TLS cache setup, 22 Trim (main+0x329a408). Its object is in the bss and its constructor is inlined into
// the GMalloc setup, which the cut skips (the game's patches.py). It has no Binned3 name to find, so it keeps a row of fixed addresses, which runs only on the main build it was found in (AGENTS.md)
// The host's slot functions are HLE_LINKAGE (hle.h: static on Apple; elsewhere external, so the thunk generator gives each a host-call thunk), for the x86-64 engine's
// vtable below (VT_GUEST)
#include "hle.h"
#include <stdatomic.h>
#include <stdbool.h>
// The heap the engine allocates from is guest memory: on Apple a host malloc zone (plat_zone_*), on the x86-64 engine the arena's garena (aot.h, "Host memory the guest sees":
// the engine's objects, thread stacks and NVN pool storage all live in it, so the guest's code reads and keeps addresses below 2^39; the garena announces the pages it commits
// and decommits to the graphics backend, as the memory SVCs do). The zone stays a name there (g_zone: the host allocator is installed)
#if DM_NATIVE_EXEC
#define UE_MALLOC(z, n) plat_zone_malloc(z, n)
#define UE_MEMALIGN(z, a, n) plat_zone_memalign(z, a, n)
#define UE_REALLOC(z, p, n) plat_zone_realloc(z, p, n)
#define UE_FREE(z, p) plat_zone_free(z, p)
#define UE_SIZE(z, p) plat_zone_size(z, p)
#else
#include "aot.h"   // dm_guest_alloc, dm_guest_fnptr, the garena
#define UE_MALLOC(z, n) dm_garena_malloc((n), 16)
#define UE_MEMALIGN(z, a, n) dm_garena_malloc((n), (a))
#define UE_REALLOC(z, p, n) dm_garena_realloc((p), (n), 16)
#define UE_FREE(z, p) dm_garena_free(p)
#define UE_SIZE(z, p) dm_garena_size(p)
#endif

// Slot layouts: which slots the host fills and which do nothing (up to 6; 0 ends the list); the others keep the guest's own functions
enum { UE5, UE4_27, UE5_6, UE3, UE4_24 };
// name: the slot of the name getter, which the rewriter reports (UE_MALLOC_SLOT); size32: the size arguments and results are 32-bit (UE3); try_malloc / try_realloc 0: no such slot
static const struct { int name, nslot, malloc_, try_malloc, realloc_, try_realloc, free_, quantize, get_size, nothing[6]; bool size32; } k_lay[] = {
    [UE5] = { 22, 26, 6, 7, 8, 9, 10, 11, 12, { 13, 14, 15, 19 } },      // Trim, the TLS cache setup and teardown, DumpAllocatorStats
    [UE4_27] = { 19, 21, 3, 4, 5, 6, 7, 8, 9, { 10, 11, 12, 16 } },
    [UE5_6] = { 26, 32, 6, 7, 8, 9, 10, 13, 14, { 15, 16, 17, 18, 19, 23 } },   // Trim, the four TLS cache calls, DumpAllocatorStats
    [UE3] = { -1, 25, 4, 0, 5, 0, 6, 3, 20, { 13, 21, 22 }, true },   // the TLS cache flush and setup, Trim
    [UE4_24] = { 17, 18, 3, 0, 4, 0, 5, 6, 7, { 8, 9, 10, 14 } },
};
static const struct { const char *build, *what; int layout; uint64_t obj, vtable, cut; uint32_t cut_insn; } k_ue[] = {   // cut_insn: the instruction the cut leaves
    // Batman: Arkham Knight 1.0.5: the allocator object (main+0x51818c0, bss), its vtable (main+0x329a408, 25 slots), the cut (main+0x192ec14: a branch past the inlined constructor)
    { "feae83dc9e9fb34c77d0a62f1fa9ae9f", "Batman: Arkham Knight 1.0.5", UE3, 0x51818c0, 0x329a408, 0x192ec14, 0x140004a4 },
};
enum { NSLOT = 32 };

static plat_zone *g_zone;
static _Atomic int64_t g_used, g_peak;   // bytes in use (usable sizes) and their high-water mark, for the [mem] line (one per 256 MB of new peak)
static void used(int64_t d) {
    int64_t after = atomic_fetch_add(&g_used, d) + d, peak = atomic_load(&g_peak);
    while (after > peak && !atomic_compare_exchange_weak(&g_peak, &peak, after)) {}
    if (after > peak && after >> 28 != peak >> 28) LOG("[mem] Unreal heap (host malloc zone) up to %.2f GB in use", after / 1073741824.0);
}
// Unreal's virtual allocator provides a 64 KiB-aligned base for span-sized blocks. Wwise masks an
// interior pointer to that boundary to find its header, even though it requests only 4 KiB alignment.
static size_t align_of(size_t n, uint32_t a) {
    size_t minimum = n >= 0x10000 ? 0x10000 : 16;
    return a > minimum ? a : minimum;
}
static void *alloc(size_t n, uint32_t a) {
    size_t alignment = align_of(n, a);
    void *p = alignment == 16 ? UE_MALLOC(g_zone, n ? n : 1) : UE_MEMALIGN(g_zone, alignment, n ? n : 1);
    if (p) used((int64_t)UE_SIZE(g_zone, p));
    return p;
}
static void release(void *p) { if (p) { used(-(int64_t)UE_SIZE(g_zone, p)); UE_FREE(g_zone, p); } }
static void *realloc_aligned(void *p, size_t n, uint32_t a) {
    if (!p) return alloc(n, a);
    if (!n) { release(p); return NULL; }
    // The zone's realloc first: it grows in place and gives back what a shrink leaves, and an in-place block keeps its boundary. Only a block it leaves off the boundary is copied
    size_t old = UE_SIZE(g_zone, p), alignment = align_of(n, a);
    void *q = UE_REALLOC(g_zone, p, n); if (!q) return NULL;
    used((int64_t)UE_SIZE(g_zone, q) - (int64_t)old);
    if (!((uintptr_t)q & (alignment - 1))) return q;
    void *r = alloc(n, a);
    if (!r) {
        if (q == p) return NULL;   // resized in place: the caller's pointer still holds its contents
        die("Unreal heap: out of memory aligning a moved block (%zu bytes, alignment %zu, %.2f GB in use)", n, alignment, atomic_load(&g_used) / 1073741824.0);   // the caller's pointer is gone, so NULL would leave it dangling
    }
    memcpy(r, q, old < n ? old : n); release(q); return r;
}

HLE_LINKAGE void *m_Malloc(void *self, size_t n, uint32_t a) {
    (void)self; void *p = alloc(n, a);
    if (!p) die("Unreal heap: out of memory (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return p;
}
HLE_LINKAGE void *m_TryMalloc(void *self, size_t n, uint32_t a) { (void)self; return alloc(n, a); }
HLE_LINKAGE void *m_Realloc(void *self, void *p, size_t n, uint32_t a) {
    (void)self; void *q = realloc_aligned(p, n, a);
    if (!q && n) die("Unreal heap: out of memory in Realloc (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return q;
}
HLE_LINKAGE void *m_TryRealloc(void *self, void *p, size_t n, uint32_t a) { (void)self; return realloc_aligned(p, n, a); }
HLE_LINKAGE void m_Free(void *self, void *p) { (void)self; release(p); }
HLE_LINKAGE size_t m_QuantizeSize(void *self, size_t n, uint32_t a) { (void)self; (void)a; return plat_zone_good_size(n); }
HLE_LINKAGE bool m_GetAllocationSize(void *self, void *p, size_t *out) { (void)self; if (!p) return false; *out = UE_SIZE(g_zone, p); return true; }
HLE_LINKAGE void *m_Malloc32(void *self, uint32_t n, uint32_t a) { return m_Malloc(self, n, a); }
HLE_LINKAGE void *m_Realloc32(void *self, void *p, uint32_t n, uint32_t a) { return m_Realloc(self, p, n, a); }
HLE_LINKAGE uint32_t m_QuantizeSize32(void *self, uint32_t n, uint32_t a) { (void)self; (void)a; size_t q = plat_zone_good_size(n); return q > UINT32_MAX ? n : (uint32_t)q; }
HLE_LINKAGE bool m_GetAllocationSize32(void *self, void *p, uint32_t *out) { (void)self; if (!p) return false; *out = (uint32_t)UE_SIZE(g_zone, p); return true; }
HLE_LINKAGE void m_Nothing(void *self) { (void)self; }   // Trim (its bool argument is ignored), the thread-local cache setup and teardown, DumpAllocatorStats
static void *g_vt[NSLOT];
#if DM_NATIVE_EXEC
#define VT_GUEST g_vt   // the guest calls the host's functions at their own addresses
#else
// The x86-64 engine: the vtable the guest reads is a copy in the arena (design 5.2) whose host slots hold the functions' guest-visible addresses (dm_guest_fnptr: the m_*
// functions are HLE_LINKAGE, so each has a host-call thunk); the guest's own slots and words stay as copied
static void **g_vt_guest;
#define VT_GUEST g_vt_guest
#endif
static const char *g_what;

// Observation gate OBS_UE_MALLOC, on the cut constructor call: the object in the register the gate number's upper bits name (x0), before anything calls it
static void obj_made(uint64_t *regs, int param) {
    *(void **)(uintptr_t)regs[param] = VT_GUEST;
    static int once; if (!once++) LOG("%s: Unreal's allocator (FMallocBinned3 at %#llx) runs on a host malloc zone", g_what, (unsigned long long)regs[param]);
}

// Whether [a, a + n) may be Unreal heap memory: with the host allocator installed, a range outside the guest block that the host has mapped readable and writable.
// Unreal allocates the stacks of the threads it creates with FMemory (OCTOPATH TRAVELER II's game thread: nnMain at main+0x544 mallocs it with 4 KB alignment; its
// FRunnableThreads take theirs from inside larger allocations, so a stack is not always a block start, and malloc has no cheap lookup from an interior pointer), and
// nn::os::CreateThread moves such a stack into the stack region with svcMapMemory, whose source must be backed memory (hle_svc.c map_source_backed): this heap lives
// outside the guest block, so the check refused it and the SDK aborted in MapAliasStack before the game started (the caller has already checked the guest block's own regions)
int dm_ue_heap_owns(uint64_t a, uint64_t n) {
    if (!g_zone || !a || !n) return 0;
    int dm_guest_as_contains(uintptr_t a);   // hle_svc.c
    if (dm_guest_as_contains((uintptr_t)a) || dm_guest_as_contains((uintptr_t)(a + n - 1))) return 0;   // inside the guest block: not this heap
    uint64_t at = 0, sz = 0; int prot = 0;
    if (plat_vm_query(a, &at, &sz, &prot)) return 0;
    return at <= a && a + n <= at + sz && (prot & (PLAT_PROT_READ | PLAT_PROT_WRITE)) == (PLAT_PROT_READ | PLAT_PROT_WRITE);
}

// The host's vtable over the guest's (the trivial slots stay the guest's), and the zone behind it
static void fill(const uint8_t *vtable, int L, const char *what) {
    memcpy(g_vt, vtable, sizeof g_vt[0] * k_lay[L].nslot);
    bool s32 = k_lay[L].size32;
    g_vt[k_lay[L].malloc_] = s32 ? (void *)m_Malloc32 : (void *)m_Malloc; g_vt[k_lay[L].realloc_] = s32 ? (void *)m_Realloc32 : (void *)m_Realloc;
    if (k_lay[L].try_malloc) g_vt[k_lay[L].try_malloc] = (void *)m_TryMalloc;
    if (k_lay[L].try_realloc) g_vt[k_lay[L].try_realloc] = (void *)m_TryRealloc;
    g_vt[k_lay[L].free_] = (void *)m_Free;
    g_vt[k_lay[L].quantize] = s32 ? (void *)m_QuantizeSize32 : (void *)m_QuantizeSize; g_vt[k_lay[L].get_size] = s32 ? (void *)m_GetAllocationSize32 : (void *)m_GetAllocationSize;
    for (int s = 0; s < 6 && k_lay[L].nothing[s]; s++) g_vt[k_lay[L].nothing[s]] = (void *)m_Nothing;
#if !DM_NATIVE_EXEC
    {
        const int host[] = {k_lay[L].malloc_, k_lay[L].try_malloc, k_lay[L].realloc_, k_lay[L].try_realloc, k_lay[L].free_, k_lay[L].quantize, k_lay[L].get_size};   // 0: no such slot
        g_vt_guest = dm_guest_alloc(sizeof g_vt);
        memcpy(g_vt_guest, g_vt, sizeof g_vt);
        for (unsigned s = 0; s < sizeof host / sizeof *host; s++) if (host[s]) g_vt_guest[host[s]] = dm_guest_fnptr(g_vt[host[s]]);
        for (int s = 0; s < 6 && k_lay[L].nothing[s]; s++) g_vt_guest[k_lay[L].nothing[s]] = dm_guest_fnptr(g_vt[k_lay[L].nothing[s]]);
    }
#endif
    g_zone = plat_zone_create("guest Unreal heap"); g_what = what;
}

// Called once the modules are loaded and relocated, before any guest code runs
void dm_ue_malloc_install(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        const module_t *m = &g_mod[i]; uint8_t *b = m->base;
        if (m->ue_malloc_vt) {   // found by the rewriter (ue_malloc.py)
            int L = -1;
            for (int k = 0; k < (int)(sizeof k_lay / sizeof k_lay[0]); k++) if (k_lay[k].name == (int)m->ue_malloc_slot) L = k;
            if (L < 0) { LOG("[RED] main: Unreal's FMallocBinned3 has its name getter at slot %u, a layout the host does not know: Unreal's own allocator runs on cut code", m->ue_malloc_slot); return; }
            if (!gate_obs_present(OBS_UE_MALLOC)) { LOG("[RED] main: the fixups name Unreal's FMallocBinned3 but main has no allocator gate: the allocator would have no vtable"); return; }
            static char what[64]; snprintf(what, sizeof what, "main %.8s", m->build_id);
            fill(b + m->ue_malloc_vt, L, what);
            gate_obs_set(OBS_UE_MALLOC, obj_made);
            return;
        }
        for (int k = 0; k < (int)(sizeof k_ue / sizeof k_ue[0]); k++) if (!strcmp(m->build_id, k_ue[k].build)) {
            if (*(uint32_t *)(b + k_ue[k].cut) != (k_ue[k].cut_insn ? k_ue[k].cut_insn : 0xd503201f)) { LOG("[RED] %s: main is rewritten without the allocator cut (its patches.py): Unreal's own allocator runs", k_ue[k].what); return; }
            fill(b + k_ue[k].vtable, k_ue[k].layout, k_ue[k].what);
            *(void **)(b + k_ue[k].obj) = VT_GUEST;
            LOG("%s: Unreal's allocator (main+%#llx) runs on a host malloc zone", k_ue[k].what, (unsigned long long)k_ue[k].obj);
        }
    }
}
