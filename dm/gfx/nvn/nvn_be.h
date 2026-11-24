// NVN graphics backend interface: what the NVN frontend (hle_nvn.c) calls in the backend that replays its command records (docs/design/vulkan-backend.md, sections 1.5 and 1.7, step M1).
// One backend per host binary, chosen at build time. On Apple every be_* below is a static inline one-line forward to the Metal backend's mtl_* function with the same parameters and
// result (nvn_mtl.m; nvn_obj.h and nvn_mtl.h declare them), so the Metal backend's behaviour is unchanged. Other hosts get the same functions as prototypes: the null backend
// (nvn_null.c, step M6) defines them where there is no Metal, until the Vulkan backend (dm/gfx/hostvk, step M7) does. Names are the mtl_* ones with be_ in front, except: mtl_link_*
// (the display link) are be_vsync_*, mtl_device is be_native_device, and be_pool_add also takes the pool's NVN flags (memory placement for a Vulkan backend; the Metal backend keeps
// them in the shared pool table, mtl_pool_add_flags, and does not read them). Outside the Apple block this header is plain C11.
// The functions the guest Vulkan and GL layers call as well (presentation of their own textures, pacing, the native device) are in nvn_be_ext.h, included here: those layers include
// that part alone, since nvn_obj.h's type names clash with theirs.
// The per-operation group at the end (step M3) is declared once for every host: the frontend's decoder (nvn_replay.c) calls it and each backend defines it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"
#include "nvn_be_ext.h"   // be_set_origin_lower_left, be_present_wait, be_fps_target, be_vsync_active, be_vsync_wait, be_native_device, be_ext_tex, be_present_ext

// Device capabilities (design 1.5): what the backend's device can do, for the frontend's later choices. The Vulkan backend fills it from its device layer (dm/gfx/hostvk, hostvk_caps)
typedef struct {
    uint32_t unified_memory : 1, gpl : 1, eds3_blend : 1, eds3_raster : 1, vertex_input_dynamic : 1, cond_render : 1, xfb : 1, geometry : 1, tessellation : 1, astc_ldr : 1, logic_op : 1;
    uint32_t max_samples, max_point_size, max_vertex_attribs;
} be_caps;

#if defined(__APPLE__)
#include "nvn_mtl.h"
// Metal backend functions that no header declares (hle_nvn.c declared them where it called them); their definitions in nvn_mtl.m have exactly these types
void mtl_res_stats(char *out, size_t n);
void mtl_barrier_stats(uint64_t *skip, uint64_t *full);
uint32_t mtl_present_qos(void);
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb);
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns);
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none);
uint64_t mtl_upload_bytes(void);
uint64_t mtl_cut_count(void);
uint64_t mtl_copy_cut_count(void);
uint64_t mtl_blit_splits(void);
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts);
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused);
void mtl_lc_stats(uint64_t *taken, uint64_t *landed);
void mtl_ubp_why(uint64_t w[6]);
void mtl_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw);
void mtl_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops);
void mtl_pso_line(char *o, size_t n);
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n);
void mtl_dispatch_barrier_stats(uint64_t *game, uint64_t *copy, uint64_t *unranged, uint64_t *none);
void mtl_tf_stats(uint64_t v[4]);
void mtl_geohoist_stats(uint64_t *hoisted, uint64_t *refused, uint64_t *stops);
void mtl_geomesh_stats(uint64_t *draws, uint64_t *wide, uint64_t *redraws, uint64_t *refused, uint64_t *reasons);
uint64_t mtl_geocomp_stats(char *out, size_t n);
void mtl_fb_stats(uint64_t *same, uint64_t *redirected, uint64_t *tile, uint64_t *whole);
void mtl_dbnd_stats(uint64_t *o, uint64_t *d, uint64_t *c, uint64_t *e);
void mtl_gsb_stats(uint64_t *n, uint64_t *bytes);
uint64_t mtl_present_screen_wait(double period);
int mtl_probe_ramp_on(void);
void mtl_present_recorded(uint64_t frame);
void mtl_pool_replace_flags(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd, uint32_t nvn_flags);

// device
static inline void be_init(void) { mtl_init(); }
// The Metal backend reads no capabilities through this interface: Apple GPUs have unified memory, which is all the frontend asks about (trivial bodies, design 1.5)
static inline const be_caps *be_get_caps(void) { static const be_caps c = {.unified_memory = 1}; return &c; }

// memory pools
static inline void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) { mtl_pool_add_flags(mem, size, nvn_flags); }
static inline void be_pool_del(const void *mem) { mtl_pool_del(mem); }
static inline void be_alias_drop_pool(const void *pool) { mtl_alias_drop_pool(pool); }
static inline int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { return mtl_addr(addr, len, buf, off); }
static inline int be_addr_known(uint64_t addr, uint64_t len) { return mtl_addr_known(addr, len); }
static inline void be_pool_addressed(const void *mem, uint64_t size) { mtl_pool_addressed(mem, size); }
static inline void be_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd, uint32_t nvn_flags) { mtl_pool_replace_flags(del, ndel, add, size, nadd, nvn_flags); }
static inline void be_sparse_alias_delta(int delta) { mtl_sparse_alias_delta(delta); }
static inline void be_sp_phys_gone(const void *mem, uint64_t size) { mtl_sp_phys_gone(mem, size); }
// Guest memory committed / about to be decommitted (design 1.5): the Metal backend's pools are zero-copy buffers made per pool, with nothing per guest region
static inline void be_guest_map(void *mem, uint64_t size) { (void)mem; (void)size; }
static inline void be_guest_unmap(void *mem, uint64_t size) { (void)mem; (void)size; }

// object hooks
static inline void be_tex_init(const tex_t *t) { mtl_tex_init(t); }
static inline void be_tex_release(tex_t *t) { mtl_tex_release(t); }
static inline void be_tex_cpu_written(const tex_t *t) { mtl_tex_cpu_written(t); }
static inline int be_tex_cpu_rewritten(const tex_t *t) { return mtl_tex_cpu_rewritten(t); }
static inline void be_tex_linear_cpu_written(const tex_t *t) { mtl_tex_linear_cpu_written(t); }
static inline void be_tex_gpu_written(const tex_t *t) { mtl_tex_gpu_written(t); }
static inline void be_tex_pin(uint64_t serial) { mtl_tex_pin(serial); }
static inline void be_tex_unpin(uint64_t serial) { mtl_tex_unpin(serial); }
static inline int be_tex_gpu_owned(uint64_t serial) { return mtl_tex_gpu_owned(serial); }
static inline void be_reg_tex(int tid, const tex_t *t, const view_t *v) { mtl_reg_tex(tid, t, v); }
static inline void be_reg_image(int tid, const tex_t *t, const view_t *v) { mtl_reg_image(tid, t, v); }
static inline void be_reg_samp(int sid, const samp_par *sp) { mtl_reg_samp(sid, sp); }
static inline void be_set_tex_pool_n(int n) { mtl_set_tex_pool_n(n); }
static inline void be_set_samp_pool_n(int n) { mtl_set_samp_pool_n(n); }
static inline void be_prog_release(prog_t *p) { mtl_prog_release(p); }
static inline void be_prog_forget(const prog_t *p) { mtl_prog_forget(p); }
static inline void be_prog_prefetch(const prog_t *p) { mtl_prog_prefetch(p); }
static inline void be_prog_snapshot(prog_t *copy, const prog_t *source) { mtl_prog_snapshot(copy, source); }
static inline void be_prog_snapshot_pin(prog_t *copy, const prog_t *source) { mtl_prog_snapshot_pin(copy, source); }
static inline void be_prog_unpin(prog_t *p) { mtl_prog_unpin(p); }
static inline int be_rt_program_set(prog_t *program, int count, const NVNshaderData *modules, uint32_t trace_depth, uint32_t callable_depth, uint32_t flags) { return mtl_rt_program_set(program, count, modules, trace_depth, callable_depth, flags); }
static inline void be_rt_group_handle(const prog_t *program, const int32_t indices[3], void *out16) { mtl_rt_group_handle(program, indices, out16); }
static inline void be_as_sizes(const nvn_as_build *b, uint64_t *storage, uint64_t *scratch, uint64_t *update) { mtl_as_sizes(b, storage, scratch, update); }
static inline void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { mtl_read_texels(t, level, layer0, r, dst, row, img); }
// The window's textures, at WindowInitialize: the Vulkan backend's presenter sizes its swap chain by them; the Metal backend marks its window textures itself since upstream
// moved that into nvn_mtl.m (mtl_xv_window_tex at a texture's registration), so the call is empty here
static inline void be_xv_window_tex(uint64_t serial) { (void)serial; }
static inline void be_set_depth_zero(int on) { mtl_set_depth_zero(on); }
static inline void be_set_queue_origin_lower_left(int on) { mtl_set_queue_origin_lower_left(on); }

// submission and sync (hle_nvn.c, under its submission lock)
static inline void be_queue_activate(uint64_t serial) { mtl_queue_activate(serial); }
static inline void be_queue_forget(uint64_t serial) { mtl_queue_forget(serial); }
static inline void be_prepare(void) { mtl_prepare(); }
static inline void be_sub_begin(void) { mtl_sub_begin(); }
static inline void be_sub_ready(uint64_t guest_ns) { mtl_sub_ready(guest_ns); }
static inline int be_sub_stream_on(void) { return mtl_sub_stream_on(); }
static inline void be_exec_rest(const uint64_t *h, int n) { mtl_exec_rest(h, n); }
static inline void be_exec(const uint8_t *handle) { mtl_exec(handle); }
static inline uint64_t be_sub_commit(void) { return mtl_sub_commit(); }
static inline uint64_t be_last_fence(void) { return mtl_last_fence(); }
static inline uint64_t be_completed_fence(void) { return mtl_completed_fence(); }
static inline int be_wait(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
static inline int be_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_acq(fence, timeout_ns); }
static inline void be_finish(void) { mtl_finish(); }
static inline void be_drain(void) { mtl_drain(); }
static inline void be_set_present_suspended(int on) { mtl_set_present_suspended(on); }
static inline void be_drop_queued_presents(void) { mtl_drop_queued_presents(); }
static inline void be_suspend_drain(void) { mtl_suspend_drain(); }
static inline void be_arch_save(int wait_ms, int ending) { mtl_arch_save(wait_ms, ending); }
static inline void be_warm_stop(void) { mtl_warm_stop(); }

// presentation and pacing
static inline uint64_t be_present(const tex_t *t, double period_s, const int crop[4], uint64_t *copy) { return mtl_present(t, period_s, crop, copy); }
static inline void be_present_wait_texture(uint64_t ticket, uint64_t copy) { mtl_present_wait_texture(ticket, copy); }
static inline int be_present_texture_ready(uint64_t ticket, uint64_t copy) { return mtl_present_texture_ready(ticket, copy); }
static inline void be_present_recorded(uint64_t frame) { mtl_present_recorded(frame); }
static inline int be_present_screen_ready(void) { return mtl_present_screen_ready(); }
static inline uint64_t be_present_screen_wait(double period) { return mtl_present_screen_wait(period); }
static inline void be_present_flush(void) { mtl_present_flush(); }
static inline int be_has_window(void) { return mtl_has_window(); }
static inline int be_display_hz(void) { return mtl_display_hz(); }
static inline int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns) { return mtl_link_wait_ticks(ticks, timeout_ns); }
static inline int be_vsync_rate(void) { return mtl_link_rate(); }
static inline int be_vsync_stalled(void) { return mtl_link_stalled(); }
static inline int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { return mtl_link_tick(n, target, lead_ms, ticks, gap_ms); }
static inline int be_vsync_ready_tick(int vsyncs) { return mtl_link_ready_tick(vsyncs); }
static inline int be_vsync_ready_ticks(int ticks) { return mtl_link_ready_ticks(ticks); }
static inline void be_vsync_mode_set(int mode) { mtl_link_mode_set(mode); }
static inline uint64_t be_vsync_drain_take_ns(void) { return mtl_link_drain_take_ns(); }
static inline int be_snap_on(void) { return mtl_snap_on(); }
static inline uint64_t be_snap_acquire_wait(void) { return mtl_snap_acquire_wait(); }
static inline int be_snap_acquire_ready(void) { return mtl_snap_acquire_ready(); }
static inline void be_set_fstep_fine(int on) { mtl_set_fstep_fine(on); }
static inline int be_probe_ramp_on(void) { return mtl_probe_ramp_on(); }

// external producers (the guest Vulkan and GL layers on the same host): in nvn_be_ext.h

// frames and measurements
static inline void be_set_frame(uint64_t f) { mtl_set_frame(f); }
static inline double be_gpu_ms(uint64_t f) { return mtl_gpu_ms(f); }
static inline int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { return mtl_gpu_span(f, t0, t1); }
static inline void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { mtl_ab_gpu(f, gpu_ms, frame_ms); }
static inline void be_ub_present(double iv_ms) { mtl_ub_present(iv_ms); }

// statistics for hle_nvn.c's log lines and per-frame rows (be_report takes over the log lines later, see the end of this file)
static inline void be_mem_stat(unsigned long long frame, unsigned long long rss_mb) { mtl_mem_stat(frame, rss_mb); }
static inline void be_stats(uint64_t *passes, uint64_t *draws) { mtl_stats(passes, draws); }
static inline void be_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch) { mtl_args_stats(enc, reuse, patch); }
static inline void be_use_stats(uint64_t *call, uint64_t *skip) { mtl_use_stats(call, skip); }
static inline void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { mtl_vbuf_stats(resolve, reuse, set, skip, copies, copy_bytes); }
static inline uint64_t be_drawable_ns_take(void) { return mtl_drawable_ns_take(); }
static inline void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { mtl_bw(load, store, full_rw_passes); }
static inline uint64_t be_udc_saved_bytes(void) { return mtl_udc_saved_bytes(); }
static inline void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { mtl_pkg_stats(uploads, reuploads); }
static inline void be_pkg_edge_stats(uint64_t *allocs, uint64_t *bytes, uint64_t *peak_bytes, uint64_t *fails, uint64_t *fail_bytes) { mtl_pkg_edge_stats(allocs, bytes, peak_bytes, fails, fail_bytes); }
static inline void be_spare_stats(uint64_t *held, uint64_t *trimmed, uint64_t *trimmed_mb) { mtl_spare_stats(held, trimmed, trimmed_mb); }
static inline void be_pool_stats(uint64_t s[POOL_STATS]) { mtl_pool_stats(s); }
static inline unsigned be_cpu_tex_count(void) { return mtl_cpu_tex_count(); }
static inline uint64_t be_pkg_edge_skips(void) { return mtl_pkg_edge_skips(); }
static inline uint64_t be_pkg_miss(void) { return mtl_pkg_miss(); }
static inline uint64_t be_upload_ns_take(void) { return mtl_upload_ns_take(); }
static inline uint64_t be_upload_gpu_ns_take(void) { return mtl_upload_gpu_ns_take(); }
static inline uint64_t be_upload_bytes(void) { return mtl_upload_bytes(); }
static inline void be_create_stats(uint64_t *lib, uint64_t *pso) { mtl_create_stats(lib, pso); }
static inline void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { mtl_create_ns(lib_ns, pso_ns); }
static inline void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { mtl_hybrid_stats(draws, made, none); }
static inline void be_res_stats(char *out, size_t n) { mtl_res_stats(out, n); }
static inline void be_barrier_stats(uint64_t *skip, uint64_t *full) { mtl_barrier_stats(skip, full); }
static inline uint32_t be_present_qos(void) { return mtl_present_qos(); }
static inline uint64_t be_cut_count(void) { return mtl_cut_count(); }
static inline uint64_t be_copy_cut_count(void) { return mtl_copy_cut_count(); }
static inline uint64_t be_blit_splits(void) { return mtl_blit_splits(); }
static inline void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { mtl_ubd_stats(deferred, conflicts); }
static inline void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { mtl_ubp_stats(made, reused, refused); }
static inline void be_ubp_why(uint64_t w[6]) { mtl_ubp_why(w); }
static inline void be_lc_stats(uint64_t *taken, uint64_t *landed) { mtl_lc_stats(taken, landed); }
static inline void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { mtl_sm_stats(np, nc, nw); }
static inline void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { mtl_cshoist_stats(moved, depth, stops); }
static inline void be_pso_line(char *o, size_t n) { mtl_pso_line(o, n); }
static inline void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { mtl_alloc_stats(waits, grown, n); }
static inline void be_dispatch_barrier_stats(uint64_t *game, uint64_t *copy, uint64_t *unranged, uint64_t *none) { mtl_dispatch_barrier_stats(game, copy, unranged, none); }
static inline void be_tf_stats(uint64_t v[4]) { mtl_tf_stats(v); }
static inline void be_geohoist_stats(uint64_t *hoisted, uint64_t *refused, uint64_t *stops) { mtl_geohoist_stats(hoisted, refused, stops); }
static inline void be_geomesh_stats(uint64_t *draws, uint64_t *wide, uint64_t *redraws, uint64_t *refused, uint64_t *reasons) { mtl_geomesh_stats(draws, wide, redraws, refused, reasons); }
static inline uint64_t be_geocomp_stats(char *out, size_t n) { return mtl_geocomp_stats(out, n); }
static inline void be_fb_stats(uint64_t *same, uint64_t *redirected, uint64_t *tile, uint64_t *whole) { mtl_fb_stats(same, redirected, tile, whole); }
static inline void be_dbnd_stats(uint64_t *o, uint64_t *d, uint64_t *c, uint64_t *e) { mtl_dbnd_stats(o, d, c, e); }
static inline void be_gsb_stats(uint64_t *n, uint64_t *bytes) { mtl_gsb_stats(n, bytes); }

#else   // other hosts: the backend defines these (the same functions, in the same order, as the Apple block above)

// device
void be_init(void);
const be_caps *be_get_caps(void);

// memory pools
void be_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void be_pool_del(const void *mem);
void be_alias_drop_pool(const void *pool);
int be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);
int be_addr_known(uint64_t addr, uint64_t len);   // the range lies in one memory pool (as be_addr finds it), with nothing made resident
void be_pool_addressed(const void *mem, uint64_t size);   // the game holds a GPU address in this pool range: raw global memory may reach it
void be_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd, uint32_t nvn_flags);   // in one step for every lookup (a sparse pool's runs)
void be_sparse_alias_delta(int delta);   // active sparse virtual pools with mapped runs
void be_sp_phys_gone(const void *mem, uint64_t size);   // a physical pool's Finalize: its shared placement sparse tiles go back once no texture maps them
void be_guest_map(void *mem, uint64_t size);     // guest memory committed (hle_svc.c, with the CPU port)
void be_guest_unmap(void *mem, uint64_t size);   // guest memory about to be decommitted: an import under it must go first (design 4.2)

// object hooks
void be_tex_init(const tex_t *t);
void be_tex_release(tex_t *t);
void be_tex_cpu_written(const tex_t *t);
int be_tex_cpu_rewritten(const tex_t *t);
void be_tex_linear_cpu_written(const tex_t *t);
void be_tex_gpu_written(const tex_t *t);
void be_tex_pin(uint64_t serial);
void be_tex_unpin(uint64_t serial);
int be_tex_gpu_owned(uint64_t serial);
void be_reg_tex(int tid, const tex_t *t, const view_t *v);
void be_reg_image(int tid, const tex_t *t, const view_t *v);
void be_reg_samp(int sid, const samp_par *sp);
void be_set_tex_pool_n(int n);
void be_set_samp_pool_n(int n);
void be_prog_release(prog_t *p);
void be_prog_forget(const prog_t *p);
void be_prog_prefetch(const prog_t *p);
void be_prog_snapshot(prog_t *copy, const prog_t *source);
void be_prog_snapshot_pin(prog_t *copy, const prog_t *source);
void be_prog_unpin(prog_t *p);
int be_rt_program_set(prog_t *program, int count, const NVNshaderData *modules, uint32_t trace_depth, uint32_t callable_depth, uint32_t flags);
void be_rt_group_handle(const prog_t *program, const int32_t indices[3], void *out16);
void be_as_sizes(const nvn_as_build *b, uint64_t *storage, uint64_t *scratch, uint64_t *update);
void be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void be_xv_window_tex(uint64_t serial);   // a window texture, at WindowInitialize (the Vulkan presenter's swap chain size)
void be_set_depth_zero(int on);
void be_set_queue_origin_lower_left(int on);

// submission and sync (hle_nvn.c, under its submission lock)
void be_queue_activate(uint64_t serial);
void be_queue_forget(uint64_t serial);
void be_prepare(void);
void be_sub_begin(void);
void be_sub_ready(uint64_t guest_ns);
int be_sub_stream_on(void);
void be_exec_rest(const uint64_t *h, int n);
void be_exec(const uint8_t *handle);
uint64_t be_sub_commit(void);
uint64_t be_last_fence(void);
uint64_t be_completed_fence(void);
int be_wait(uint64_t fence, uint64_t timeout_ns);
int be_wait_acq(uint64_t fence, uint64_t timeout_ns);
void be_finish(void);
void be_drain(void);
void be_set_present_suspended(int on);
void be_drop_queued_presents(void);
void be_suspend_drain(void);
void be_arch_save(int wait_ms, int ending);
void be_warm_stop(void);

// presentation and pacing
uint64_t be_present(const tex_t *t, double period_s, const int crop[4], uint64_t *copy);
void be_present_wait_texture(uint64_t ticket, uint64_t copy);
int be_present_texture_ready(uint64_t ticket, uint64_t copy);
void be_present_recorded(uint64_t frame);
int be_present_screen_ready(void);
uint64_t be_present_screen_wait(double period);
void be_present_flush(void);
int be_has_window(void);
int be_display_hz(void);
int be_vsync_wait_ticks(int ticks, uint64_t timeout_ns);
int be_vsync_rate(void);
int be_vsync_stalled(void);
int be_vsync_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms);
int be_vsync_ready_tick(int vsyncs);
int be_vsync_ready_ticks(int ticks);
void be_vsync_mode_set(int mode);
uint64_t be_vsync_drain_take_ns(void);
int be_snap_on(void);
uint64_t be_snap_acquire_wait(void);
int be_snap_acquire_ready(void);
void be_set_fstep_fine(int on);
int be_probe_ramp_on(void);

// external producers: in nvn_be_ext.h

// frames and measurements
void be_set_frame(uint64_t f);
double be_gpu_ms(uint64_t f);
int be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);
void be_ab_gpu(uint64_t f, double gpu_ms, double frame_ms);
void be_ub_present(double iv_ms);

// statistics
void be_mem_stat(unsigned long long frame, unsigned long long rss_mb);
void be_stats(uint64_t *passes, uint64_t *draws);
void be_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch);
void be_use_stats(uint64_t *call, uint64_t *skip);
void be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes);
uint64_t be_drawable_ns_take(void);
void be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);
uint64_t be_udc_saved_bytes(void);
void be_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
void be_pkg_edge_stats(uint64_t *allocs, uint64_t *bytes, uint64_t *peak_bytes, uint64_t *fails, uint64_t *fail_bytes);
void be_spare_stats(uint64_t *held, uint64_t *trimmed, uint64_t *trimmed_mb);
enum { POOL_STATS = 14 };   // the pool tables' counts (nvn_mtl.h POOL_STATS on Apple)
void be_pool_stats(uint64_t s[POOL_STATS]);
unsigned be_cpu_tex_count(void);
uint64_t be_pkg_edge_skips(void);
uint64_t be_pkg_miss(void);
uint64_t be_upload_ns_take(void);
uint64_t be_upload_gpu_ns_take(void);
uint64_t be_upload_bytes(void);
void be_create_stats(uint64_t *lib, uint64_t *pso);
void be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns);
void be_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none);
void be_res_stats(char *out, size_t n);
void be_barrier_stats(uint64_t *skip, uint64_t *full);
uint32_t be_present_qos(void);
uint64_t be_cut_count(void);
uint64_t be_copy_cut_count(void);
uint64_t be_blit_splits(void);
void be_ubd_stats(uint64_t *deferred, uint64_t *conflicts);
void be_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused);
void be_ubp_why(uint64_t w[6]);
void be_lc_stats(uint64_t *taken, uint64_t *landed);
void be_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw);
void be_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops);
void be_pso_line(char *o, size_t n);
void be_alloc_stats(uint64_t *waits, uint64_t *grown, int *n);
void be_dispatch_barrier_stats(uint64_t *game, uint64_t *copy, uint64_t *unranged, uint64_t *none);
void be_tf_stats(uint64_t v[4]);
void be_geohoist_stats(uint64_t *hoisted, uint64_t *refused, uint64_t *stops);
void be_geomesh_stats(uint64_t *draws, uint64_t *wide, uint64_t *redraws, uint64_t *refused, uint64_t *reasons);
uint64_t be_geocomp_stats(char *out, size_t n);
void be_fb_stats(uint64_t *same, uint64_t *redirected, uint64_t *tile, uint64_t *whole);
void be_dbnd_stats(uint64_t *o, uint64_t *d, uint64_t *c, uint64_t *e);
void be_gsb_stats(uint64_t *n, uint64_t *bytes);

#endif

// ---------- Per operation (step M3): the frontend's decoder, nvn_replay_exec (nvn_replay.c), calls these in record order ----------
// New functions, not renames: the null backend (nvn_null.c) and the Vulkan backend (dm/gfx/hostvk) define them. The Metal backend defined them in nvn_mtl.m until the merge of
// upstream 90edd532 (each the body of one case arm of the old mtl_exec, verbatim); it now replays with its own mtl_exec again (upstream's text, be_exec), and nvn_replay.c, with
// the frontend's state, ledger, resolution and pool files, is left out of the Apple builds. Each per-record function takes exactly what that arm had: rec, the record (rec + h->size is the next one), h, its header as the decoder read it, and a, its payload
// (rec + 8), which the backend reads through the payload types of nvn_obj.h as the arm did. The decoder applies the state records itself (nvn_state_apply) and hands the mask to
// be_state_dirty; the operations that are also state records call the backend before the apply (be_tf_bind, be_tf_state_note, be_rast_discard) or after it (be_bind_program,
// be_tf_state), as the Metal arm ordered its own work around state_op
int be_list_begin(void);                                             // a command list or a called sub-list starts: a value the decoder gives back to be_rec_pre for each of its records (Metal: whether DM_NVN_PASSDUMP_ALL traces them)
int be_rec_pre(const uint8_t *rec, const op_hdr *h, int list);       // before every record (Metal: the look-ahead hooks); nonzero: consumed, the decoder goes on with the next record
void be_state_dirty(uint32_t mask);                                  // what nvn_state_apply / nvn_state_reset returned (NVN_DIRTY_*, NVN_STATE_APPLIED; nvn_state.h)
void be_end(const uint8_t *rec, const op_hdr *h, const void *a);             // OP_END: the list ends (a called sub-list while nvn_replay_ret_n > 0, nvn_replay.h)
void be_signal_event(const uint8_t *rec, const op_hdr *h, const void *a);    // OP_SIGNAL_EVENT
void be_report_counter(const uint8_t *rec, const op_hdr *h, const void *a);  // OP_REPORT_COUNTER
void be_render_cond(const uint8_t *rec, const op_hdr *h, const void *a);     // OP_RENDER_COND (p_render_cond)
void be_fence_sync(const uint8_t *rec, const op_hdr *h, const void *a);      // OP_FENCE_SYNC
void be_reset_counter(const uint8_t *rec, const op_hdr *h, const void *a);   // OP_RESET_COUNTER
void be_fill(const uint8_t *rec, const op_hdr *h, const void *a);            // OP_CLEAR_BUF (p_clear_buf)
void be_copy_b2b(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_COPY_B2B
void be_update_ub(const uint8_t *rec, const op_hdr *h, const void *a);       // OP_UPDATE_UB
void be_set_rt(const uint8_t *rec, const op_hdr *h, const void *a);          // OP_SET_RT (p_set_rt)
void be_clear_color(const uint8_t *rec, const op_hdr *h, const void *a);     // OP_CLEAR_COLOR (p_clear_color)
void be_clear_ds(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_CLEAR_DS (p_clear_ds)
void be_draw(const uint8_t *rec, const op_hdr *h, const void *a);            // OP_DRAW (p_draw through draw_rec)
void be_draw_idx(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_DRAW_IDX (p_draw_idx through draw_idx_rec)
void be_multi_draw(const uint8_t *rec, const op_hdr *h, const void *a);      // OP_MULTI_DRAW (p_multi_draw: indirect draws with a GPU-written count)
void be_bind_program(const uint8_t *rec, const op_hdr *h, const void *a);    // OP_BIND_PROGRAM, after the apply
void be_tf_bind(const uint8_t *rec, const op_hdr *h, const void *a);         // OP_TF_BIND, before the apply
void be_tf_state_note(const uint8_t *rec, const op_hdr *h, const void *a);   // OP_TF_STATE, before the apply
void be_tf_state(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_TF_STATE, after the apply
void be_rast_discard(const uint8_t *rec, const op_hdr *h, const void *a);    // OP_RAST_DISCARD, before the apply
void be_copy_b2t(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_COPY_B2T (p_copy_b2t)
void be_copy_t2b(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_COPY_T2B (p_copy_t2t)
void be_copy_t2t(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_COPY_T2T (p_copy_t2t)
void be_clear_tex(const uint8_t *rec, const op_hdr *h, const void *a);       // OP_CLEAR_TEX (p_clear_tex)
void be_draw_tex(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_DRAW_TEX (p_draw_tex: nvnCommandBufferDrawTexture)
void be_dlss(const uint8_t *rec, const op_hdr *h, const void *a);            // OP_DLSS (p_dlss: NVN2's DLSS evaluation)
void be_as_build(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_AS_BUILD (p_as_build: NVN2 acceleration structures, nvn_accel.h)
void be_as_copy(const uint8_t *rec, const op_hdr *h, const void *a);         // OP_AS_COPY (p_as_copy)
void be_bind_as(const uint8_t *rec, const op_hdr *h, const void *a);         // OP_BIND_AS (p_bind_as)
void be_rt_dispatch(const uint8_t *rec, const op_hdr *h, const void *a);     // OP_RT_DISPATCH (p_rt_dispatch: NVN2 ray tracing)
void be_dispatch(const uint8_t *rec, const op_hdr *h, const void *a);        // OP_DISPATCH (p_dispatch)
void be_barrier(const uint8_t *rec, const op_hdr *h, const void *a);         // OP_BARRIER

// Not part of step M1 (docs/design/vulkan-backend.md, section 1.5); none of these is declared yet, except where said:
// - Declared with the Vulkan backend's core (phase P3): be_get_caps (device capabilities) and be_guest_map / be_guest_unmap (guest memory committed / about to be decommitted, so the
//   backend can release an import first), above; on Apple trivial bodies. hle_svc.c is to call the last two with the CPU port; nothing calls them yet.
// - be_addr stays after step M6: the frontend's pool table (nvn_pool_lookup, nvn_pools.c) is read under the backend's lock (g_mu on Metal, hvk.mu in dm/gfx/hostvk), and on Metal
//   mtl_addr also makes the pool it finds resident, which the Vulkan backend's imports need not.
// - Step M3 added the per-operation group above, in the raw form (rec, h, a): the typed arguments of the design's sketch (section 1.5) come when the Vulkan backend needs them.
//   be_exec stays (on Apple mtl_exec, upstream's own decoder again since the merge of upstream 90edd532; the null and Vulkan backends' is nvn_replay_exec) and be_exec_rest too
//   (the look-ahead's handles still to come; upstream's queue worker passes none: a job is one flattened stream, nvn_queue.inc).
// - void be_report(uint32_t tick, char *out, size_t n): designed, not yet used. The backend is to format its own statistics lines, replacing the 41 calls in hle_nvn.c's log lines and
//   per-frame rows of these 33 functions: be_mem_stat, be_stats, be_args_stats, be_use_stats, be_vbuf_stats, be_ds_omit, be_drawable_ns_take, be_bw, be_udc_saved_bytes, be_pkg_stats,
//   be_cpu_tex_count, be_pkg_edge_skips, be_pkg_miss, be_upload_ns_take, be_upload_gpu_ns_take, be_upload_bytes, be_create_stats, be_create_ns, be_hybrid_stats, be_res_stats,
//   be_barrier_stats, be_present_qos, be_cut_count, be_copy_cut_count, be_blit_splits, be_ubd_stats, be_ubp_stats, be_ubp_why, be_lc_stats, be_sm_stats, be_cshoist_stats,
//   be_pso_line, be_alloc_stats. Several of them also feed the per-frame TSV rows (DM_NVN_TSV: @upload_ms, @drawable_ms, @ds_omit, ...), which a tick-based be_report does not cover.
// - Since step M6 the guest Vulkan and GL layers (dm/gfx/vk/hle_vk.m, dm/gfx/gl/hle_gl.m) call the be_* names of nvn_be_ext.h. Still mtl_*, with no be_* counterpart, because no file
//   built without Metal needs them: mtl_device_classic (those two layers, Apple only), mtl_pso_prebuild_tool (dm/host/main.c, not built elsewhere), mtl_present_count
//   (dm/ios/Sources/front_ios.m) and mtl_link_wait_vsync (dm/host/hle_sdk16.c, whose weak definition answers "no display link" in every build without the Metal backend, the
//   host tests that include hle_sdk16.c too; the Vulkan presenter's vsync source, design section 7.3, is to give it a be_* name).
