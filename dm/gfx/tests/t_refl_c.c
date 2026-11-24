// The reflection parser (nvn_refl.c, nvn_refl_from_json_text; every backend's since step M6) on every host: the samples and hand-computed expectations of the Objective-C parser's
// old test (t_refl.m), every failure path with its text, the array<…> forms, the storage buffer write flag, the c1 segment, the limits, every texture type prefix and pixel class,
// and the text Foundation's parser refused, which this one refuses too. (The Mac's comparison of the two parsers, t_refl_cmp.m, went with the Objective-C parser in step M6.)
// The cases the Objective-C parser's test gained upstream after the fork (raw pointer sources, row-wrapped buffer textures, pixel-position and projective samples, separate
// sampler IDs, handles of images and buffer textures in uniform slots, image pools) are here too, and so are its two real-shader cases, cut down from Super Mario Bros. Wonder's
// translation (chain f0f7a7b25de7) as inline fixtures, so the test needs no game files; the binding kinds the build gained since then have a case of their own.
#include "nvn_refl.h"
#include "nvn_refl_textype.h"
#include "dmsh_abi.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define J(...) #__VA_ARGS__   // a json document written as tokens (true / false / null are not expanded under #)

static int n_checks, n_failed;
static const char *cur = "";
static void check(const char *what, long want, long got) {
    n_checks++;
    if (want != got) { n_failed++; printf("  %s: %-48s expected %ld, got %ld FAILED\n", cur, what, want, got); }
}
#define CK(what, want, got) check(what, (long)(want), (long)(got))

enum { F = NVN_DPF_FLOAT, U = NVN_DPF_UINT, S = NVN_DPF_SINT, D = NVN_DPF_DEPTH, X = -1 };
typedef struct { int i, kind, reg, aid, rw, dtt, dpf, n, off; } want_bind;   // X = not checked
static void check_binds(const nvn_refl *r, const want_bind *w, int count) {
    for (int k = 0; k < count; k++) {
        const nvn_rbind *g = &r->b[w[k].i]; char m[64];
#define WB(field, label) if (w[k].field != X) { snprintf(m, sizeof m, "binding[%d] %s", w[k].i, label); CK(m, w[k].field, g->field); }
        WB(kind, "kind") WB(reg, "slot") WB(aid, "argument index") WB(rw, "read-write") WB(dtt, "dummy texture type") WB(dpf, "pixel class") WB(n, "array length") WB(off, "offset")
#undef WB
    }
}
#define NW(w) (int)(sizeof w / sizeof *w)

// ---- the cases: a document, whether it fails, its error text, the checks of its struct ----
static void v_atomic(const nvn_refl *r) {   // native image atomics (dmsh_atomic.py): one read_write image, marked atomic by the build; an image the shader only stores to is not
    CK("IMG kind", NVN_RB_IMG, r->b[0].kind); CK("texture type buffer", NVN_DTT_BUFFER, r->b[0].dtt); CK("unsigned format", U, r->b[0].dpf);
    CK("written", 1, r->b[0].rw); CK("atomic flag", 1, r->b[0].atom); CK("stored image: 3D texture type", NVN_DTT_3D, r->b[1].dtt); CK("stored image: no atomic flag", 0, r->b[1].atom);
    CK("stored image: written (access::write)", 1, r->b[1].rw); CK("binding count", 2, r->nb); CK("B flag", 1, r->route_b);
}
static void v_handle_array(const nvn_refl *r) {   // Paper Mario: The Thousand-Year Door b352f7af1c86e9fa_b, its last four bindings (as t_refl.m had them)
    CK("single texture n", 0, r->b[0].n); CK("single texture offset", 16, r->b[0].off); CK("single sampler n", 0, r->b[1].n);
    CK("texture kind", NVN_RB_TEXU, r->b[2].kind); CK("texture n", 32, r->b[2].n); CK("texture dummy type 2D", 2, r->b[2].dtt); CK("texture slot", 11, r->b[2].reg);
    CK("sampler kind", NVN_RB_SAMPU, r->b[3].kind); CK("sampler n", 32, r->b[3].n); CK("bindless", 1, r->bindless); CK("argument index of the sampler array", 48, r->b[3].aid);
}
static void v_every_kind(const nvn_refl *r) {
    static const want_bind w[] = {
        {0, NVN_RB_SUPPORT, 0, 0, 0, 0, 0, 0, 0}, {1, NVN_RB_CBUF, 3, 1, 0, 0, 0, 0, 0}, {2, NVN_RB_C1, 0, 2, X, X, X, X, X}, {3, NVN_RB_DRV0, 0, 3, X, X, X, X, X},
        {4, NVN_RB_DRV2, 0, 4, X, X, X, X, X}, {5, NVN_RB_SAMP, 7, 5, X, X, X, X, X}, {6, NVN_RB_SAMPT, 4, 6, X, X, X, X, X}, {7, NVN_RB_TEX, 41, 7, 0, 2, F, 0, 0},
        {8, NVN_RB_SAMPI, 12, 8, X, X, X, X, X}, {9, NVN_RB_SBUFP, 4, 9, 1, X, X, X, X}, {10, NVN_RB_SBUFP, 5, 10, 0, X, X, X, X}, {11, NVN_RB_SBUFP, 6, 11, 0, X, X, X, X},
        {12, NVN_RB_SBUFP, 7, 12, 1, X, X, X, X}, {13, NVN_RB_UBOP, 2, 13, 0, X, X, X, X}, {14, NVN_RB_SBUFU, 0, 14, 1, X, X, X, 56}, {15, NVN_RB_GEOMETRY, 2, 15, X, X, X, X, X},
        {16, NVN_RB_TBUF, 8, 16, 0, 0, U, X, X}, {17, NVN_RB_IMG, 1, 17, 0, 2, F, X, X}, {18, NVN_RB_TEXA, 0, 18, X, 3, F, 4, 128}, {19, NVN_RB_SAMPA, 0, 22, X, 0, X, 4, 128},
        {20, NVN_RB_SAMPD, 0, 26, X, 0, X, 11, X}, {21, NVN_RB_TEXD, 0, 37, X, 2, F, 11, X}, {22, NVN_RB_TEXD, 0, 48, X, NVN_DTT_BUFFER, S, 2, X}, {23, NVN_RB_TEXD, 0, 50, X, 2, D, 3, X}};
    check_binds(r, w, NW(w));
    CK("binding count", 24, r->nb); CK("fetch1d flag of the texture", 1, r->b[7].f1d); CK("dynamic handle arrays", 1, r->dyn); CK("no uniform-buffer handles", 0, r->bindless);
    CK("reads c0 (declared binding)", 1, r->drv_read[0]); CK("reads c2 (declared binding)", 1, r->drv_read[2]); CK("entry after the last binding zero", 0, r->b[24].kind);
}
static void v_tex_types(const nvn_refl *r) {   // every prefix rule of dummy_kind and every pixel class
    static const struct { int dtt, dpf; } w[] = {
        {NVN_DTT_1D, F}, {NVN_DTT_1D_ARRAY, F}, {NVN_DTT_2D, F}, {NVN_DTT_2D_ARRAY, F}, {NVN_DTT_CUBE, F}, {NVN_DTT_CUBE_ARRAY, F}, {NVN_DTT_3D, F},
        {NVN_DTT_BUFFER, F}, {NVN_DTT_2D, D}, {NVN_DTT_2D_ARRAY, D}, {NVN_DTT_CUBE, D}, {NVN_DTT_CUBE_ARRAY, D}, {NVN_DTT_2D_MULTISAMPLE, F}, {NVN_DTT_2D, U},
        {NVN_DTT_2D_ARRAY, S}, {NVN_DTT_3D, F}, {NVN_DTT_2D, F}, {NVN_DTT_1D_ARRAY, U}};
    for (int i = 0; i < NW(w); i++) { char m[48]; snprintf(m, sizeof m, "binding[%d] dummy texture type", i); CK(m, w[i].dtt, r->b[i].dtt);
        snprintf(m, sizeof m, "binding[%d] pixel class", i); CK(m, w[i].dpf, r->b[i].dpf); }
    CK("binding count", 18, r->nb); CK("image written (access::write)", 1, r->b[17].rw);
}
static void v_ms_types(const nvn_refl *r) {   // multisample slots are their own types (URP's CopyDepth), 64-bit texels are unsigned, a 64-bit atomic image takes atom 2
    static const struct { int dtt, dpf; } w[] = { {NVN_DTT_2D_MULTISAMPLE_ARRAY, F}, {NVN_DTT_2D_MULTISAMPLE, D}, {NVN_DTT_2D_MULTISAMPLE_ARRAY, D}, {NVN_DTT_2D, U}, {NVN_DTT_2D, U} };
    for (int i = 0; i < NW(w); i++) { char m[48]; snprintf(m, sizeof m, "binding[%d] dummy texture type", i); CK(m, w[i].dtt, r->b[i].dtt);
        snprintf(m, sizeof m, "binding[%d] pixel class", i); CK(m, w[i].dpf, r->b[i].dpf); }
    CK("64-bit atomic image", 2, r->b[4].atom); CK("32-bit atomic image", 1, r->b[5].atom);
}
static void v_arrays(const nvn_refl *r) {
    static const want_bind w[] = {
        {0, NVN_RB_TEXU, 1, 0, X, 2, F, 0, 8}, {1, NVN_RB_TEXU, 1, 1, X, 3, U, 8, 0}, {2, NVN_RB_SAMPU, 1, 9, X, X, X, 32, 4}, {3, NVN_RB_TEXU, 1, 41, X, 2, F, 0, X},
        {4, NVN_RB_TEXU, 2, 42, X, 5, S, 1, X}, {5, NVN_RB_TEXU, 2, 43, X, X, X, 255, X}, {6, NVN_RB_SAMPU, 2, 44, X, X, X, 0, X}, {7, NVN_RB_TEXU, 2, 45, X, 2, F, 0, X}};
    check_binds(r, w, NW(w));   // [3] "array<…,32>" has no ", ": not an array; [5] a negative count passes the n > 32 test (as the Objective-C parser did: n = (uint8_t)-1); [7] no closing ">" last
}
static void v_rw(const nvn_refl *r) {
    static const int w[] = {1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
    for (int i = 0; i < NW(w); i++) { char m[40]; snprintf(m, sizeof m, "binding[%d] read-write", i); CK(m, w[i], r->b[i].rw); }
}
static void v_c1(const nvn_refl *r) {
    CK("has c1", 1, r->has_c1); CK("c1 length", 4, r->c1_len); CK("c1 allocated", 1, r->c1 != NULL);
    if (r->c1) { CK("c1[0]", 0x00, r->c1[0]); CK("c1[1]", 0x01, r->c1[1]); CK("c1[2]", 0xff, r->c1[2]); CK("c1[3] (upper case hex)", 0x7f, r->c1[3]); }
}
static void v_c1_odd_digits(const nvn_refl *r) {   // strtoul of each two-character substring: no digit → 0; "-1" → ULONG_MAX → 0xff; " f" → 15; "1z" → 1
    static const int w[] = {0, 0x10, 0xff, 15, 1};
    CK("c1 length", 5, r->c1_len);
    for (int i = 0; r->c1 && i < NW(w); i++) { char m[24]; snprintf(m, sizeof m, "c1[%d]", i); CK(m, w[i], r->c1[i]); }
}
static void v_no_c1(const nvn_refl *r) { CK("no c1", 0, r->has_c1); CK("c1 length 0", 0, r->c1_len); CK("c1 NULL", 1, r->c1 == NULL); }
static void v_defaults(const nvn_refl *r) {   // a document with nothing but its bindings: every default the parser sets (the Objective-C parser's)
    CK("B flag", 1, r->route_b); CK("argument buffer slot", 0, r->abi); CK("no bindings", 0, r->nb); CK("no inputs", 0, r->nv); CK("no c1", 1, r->c1 == NULL && !r->has_c1);
    CK("pos_attr unknown", -1, r->pos_attr); CK("may discard", 1, r->may_discard); CK("may write sample_mask", 1, r->writes_smask); CK("colors unknown", 0, r->color_known);
    CK("color mask", 0, r->color_mask); CK("pos_w1", 0, r->pos_w1); CK("uv_attr[0]", -1, r->uv_attr[0]); CK("uv_attr[31]", -1, r->uv_attr[31]); CK("uv_aff_attr[17]", -1, r->uv_aff_attr[17]);
    CK("thread group", 0, r->tg[0] | r->tg[1] | r->tg[2]); CK("no shadow library", 0, r->has_vin); CK("no driver reads", 0, r->drv_read[0] | r->drv_read[2]); CK("no fetch calls", 0, r->nfc);
    CK("input shadows untouched (memset 0)", 0, r->v[0].sh[0]);
}
static void v_numbers(const nvn_refl *r) {   // NSNumber / NSString answers: 2.0 → 2, true → 1, "7" → 7, 1e1 → 10, false → 0, "24" → 24, "2" → bank 2
    static const want_bind w[] = {
        {0, NVN_RB_CBUF, 2, 7, X, X, X, X, X}, {1, NVN_RB_CBUF, 1, 10, X, X, X, X, X}, {2, NVN_RB_TEXU, 0, 3, X, X, X, X, 24}, {3, NVN_RB_CBUF, 0, 4, X, X, X, X, X},
        {4, NVN_RB_DRV2, 0, 5, X, X, X, X, X}, {5, NVN_RB_CBUF, 0, 3, X, X, X, X, X}, {6, NVN_RB_CBUF, 9, 2, X, X, X, X, X}};
    check_binds(r, w, NW(w));   // [5] arg_id 65539 → uint16 3; [6] reg 9.75 → 9, arg_id -65534 → uint16 2
}
static void v_vin(const nvn_refl *r) {
    static const struct { int loc, ct, sf, su, si; } w[] = {
        {0, NVN_CT_FLOAT, 0xff, 0xff, 0xff}, {1, NVN_CT_UINT, 16, 0xff, 17}, {2, NVN_CT_INT, 18, 19, 0xff}, {3, NVN_CT_FLOAT, 0xff, 0xff, 0xff}, {4, NVN_CT_UINT, 0xff, 0xff, 30}};
    CK("input count", 5, r->nv); CK("has the shadow library", 1, r->has_vin);
    for (int i = 0; i < NW(w); i++) { char m[48];
        snprintf(m, sizeof m, "input[%d] location", i); CK(m, w[i].loc, r->v[i].loc); snprintf(m, sizeof m, "input[%d] NVN attribute", i); CK(m, w[i].loc, r->v[i].nvn);
        snprintf(m, sizeof m, "input[%d] component type", i); CK(m, w[i].ct, r->v[i].ct);
        snprintf(m, sizeof m, "input[%d] float shadow", i); CK(m, w[i].sf, r->v[i].sh[NVN_CT_FLOAT]); snprintf(m, sizeof m, "input[%d] uint shadow", i); CK(m, w[i].su, r->v[i].sh[NVN_CT_UINT]);
        snprintf(m, sizeof m, "input[%d] sint shadow", i); CK(m, w[i].si, r->v[i].sh[NVN_CT_INT]); }
}
static void v_vin_empty(const nvn_refl *r) { CK("no shadow attributes → no shadow library", 0, r->has_vin); CK("input count", 1, r->nv); CK("shadow none", 0xff, r->v[0].sh[NVN_CT_FLOAT]); }
static void v_raster_ps(const nvn_refl *r) {
    CK("may discard", 0, r->may_discard); CK("writes sample_mask", 0, r->writes_smask); CK("color mask {0, 2}", 5, r->color_mask); CK("colors known", 1, r->color_known); CK("pos_attr", -1, r->pos_attr);
}
static void v_raster_vs(const nvn_refl *r) {
    CK("pos_attr", 3, r->pos_attr); CK("w is one", 1, r->pos_w1); CK("discard default", 1, r->may_discard); CK("colors unknown", 0, r->color_known);
    CK("uv_attr[0]", 1, r->uv_attr[0]); CK("uv_attr[5]", 3, r->uv_attr[5]); CK("uv_attr[1] (attribute 20 out of range)", -1, r->uv_attr[1]);
    CK("uv_aff_attr[2]", 1, r->uv_aff_attr[2]); CK("uv_aff[2] sx", 3 << 16 | 4 << 2 | 1, r->uv_aff[2][0]); CK("uv_aff[2] ox", 3 << 16 | 4 << 2 | 3, r->uv_aff[2][1]);
    CK("uv_aff[2] sy", 3 << 16 | 5 << 2 | 1, r->uv_aff[2][2]); CK("uv_aff[2] oy", 3 << 16 | 5 << 2 | 3, r->uv_aff[2][3]);
    CK("uv_aff_attr[3] (slot 16 refused)", -1, r->uv_aff_attr[3]); CK("uv_aff[3] sx written before the refusal", 3 << 16 | 4 << 2 | 1, r->uv_aff[3][0]); CK("uv_aff[3] ox", 0, r->uv_aff[3][1]);
    CK("uv_aff_attr[7] (vector 989, past c[x][63])", 1, r->uv_aff_attr[7]); CK("uv_aff[7] sx", 6 << 16 | 989 << 2, r->uv_aff[7][0]); CK("uv_aff_attr[8] (vector 4096)", -1, r->uv_aff_attr[8]);
    CK("uv_aff_attr[4] (attribute 20)", -1, r->uv_aff_attr[4]); CK("uv_aff_attr[6] (two-element reference)", -1, r->uv_aff_attr[6]); CK("uv_aff[6] sx", 0, r->uv_aff[6][0]);
}
static void v_raster_odd(const nvn_refl *r) {   // pos_attr 16 → -1, pos_w other than "one", a dictionary of colors (its keys count, colors not known), discard 2, sample_mask "0"
    CK("pos_attr out of range", -1, r->pos_attr); CK("w not one", 0, r->pos_w1); CK("color mask from the keys {1, 3}", 10, r->color_mask); CK("colors not an array", 0, r->color_known);
    CK("discard 2", 1, r->may_discard); CK("sample_mask \"0\"", 0, r->writes_smask);
}
static void v_raster_str(const nvn_refl *r) { CK("pos_attr \"2\"", 2, r->pos_attr); CK("colors [\"4\"]", 16, r->color_mask); CK("known", 1, r->color_known); }
static void v_fetch(const nvn_refl *r) {
    CK("fetch calls", 2, r->nfc); CK("call 0 slot", 5, r->fc_reg[0]); CK("call 0 output", 2, r->fc_locn[0]); CK("call 1 slot", 200, r->fc_reg[1]); CK("call 1 output", 31, r->fc_locn[1]);
    CK("mixed slot 3", 8, (long)r->fc_mixed[0]); CK("mixed slot 64", 1, (long)r->fc_mixed[1]); CK("mixed slot 255", 1, r->fc_mixed[3] == 1ull << 63); CK("mixed 2", 0, (long)r->fc_mixed[2]);
    CK("outputs", 2, r->nfo); CK("output 0 color", 0, r->fo_color[0]); CK("output 1 color", 2, r->fo_color[1]);
}
static void v_tg(const nvn_refl *r) { CK("tg x", 32, r->tg[0]); CK("tg y", 8, r->tg[1]); CK("tg z", 1, r->tg[2]); }
static void v_tg_none(const nvn_refl *r) { CK("tg of two elements ignored", 0, r->tg[0]); }
static void v_tg_wrap(const nvn_refl *r) { CK("tg 70000 → uint16", 70000 & 0xffff, r->tg[0]); }
static void v_drv0(const nvn_refl *r) { CK("reads c0", 1, r->drv_read[0]); CK("not c2", 0, r->drv_read[2]); }
static void v_drv_none(const nvn_refl *r) { CK("drv_reads as a string: none", 0, r->drv_read[0] | r->drv_read[2]); }
static void v_drv2_null(const nvn_refl *r) { CK("drv_reads \"2\": null counts", 1, r->drv_read[2]); CK("not c0", 0, r->drv_read[0]); }
static void v_ws(const nvn_refl *r) { CK("white space around the document", 1, r->route_b); }
static void v_c2a_whole(const nvn_refl *r) { CK("kind", NVN_RB_TEXA, r->b[0].kind); CK("n", 301, r->b[0].n); CK("offset", 32, r->b[0].off); CK("sampler word", 1384, r->b[0].soff); }
static void v_geom4(const nvn_refl *r) { CK("geometry slot 4", 4, r->b[0].reg); CK("geometry kind", NVN_RB_GEOMETRY, r->b[0].kind); }
static void v_fail_c1(const nvn_refl *r) { CK("c1 freed by the failure", 1, r->c1 == NULL); CK("has_c1 left as set", 1, r->has_c1); CK("c1_len left as set", 2, r->c1_len); }
_Static_assert(NVN_REFL_MAXARG == 768, "the case \"argument indices at the replay's limit\" writes NVN_REFL_MAXARG - 1 and NVN_REFL_MAXARG as numbers");
static void v_aid_limit(const nvn_refl *r) {   // the parser keeps arg_id as is; the argument-buffer limit NVN_REFL_MAXARG is the replay's check (nvn_mtl.m, max_aid >= NVN_REFL_MAXARG)
    CK("arg_id MAXARG - 1", NVN_REFL_MAXARG - 1, r->b[0].aid); CK("arg_id MAXARG kept", NVN_REFL_MAXARG, r->b[1].aid);
}
static void v_gptr(const nvn_refl *r) {   // pointer discovery is metadata on the existing pool-table binding; there is no new GPU argument
    CK("existing table kind", NVN_RB_GMEM, r->b[0].kind); CK("count", 2, r->ngptr); CK("slot", 1, r->gptr[0].reg); CK("byte", 8, r->gptr[0].off); CK("binding count", 1, r->nb);
}
static void v_fetch_kinds(const nvn_refl *r) {   // one at an interpolated output, one projective, one at the pixel position times c[6].xy (frag_pos, Splatoon Raiders' ink stamps)
    CK("three", 3, r->nfc); CK("projective bit", 2, r->fc_proj); CK("pixel-position bit", 4, r->fc_fpos); CK("pixel-position has no interpolated output", 0xff, r->fc_locn[2]);
    CK("x scale c[6].x", 6 << 2, r->fc_scale[2][0]); CK("y scale c[6].y", 6 << 2 | 1, r->fc_scale[2][1]);
}
static void v_fetch_far(const nvn_refl *r) { CK("slot 6, vector 989, x", 6 << 16 | 989 << 2, r->fc_scale[0][0]); }
static void v_raw_id(const nvn_refl *r) {   // a SeparateSamplerId (handle type 2, The Witcher 3): the texture from c2, the sampler ID itself in word 0 of uniform slot 7
    CK("kind", NVN_RB_SAMPU, r->b[1].kind); CK("uniform slot", 7, r->b[1].reg); CK("raw_id", 1, r->b[1].raw_id); CK("a handle's sampler has no raw_id", 0, r->b[2].raw_id);
}
static void v_ubo_handles(const nvn_refl *r) {   // Unreal Engine 5.4 on the Switch 2 keeps images and buffer textures in uniform slots, and ORs a texture and a sampler word in either order
    CK("image kind", NVN_RB_IMGU, r->b[0].kind); CK("image offset", 8, r->b[0].off); CK("image written", 1, r->b[0].rw); CK("image atomic", 1, r->b[0].atom);
    CK("buffer texture kind", NVN_RB_TBUFU, r->b[1].kind); CK("buffer texture slot", 1, r->b[1].reg); CK("bindless", 1, r->bindless);
    CK("buffer texture's other word at byte 0", 1, r->b[1].soff); CK("buffer texture's other slot", 2, r->b[1].sreg);
    CK("other word at byte 0 of slot 2", 1, r->b[2].soff); CK("its slot", 2, r->b[2].sreg); CK("one word has no other", 0, r->b[3].soff);
}
static void v_img_pool(const nvn_refl *r) {   // image pools (NVN_RB_IMGP): 3D read_write, 2D read-only, and texture_buffer write-only
    CK("pool flag", 1, r->pool); CK("bindless flag", 1, r->bindless); CK("dyn stays 0", 0, r->dyn);
    CK("[0] kind", NVN_RB_IMGP, r->b[0].kind); CK("[0] 3D", NVN_DTT_3D, r->b[0].dtt); CK("[0] float", F, r->b[0].dpf); CK("[0] rw", 1, r->b[0].rw);
    CK("[1] kind", NVN_RB_IMGP, r->b[1].kind); CK("[1] 2D", NVN_DTT_2D, r->b[1].dtt); CK("[1] uint", U, r->b[1].dpf); CK("[1] read-only", 0, r->b[1].rw);
    CK("[2] kind", NVN_RB_IMGP, r->b[2].kind); CK("[2] buffer", NVN_DTT_BUFFER, r->b[2].dtt); CK("[2] sint", S, r->b[2].dpf); CK("[2] rw", 1, r->b[2].rw);
}
static void v_new_kinds(const nvn_refl *r) {   // the binding kinds and facts the build gained after the fork, one of each
    static const want_bind w[] = {
        {0, NVN_RB_CBUF, 3, 0, X, X, X, X, X}, {1, NVN_RB_TBIAS, 0, 1, X, X, X, X, X}, {2, NVN_RB_SBLEN, 0, 2, X, X, X, X, X}, {3, NVN_RB_SBUFQ, 0, 3, 1, X, X, X, 16},
        {4, NVN_RB_SBUFP, 2, 4, 0, X, X, X, X}, {5, NVN_RB_TEXP, 0, 5, X, NVN_DTT_2D_ARRAY, F, X, X}, {6, NVN_RB_SAMPP, 0, 6, X, X, X, X, X}, {7, NVN_RB_TEXH, 4, 7, X, NVN_DTT_CUBE, F, X, 64},
        {8, NVN_RB_SAMPH, 4, 8, X, X, X, X, 64}, {9, NVN_RB_CBSIZE, 0, 9, X, X, X, X, X}, {10, NVN_RB_PUSH, 0, 10, X, X, X, X, X}, {11, NVN_RB_BORDER, 0, 11, X, X, X, X, X},
        {12, NVN_RB_SAMP, 5, 12, X, X, X, X, X}, {13, NVN_RB_GMEML, 0, 13, 1, X, X, X, X}, {14, NVN_RB_GMEM, 0, 14, 1, X, X, X, X}, {15, NVN_RB_RT_SCENE, 0, 15, X, X, X, X, X},
        {16, NVN_RB_RT_PARAMETERS, 0, 16, X, X, X, X, X}, {17, NVN_RB_RT_ERRORS, 0, 17, X, X, X, X, X}, {18, NVN_RB_RT_INSTANCE_SBT, 0, 18, X, X, X, X, X},
        {19, NVN_RB_RT_BANK0, 0, 19, X, X, X, X, X}, {20, NVN_RB_RT_BANK3, 0, 20, X, X, X, X, X}, {21, NVN_RB_IMG, 0, 21, 1, NVN_DTT_BUFFER, F, X, X}, {22, NVN_RB_TEX, 6, 22, X, NVN_DTT_2D, U, X, X},
        {23, NVN_RB_TEXD, 0, 23, X, NVN_DTT_BUFFER, F, 3, X}};
    check_binds(r, w, NW(w));
    CK("binding count", 24, r->nb); CK("fixed words of the uniform slot", 64, r->b[0].fixw); CK("offset table length", 8, r->tbias_n); CK("length table", 1, r->sblen);
    CK("push storage buffer's length slot", 1, r->b[3].sbx); CK("pointer storage buffer's length slot", 3, r->b[4].sbx); CK("pools", 1, r->pool); CK("handle tables", 1, r->htab); CK("bindless", 1, r->bindless);
    CK("the table's sampler word", 9, r->b[7].soff); CK("the table's sampler slot", 2, r->b[7].sreg); CK("border block", 12, r->border); CK("twin sampler's bit", 4, r->b[12].bw);
    CK("buffer image's offset entry", 1, r->b[21].tbx); CK("rows: lead entry", 3, r->b[22].tbx); CK("rows: fetch1d", 1, r->b[22].f1d); CK("buffer texture array's first entry", 6, r->b[23].tbx);
    CK("dynamic handle arrays (the buffer texture array)", 1, r->dyn);
    CK("thread group spill", 4096, r->tg_spill); CK("loop sites", 2, r->loop_sites); CK("mesh spill", 1, r->mesh_spill);
    CK("first stores", 2, r->nfirst); CK("first store slot", 3, r->first[1].reg); CK("first store word", 7, r->first[1].word); CK("first store value", 0xffffffffL, r->first[1].value);
    CK("reads the primitive ID", 1, r->reads_prim_id); CK("z = w", 1, r->pos_far); CK("float colours", 5, r->fcols); CK("unsigned colours", 2, r->ucols); CK("signed colours", 8, r->scols);
    CK("colour classes known", 1, r->fcols_known); CK("output map", 0x8f0fL, r->omap); CK("output map known", 1, r->omap_known); CK("screen varyings {1, 31}", 0x80000002L, r->scr_locn);
}
static void v_old_facts(const nvn_refl *r) {   // a document of an older chain: none of the new facts, their defaults
    CK("primitive ID", 0, r->reads_prim_id); CK("far", 0, r->pos_far); CK("colour classes unknown", 0, r->fcols_known); CK("output map unknown", 0, r->omap_known);
    CK("no spill", 0, r->tg_spill); CK("no loop sites", 0, r->loop_sites); CK("no first stores", 0, r->nfirst); CK("no pointer sources", 0, r->ngptr); CK("no border block", 0, r->border);
}

typedef struct { const char *name, *json; int fails; const char *err; void (*expect)(const nvn_refl *); } refl_case;
static const refl_case CASES[] = {
    // the synthetic samples of the Objective-C parser's test (t_refl.m, gone with it in step M6)
    {"atomic image", J({"route": "B", "bindings": [
        {"kind": "img", "reg": 2, "arg_id": 0, "msl_name": "cp_i_cb2_4C_r32ui", "msl_type": "texture_buffer<uint, access::read_write>", "atomic": true},
        {"kind": "img", "reg": 3, "arg_id": 1, "msl_name": "cp_i_cb2_4E_r32ui", "msl_type": "texture3d<uint, access::write>"}]}), 0, NULL, v_atomic},
    {"PC chain reflection (no route = B) rejected", J({"arg_buffer_index": 0, "bindings": [{"type": "cbuffer", "reg": 10, "arg_id": 0, "access": "constant", "msl_type": "constant cb1_struct*"}],
        "vertex_inputs": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"array of 32 handles in uniform slot 11", J({"route": "B", "bindings": [
        {"kind": "tex_ubo", "reg": 11, "off": 16, "arg_id": 14, "msl_name": "fp_t_cb14_4", "msl_type": "texture2d<float>"},
        {"kind": "samp_ubo", "reg": 11, "off": 16, "arg_id": 15, "msl_name": "fp_t_cb14_4Smplr", "msl_type": "sampler"},
        {"kind": "tex_ubo", "reg": 11, "off": 0, "arg_id": 16, "msl_name": "fp_t2d_cb14_0", "msl_type": "array<texture2d<float>, 32>"},
        {"kind": "samp_ubo", "reg": 11, "off": 0, "arg_id": 48, "msl_name": "fp_t2d_cb14_0Smplr", "msl_type": "array<sampler, 32>"}]}), 0, NULL, v_handle_array},
    {"unknown kind", J({"route": "B", "bindings": [{"kind": "foo", "arg_id": 0, "msl_name": "x", "msl_type": "constant _16*"}]}), 1, "B binding kind foo not implemented (x)", NULL},
    {"texture without a resolved slot", J({"route": "B", "bindings": [{"kind": "tex", "reg": null, "arg_id": 0, "msl_name": "fp_t_x", "msl_type": "texture2d<float>"}]}),
        1, "B binding fp_t_x has no resolved slot number", NULL},
    {"sampler without a resolved slot", J({"route": "B", "bindings": [{"kind": "samp", "reg": null, "arg_id": 0, "msl_name": "fp_s_x", "msl_type": "sampler"}]}),
        1, "B binding fp_s_x has no resolved slot number", NULL},
    // the cases the Objective-C parser's test gained after the fork
    {"raw pointer sources", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [[1, 8], [0, 0]]}]}), 0, NULL, v_gptr},
    {"raw pointer slot 16", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [[16, 0]]}]}), 1, "B binding cp_s0: raw pointer slot 16 byte 0 out of range", NULL},
    {"raw pointer byte 65529", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [[0, 65529]]}]}), 1, "B binding cp_s0: raw pointer slot 0 byte 65529 out of range", NULL},
    {"raw pointer of one number", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [[0]]}]}), 1, "B binding cp_s0: a raw pointer source is not [slot, byte]", NULL},
    {"raw pointer slot as a string", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [["bad", 0]]}]}), 1, "B binding cp_s0: a raw pointer source is not [slot, byte]", NULL},
    {"raw pointers as a string", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": "bad"}]}), 1, "B binding cp_s0: invalid or duplicate raw pointer sources", NULL},
    {"two raw pointer tables", J({"route": "B", "bindings": [{"kind": "gmem_map", "arg_id": 0, "msl_name": "cp_s0", "pointers": [[1, 8]]}, {"kind": "gmem_map", "arg_id": 1, "msl_name": "cp_s1", "pointers": []}]}),
        1, "B binding cp_s1: invalid or duplicate raw pointer sources", NULL},
    {"readable-tile calls: projective and pixel-position", J({"route": "B", "stage": "ps", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 1, "varying_locn": 3},
        {"fc": 1, "tex_reg": 2, "varying_locn": 4, "proj": 1}, {"fc": 2, "tex_reg": 3, "frag_pos": [[0, 6, 0], [0, 6, 1]]}]}}), 0, NULL, v_fetch_kinds},
    {"readable-tile call past c[x][63] (Mario Kart World's c6[989])", J({"route": "B", "stage": "ps", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 6, "frag_pos": [[6, 989, 0], [6, 989, 1]]}]}}),
        0, NULL, v_fetch_far},
    {"readable-tile call of uniform slot 16", J({"route": "B", "stage": "ps", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 3, "frag_pos": [[16, 6, 0], [0, 6, 1]]}]}}),
        1, "fetch call 0: frag_pos uniform reference out of range", NULL},
    {"readable-tile call of one reference", J({"route": "B", "stage": "ps", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 3, "frag_pos": [[0, 6, 0]]}]}}),
        1, "fetch call 0: frag_pos is not two uniform references", NULL},
    {"separate sampler ID in uniform slot 7", J({"route": "B", "bindings": [
        {"kind": "tex", "reg": 128, "arg_id": 0, "msl_name": "fp_t_cb720898_2000005A", "msl_type": "texture2d<float>"},
        {"kind": "samp_ubo", "reg": 7, "off": 0, "raw_id": true, "arg_id": 1, "msl_name": "fp_t_cb720898_2000005ASmplr", "msl_type": "sampler"},
        {"kind": "samp_ubo", "reg": 11, "off": 16, "arg_id": 2, "msl_name": "fp_t_cb14_4Smplr", "msl_type": "sampler"}]}), 0, NULL, v_raw_id},
    {"images, buffer textures and two-word handles in uniform slots", J({"route": "B", "bindings": [
        {"kind": "img_ubo", "reg": 0, "off": 8, "arg_id": 0, "msl_name": "cp_i_cb3_2_", "msl_type": "texture_buffer<float, access::write>", "atomic": true},
        {"kind": "tbuf_ubo", "reg": 1, "off": 16, "soff": 0, "sreg": 2, "arg_id": 1, "msl_name": "cp_tb_cb4_4", "msl_type": "texture_buffer<uint>"},
        {"kind": "tex_ubo", "reg": 0, "off": 88, "soff": 0, "sreg": 2, "arg_id": 2, "msl_name": "fp_t_cb262147_10000016", "msl_type": "texture2d<float>"},
        {"kind": "tex_ubo", "reg": 0, "off": 24, "arg_id": 3, "msl_name": "fp_t_cb3_6", "msl_type": "texture2d<float>"}]}), 0, NULL, v_ubo_handles},
    {"image pools: 3D rw, 2D read-only, texture_buffer write", J({"route": "B", "bindings": [
        {"kind": "img_pool", "arg_id": 0, "msl_name": "cp_i3d_cb0_40000000_", "msl_type": "constant spvPool_cp_i3d_cb0_40000000_*", "elem": "texture3d<float, access::read_write>"},
        {"kind": "img_pool", "arg_id": 1, "msl_name": "cp_i2d_cb0_40000000_r32ui", "msl_type": "constant spvPool_cp_i2d_cb0_40000000_r32ui*", "elem": "texture2d<uint, access::read>"},
        {"kind": "img_pool", "arg_id": 2, "msl_name": "cp_ib_cb0_40000000_", "msl_type": "constant spvPool_cp_ib_cb0_40000000_*", "elem": "texture_buffer<int, access::write>"}]}), 0, NULL, v_img_pool},
    // the binding kinds and facts the build gained after the fork (dmsh_abi.h DMSH_KIND_*), and their refusals
    {"multisample and 64-bit texture types", J({"route": "B", "bindings": [
        {"kind": "tex", "reg": 0, "arg_id": 0, "msl_type": "texture2d_ms_array<float>"}, {"kind": "tex", "reg": 1, "arg_id": 1, "msl_type": "depth2d_ms<float>"},
        {"kind": "tex", "reg": 2, "arg_id": 2, "msl_type": "depth2d_ms_array<float>"}, {"kind": "tex", "reg": 3, "arg_id": 3, "msl_type": "texture2d<ulong>"},
        {"kind": "img", "reg": 4, "arg_id": 4, "msl_type": "texture2d<ulong, access::read_write>", "atomic": true},
        {"kind": "img", "reg": 5, "arg_id": 5, "msl_type": "texture2d<uint, access::read_write>", "atomic": 1}]}), 0, NULL, v_ms_types},
    {"every binding kind gained after the fork", J({"route": "B", "raster": {"primitive_id": 1, "far": 1, "float_colors": 5, "uint_colors": 2, "sint_colors": 8, "omap": 36623, "screen": [1, 31, 32, -1]},
        "tg_spill": 4096, "loop_sites": [{"id": 0}, {"id": 1}], "mesh_spill": true, "first_store": [{"reg": 1, "word": 0, "value": 1}, {"reg": 3, "word": 7, "value": 4294967295}], "bindings": [
        {"kind": "cbuf", "reg": 3, "arg_id": 0, "msl_name": "fp_c6", "fixed": 256},
        {"kind": "tbias", "n": 8, "arg_id": 1, "msl_name": "fp_c17"},
        {"kind": "sblen", "arg_id": 2, "msl_name": "fp_c18"},
        {"kind": "sbuf_push", "off": 16, "sbx": 0, "rw": true, "arg_id": 3, "msl_name": "cp_sq_16"},
        {"kind": "sbuf", "reg": 2, "sbx": 2, "rw": false, "arg_id": 4, "msl_name": "fp_s2"},
        {"kind": "tex_pool", "arg_id": 5, "msl_name": "fp_t2da_cb0_40000000", "elem": "texture2d_array<float>"},
        {"kind": "samp_pool", "arg_id": 6, "msl_name": "fp_s_cb0_40000000"},
        {"kind": "tex_htab", "reg": 4, "off": 64, "soff": 8, "sreg": 2, "arg_id": 7, "msl_name": "fp_tc_cb7_10", "elem": "texturecube<float>"},
        {"kind": "samp_htab", "reg": 4, "off": 64, "arg_id": 8, "msl_name": "fp_tc_cb7_10Smplr"},
        {"kind": "cbsize", "arg_id": 9, "msl_name": "fp_c16"},
        {"kind": "drvcb", "bank": 24, "arg_id": 10, "msl_name": "fp_c24"},
        {"kind": "border", "arg_id": 11, "msl_name": "fp_border"},
        {"kind": "samp", "reg": 5, "border_bit": 3, "arg_id": 12, "msl_name": "fp_t_cb2_5Smplr_b"},
        {"kind": "gmem_log", "arg_id": 13, "msl_name": "cp_gml"},
        {"kind": "gmem_map", "rw": true, "arg_id": 14, "msl_name": "cp_gmem"},
        {"kind": "rt_scene", "arg_id": 15}, {"kind": "rt_parameters", "arg_id": 16}, {"kind": "rt_errors", "arg_id": 17}, {"kind": "rt_instance_sbt", "arg_id": 18},
        {"kind": "rt_bank0", "arg_id": 19}, {"kind": "rt_bank3", "arg_id": 20},
        {"kind": "img", "reg": 0, "tbx": 0, "arg_id": 21, "msl_name": "cp_i_cb2_48", "msl_type": "texture_buffer<float, access::write>"},
        {"kind": "tex", "reg": 6, "tbx": 2, "fetch1d": true, "arg_id": 22, "msl_name": "cp_t_cb2_50", "msl_type": "texture2d<uint>"},
        {"kind": "tbuf", "tbx": 5, "arg_id": 23, "msl_name": "vp_tb_cb0_40000000", "msl_type": "array<texture_buffer<float>, 3>"}]}), 0, NULL, v_new_kinds},
    {"an older chain's document: no new facts", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 0, "arg_id": 0}], "raster": {"colors": [0]}}), 0, NULL, v_old_facts},
    {"uniform slot read at 65540 fixed bytes", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 0, "arg_id": 0, "msl_name": "fp_c3", "fixed": 65540}]}), 1, "B binding fp_c3: 65540 bytes read at fixed offsets", NULL},
    {"uniform slot read at 6 fixed bytes", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 0, "arg_id": 0, "msl_name": "fp_c3", "fixed": 6}]}), 1, "B binding fp_c3: 6 bytes read at fixed offsets", NULL},
    {"offset table of 6 entries", J({"route": "B", "bindings": [{"kind": "tbias", "n": 6, "arg_id": 0, "msl_name": "fp_c17"}]}), 1, "B binding fp_c17: offset table of 6 entries", NULL},
    {"two offset tables", J({"route": "B", "bindings": [{"kind": "tbias", "n": 4, "arg_id": 0, "msl_name": "fp_c17"}, {"kind": "tbias", "n": 4, "arg_id": 1, "msl_name": "fp_c17b"}]}),
        1, "B binding fp_c17b: offset table of 4 entries", NULL},
    {"two length tables", J({"route": "B", "bindings": [{"kind": "sblen", "arg_id": 0, "msl_name": "fp_c18"}, {"kind": "sblen", "arg_id": 1, "msl_name": "fp_c18b"}]}),
        1, "B binding fp_c18b: a second length table", NULL},
    {"two border colour blocks", J({"route": "B", "bindings": [{"kind": "border", "arg_id": 0}, {"kind": "border", "arg_id": 1}]}), 1, "two border colour blocks", NULL},
    {"border twin of a texture", J({"route": "B", "bindings": [{"kind": "tex", "reg": 0, "border_bit": 0, "arg_id": 0, "msl_name": "fp_t_x", "msl_type": "texture2d<float>"}]}),
        1, "B binding fp_t_x: border colour twin 0 of a binding that is not one sampler", NULL},
    {"border twin 32", J({"route": "B", "bindings": [{"kind": "samp", "reg": 0, "border_bit": 32, "arg_id": 0, "msl_name": "fp_s_x"}]}),
        1, "B binding fp_s_x: border colour twin 32 of a binding that is not one sampler", NULL},
    {"length slot of a texture", J({"route": "B", "bindings": [{"kind": "sblen", "arg_id": 0}, {"kind": "tex", "reg": 0, "sbx": 0, "arg_id": 1, "msl_name": "fp_t_x", "msl_type": "texture2d<float>"}]}),
        1, "B binding fp_t_x: length table slot 0 of a binding that is not a storage buffer", NULL},
    {"bounded storage buffer without a length table", J({"route": "B", "bindings": [{"kind": "sbuf", "reg": 0, "sbx": 0, "arg_id": 0, "msl_name": "fp_s0"}]}),
        1, "B binding 0: a bounded storage buffer in a shader with no length table", NULL},
    {"buffer image's entry past the table", J({"route": "B", "bindings": [{"kind": "tbias", "n": 4, "arg_id": 0}, {"kind": "img", "reg": 0, "tbx": 4, "arg_id": 1, "msl_name": "cp_i_x", "msl_type": "texture_buffer<float, access::write>"}]}),
        1, "B binding 1: offset table entries 4 + 1 past the table's 4", NULL},
    {"five first-invocation stores", J({"route": "B", "bindings": [], "first_store": [{"reg": 0}, {"reg": 0}, {"reg": 0}, {"reg": 0}, {"reg": 0}]}), 1, "more than 4 first-invocation stores", NULL},
    {"first-invocation store to slot 16", J({"route": "B", "bindings": [], "first_store": [{"reg": 16, "word": 0, "value": 0}]}), 1, "first-invocation store to storage slot 16", NULL},
    // every binding kind
    {"every binding kind", J({"route": "B", "drv_reads": {}, "bindings": [
        {"kind": "support", "arg_id": 0, "msl_name": "support_buffer", "msl_type": "constant support_buf&"},
        {"kind": "cbuf", "reg": 3, "arg_id": 1, "msl_name": "fp_c6", "msl_type": "constant fp_c6_t*"},
        {"kind": "c1", "bank": 1, "arg_id": 2, "msl_name": "fp_c1", "msl_type": "constant fp_c1_t*"},
        {"kind": "drvcb", "bank": 0, "arg_id": 3, "msl_name": "fp_c0", "msl_type": "constant fp_c0_t*"},
        {"kind": "drvcb", "bank": 2, "arg_id": 4, "msl_name": "fp_c2", "msl_type": "constant fp_c2_t*"},
        {"kind": "samp", "reg": 7, "arg_id": 5, "msl_name": "fp_t_cb2_8Smplr", "msl_type": "sampler"},
        {"kind": "samp_tex", "reg": 4, "arg_id": 6, "msl_name": "fp_t_cb2_9Smplr", "msl_type": "sampler"},
        {"kind": "tex", "reg": 41, "arg_id": 7, "msl_name": "fp_t_cb2_a", "msl_type": "texture2d<float>", "fetch1d": true},
        {"kind": "samp_id", "reg": 12, "arg_id": 8, "msl_name": "fp_t_cb2_bSmplr", "msl_type": "sampler"},
        {"kind": "sbuf", "index": 0, "reg": 4, "rw": true, "arg_id": 9, "msl_name": "fp_s0", "msl_type": "device fp_s0_t*"},
        {"kind": "sbuf", "index": 1, "reg": 5, "rw": false, "arg_id": 10, "msl_name": "fp_s1", "msl_type": "device fp_s1_t*"},
        {"kind": "sbuf", "index": 2, "reg": 6, "arg_id": 11, "msl_name": "fp_s2", "msl_type": "const device fp_s2_t*"},
        {"kind": "sbuf", "index": 3, "reg": 7, "arg_id": 12, "msl_name": "fp_s3", "msl_type": "device fp_s3_t*"},
        {"kind": "ubo_ptr", "reg": 2, "rw": false, "arg_id": 13, "msl_name": "cp_s4", "msl_type": "device cp_s4_t*"},
        {"kind": "sbuf_ubo", "reg": 0, "off": 56, "rw": true, "arg_id": 14, "msl_name": "cp_s5", "msl_type": "device cp_s5_t*"},
        {"kind": "geometry", "reg": 2, "arg_id": 15, "msl_name": "geo_x", "msl_type": "device uint*"},
        {"kind": "tbuf", "reg": 8, "arg_id": 16, "msl_name": "fp_tb_cb2_c", "msl_type": "texture_buffer<uint>"},
        {"kind": "img", "reg": 1, "arg_id": 17, "msl_name": "fp_i_cb2_d", "msl_type": "texture2d<float, access::read>"},
        {"kind": "tex_c2a", "off": 128, "arg_id": 18, "msl_name": "fp_t2das_cb2_20", "msl_type": "array<texture2d_array<float>, 4>"},
        {"kind": "samp_c2a", "off": 128, "arg_id": 22, "msl_name": "fp_t2das_cb2_20Smplr", "msl_type": "array<sampler, 4>"},
        {"kind": "?", "arg_id": 26, "msl_name": "vp_s_cb0_40000000", "msl_type": "array<sampler, 11>"},
        {"kind": "tex", "arg_id": 37, "msl_name": "vp_t_cb0_40000000", "msl_type": "array<texture2d<float>, 11>"},
        {"kind": "tbuf", "arg_id": 48, "msl_name": "vp_tb_cb0_40000000", "msl_type": "array<texture_buffer<int>, 2>"},
        {"kind": "tex", "arg_id": 50, "msl_name": "fp_t_cb0_40000000", "msl_type": "array<depth2d<float>, 3>"}]}), 0, NULL, v_every_kind},
    {"every texture type prefix and pixel class", J({"route": "B", "bindings": [
        {"kind": "tex", "reg": 0, "arg_id": 0, "msl_type": "texture1d<float>"}, {"kind": "tex", "reg": 1, "arg_id": 1, "msl_type": "texture1d_array<float>"},
        {"kind": "tex", "reg": 2, "arg_id": 2, "msl_type": "texture2d<float>"}, {"kind": "tex", "reg": 3, "arg_id": 3, "msl_type": "texture2d_array<float>"},
        {"kind": "tex", "reg": 4, "arg_id": 4, "msl_type": "texturecube<float>"}, {"kind": "tex", "reg": 5, "arg_id": 5, "msl_type": "texturecube_array<float>"},
        {"kind": "tex", "reg": 6, "arg_id": 6, "msl_type": "texture3d<float>"}, {"kind": "tex", "reg": 7, "arg_id": 7, "msl_type": "texture_buffer<float>"},
        {"kind": "tex", "reg": 8, "arg_id": 8, "msl_type": "depth2d<float>"}, {"kind": "tex", "reg": 9, "arg_id": 9, "msl_type": "depth2d_array<float>"},
        {"kind": "tex", "reg": 10, "arg_id": 10, "msl_type": "depthcube<float>"}, {"kind": "tex", "reg": 11, "arg_id": 11, "msl_type": "depthcube_array<float>"},
        {"kind": "tex", "reg": 12, "arg_id": 12, "msl_type": "texture2d_ms<float>"}, {"kind": "tex", "reg": 13, "arg_id": 13, "msl_type": "texture2d<uint>"},
        {"kind": "tex", "reg": 14, "arg_id": 14, "msl_type": "texture2d_array<int>"}, {"kind": "tex", "reg": 15, "arg_id": 15, "msl_type": "texture3d<half>"},
        {"kind": "tex", "reg": 16, "arg_id": 16, "msl_name": "fp_t_no_type"}, {"kind": "img", "reg": 17, "arg_id": 17, "msl_type": "texture1d_array<uint, access::write>"}]}), 0, NULL, v_tex_types},
    {"array<...> forms", J({"route": "B", "bindings": [
        {"kind": "tex_ubo", "reg": 1, "off": 8, "arg_id": 0, "msl_name": "fp_t_cb4_2", "msl_type": "texture2d<float>"},
        {"kind": "tex_ubo", "reg": 1, "off": 0, "arg_id": 1, "msl_name": "fp_t2da_cb4_0", "msl_type": "array<texture2d_array<uint>, 8>"},
        {"kind": "samp_ubo", "reg": 1, "off": 4, "arg_id": 9, "msl_name": "fp_t2da_cb4_0Smplr", "msl_type": "array<sampler, 32>"},
        {"kind": "tex_ubo", "reg": 1, "arg_id": 41, "msl_name": "fp_t2d_cb4_8", "msl_type": "array<texture2d<float>,32>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 42, "msl_name": "fp_tc_cb5_0", "msl_type": "array<texturecube<int>, 1>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 43, "msl_name": "fp_t2d_cb5_8", "msl_type": "array<texture2d<float>, -1>"},
        {"kind": "samp_ubo", "reg": 2, "arg_id": 44, "msl_name": "fp_t2d_cb5_8Smplr", "msl_type": "array<sampler, 0>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 45, "msl_name": "fp_t3d_cb5_10", "msl_type": "array<texture3d<float>, 4> "}]}), 0, NULL, v_arrays},
    {"texture array of 33 handles", J({"route": "B", "bindings": [{"kind": "tex_ubo", "reg": 11, "off": 0, "arg_id": 0, "msl_name": "fp_t2d_cb14_0", "msl_type": "array<texture2d<float>, 33>"}]}),
        1, "B binding fp_t2d_cb14_0: array of 33", NULL},
    {"sampler array of 40 handles, no name", J({"route": "B", "bindings": [{"kind": "samp_ubo", "reg": 11, "off": 0, "arg_id": 0, "msl_type": "array<sampler, 40>"}]}),
        1, "B binding (null): array of 40", NULL},
    {"dynamic sampler array of 33", J({"route": "B", "bindings": [{"kind": "?", "arg_id": 0, "msl_name": "vp_s_cb0_40000000", "msl_type": "array<sampler, 33>"}]}),
        1, "B binding vp_s_cb0_40000000: array of 33", NULL},
    {"dynamic array of 0", J({"route": "B", "bindings": [{"kind": "tex", "arg_id": 0, "msl_name": "vp_t_cb0_40000000", "msl_type": "array<texture2d<float>, 0>"}]}),
        1, "B binding vp_t_cb0_40000000: array of 0", NULL},
    {"dynamic array of buffers", J({"route": "B", "bindings": [{"kind": "?", "arg_id": 0, "msl_name": "vp_x_cb0_40000000", "msl_type": "array<float4, 4>"}]}),
        1, "B binding vp_x_cb0_40000000: dynamic handle array of float4", NULL},
    {"driver-table texture array that is no array", J({"route": "B", "bindings": [{"kind": "tex_c2a", "off": 128, "arg_id": 0, "msl_name": "fp_t2das_cb2_20", "msl_type": "texture2d_array<float>"}]}),
        1, "B binding fp_t2das_cb2_20: array of 0", NULL},
    {"driver-table sampler array of 302", J({"route": "B", "bindings": [{"kind": "samp_c2a", "off": 128, "arg_id": 0, "msl_name": "fp_t2das_cb2_20Smplr", "msl_type": "array<sampler, 302>"}]}),
        1, "B binding fp_t2das_cb2_20Smplr: array of 302", NULL},
    {"driver-table texture array of 301 with its sampler word", J({"route": "B", "bindings": [{"kind": "tex_c2a", "off": 32, "soff": 1384, "arg_id": 0, "msl_name": "fp_t2d_cb2_4", "msl_type": "array<texture2d<float>, 301>"}]}),
        0, NULL, v_c2a_whole},
    {"driver-table array's sampler word off its row", J({"route": "B", "bindings": [{"kind": "tex_c2a", "off": 32, "soff": 1388, "arg_id": 0, "msl_name": "fp_t2d_cb2_4", "msl_type": "array<texture2d<float>, 4>"}]}),
        1, "B binding fp_t2d_cb2_4: sampler word at c2 byte 0x56c", NULL},
    // the storage buffer write flag: rw true / false / "1" / "no" / 0 / 1 / "yes", absent with and without the const prefix
    {"storage buffer write flag", J({"route": "B", "bindings": [
        {"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": true, "msl_type": "const device a*"}, {"kind": "sbuf", "reg": 1, "arg_id": 1, "rw": false, "msl_type": "device a*"},
        {"kind": "sbuf", "reg": 2, "arg_id": 2, "msl_type": "const device a*"}, {"kind": "sbuf", "reg": 3, "arg_id": 3, "msl_type": "device a*"},
        {"kind": "sbuf", "reg": 4, "arg_id": 4, "rw": "1"}, {"kind": "sbuf", "reg": 5, "arg_id": 5, "rw": "no"},
        {"kind": "sbuf_ubo", "reg": 6, "arg_id": 6, "rw": 1, "off": 8}, {"kind": "sbuf_ubo", "reg": 7, "arg_id": 7, "rw": 0, "msl_type": "device a*"},
        {"kind": "sbuf_ubo", "reg": 8, "arg_id": 8, "msl_type": "const device b*"}, {"kind": "sbuf", "reg": 9, "arg_id": 9, "rw": "yes"}]}), 0, NULL, v_rw},
    // the c1 segment
    {"c1 segment", J({"route": "B", "bindings": [], "c1": "0001fF7F"}), 0, NULL, v_c1},
    {"c1 bytes strtoul reads oddly", J({"route": "B", "bindings": [], "c1": "zz10-1 f1z"}), 0, NULL, v_c1_odd_digits},
    {"c1 empty", J({"route": "B", "bindings": [], "c1": ""}), 0, NULL, v_no_c1},
    {"c1 absent", J({"route": "B", "bindings": []}), 0, NULL, v_no_c1},
    {"c1 of an odd hex length", J({"route": "B", "bindings": [], "c1": "abc"}), 1, "c1 is 1 bytes: over the limit or an odd hex length", NULL},
    {"c1 freed by a later failure", J({"route": "B", "bindings": [], "c1": "0011", "vertex_inputs": [{"location": 40, "msl_type": "float4"}]}), 1, "B vertex input 40 out of range", v_fail_c1},
    // defaults, numbers, routes
    {"defaults of a bare document", J({"route": "B", "bindings": []}), 0, NULL, v_defaults},
    {"numbers and strings as Foundation reads them", J({"route": "B", "bindings": [
        {"kind": "cbuf", "reg": 2.0, "arg_id": "7", "msl_name": "vp_c5"}, {"kind": "cbuf", "reg": true, "arg_id": 1e1}, {"kind": "tex_ubo", "reg": 0, "off": "24", "arg_id": 3, "msl_type": "texture2d<float>"},
        {"kind": "cbuf", "reg": false, "arg_id": 4}, {"kind": "drvcb", "bank": "2", "arg_id": 5}, {"kind": "cbuf", "reg": -0, "arg_id": 65539}, {"kind": "cbuf", "reg": 9.75, "arg_id": -65534}]}),
        0, NULL, v_numbers},
    {"slot given as a string", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": "3", "arg_id": 0, "msl_name": "vp_c6"}]}), 1, "B binding vp_c6 has no resolved slot number", NULL},
    {"negative slot", J({"route": "B", "bindings": [{"kind": "samp", "reg": -1, "arg_id": 0, "msl_name": "fp_t_cb2_8Smplr"}]}), 1, "B binding fp_t_cb2_8Smplr has no resolved slot number", NULL},
    {"storage buffer without a slot", J({"route": "B", "bindings": [{"kind": "sbuf", "index": 0, "reg": null, "rw": false, "arg_id": 0, "msl_name": "hp_s0"}]}), 1, "B binding hp_s0 has no resolved slot number", NULL},
    {"image without a slot", J({"route": "B", "bindings": [{"kind": "img", "arg_id": 0, "msl_name": "fp_i_x", "msl_type": "texture2d<float, access::write>"}]}), 1, "B binding fp_i_x has no resolved slot number", NULL},
    {"uniform-buffer texture without a slot", J({"route": "B", "bindings": [{"kind": "tex_ubo", "off": 0, "arg_id": 0, "msl_name": "fp_t_y"}]}), 1, "B binding fp_t_y has no resolved slot number", NULL},
    {"constant sampler without a slot", J({"route": "B", "bindings": [{"kind": "samp_id", "arg_id": 0}]}), 1, "B binding (null) has no resolved slot number", NULL},
    {"compute uniform pointer without a slot", J({"route": "B", "bindings": [{"kind": "ubo_ptr", "arg_id": 0, "msl_name": "cp_s1"}]}), 1, "B binding cp_s1 has no resolved slot number", NULL},
    {"route A", J({"route": "A", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route b (case)", J({"route": "b", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"key Route (case)", J({"Route": "B", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route as an array", J({"route": ["B"], "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route as a number", J({"route": 66, "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"bindings missing", J({"route": "B"}), 1, "missing bindings", NULL},
    {"bindings as a dictionary", J({"route": "B", "bindings": {}}), 1, "missing bindings", NULL},
    {"kind and name missing", J({"route": "B", "bindings": [{"reg": 0, "arg_id": 0}]}), 1, "B binding kind (null) not implemented ((null))", NULL},
    {"geometry helper slot 5", J({"route": "B", "bindings": [{"kind": "geometry", "reg": 5, "arg_id": 0}]}), 1, "invalid geometry helper slot", NULL},
    {"geometry helper without a slot", J({"route": "B", "bindings": [{"kind": "geometry", "arg_id": 0}]}), 1, "invalid geometry helper slot", NULL},
    {"geometry helper slot 4", J({"route": "B", "bindings": [{"kind": "geometry", "reg": 4, "arg_id": 0}]}), 0, NULL, v_geom4},
    {"argument indices at the replay's limit", J({"route": "B", "bindings": [{"kind": "support", "arg_id": 767}, {"kind": "support", "arg_id": 768}]}), 0, NULL, v_aid_limit},
    // vertex inputs and their shadows
    {"vertex inputs and shadow attributes", J({"route": "B", "bindings": [], "vertex_inputs": [
        {"location": 0, "msl_type": "float4"}, {"location": 1, "msl_type": "uint4"}, {"location": 2, "msl_type": "int2"}, {"location": 3, "msl_type": "half4"}, {"location": "4", "msl_type": "uint"}],
        "vin": {"entry": "main0", "entry_z": "main0_z", "shadow": [{"location": 1, "float": 16, "sint": 17}, {"location": 2, "float": 18, "uint": 19}, {"location": 4, "sint": 30}]}}), 0, NULL, v_vin},
    {"shadow list missing", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "uint"}], "vin": {}}), 0, NULL, v_vin_empty},
    {"vertex input at location 32", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 32, "msl_type": "float4"}]}), 1, "B vertex input 32 out of range", NULL},
    {"vertex input at location -1", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": -1, "msl_type": "float4"}]}), 1, "B vertex input -1 out of range", NULL},
    {"shadow of a float input", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "float4"}], "vin": {"shadow": [{"location": 0, "uint": 16}]}}),
        1, "shadow input for location 0, which is not an integer input", NULL},
    {"shadow of a missing input", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 9, "float": 16}]}}),
        1, "shadow input for location 9, which is not an integer input", NULL},
    {"shadow attribute 15", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "float": 15}]}}),
        1, "shadow input float of location 1 at attribute 15: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow attribute 31", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "sint": 31}]}}),
        1, "shadow input sint of location 1 at attribute 31: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow attribute used twice", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}, {"location": 2, "msl_type": "int4"}],
        "vin": {"shadow": [{"location": 1, "float": 16}, {"location": 2, "float": 16}]}}), 1, "shadow input float of location 2 at attribute 16: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow of the input's own type", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "uint": 20}]}}),
        1, "shadow input uint of location 1 at attribute 20: out of 16-30, used twice or of the input's own type", NULL},
    // raster facts
    {"raster facts of a fragment shader", J({"route": "B", "bindings": [], "raster": {"discard": 0, "sample_mask": 0, "colors": [0, 2, 9, -1]}}), 0, NULL, v_raster_ps},
    {"raster facts of a vertex shader", J({"route": "B", "bindings": [], "raster": {"pos_attr": 3, "pos_w": "one",
        "uv_from_attr": {"0": 1, "5": 3, "40": 2, "1": 20, "-1": 2},
        "uv_affine": {"2": {"attr": 1, "sx": [3, 4, 1], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]}, "3": {"attr": 1, "sx": [3, 4, 1], "ox": [16, 0, 0], "sy": [3, 5, 1], "oy": [3, 5, 3]},
                      "4": {"attr": 20, "sx": [3, 4, 1], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]}, "6": {"attr": 2, "sx": [1, 2], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]},
                      "7": {"attr": 1, "sx": [6, 989, 0], "ox": [6, 989, 1], "sy": [6, 990, 0], "oy": [6, 990, 1]}, "8": {"attr": 1, "sx": [6, 4096, 0], "ox": [6, 989, 1], "sy": [6, 990, 0], "oy": [6, 990, 1]}}}}),
        0, NULL, v_raster_vs},
    {"raster facts out of range", J({"route": "B", "bindings": [], "raster": {"pos_attr": 16, "pos_w": "attr", "colors": {"1": 0, "3": 5}, "discard": 2, "sample_mask": "0"}}), 0, NULL, v_raster_odd},
    {"raster facts as strings", J({"route": "B", "bindings": [], "raster": {"pos_attr": "2", "colors": ["4"]}}), 0, NULL, v_raster_str},
    // readable-tile fetch tables, thread group, driver reads
    {"fetch tables", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 5, "varying_locn": 2}, {"fc": 1, "tex_reg": 200, "varying_locn": 31}],
        "mixed_regs": [3, 64, 255, 256, -1], "outputs": [{"color": 0, "type": "float4"}, {"color": 2, "type": "half4"}]}}), 0, NULL, v_fetch},
    {"fetch call table not contiguous", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 1, "tex_reg": 5, "varying_locn": 2}]}}),
        1, "fetch call table not contiguous or out of range (entry 1)", NULL},
    {"fetch call slot 256", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 256, "varying_locn": 2}]}}),
        1, "fetch call table not contiguous or out of range (entry 0)", NULL},
    {"five fetch outputs", J({"route": "B", "bindings": [], "fetch": {"outputs": [{"color": 0}, {"color": 1}, {"color": 2}, {"color": 3}, {"color": 4}]}}), 1, "more than 4 fetch outputs", NULL},
    {"thread group size", J({"route": "B", "bindings": [], "threadgroup_size": [32, 8, 1]}), 0, NULL, v_tg},
    {"thread group of two", J({"route": "B", "bindings": [], "threadgroup_size": [64, 1]}), 0, NULL, v_tg_none},
    {"thread group null", J({"route": "B", "bindings": [], "threadgroup_size": null}), 0, NULL, v_tg_none},
    {"thread group over 16 bits", J({"route": "B", "bindings": [], "threadgroup_size": [70000, 1, 1]}), 0, NULL, v_tg_wrap},
    {"driver reads of c0", J({"route": "B", "bindings": [], "drv_reads": {"0": {"vec4_offsets": [16], "dynamic": false}}}), 0, NULL, v_drv0},
    {"driver reads as a string", J({"route": "B", "bindings": [], "drv_reads": ""}), 0, NULL, v_drv_none},
    {"driver reads with a null", J({"route": "B", "bindings": [], "drv_reads": {"2": null}}), 0, NULL, v_drv2_null},
    {"driver locations the replay cannot fill", J({"route": "B", "bindings": [], "drv_unfillable": ["c0 dynamic index", "c2+0x160"]}),
        1, "reads driver-bank locations at run time that the replay layer cannot fill: c0 dynamic index c2+0x160", NULL},
    {"no unfillable driver locations", J({"route": "B", "bindings": [], "drv_unfillable": []}), 0, NULL, NULL},
    // the text itself
    {"white space around the document", " \n\t{\"route\": \"B\", \"bindings\": []}\r\n ", 0, NULL, v_ws},
    // what the refusal of leading zeros, control characters and NULs (strict_text) must let through: zeros that are not leading, exponent digits, escapes inside strings
    {"numbers with zeros that are not leading", J({"route": "B", "bindings": [], "n": [0, -0, 0.5, -0.25, 10, 100, 1e05, 1E+07, 2e-08, -0e0, 0E00]}), 0, NULL, NULL},
    {"an escaped backslash before u0000 is no NUL", "{\"route\": \"B\", \"bindings\": [], \"x\": \"\\\\u0000\"}", 0, NULL, NULL},
    {"an escaped quote keeps the string open", "{\"route\": \"B\", \"bindings\": [], \"x\": \"a\\\"012\\\" 012\"}", 0, NULL, NULL},
    {"a non-NUL escape and DEL inside a string", "{\"route\": \"B\", \"bindings\": [], \"x\": \"\\u0001\\u00e9\x7f\"}", 0, NULL, NULL},
};
#define NCASES (int)(sizeof CASES / sizeof *CASES)
// Text that is not one json value, and text NSJSONSerialization refused that cJSON would take (t_refl_cmp's probes on the Mac, macOS CI on 093317b: a leading zero, a control
// character inside a string; and a NUL inside a string, which Foundation kept and cJSON cuts short): the parser's own failure, "reflection json does not parse"
static const char *const BAD_TEXT[] = { "{", "", "{\"route\": \"B\", \"bindings\": []} x", "{\"route\": \"B\", \"bindings\": [}", "nul",
    "{\"route\": \"B\", \"bindings\": [{\"kind\": \"cbuf\", \"reg\": 012, \"arg_id\": 0}]}", "{\"route\": \"B\", \"bindings\": [{\"kind\": \"cbuf\", \"reg\": -01, \"arg_id\": 0}]}",
    "{\"route\": \"B\", \"bindings\": [{\"kind\": \"cbuf\", \"reg\": 1, \"arg_id\": 0, \"msl_name\": \"a\tb\"}]}", "{\"route\": \"B\", \"bindings\": [], \"x\": \"line\nbreak\"}",
    "{\"route\": \"B\\u0000\", \"bindings\": []}", "{\"route\": \"B\", \"bindings\": [], \"x\": \"\\u0000\"}" };
#define NBAD (int)(sizeof BAD_TEXT / sizeof *BAD_TEXT)

// ---- generated documents: the limits ----
typedef struct { char *s; size_t n, cap; } sbuf;
static void put(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sbuf *b, const char *fmt, ...) {
    va_list a; va_start(a, fmt); int k = vsnprintf(NULL, 0, fmt, a); va_end(a);
    if (b->n + (size_t)k + 1 > b->cap) { b->cap = (b->n + (size_t)k + 1) * 2; b->s = realloc(b->s, b->cap); if (!b->s) { perror("realloc"); exit(2); } }
    va_start(a, fmt); vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a); va_end(a); b->n += (size_t)k;
}
enum { LIM_BMAX, LIM_BOVER, LIM_V32, LIM_V33, LIM_C1MAX, LIM_C1OVER, LIM_FC32, LIM_FC33, LIM_GPTR64, LIM_GPTR65, LIM_COUNT };
static const char *const LIM_NAME[LIM_COUNT] = { "NVN_REFL_MAXB bindings", "NVN_REFL_MAXB + 1 bindings", "32 vertex inputs (NVN_REFL_MAXV)", "33 vertex inputs", "c1 of 65535 bytes",
                                                 "c1 of 65536 bytes", "32 fetch calls", "33 fetch calls", "DMSH_GPTR_MAX raw pointer sources", "DMSH_GPTR_MAX + 1 raw pointer sources" };
static char *limit_doc(int i) {   // malloc'd
    sbuf b = {0};
    put(&b, "{\"route\": \"B\", \"bindings\": [");
    int nb = i == LIM_BMAX ? NVN_REFL_MAXB : i == LIM_BOVER ? NVN_REFL_MAXB + 1 : 0;
    for (int k = 0; k < nb; k++) put(&b, "%s{\"kind\": \"cbuf\", \"reg\": %d, \"arg_id\": %d, \"msl_name\": \"fp_c%d\"}", k ? ", " : "", k % 14, k, k % 14 + 3);
    if (i == LIM_GPTR64 || i == LIM_GPTR65) {
        put(&b, "{\"kind\": \"gmem_map\", \"arg_id\": 0, \"msl_name\": \"cp_s0\", \"pointers\": [");
        for (int k = 0; k < (i == LIM_GPTR64 ? DMSH_GPTR_MAX : DMSH_GPTR_MAX + 1); k++) put(&b, "%s[0, %d]", k ? ", " : "", 8 * k);
        put(&b, "]}");
    }
    put(&b, "]");
    if (i == LIM_V32 || i == LIM_V33) {
        put(&b, ", \"vertex_inputs\": [");
        for (int k = 0; k < (i == LIM_V32 ? NVN_REFL_MAXV : NVN_REFL_MAXV + 1); k++) put(&b, "%s{\"location\": %d, \"msl_type\": \"%s\"}", k ? ", " : "", k < NVN_REFL_MAXV ? k : 5, k & 1 ? "uint4" : "float4");
        put(&b, "]");
    }
    if (i == LIM_C1MAX || i == LIM_C1OVER) {
        put(&b, ", \"c1\": \"");
        for (int k = 0; k < (i == LIM_C1MAX ? 0xffff : 0x10000); k++) put(&b, "%02x", k & 0xff);
        put(&b, "\"");
    }
    if (i == LIM_FC32 || i == LIM_FC33) {
        put(&b, ", \"fetch\": {\"calls\": [");
        for (int k = 0; k < (i == LIM_FC32 ? 32 : 33); k++) put(&b, "%s{\"fc\": %d, \"tex_reg\": %d, \"varying_locn\": %d}", k ? ", " : "", k, k * 7, 31 - k % 32);
        put(&b, "]}");
    }
    put(&b, "}");
    return b.s;
}

static void check_str(const char *what, const char *want, const char *got) {
    n_checks++;
    if (strcmp(want, got)) { n_failed++; printf("  %s: %s\n    expected \"%s\"\n    got      \"%s\" FAILED\n", cur, what, want, got); }
}
static void run_case(const refl_case *c) {
    nvn_refl r; char err[256] = "";
    cur = c->name; int f0 = n_failed;
    int rc = nvn_refl_from_json_text(c->json, strlen(c->json), &r, err, sizeof err);
    CK("fails", c->fails, rc != 0);
    if (c->err) check_str("error text", c->err, err);
    if (c->fails) CK("c1 NULL after a failure", 1, r.c1 == NULL);
    if (c->expect) c->expect(&r);
    free(r.c1);
    printf("%-64s %s\n", c->name, n_failed == f0 ? "ok" : "FAILED");
}
static void run_limit(int i) {
    char *doc = limit_doc(i); nvn_refl r; char err[256] = "";
    cur = LIM_NAME[i]; int f0 = n_failed;
    int rc = nvn_refl_from_json_text(doc, strlen(doc), &r, err, sizeof err);
    switch (i) {
    case LIM_BMAX: CK("succeeds", 0, rc); CK("binding count", NVN_REFL_MAXB, r.nb); CK("last slot", (NVN_REFL_MAXB - 1) % 14, r.b[NVN_REFL_MAXB - 1].reg); break;
    case LIM_BOVER: { char want[64]; snprintf(want, sizeof want, "%d bindings, over the limit %d", NVN_REFL_MAXB + 1, NVN_REFL_MAXB);
        CK("fails", 1, rc != 0); check_str("error text", want, err); CK("no binding converted", 0, r.nb); break; }
    case LIM_V32: CK("succeeds", 0, rc); CK("input count", NVN_REFL_MAXV, r.nv); CK("last input", 31, r.v[31].loc); CK("last input uint", NVN_CT_UINT, r.v[31].ct); break;
    case LIM_V33: CK("fails", 1, rc != 0); check_str("error text", "B vertex input 5 out of range", err); CK("inputs before the failure", NVN_REFL_MAXV, r.nv); break;
    case LIM_C1MAX: CK("succeeds", 0, rc); CK("c1 length", 0xffff, r.c1_len); CK("c1 allocated", 1, r.c1 != NULL); if (r.c1) { CK("c1 last byte", 0xfe, r.c1[0xfffe]); CK("c1[256]", 0, r.c1[256]); } break;
    case LIM_C1OVER: CK("fails", 1, rc != 0); check_str("error text", "c1 is 65536 bytes: over the limit or an odd hex length", err); CK("c1 NULL", 1, r.c1 == NULL); break;
    case LIM_FC32: CK("succeeds", 0, rc); CK("fetch calls", 32, r.nfc); CK("last call slot", 31 * 7, r.fc_reg[31]); CK("last call output", 0, r.fc_locn[31]); break;
    case LIM_FC33: CK("fails", 1, rc != 0); check_str("error text", "fetch call table not contiguous or out of range (entry 32)", err); break;
    case LIM_GPTR64: CK("succeeds", 0, rc); CK("sources", DMSH_GPTR_MAX, r.ngptr); CK("last byte", 8 * (DMSH_GPTR_MAX - 1), r.gptr[DMSH_GPTR_MAX - 1].off); break;
    case LIM_GPTR65: CK("fails", 1, rc != 0); check_str("error text", "B binding cp_s0: invalid or duplicate raw pointer sources", err); break;
    }
    free(r.c1); free(doc);
    printf("%-64s %s\n", LIM_NAME[i], n_failed == f0 ? "ok" : "FAILED");
}

// A row-wrapped buffer texture (fetch1d with an offset table entry, dmsh_reflect.py buffer_image_bias) has a lead group followed by logical widths, in each of its five
// binding forms: a texture slot, a uniform-buffer handle, an array of them, a dynamic handle array and a driver-table array (the Objective-C parser's test after the fork)
static void row_bindings(void) {
    static const char *const FORM[] = {
        "\"kind\": \"tex\", \"reg\": 0, \"msl_name\": \"cp_t_tcb_8\", \"msl_type\": \"texture2d<uint>\"",
        "\"kind\": \"tex_ubo\", \"reg\": 0, \"msl_name\": \"cp_t_cb3_A\", \"msl_type\": \"texture2d<uint>\"",
        "\"kind\": \"tex_ubo\", \"reg\": 0, \"msl_name\": \"cp_t_cb3_A\", \"msl_type\": \"array<texture2d<uint>, 2>\"",
        "\"kind\": \"tex\", \"msl_name\": \"cp_t_cb0_40000000\", \"msl_type\": \"array<texture2d<uint>, 2>\"",
        "\"kind\": \"tex_c2a\", \"off\": 0, \"soff\": 0, \"msl_name\": \"cp_t_cb2_0\", \"msl_type\": \"array<texture2d<uint>, 2>\"" };
    static const struct { const char *what; int tbx, table, fetch1d, fails; } V[] = {
        {"conversion succeeds", 0, 4, 1, 0}, {"width past the declared table rejected", 3, 4, 1, 1}, {"width past capacity rejected", 511, 512, 1, 1},
        {"ordinary 2D offset metadata rejected", 0, 4, 0, 1} };
    for (int f = 0; f < NW(FORM); f++) {
        int f0 = n_failed; char name[96]; snprintf(name, sizeof name, "row-wrapped buffer binding, form %d", f); cur = name;
        for (int v = 0; v < NW(V); v++) {
            sbuf b = {0}; nvn_refl r; char err[256] = "";
            put(&b, "{\"route\": \"B\", \"bindings\": [{\"kind\": \"tbias\", \"n\": %d, \"msl_name\": \"cp_c17\"}, {%s, \"tbx\": %d%s}]}", V[v].table, FORM[f], V[v].tbx,
                V[v].fetch1d ? ", \"fetch1d\": true" : "");
            int rc = nvn_refl_from_json_text(b.s, b.n, &r, err, sizeof err);
            CK(V[v].what, V[v].fails, rc != 0);
            if (!V[v].fails) { CK("fetch1d preserved", 1, r.b[1].f1d); CK("lead index", 1, r.b[1].tbx); }
            free(r.c1); free(b.s);
        }
        printf("%-64s %s\n", name, n_failed == f0 ? "ok" : "FAILED");
    }
}

// The real samples of the Objective-C parser's test, inline (upstream 913fe496): Super Mario Bros. Wonder's ps_models_p#0 (5c1ca4e7c3e4657c_b, 12 of its 44 bindings: one of each
// kind and texture type, their argument IDs as the build wrote them) and vs_models_v#0 (54f730a6c0efc118_b, whole), their field values copied from the json text
static void real_samples(void) {
    nvn_refl r; char err[256] = "";
    {   cur = "B fragment ps_models_p#0"; int f1 = n_failed; sbuf b = {0};
        const char *hex = "71c49cbf0000803f6f12833a83f9a23e818080bb8180803c0ad7233dcdcccc3dacc52737f343be3dcdcc4c3e800080b79a99993e9a99193fabaa2a3e";   // 15 words of constants, then zeros to 256 bytes
        put(&b, "{\"route\": \"B\", \"stage\": \"ps\", \"c1\": \"%s", hex);
        for (size_t k = strlen(hex); k < 512; k++) put(&b, "0");
        put(&b, "\", \"vertex_inputs\": [], \"drv_unfillable\": [], \"drv_reads\": {\"0\": {\"vec4_offsets\": [1376], \"dynamic\": false}, \"2\": {\"vec4_offsets\": [592, 1392], \"dynamic\": false}}, \"bindings\": [%s]}",
            J({"kind": "support", "arg_id": 0, "msl_name": "support_buffer", "msl_type": "constant _10*"},
              {"kind": "cbuf", "reg": 10, "arg_id": 1, "msl_name": "fp_c13", "msl_type": "constant _16*"},
              {"kind": "c1", "bank": 1, "arg_id": 4, "msl_name": "fp_c1", "msl_type": "constant _16*"},
              {"kind": "drvcb", "bank": 0, "arg_id": 7, "msl_name": "fp_c0", "msl_type": "constant _16*"},
              {"kind": "drvcb", "bank": 2, "arg_id": 8, "msl_name": "fp_c2", "msl_type": "constant _16*"},
              {"kind": "sbuf", "index": 0, "reg": 4, "rw": false, "arg_id": 11, "msl_name": "fp_s0", "msl_type": "const device _39*"},
              {"kind": "tbuf", "reg": 8, "arg_id": 13, "msl_name": "fp_t_tcb_6A", "msl_type": "texture_buffer<float>"},
              {"kind": "tex", "reg": 41, "arg_id": 14, "msl_name": "fp_t_cb196610_105800AC", "msl_type": "texture2d<float>"},
              {"kind": "samp", "reg": 3, "arg_id": 15, "msl_name": "fp_t_cb196610_105800ACSmplr", "msl_type": "sampler"},
              {"kind": "tex", "reg": 39, "arg_id": 28, "msl_name": "fp_t_cb196610_105980A8", "msl_type": "depth2d_array<float>"},
              {"kind": "tex", "reg": 33, "arg_id": 34, "msl_name": "fp_t_cb196610_105A009C", "msl_type": "depthcube<float>"},
              {"kind": "tex", "reg": 46, "arg_id": 36, "msl_name": "fp_t_cb196610_105880B6", "msl_type": "texture3d<float>"}));
        CK("conversion succeeds", 0, nvn_refl_from_json_text(b.s, b.n, &r, err, sizeof err));
        CK("B flag", 1, r.route_b); CK("argument buffer slot", 0, r.abi); CK("binding count", 12, r.nb); CK("vertex input count", 0, r.nv);
        CK("has c1", 1, r.has_c1); CK("c1 length 256 (control section field)", 256, r.c1_len);
        uint32_t c1w = 0; if (r.c1) memcpy(&c1w, r.c1, 4); CK("first c1 word = -sqrt(1.5) (0xbf9cc471)", 0xbf9cc471u, c1w);
        c1w = 1; if (r.c1) memcpy(&c1w, r.c1 + 252, 4); CK("last c1 word zero", 0, c1w);
        CK("reads driver bank c0", 1, r.drv_read[0]); CK("reads driver bank c2", 1, r.drv_read[2]);
        static const want_bind w[] = {
            {0, NVN_RB_SUPPORT, 0, 0, 0, X, X, X, X}, {1, NVN_RB_CBUF, 10, 1, 0, X, X, X, X}, {2, NVN_RB_C1, 0, 4, 0, X, X, X, X}, {3, NVN_RB_DRV0, 0, 7, 0, X, X, X, X},
            {4, NVN_RB_DRV2, 0, 8, 0, X, X, X, X}, {5, NVN_RB_SBUFP, 4, 11, 0, X, X, X, X}, {6, NVN_RB_TBUF, 8, 13, 0, X, X, X, X}, {7, NVN_RB_TEX, 41, 14, 0, NVN_DTT_2D, F, X, X},
            {8, NVN_RB_SAMP, 3, 15, 0, X, X, X, X}, {9, NVN_RB_TEX, 39, 28, 0, NVN_DTT_2D_ARRAY, D, X, X}, {10, NVN_RB_TEX, 33, 34, 0, NVN_DTT_CUBE, D, X, X}, {11, NVN_RB_TEX, 46, 36, 0, NVN_DTT_3D, F, X, X}};
        check_binds(&r, w, NW(w)); free(r.c1); free(b.s);
        printf("%-64s %s\n", "B fragment ps_models_p#0: support / c1 / driver banks / storage / textures", n_failed == f1 ? "ok" : "FAILED"); }
    {   cur = "B vertex vs_models_v#0"; int f1 = n_failed;
        const char *d = J({"route": "B", "stage": "vs", "c1": "", "drv_reads": {}, "drv_unfillable": [],
            "bindings": [{"kind": "cbuf", "reg": 0, "arg_id": 0, "msl_name": "vp_c3", "msl_type": "constant _16*"},
                         {"kind": "cbuf", "reg": 3, "arg_id": 1, "msl_name": "vp_c6", "msl_type": "constant _16*"}],
            "vertex_inputs": [{"location": 0, "msl_type": "float4"}, {"location": 1, "msl_type": "float4"}, {"location": 2, "msl_type": "float4"}, {"location": 3, "msl_type": "float4"}],
            "raster": {"pos_attr": -1}});
        CK("conversion succeeds", 0, nvn_refl_from_json_text(d, strlen(d), &r, err, sizeof err));
        CK("binding count", 2, r.nb); CK("input count", 4, r.nv); CK("c1 length 0 → no c1", 0, r.has_c1); CK("reads no driver bank", 0, r.drv_read[0] | r.drv_read[2]);
        for (int i = 0; i < 4 && i < r.nv; i++) { char m[48]; snprintf(m, sizeof m, "input[%d] attribute = location", i); CK(m, i, r.v[i].nvn); CK(m, i, r.v[i].loc); CK("component float", NVN_CT_FLOAT, r.v[i].ct); }
        CK("binding[0] uniform slot 0", 0, r.b[0].reg); CK("binding[1] uniform slot 3", 3, r.b[1].reg); free(r.c1);
        printf("%-64s %s\n", "B vertex vs_models_v#0: 4 attributes", n_failed == f1 ? "ok" : "FAILED"); }
}

int main(void) {
    for (int i = 0; i < NCASES; i++) run_case(&CASES[i]);
    for (int i = 0; i < LIM_COUNT; i++) run_limit(i);
    for (int i = 0; i < NBAD; i++) {
        nvn_refl r; char err[256] = ""; char m[64]; cur = "text that is not one json value";
        snprintf(m, sizeof m, "bad text %d fails", i); CK(m, 1, nvn_refl_from_json_text(BAD_TEXT[i], strlen(BAD_TEXT[i]), &r, err, sizeof err) != 0);
        snprintf(m, sizeof m, "bad text %d error text", i); CK(m, 1, !strncmp(err, "reflection json does not parse", 30));
    }
    {   nvn_refl r; char err[8] = ""; cur = "error buffer"; int f0 = n_failed;   // a short buffer gets the truncated text, as snprintf writes it
        CK("fails", 1, nvn_refl_from_json_text("{\"route\": \"B\"}", 14, &r, err, sizeof err) != 0); check_str("truncated text", "missing", err);
        CK("NULL text fails", 1, nvn_refl_from_json_text(NULL, 0, &r, err, sizeof err) != 0);
        const char *t = "{\"route\": \"B\", \"bindings\": []}garbage"; CK("length bounds the text", 0, nvn_refl_from_json_text(t, strlen(t) - 7, &r, err, sizeof err));
        printf("%-64s %s\n", "error buffer, NULL text, length-bounded text", n_failed == f0 ? "ok" : "FAILED"); }
    row_bindings();
    real_samples();
    printf("== t_refl_c: %d checks, %d failed ==\n", n_checks, n_failed);
    return n_failed != 0;
}
