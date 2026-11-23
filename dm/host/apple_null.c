// Declared null forms of the host's Apple-only sources, for the Linux and Windows host: dm/host/CMakeLists.txt does not build those sources, and Apple's Makefile builds them
// and not this file (its wildcard takes hle_*.c). What the rest of the host calls in them answers as "not there", and a request that needs one says so once with a [RED] line:
//   hle_exl.c        exlaunch mod code plugins (Mac only: the plugin writes the game's code through a writable alias, which a recompiled pack never reads)
//   hle_cheats.c     Atmosphère cheat codes (the cheat VM writes the game's code and data through Mach calls; the Mac Cheats panel)
//   hle_movie_av1.c  AV1 for nn::codec's video decoders (VideoToolbox, and dav1d built for arm64 only)
//   save_sync.c      saves through an iCloud Drive folder (DM_SAVE_SYNC)
// hle_jit.c (nn::jit) has no caller in the host: its guest entries are unimplemented imports here
#include "dm.h"
#include "hle_movie_av1.h"
#include <stdatomic.h>

#define NULL_RED(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) LOG("[RED] " __VA_ARGS__); } while (0)

// ---------- hle_exl.c ----------
const uint32_t dm_exl_process_handle = 0xFFFF8001u;   // CUR_PROCESS_HANDLE, what GetInfo 65001 answers (hle_svc.c), as on Apple
void dm_exl_fake_rtld(void) {}
int dm_exl_rtld_query(uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *state, uint64_t *perm) { (void)addr; (void)base; (void)size; (void)state; (void)perm; return 0; }
uint64_t dm_exl_rtld_base(void) { return 0; }
uint64_t dm_exl_rtld_size(void) { return 0; }
int dm_exl_view_source_ok(uint64_t src, uint64_t size) {
    (void)src; (void)size;
    NULL_RED("svc MapProcessMemory: an exlaunch code plugin's view of the game's code is the Mac host's (hle_exl.c): refused");
    return 0;
}
void dm_exl_code_prepare(uint64_t src) { (void)src; }
int dm_exl_view_add(uint64_t dst, uint64_t src, uint64_t size) { (void)dst; (void)src; (void)size; return -1; }
int dm_exl_view_remove(uint64_t dst, uint64_t src, uint64_t size) { (void)dst; (void)src; (void)size; return 0; }
void dm_exl_sync_all(void) {}
int dm_exl_mod_modules(const char **names, int n, int room) { (void)names; (void)room; return n; }

// ---------- hle_cheats.c ----------
void dm_cheats_init(void) {
    const char *e = getenv("DM_CHEATS");
    if (e && *e) NULL_RED("DM_CHEATS=%s: cheat codes are the Mac and iPhone host's (hle_cheats.c); none are applied here", e);
}
void dm_cheats_present(void) {}
void dm_cheats_exit(void) {}
int dm_cheats_found(void) { return 0; }

// ---------- hle_movie_av1.c ----------
// No AV1 decoder: mav1_new answers NULL, which hle_codec_video.c takes as a decoder that cannot be made (the movie module is told so)
mav1 *mav1_new(struct movie_vt_control *control) {
    (void)control;
    NULL_RED("movie: an AV1 stream, and no AV1 decoder on this platform (hle_movie_av1.c is VideoToolbox's and arm64 dav1d's)");
    return NULL;
}
void mav1_free(mav1 *a) { (void)a; }
void mav1_reset(mav1 *a) { (void)a; }
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) { (void)a; (void)data; (void)n; (void)ts; (void)out; return -1; }
int mav1_next(mav1 *a, int drain, mav1_picture *out) { (void)a; (void)drain; (void)out; return 0; }
const char *mav1_backend(const mav1 *a) { (void)a; return "none"; }
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) { (void)obu; (void)n; (void)c; (void)w; (void)h; return 0; }

// ---------- save_sync.c ----------
// 0: the save is mounted as it is (no cloud folder to pull from); a set DM_SAVE_SYNC is said once
int save_sync_mount(const char *save, const char *title) {
    (void)save; (void)title;
    const char *e = getenv("DM_SAVE_SYNC");
    if (e && *e) NULL_RED("DM_SAVE_SYNC=%s: saves through a cloud folder are the Mac and iPhone host's (save_sync.c); this save stays local", e);
    return 0;
}
void save_sync_committed(void) {}
int save_sync_flush(int timeout_ms) { (void)timeout_ms; return 1; }   // in step: there is no cloud to send to

// ---------- DM_CTL's Metal and probe commands (hle_hid.c ctl_main): nvn_mtl.m's residency, memory trim, buffer snapshot and address notes, probe.c's access trace ----------
// Weak: a backend of the port that serves one defines it
__attribute__((weak)) void dm_gfx_restop(void) { NULL_RED("DM_CTL restop: the Metal backend's residency set (nvn_mtl.m); none here"); }
__attribute__((weak)) void dm_gfx_memtrim(void) { NULL_RED("DM_CTL memtrim: the Metal backend's memory-pressure trim (nvn_mtl.m); none here"); }
__attribute__((weak)) void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) { (void)key; (void)a; (void)n; (void)path; NULL_RED("DM_CTL bufsnap: the Metal backend's (nvn_mtl.m); none here"); }
__attribute__((weak)) void dm_gfx_addr_note(uint64_t a, char *note, size_t n) { (void)a; if (n) note[0] = 0; NULL_RED("DM_CTL addr: the Metal backend's notes on a guest address (nvn_mtl.m); none here"); }
__attribute__((weak)) void probe_atrace(uint64_t lo, uint64_t n) { (void)lo; (void)n; NULL_RED("DM_CTL atrace: the access trace is native execution's (probe.c); none on the x86-64 engine"); }
