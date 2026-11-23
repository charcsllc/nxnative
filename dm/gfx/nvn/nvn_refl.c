// Shader reflection json (B: produced by translating the original Switch shaders, output of tools/dmsh_build.py) → fixed-length C arrays (nvn_refl.h), in plain C for every host;
// the Metal backend's parser too since step M6. It is the conversion of the Objective-C parser it replaced (Foundation's NSJSONSerialization and NSDictionary) rule for rule, in the
// same order, with the same failure texts, so the structs are byte for byte the ones that parser made: the Mac compared both over a corpus of 37,694 documents (t_refl_cmp, macOS CI
// on 093317b) and found no difference. cJSON (dm/third_party/cjson) reads the text; a cJSON item stands for the object NSJSONSerialization made of it, NULL for nil. Where the
// Objective-C code sent a message, the helpers below answer as Foundation does: intValue / boolValue of a number or a string, isKindOfClass checks, nil for a missing key. A value
// of a type on which the Objective-C code raised (intValue of null, an array or a dictionary; a subscript of a string) takes the nil answer here instead. Text Foundation's parser
// refused is refused here too (strict_text)
#include "nvn_refl.h"
#include "nvn_refl_textype.h"
#include "dmsh_abi.h"   // the binding kinds the build writes (DMSH_KIND_*)
#include "../../third_party/cjson/cJSON.h"
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef const cJSON *jv;   // what NSJSONSerialization makes of a json value: NSDictionary, NSArray, NSString, NSNumber (numbers and true / false), NSNull; NULL = nil

static int is_str(jv v) { return cJSON_IsString(v) && v->valuestring; }
static int is_num(jv v) { return cJSON_IsNumber(v) || cJSON_IsBool(v); }   // true / false are NSNumbers too (__NSCFBoolean)
static const char *str(jv v) { return is_str(v) ? v->valuestring : NULL; }
static const char *u8(jv v) { return is_str(v) ? v->valuestring : "(null)"; }   // UTF8String of nil, printed with %s by Apple's libc

// NSDictionary subscript: nil unless a dictionary has the key. Keys compare exactly (cJSON_GetObjectItem would ignore case); the last of duplicate keys wins, as in a dictionary built
// by setting each pair in turn
static jv get(jv o, const char *k) {
    jv r = NULL;
    if (cJSON_IsObject(o))
        for (jv c = o->child; c; c = c->next) if (c->string && !strcmp(c->string, k)) r = c;
    return r;
}
// A pair of a dictionary that enumeration visits: the last one of its key (for-in over an NSDictionary visits each key once)
static int last_of_key(jv c) {
    for (jv d = c->next; d; d = d->next) if (d->string && c->string && !strcmp(d->string, c->string)) return 0;
    return 1;
}

static int ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
// -[NSString intValue]: leading white space, an optional sign, decimal digits; INT_MAX / INT_MIN on overflow, 0 without digits
static int str_int(const char *s) {
    while (ws(*s)) s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    long long v = 0;
    for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (*s - '0'); if (v > 2147483648LL) v = 2147483648LL; }
    if (neg) return v >= 2147483648LL ? INT_MIN : (int)-v;
    return v > INT_MAX ? INT_MAX : (int)v;
}
// -[NSString boolValue]: YES on Y, y, T, t or a digit 1-9 after leading white space, an optional sign and zeros
static int str_bool(const char *s) {
    while (ws(*s)) s++;
    if (*s == '-' || *s == '+') s++;
    while (*s == '0') s++;
    return *s == 'Y' || *s == 'y' || *s == 'T' || *s == 't' || (*s >= '1' && *s <= '9');
}
// -[NSNumber intValue]. NSJSONSerialization makes a long long of an integer, and intValue keeps its low 32 bits; a number with a fraction is a double, which CFNumber converts by
// truncation, clamped to the int range. cJSON keeps every number as a double: an integral value is taken as an integer (exact up to 2^53)
static int num_int(double d) {
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
        int64_t i = (int64_t)d;
        if ((double)i == d) return (int)(uint32_t)(uint64_t)i;
        return d >= (double)INT_MAX ? INT_MAX : d <= (double)INT_MIN ? INT_MIN : (int)d;
    }
    return d < 0 ? INT_MIN : INT_MAX;
}
// -[NSNumber boolValue]: the value as a char (BOOL), not zero: an integer's low 8 bits; a double truncated and clamped to the char range
static int num_bool(double d) {
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
        int64_t i = (int64_t)d;
        if ((double)i == d) return (int8_t)(uint8_t)(uint64_t)i != 0;
        return d >= 1.0 || d <= -1.0;
    }
    return 1;
}
static int ival(jv v) {   // [v intValue]
    if (cJSON_IsTrue(v)) return 1;
    if (cJSON_IsNumber(v)) return num_int(v->valuedouble);
    if (is_str(v)) return str_int(v->valuestring);
    return 0;   // nil (and false); null, an array or a dictionary would raise in Objective-C
}
static int bval(jv v) {   // [v boolValue]
    if (cJSON_IsTrue(v)) return 1;
    if (cJSON_IsNumber(v)) return num_bool(v->valuedouble);
    if (is_str(v)) return str_bool(v->valuestring);
    return 0;
}
// -[NSNumber unsignedLongLongValue] / -[NSNumber unsignedIntValue]: an integer's low 64 / 32 bits; a number with a fraction truncated (CFNumber's conversion of a double,
// exact for the values the chain writes, which are integers). NSString answers neither: a string, like null, an array or a dictionary, takes the nil answer 0
static uint64_t u64val(jv v) {
    if (cJSON_IsTrue(v)) return 1;
    if (!cJSON_IsNumber(v)) return 0;
    double d = v->valuedouble;
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return (uint64_t)(int64_t)d;
    return d < 0 ? (uint64_t)INT64_MIN : d < 18446744073709551616.0 ? (uint64_t)d : UINT64_MAX;
}
static uint32_t uval(jv v) { return (uint32_t)u64val(v); }
static int count_of(jv v) { return cJSON_IsArray(v) || cJSON_IsObject(v) ? cJSON_GetArraySize(v) : 0; }   // [v count]: an array's or a dictionary's; 0 for nil
static int pre(const char *s, const char *p) { return s && !strncmp(s, p, strlen(p)); }    // hasPrefix: (NO for nil)
static int suf(const char *s, const char *p) { size_t a, b; return s && (a = strlen(s)) >= (b = strlen(p)) && !strcmp(s + a - b, p); }   // hasSuffix:
static int has(const char *s, const char *p) { return s && strstr(s, p) != NULL; }   // containsString:

// Dummy texture type: same prefix rules as the original dummy_tex (an unbound texture slot reads 0 per D3D semantics)
static void dummy_kind(const char *t, uint8_t *tt, uint8_t *pf) {
    enum nvn_dtt y = NVN_DTT_2D;
    // A multisample slot (texelFetch of a multisample texture: URP's CopyDepth reads its MSAA depth through texture2d_ms) is its own type; read as 2D, the multisample
    // texture bound there was reported as a type mismatch, and the slot's dummy and handle pools took 2D textures, which Metal does not accept in it
    if (pre(t, "texture2d_ms_array") || pre(t, "depth2d_ms_array")) y = NVN_DTT_2D_MULTISAMPLE_ARRAY;
    else if (pre(t, "texture2d_ms") || pre(t, "depth2d_ms")) y = NVN_DTT_2D_MULTISAMPLE;
    else if (pre(t, "texture2d_array") || pre(t, "depth2d_array")) y = NVN_DTT_2D_ARRAY;
    else if (pre(t, "texturecube_array") || pre(t, "depthcube_array")) y = NVN_DTT_CUBE_ARRAY;
    else if (pre(t, "texturecube") || pre(t, "depthcube")) y = NVN_DTT_CUBE;
    else if (pre(t, "texture3d")) y = NVN_DTT_3D;
    else if (pre(t, "texture1d_array")) y = NVN_DTT_1D_ARRAY;
    else if (pre(t, "texture1d")) y = NVN_DTT_1D;
    else if (pre(t, "texture_buffer")) y = NVN_DTT_BUFFER;
    *tt = (uint8_t)y;
    *pf = pre(t, "depth") ? NVN_DPF_DEPTH : has(t, "<uint") || has(t, "<ulong") ? NVN_DPF_UINT : has(t, "<int") ? NVN_DPF_SINT : NVN_DPF_FLOAT;
}

// array<element, n> → n and the element type (0 and the type itself for anything else). The element is copied into *buf (freed by the caller); the last ", " separates the count
static int arr_len(const char *ty, const char **el, char **buf) {
    *el = ty;
    if (!ty || !pre(ty, "array<") || !suf(ty, ">")) return 0;
    const char *c = NULL;
    for (const char *p = strstr(ty, ", "); p; p = strstr(p + 1, ", ")) c = p;
    if (!c) return 0;
    size_t n = (size_t)(c - ty) - 6, tail = strlen(c + 2) - 1;
    char *b = malloc(n + 1 + tail + 1);
    if (!b) return 0;
    memcpy(b, ty + 6, n); b[n] = 0;
    memcpy(b + n + 1, c + 2, tail); b[n + 1 + tail] = 0;   // the count after the element, for str_int
    free(*buf); *buf = b; *el = b;
    return str_int(b + n + 1);
}

// Whether a storage buffer binding may be written: current B reflection carries the translator's write flag ("rw"); old samples without it fall back to SPIRV-Cross's const device qualifier.
static int sbuf_rw(jv b) { jv rw = get(b, "rw"); if (rw) return bval(rw); return !pre(str(get(b, "msl_type")), "const "); }

// -description of an element of drv_unfillable, for componentsJoinedByString: (the chain writes strings)
static void desc(jv v, char *d, size_t n) {
    if (is_str(v)) snprintf(d, n, "%s", v->valuestring);
    else if (cJSON_IsBool(v)) snprintf(d, n, "%d", cJSON_IsTrue(v));
    else if (cJSON_IsNull(v)) snprintf(d, n, "<null>");
    else if (cJSON_IsNumber(v)) {
        double x = v->valuedouble;
        if (x >= -9223372036854775808.0 && x < 9223372036854775808.0 && (double)(int64_t)x == x) snprintf(d, n, "%lld", (long long)x);
        else snprintf(d, n, "%0.16g", x);
    } else snprintf(d, n, "%s", cJSON_IsArray(v) ? "()" : "{}");   // a nested array or dictionary: not written by the chain (Foundation prints its contents)
}

// UTF-16 length of a UTF-8 string (NSString length) and, per UTF-16 unit, its character when ASCII (0x80 for any other unit); *u is malloc'd, or NULL for an all-ASCII string
static size_t utf16_units(const char *s, unsigned char **u) {
    size_t n = 0, len = strlen(s), k = 0;
    int ascii = 1;
    for (size_t i = 0; i < len; i++) if ((unsigned char)s[i] >= 0x80) { ascii = 0; break; }
    *u = NULL;
    if (ascii) return len;
    for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)s[i]; if ((c & 0xc0) != 0x80) n += c >= 0xf0 ? 2 : 1; }
    if (!(*u = malloc(n ? n : 1))) return 0;
    for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)s[i];
        if (c < 0x80) (*u)[k++] = c; else if ((c & 0xc0) != 0x80) { (*u)[k++] = 0x80; if (c >= 0xf0) (*u)[k++] = 0x80; } }
    return n;
}

// Text the two parsers read differently, found by the Mac's comparison of them (t_refl_cmp's probes, macOS CI on 093317b), refused here since step M6: a control character
// (U+0000 to U+001F) inside a string and a number with a leading zero (012), both invalid json (RFC 8259), which NSJSONSerialization refused and cJSON takes; and a NUL inside a
// string (the escape \u0000), which Foundation kept in the string while cJSON's C strings end there (the route "B\u0000" was not B to Foundation, and is B to strcmp). The chain
// writes none of them. The first two never reach this parser on the Metal backend, which reads the file with Foundation first (shader_load) and gives up when that fails; on every
// other backend this parser is the only reader, and refuses them as Foundation did. Returns NULL, or what was found and its byte offset in *at
static int num_char(unsigned char c) { return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'; }
static const char *strict_text(const char *s, size_t n, size_t *at) {
    int in_str = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (in_str) {
            if (c == '"') in_str = 0;
            else if (c < 0x20) { *at = i; return "a control character inside a string"; }
            else if (c == '\\' && i + 1 < n) {
                if (s[i + 1] == 'u' && i + 5 < n && !memcmp(s + i + 2, "0000", 4)) { *at = i; return "a NUL inside a string"; }
                i++;   // the escaped character: an escaped quote or backslash neither ends the string nor escapes what follows
            }
        } else if (c == '"') in_str = 1;
        else if ((c == '-' || (c >= '0' && c <= '9')) && !(i && num_char((unsigned char)s[i - 1]))) {   // the first character of a number
            size_t k = i + (c == '-');
            if (k + 1 < n && s[k] == '0' && s[k + 1] >= '0' && s[k + 1] <= '9') { *at = i; return "a number with a leading zero"; }
        }
    }
    return NULL;
}
// cJSON records where a parse failed in one global (global_error, cJSON_GetErrorPtr), which every parse writes. The Metal backend loads shaders on several threads at once (the
// shader prefetch's dispatch_apply, the prewarm workers) and called the parser it replaced with no lock, which had no shared state: the parse is serialized here, so concurrent
// calls do not race on that global. Nothing reads it; the conversion after the parse works on the parse's own tree and runs in parallel
static pthread_mutex_t g_parse_mu = PTHREAD_MUTEX_INITIALIZER;

int nvn_refl_from_json_text(const char *json, size_t len, nvn_refl *o, char *err, int errlen) {
    memset(o, 0, sizeof *o);
    int rc = 0;
    char *elbuf = NULL, *joined = NULL;
    unsigned char *units = NULL;
    cJSON *root = NULL;
    jv j = NULL;
    #define FAIL(...) do { free(o->c1); o->c1 = NULL; snprintf(err, errlen, __VA_ARGS__); rc = 1; goto done; } while (0)
    {   // the text: one json value and white space around it (NSJSONSerialization takes no trailing data)
        const char *end = NULL, *bad; size_t at = 0;
        if (json && (bad = strict_text(json, len, &at))) FAIL("reflection json does not parse (%s at byte %lu)", bad, (unsigned long)at);
        pthread_mutex_lock(&g_parse_mu);
        root = json && len ? cJSON_ParseWithLengthOpts(json, len, &end, 0) : NULL;
        pthread_mutex_unlock(&g_parse_mu);
        if (root) while (end < json + len && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        if (!root || end != json + len) FAIL("reflection json does not parse (at byte %ld)", end && json ? (long)(end - json) : 0L);
    }
    j = root;
    {   const char *route = str(get(j, "route"));   // isEqual:@"B": a string, exactly B
        if (!route || strcmp(route, "B")) FAIL("not a B reflection (route ≠ B; route A / PC-chain reflection is retired)"); }
    {   // Translated original Switch shaders: kind and NVN slot number are already parsed from the MSL member names by dmsh_build.py
        jv bs = get(j, "bindings");
        if (!cJSON_IsArray(bs)) FAIL("missing bindings");
        unsigned long nbs = (unsigned long)cJSON_GetArraySize(bs);
        if (nbs > NVN_REFL_MAXB) FAIL("%lu bindings, over the limit %d", nbs, NVN_REFL_MAXB);
        o->route_b = 1; o->abi = 0;   // spirv-cross set 0 → [[buffer(0)]]
        for (jv b = bs->child; b; b = b->next) {
            const char *k = str(get(b, "kind")); nvn_rbind *r = &o->b[o->nb];
            jv rg = get(b, "reg"); int reg = is_num(rg) ? ival(rg) : -1, aid = ival(get(b, "arg_id"));
            r->aid = (uint16_t)aid; r->reg = reg < 0 ? 0 : (uint16_t)reg;
            jv nmv = get(b, "msl_name"); const char *nm = str(nmv), *ty = str(get(b, "msl_type"));
            // Ryujinx's fallback for a handle it cannot trace to one slot (handle type 4, nvn_refl.h TEXD / SAMPD)
            const char *el; int n = arr_len(ty, &el, &elbuf);
            if (suf(nm, "_cb0_40000000") && pre(ty, "array<") && suf(ty, ">")) {
                if (n < 1 || n > 32) FAIL("B binding %s: array of %d", nm, n);
                if (pre(el, "sampler")) r->kind = NVN_RB_SAMPD;
                else if (pre(el, "texture_buffer")) { r->kind = NVN_RB_TEXD; r->dtt = (uint8_t)NVN_DTT_BUFFER; uint8_t tt; dummy_kind(el, &tt, &r->dpf); }
                else if (pre(el, "texture") || pre(el, "depth")) { r->kind = NVN_RB_TEXD; dummy_kind(el, &r->dtt, &r->dpf); }
                else FAIL("B binding %s: dynamic handle array of %s", nm, el ? el : "(null)");
                r->f1d = (uint8_t)bval(get(b, "fetch1d"));
                jv dx = get(b, "tbx");   // an array of buffer textures: its first element's entry in the offset table, the rest following it (dmsh_reflect.py buffer_image_bias)
                if (dx) { int x = ival(dx), rows = r->f1d && r->dtt == NVN_DTT_2D;
                    if (x < 0 || x + n * (rows ? 2 : 1) > DMSH_TBIAS_SLOTS || (!rows && r->dtt != (uint8_t)NVN_DTT_BUFFER)) FAIL("B binding %s: offset table entries %d + %d of an array that is not of buffer textures or rows", nm, x, n);
                    r->tbx = (uint16_t)(x + 1); }
                r->n = (uint8_t)n; o->dyn = 1; o->nb++; continue;
            }
            #define IS(s) (k && !strcmp(k, s))
            #define SOFF() do { if (get(b, "soff")) { r->soff = (uint16_t)(ival(get(b, "soff")) + 1); r->sreg = (uint8_t)ival(get(b, "sreg")); } } while (0)
            #define WRITES(t) ((t) && (has((t), "access::write") || has((t), "access::read_write")))
            if (IS(DMSH_KIND_CBUF)) { int fx = ival(get(b, "fixed")); if (fx < 0 || fx > 65536 || fx % 4) FAIL("B binding %s: %d bytes read at fixed offsets", u8(nmv), fx);
                r->kind = NVN_RB_CBUF; r->fixw = (uint16_t)(fx / 4); }
            else if (IS(DMSH_KIND_SAMP)) r->kind = NVN_RB_SAMP;
            else if (IS(DMSH_KIND_SAMP_TEX)) r->kind = NVN_RB_SAMPT;
            else if (IS(DMSH_KIND_TEX)) { r->kind = NVN_RB_TEX; dummy_kind(ty, &r->dtt, &r->dpf); r->f1d = (uint8_t)bval(get(b, "fetch1d")); }
            // An array of them (array<element, n>): the handles of a uniform buffer the shader indexes at run time (nvn_refl.h TEXU / SAMPU)
            else if (IS(DMSH_KIND_TEX_UBO)) { r->kind = NVN_RB_TEXU; dummy_kind(el, &r->dtt, &r->dpf); r->off = (uint16_t)ival(get(b, "off")); o->bindless = 1; SOFF();
                if (n) { if (n > 32) FAIL("B binding %s: array of %d", u8(nmv), n); r->n = (uint8_t)n; } }
            else if (IS(DMSH_KIND_SAMP_UBO)) { r->kind = NVN_RB_SAMPU; r->off = (uint16_t)ival(get(b, "off")); r->raw_id = (uint8_t)bval(get(b, "raw_id")); o->bindless = 1; SOFF();
                if (n) { if (n > 32) FAIL("B binding %s: array of %d", u8(nmv), n); r->n = (uint8_t)n; } }
            else if (IS(DMSH_KIND_SAMP_ID)) r->kind = NVN_RB_SAMPI;
            // The pools, indexed by the handle's IDs (nvn_refl.h TEXP / SAMPP / IMGP): which texture or image is read is known only when the GPU runs the draw
            else if (IS(DMSH_KIND_TEX_POOL)) { r->kind = NVN_RB_TEXP; dummy_kind(str(get(b, "elem")), &r->dtt, &r->dpf); o->bindless = o->pool = 1; }
            else if (IS(DMSH_KIND_SAMP_POOL)) { r->kind = NVN_RB_SAMPP; o->bindless = o->pool = 1; }
            else if (IS(DMSH_KIND_IMG_POOL)) { const char *el_p = str(get(b, "elem")); r->kind = NVN_RB_IMGP; dummy_kind(el_p, &r->dtt, &r->dpf);
                r->rw = WRITES(el_p); o->bindless = o->pool = 1; }   // images indexed by the IDs the shader loads (img_pool_gpu)
            // A table of the handles in a uniform buffer (nvn_refl.h TEXH / SAMPH)
            else if (IS(DMSH_KIND_TEX_HTAB) || IS(DMSH_KIND_SAMP_HTAB)) { int samp = IS(DMSH_KIND_SAMP_HTAB);
                r->kind = samp ? NVN_RB_SAMPH : NVN_RB_TEXH; if (!samp) dummy_kind(str(get(b, "elem")), &r->dtt, &r->dpf); r->off = (uint16_t)ival(get(b, "off")); o->bindless = o->htab = 1; SOFF(); }
            else if (IS(DMSH_KIND_TEX_C2A) || IS(DMSH_KIND_SAMP_C2A)) {   // an array of the driver table's handles (nvn_refl.h TEXA / SAMPA)
                int words = (DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS) / DMSH_C2_ROW, soff = ival(get(b, "soff"));   // one element per c2 handle word (Program.cs C2HandleWords)
                if (n < 1 || n > words) FAIL("B binding %s: array of %d", u8(nmv), n);
                if (soff < 0 || soff % DMSH_C2_ROW || soff >= DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS) FAIL("B binding %s: sampler word at c2 byte %#x", u8(nmv), soff);
                r->kind = IS(DMSH_KIND_TEX_C2A) ? NVN_RB_TEXA : NVN_RB_SAMPA; r->n = (uint16_t)n; r->off = (uint16_t)ival(get(b, "off")); r->soff = (uint16_t)soff;
                if (r->kind == NVN_RB_TEXA) dummy_kind(el, &r->dtt, &r->dpf);
            }
            else if (IS(DMSH_KIND_IMG)) { r->kind = NVN_RB_IMG; dummy_kind(ty, &r->dtt, &r->dpf);   // rw: the shader writes it (access::write / read_write; a load-only view of the slot is not)
                r->rw = WRITES(ty); r->atom = bval(get(b, "atomic")) ? (has(ty, "<ulong") ? 2 : 1) : 0; }   // 2: 64-bit atomics (texture2d<ulong>)
            else if (IS(DMSH_KIND_TBUF)) { r->kind = NVN_RB_TBUF; uint8_t tt; dummy_kind(ty, &tt, &r->dpf); }
            else if (IS(DMSH_KIND_IMG_UBO)) { r->kind = NVN_RB_IMGU; dummy_kind(ty, &r->dtt, &r->dpf); r->off = (uint16_t)ival(get(b, "off")); o->bindless = 1;
                r->rw = WRITES(ty); r->atom = bval(get(b, "atomic")) ? (has(ty, "<ulong") ? 2 : 1) : 0; }   // 2: 64-bit atomics (texture2d<ulong>)
            else if (IS(DMSH_KIND_TBUF_UBO)) { r->kind = NVN_RB_TBUFU; uint8_t tt; dummy_kind(ty, &tt, &r->dpf); r->off = (uint16_t)ival(get(b, "off")); o->bindless = 1; SOFF(); }
            else if (IS(DMSH_KIND_SBUF)) { r->kind = NVN_RB_SBUFP; r->rw = (uint8_t)sbuf_rw(b); }
            else if (IS(DMSH_KIND_UBO_PTR)) r->kind = NVN_RB_UBOP;
            else if (IS(DMSH_KIND_SBUF_UBO)) { r->kind = NVN_RB_SBUFU; r->rw = (uint8_t)sbuf_rw(b); r->off = (uint16_t)ival(get(b, "off")); }
            else if (IS(DMSH_KIND_SBUF_PUSH)) { r->kind = NVN_RB_SBUFQ; r->rw = (uint8_t)sbuf_rw(b); r->off = (uint16_t)ival(get(b, "off")); }
            else if (IS(DMSH_KIND_GEOMETRY)) { if (reg < 0 || reg > 4) FAIL("invalid geometry helper slot"); r->kind = NVN_RB_GEOMETRY; }
            else if (IS(DMSH_KIND_SUPPORT)) r->kind = NVN_RB_SUPPORT;
            else if (IS(DMSH_KIND_GMEM_MAP)) {
                r->kind = NVN_RB_GMEM; r->rw = (uint8_t)bval(get(b, "rw"));
                _Static_assert(DMSH_GPTR_MAX == sizeof o->gptr / sizeof o->gptr[0], "nvn_refl.gptr must hold DMSH_GPTR_MAX sources");
                jv sources = get(b, "pointers");   // the table's fixed pointer sources, [slot, byte] each (nvn_mtl.m gmem_discover)
                if (sources && (!cJSON_IsArray(sources) || count_of(sources) > DMSH_GPTR_MAX || o->ngptr)) FAIL("B binding %s: invalid or duplicate raw pointer sources", u8(nmv));
                for (jv e = sources ? sources->child : NULL; e; e = e->next) {
                    if (!cJSON_IsArray(e) || count_of(e) != 2 || !is_num(cJSON_GetArrayItem(e, 0)) || !is_num(cJSON_GetArrayItem(e, 1))) FAIL("B binding %s: a raw pointer source is not [slot, byte]", u8(nmv));
                    int sreg = ival(cJSON_GetArrayItem(e, 0)), soff = ival(cJSON_GetArrayItem(e, 1));
                    if (sreg < 0 || sreg >= 16 || soff < 0 || soff > 0xfff8) FAIL("B binding %s: raw pointer slot %d byte %d out of range", u8(nmv), sreg, soff);
                    o->gptr[o->ngptr].reg = (uint8_t)sreg; o->gptr[o->ngptr].off = (uint16_t)soff; o->ngptr++;
                }
            }
            else if (IS(DMSH_KIND_RT_SCENE)) r->kind = NVN_RB_RT_SCENE;
            else if (IS(DMSH_KIND_RT_PARAMETERS)) r->kind = NVN_RB_RT_PARAMETERS;
            else if (IS(DMSH_KIND_RT_ERRORS)) r->kind = NVN_RB_RT_ERRORS;
            else if (IS(DMSH_KIND_RT_INSTANCE_SBT)) r->kind = NVN_RB_RT_INSTANCE_SBT;
            else if (IS(DMSH_KIND_RT_BANK0)) r->kind = NVN_RB_RT_BANK0;
            else if (IS(DMSH_KIND_RT_BANK3)) r->kind = NVN_RB_RT_BANK3;
            else if (IS(DMSH_KIND_GMEM_LOG)) { r->kind = NVN_RB_GMEML; r->rw = 1; }
            else if (IS(DMSH_KIND_C1)) r->kind = NVN_RB_C1;
            else if (IS(DMSH_KIND_CBSIZE)) r->kind = NVN_RB_CBSIZE;
            else if (IS(DMSH_KIND_SBLEN)) { if (o->sblen) FAIL("B binding %s: a second length table", u8(nmv)); r->kind = NVN_RB_SBLEN; o->sblen = 1; }
            else if (IS(DMSH_KIND_TBIAS)) { int tn = ival(get(b, "n"));   // the offset table: its length in entries, as the shader declares it
                if (tn < 4 || tn > DMSH_TBIAS_SLOTS || tn % 4 || o->tbias_n) FAIL("B binding %s: offset table of %d entries", u8(nmv), tn);
                r->kind = NVN_RB_TBIAS; o->tbias_n = (uint16_t)tn; }
            else if (IS(DMSH_KIND_DRVCB)) r->kind = ival(get(b, "bank")) == DMSH_PUSH_BANK ? NVN_RB_PUSH : ival(get(b, "bank")) == 2 ? NVN_RB_DRV2 : NVN_RB_DRV0;
            else if (IS(DMSH_KIND_BORDER)) { if (o->border) FAIL("two border colour blocks"); r->kind = NVN_RB_BORDER; o->border = (uint8_t)(o->nb + 1); }
            else FAIL("B binding kind %s not implemented (%s)", k ? k : "(null)", u8(nmv));
            #undef IS
            #undef SOFF
            #undef WRITES
            if (r->kind == NVN_RB_TEXU || r->kind == NVN_RB_TEXA) r->f1d = (uint8_t)bval(get(b, "fetch1d"));
            jv tx = get(b, "tbx");   // a buffer image's or buffer texture's index; rows also take a logical-width group after their leads
            if (tx) { int x = ival(tx), arr = r->kind == NVN_RB_TEXA, rows = r->f1d && r->dtt == NVN_DTT_2D;
                int count = (r->n ? r->n : 1) * (rows ? 2 : 1);
                if (x < 0 || x + count > DMSH_TBIAS_SLOTS || (r->kind != NVN_RB_IMG && r->kind != NVN_RB_IMGU && r->kind != NVN_RB_TBUF && r->kind != NVN_RB_TBUFU &&
                                                              !(arr && r->dtt == (uint8_t)NVN_DTT_BUFFER) && !rows))
                    FAIL("B binding %s: offset table index %d of a binding that is not an image or buffer texture, nor buffer rows", u8(nmv), x);
                r->tbx = (uint16_t)(x + 1); }
            jv sx = get(b, "sbx");   // a bounded storage buffer's slot in the length table (dmsh_reflect.py; Ryujinx TranslationFlags.MetalStorageBounds)
            if (sx) { int x = ival(sx);
                if (x < 0 || x >= DMSH_SBLEN_SLOTS || (r->kind != NVN_RB_SBUFP && r->kind != NVN_RB_UBOP && r->kind != NVN_RB_SBUFU && r->kind != NVN_RB_SBUFQ)) FAIL("B binding %s: length table slot %d of a binding that is not a storage buffer", u8(nmv), x);
                r->sbx = (uint8_t)(x + 1); }
            jv bb = get(b, "border_bit");   // a twin sampler (dmsh_rewrite.py border_emu)
            if (bb) { int bit = ival(bb); if (bit < 0 || bit >= DMSH_BORDER_SAMPLERS || (r->kind != NVN_RB_SAMP && r->kind != NVN_RB_SAMPT && r->kind != NVN_RB_SAMPU && r->kind != NVN_RB_SAMPI) || r->n)
                    FAIL("B binding %s: border colour twin %d of a binding that is not one sampler", u8(nmv), bit);
                r->bw = (uint8_t)(bit + 1); }
            if ((r->kind == NVN_RB_CBUF || r->kind == NVN_RB_SAMP || r->kind == NVN_RB_SAMPT || r->kind == NVN_RB_TEX || r->kind == NVN_RB_IMG || r->kind == NVN_RB_TBUF || r->kind == NVN_RB_SBUFP || r->kind == NVN_RB_UBOP || r->kind == NVN_RB_SBUFU || r->kind == NVN_RB_TEXU || r->kind == NVN_RB_SAMPU || r->kind == NVN_RB_SAMPI || r->kind == NVN_RB_IMGU || r->kind == NVN_RB_TBUFU || r->kind == NVN_RB_TEXH || r->kind == NVN_RB_SAMPH) && reg < 0)
                FAIL("B binding %s has no resolved slot number", u8(nmv));
            o->nb++;
        }
        for (int i = 0; i < o->nb; i++) {   // every offset table entry a binding takes lies in the table the shader declares
            const nvn_rbind *r = &o->b[i]; int count = (r->n ? r->n : 1) * (r->f1d && r->dtt == NVN_DTT_2D ? 2 : 1);
            if (r->tbx && r->tbx - 1 + count > o->tbias_n) FAIL("B binding %d: offset table entries %d + %d past the table's %d", i, r->tbx - 1, count, o->tbias_n);
            if (r->sbx && !o->sblen) FAIL("B binding %d: a bounded storage buffer in a shader with no length table", i);
        }
        jv dr = get(j, "drv_reads");
        if (cJSON_IsObject(dr)) { o->drv_read[0] = get(dr, "0") != NULL; o->drv_read[2] = get(dr, "2") != NULL; }
        // A declared driver-segment binding means the shader reads it (docs/reference/shaders.md)
        for (int i = 0; i < o->nb; i++) { if (o->b[i].kind == NVN_RB_DRV0) o->drv_read[0] = 1; else if (o->b[i].kind == NVN_RB_DRV2) o->drv_read[2] = 1; }
        jv uf = get(j, "drv_unfillable");
        if (cJSON_IsArray(uf) && cJSON_GetArraySize(uf)) {
            size_t cap = 1;
            for (jv e = uf->child; e; e = e->next) cap += (is_str(e) ? strlen(e->valuestring) : 32) + 1;
            if ((joined = malloc(cap))) {
                size_t at = 0;
                for (jv e = uf->child; e; e = e->next) { if (e != uf->child) joined[at++] = ' '; desc(e, joined + at, cap - at); at += strlen(joined + at); }
                joined[at] = 0;
            }
            FAIL("reads driver-bank locations at run time that the replay layer cannot fill: %s", joined ? joined : "");
        }
        const char *c1 = str(get(j, "c1"));
        size_t c1n = c1 ? utf16_units(c1, &units) : 0;   // NSString length: UTF-16 units
        if (c1n % 2 || c1n / 2 > 0xffff) FAIL("c1 is %lu bytes: over the limit or an odd hex length", (unsigned long)c1n / 2);
        if (c1n && !(o->c1 = malloc(c1n / 2))) FAIL("c1 allocation of %lu bytes failed", (unsigned long)c1n / 2);
        if (c1n) { o->has_c1 = 1; o->c1_len = (uint16_t)(c1n / 2);
            for (unsigned i = 0; i < o->c1_len; i++) {   // strtoul of each two-unit substring (only its ASCII start can be read as hex)
                char two[3] = { 0, 0, 0 };
                if (units) { if (units[2 * i] < 0x80) { two[0] = (char)units[2 * i]; if (units[2 * i + 1] < 0x80) two[1] = (char)units[2 * i + 1]; } }
                else { two[0] = c1[2 * i]; two[1] = c1[2 * i + 1]; }
                o->c1[i] = (uint8_t)strtoul(two, NULL, 16);
            } }
        jv vi = get(j, "vertex_inputs");
        for (jv v = cJSON_IsArray(vi) ? vi->child : NULL; v; v = v->next) {   // [[attribute(n)]]: n is the NVN vertex attribute number
            int loc = ival(get(v, "location")); const char *t = str(get(v, "msl_type"));
            if (loc < 0 || loc >= NVN_REFL_MAXV || o->nv >= NVN_REFL_MAXV) FAIL("B vertex input %d out of range", loc);
            o->v[o->nv].loc = o->v[o->nv].nvn = (uint8_t)loc;
            o->v[o->nv].ct = pre(t, "uint") ? NVN_CT_UINT : pre(t, "int") ? NVN_CT_INT : NVN_CT_FLOAT;
            memset(o->v[o->nv].sh, 0xff, sizeof o->v[o->nv].sh);
            o->nv++;
        }
        jv vin = get(j, "vin");   // shadow inputs of the <key>_bv library (dmsh_vin.py): per input, the Metal attribute index of its float / other-sign declaration
        if (cJSON_IsObject(vin)) {
            uint32_t used = 0;
            jv sh = get(vin, "shadow");
            for (jv e = cJSON_IsArray(sh) ? sh->child : NULL; e; e = e->next) {
                int loc = ival(get(e, "location")), v = 0;
                while (v < o->nv && o->v[v].loc != loc) v++;
                if (v == o->nv || o->v[v].ct == NVN_CT_FLOAT) FAIL("shadow input for location %d, which is not an integer input", loc);
                static const struct { const char *k; int ct; } T[3] = { { "float", NVN_CT_FLOAT }, { "sint", NVN_CT_INT }, { "uint", NVN_CT_UINT } };
                for (int q = 0; q < 3; q++) { jv x = get(e, T[q].k); if (!x) continue; int idx = ival(x);
                    if (idx < 16 || idx > 30 || used >> idx & 1 || T[q].ct == o->v[v].ct) FAIL("shadow input %s of location %d at attribute %d: out of 16-30, used twice or of the input's own type", T[q].k, loc, idx);
                    used |= 1u << idx; o->v[v].sh[T[q].ct] = (uint8_t)idx; }
            }
            o->has_vin = used != 0;
        }
        jv ra = get(j, "raster");
        o->pos_attr = -1; o->may_discard = 1; o->writes_smask = 1;
        if (cJSON_IsObject(ra)) {
            if (get(ra, "pos_attr")) { int pa = ival(get(ra, "pos_attr")); o->pos_attr = (int8_t)(pa >= 0 && pa < 16 ? pa : -1); const char *w = str(get(ra, "pos_w")); o->pos_w1 = w && !strcmp(w, "one"); }
            if (get(ra, "discard")) o->may_discard = ival(get(ra, "discard")) != 0;
            if (get(ra, "sample_mask")) o->writes_smask = ival(get(ra, "sample_mask")) != 0;
            o->reads_prim_id = ival(get(ra, "primitive_id")) != 0;
            o->pos_far = ival(get(ra, "far")) != 0;
            jv cs = get(ra, "colors");   // for-in: an array's elements, a dictionary's keys
            if (cJSON_IsArray(cs)) { for (jv c = cs->child; c; c = c->next) { int ci = ival(c); if (ci >= 0 && ci < 8) o->color_mask |= (uint8_t)(1u << ci); } }
            else if (cJSON_IsObject(cs)) { for (jv c = cs->child; c; c = c->next) { int ci = c->string ? str_int(c->string) : 0; if (ci >= 0 && ci < 8) o->color_mask |= (uint8_t)(1u << ci); } }
            o->color_known = cJSON_IsArray(cs);
            if (is_num(get(ra, "float_colors"))) { o->fcols = (uint8_t)uval(get(ra, "float_colors")); o->fcols_known = 1;
                o->ucols = (uint8_t)uval(get(ra, "uint_colors")); o->scols = (uint8_t)uval(get(ra, "sint_colors")); }
            if (is_num(get(ra, "omap"))) { o->omap = (uint32_t)u64val(get(ra, "omap")); o->omap_known = 1; }
        }
        memset(o->uv_attr, -1, sizeof o->uv_attr); memset(o->uv_aff_attr, -1, sizeof o->uv_aff_attr);
        jv ua = cJSON_IsObject(ra) ? get(ra, "uv_affine") : NULL;
        if (cJSON_IsObject(ua)) for (jv kv = ua->child; kv; kv = kv->next) { if (!kv->string || !last_of_key(kv)) continue;
            int l = str_int(kv->string); jv e = kv; int a = ival(get(e, "attr"));
            if (l < 0 || l >= 32 || a < 0 || a >= 16) continue;
            jv refs[4] = { get(e, "sx"), get(e, "ox"), get(e, "sy"), get(e, "oy") }; int ok = 1;
            for (int q = 0; q < 4; q++) { jv r = refs[q]; if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 3) { ok = 0; break; }
                int sl = ival(cJSON_GetArrayItem(r, 0)), vec = ival(cJSON_GetArrayItem(r, 1)), cmp = ival(cJSON_GetArrayItem(r, 2)); if (sl < 0 || sl > 15 || vec < 0 || vec > 4095 || cmp < 0 || cmp > 3) { ok = 0; break; }
                o->uv_aff[l][q] = (uint32_t)(sl << 16 | vec << 2 | cmp); }
            if (ok) o->uv_aff_attr[l] = (int8_t)a; }
        jv uv = cJSON_IsObject(ra) ? get(ra, "uv_from_attr") : NULL;
        if (cJSON_IsObject(uv)) for (jv kv = uv->child; kv; kv = kv->next) { if (!kv->string || !last_of_key(kv)) continue;
            int l = str_int(kv->string), a = ival(kv); if (l >= 0 && l < 32 && a >= 0 && a < 16) o->uv_attr[l] = (int8_t)a; }
        jv scr = cJSON_IsObject(ra) ? get(ra, "screen") : NULL;   // the clip -> screen varyings of gl_Position (nvn_refl.h scr_locn)
        if (cJSON_IsArray(scr)) for (jv k = scr->child; k; k = k->next) { int ki = ival(k); if (ki >= 0 && ki < 32) o->scr_locn |= 1u << ki; }
        jv fe = get(j, "fetch");
        if (cJSON_IsObject(fe)) {
            jv calls = get(fe, "calls");
            for (jv c = cJSON_IsArray(calls) ? calls->child : NULL; c; c = c->next) { int i = ival(get(c, "fc")), r = ival(get(c, "tex_reg")), l = ival(get(c, "varying_locn")); jv fp = get(c, "frag_pos");
                if (cJSON_IsArray(fp)) {   // a pixel-position sample: two uniform references, no interpolated output
                    if (count_of(fp) != 2 || i >= 32) FAIL("fetch call %d: frag_pos is not two uniform references", i);
                    for (int q = 0; q < 2; q++) { jv x = cJSON_GetArrayItem(fp, q); int sl = cJSON_IsArray(x) && count_of(x) == 3 ? ival(cJSON_GetArrayItem(x, 0)) : -1, vec = sl >= 0 ? ival(cJSON_GetArrayItem(x, 1)) : -1, cmp = sl >= 0 ? ival(cJSON_GetArrayItem(x, 2)) : -1;
                        if (sl < 0 || sl > 15 || vec < 0 || vec > 4095 || cmp < 0 || cmp > 3) FAIL("fetch call %d: frag_pos uniform reference out of range", i);
                        if (i >= 0) o->fc_scale[i][q] = (uint32_t)(sl << 16 | vec << 2 | cmp); }   // a negative entry fails below, as it did after the Objective-C parser wrote before the array
                    if (i >= 0) o->fc_fpos |= 1u << i;
                    l = 0xff; }
                else if (l < 0 || l >= 32) l = -1;
                if (i != o->nfc || i >= 32 || r < 0 || r >= 256 || l < 0) FAIL("fetch call table not contiguous or out of range (entry %d)", i);
                if (ival(get(c, "proj"))) o->fc_proj |= 1u << i;
                o->fc_reg[o->nfc] = (uint8_t)r; o->fc_locn[o->nfc] = (uint8_t)l; o->nfc++; }
            jv mr = get(fe, "mixed_regs");   // for-in: an array's elements, a dictionary's keys
            for (jv r = cJSON_IsArray(mr) || cJSON_IsObject(mr) ? mr->child : NULL; r; r = r->next) { int ri = cJSON_IsArray(mr) ? ival(r) : r->string ? str_int(r->string) : 0;
                if (ri >= 0 && ri < 256) o->fc_mixed[ri >> 6] |= 1ull << (ri & 63); }
            jv outs = get(fe, "outputs");
            for (jv u = cJSON_IsArray(outs) ? outs->child : NULL; u; u = u->next) { if (o->nfo >= 4) FAIL("more than 4 fetch outputs"); o->fo_color[o->nfo++] = (uint8_t)ival(get(u, "color")); }
        }
        jv tg = get(j, "threadgroup_size");
        if (cJSON_IsArray(tg) && cJSON_GetArraySize(tg) == 3) for (int i = 0; i < 3; i++) o->tg[i] = (uint16_t)ival(cJSON_GetArrayItem(tg, i));
        o->tg_spill = uval(get(j, "tg_spill"));
        o->loop_sites = (uint32_t)count_of(get(j, "loop_sites"));
        o->mesh_spill = (uint8_t)bval(get(j, "mesh_spill"));
        jv fs = get(j, "first_store");
        for (jv u = cJSON_IsArray(fs) ? fs->child : NULL; u; u = u->next) { if (o->nfirst >= 4) FAIL("more than 4 first-invocation stores");
            int sreg = ival(get(u, "reg")); if (sreg < 0 || sreg >= 16) FAIL("first-invocation store to storage slot %d", sreg);
            o->first[o->nfirst].reg = (uint8_t)sreg; o->first[o->nfirst].word = (uint16_t)ival(get(u, "word")); o->first[o->nfirst++].value = uval(get(u, "value")); }
    }
    #undef FAIL
done:
    free(elbuf); free(joined); free(units);
    cJSON_Delete(root);
    return rc;
}
