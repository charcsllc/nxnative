// Host lifecycle: the graceful exit, for SIGTERM (a harness or `kill` ending a run) and oe::ExecuteProgram's hand-over to the next program (hle_oe.c). A player's quit (⌘Q, closing
// the window, exit()) is atexit's, and the iOS app has no termination callback: an iPhone ends the process only through the hand-over or without a word.
// In order: stop the background threads that write state (hlecount, DM_REC, cheats), pause audio, stop the pipeline prewarm and start the archive's write, drain graphics (at most
// EXIT_DRAIN_MS), wait for the archive's write within DM_QUIT_ARCH_MS of the start, the same time a quit gives it, then the continuation (then: execve of the next program) or _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is that order, exit code 0, the pipelines the run compiled kept, and the next program.
// The deadlines and the drain's thread are the platform layer's (plat_after_ms, plat_sema: libdispatch on Apple, as before). On the x86-64 engine the session's files for the
// next pack (the sample profile, the misses, the targets, the bindings) are written first, under a bound of their own (below)
#include "hle.h"
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void); void dm_rec_finish(void);
// The drain waits for the NVN calls under way. One that compiles its pipeline can take seconds (a draw waiting in pso_get → arch_capture → MTLCompiler: the stall a harness stops a
// run for with SIGTERM), and before, the exit waited for it and ended at a 1.5 s fallback with nothing written: every SIGTERM run of Ocarina of Time without a window lost 400-500
// pipelines, built again at the next start (PR #2625). The drain runs beside the rest now and is waited for EXIT_DRAIN_MS; the archive's write goes on either way
enum { EXIT_DRAIN_MS = 1500, EXIT_LATE_MS = 500 };
static _Atomic int g_exit_taken;   // the one path that ends the process: the exit itself, or the deadline if the exit is stuck
static void (*g_then)(void *); static void *g_then_arg;   // the exit's continuation, for the deadlines (plat_after_ms takes a function without arguments)
static void exit_end(void (*then)(void *), void *arg) {
    if (atomic_exchange(&g_exit_taken, 1)) for (;;) pause();   // the other path is ending the process (then may be execve under way: one runs it)
    if (then) then(arg);   // returns only if it failed (prog_exec logs why)
    _exit(0);
}
static void exit_late(void) { LOG("[RED] graceful exit not done in %.1f seconds, exiting at once", (DM_QUIT_ARCH_MS + EXIT_LATE_MS) / 1e3); exit_end(g_then, g_then_arg); }
#if !DM_NATIVE_EXEC
static void files_late(void) { LOG("[RED] graceful exit: the session's files not written in 20 seconds, exiting at once"); exit_end(g_then, g_then_arg); }
#endif
static plat_sema *g_drained;
static void drain(void) { dm_gfx_suspend(1); plat_sema_signal(g_drained); }
static int ms_left(uint64_t t0, int budget_ms) { int64_t left = budget_ms - (int64_t)((now_ns() - t0) / 1000000); return left > 0 ? (int)left : 0; }
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    g_then = then; g_then_arg = arg;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
#if !DM_NATIVE_EXEC
    // The session's files first, under a bound of their own: each is written to a temporary name and renamed over the old one (aot_profile.c), and a bound armed before
    // them cut a long write between the two (a sample profile, the misses, the targets and three modules' bindings under load took longer), leaving <module>.bind.tsv.tmp
    // beside the profile and the file not updated. The exit's own bounds count from their end
    plat_after_ms(20000, files_late);
    { int dm_aot_profile_write(void); dm_aot_profile_write(); }   // DM_AOT_PROFILE (aot_profile.c): the sample profile; nothing without the switch
    { int dm_aot_targets_write(void); dm_aot_targets_write(); }   // and beside it the inline caches' targets (aot_targets.c, design 12's hot indirect targets)
    t0 = now_ns();
#endif
    // Past every wait below (something else stuck): end the process all the same, and start the next program if the exit has not, so a hand-over does not become a quit
    plat_after_ms(DM_QUIT_ARCH_MS + EXIT_LATE_MS, exit_late);
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_rec_finish();                                // DM_REC: the held state and the buffered steps (hle_hid.c)
    dm_cheats_exit();                               // a clean exit: no safe mode for the cheats at the next launch (atexit does not run either)
    dm_audio_suspend(1);
    dm_gfx_quit_begin();                            // no more prewarm compiles; what the pipeline archive holds starts writing now, beside the drain
    g_drained = plat_sema_create(0);
    plat_after_ms(0, drain);                        // the drain on another thread
    if (plat_sema_wait(g_drained, (int64_t)ms_left(t0, EXIT_DRAIN_MS) * 1000000))
        LOG("[RED] graceful exit: graphics not suspended in %.1f seconds (a draw under way, most often one compiling its pipeline): the pipeline archive is written all the same", EXIT_DRAIN_MS / 1e3);
    dm_gfx_quit(ms_left(t0, DM_QUIT_ARCH_MS));      // what the archive has not written yet, within the time a quit gives it
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    exit_end(then, arg);
}
void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }
// SIGTERM (Apple: a GCD signal source; Linux: a signal handler that wakes a thread; Windows: the console control handler) runs the graceful exit on an ordinary thread
void dm_term_install(void) { plat_on_terminate(dm_host_shutdown); }
