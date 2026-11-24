"""The host-call thunk generator (dm/host/tools/gen_hle_thunks.py, docs/design/aot-recompiler.md 2.4): every entry shape it cannot pass as AAPCS64 does (a struct by
value, a sub-32-bit parameter, a variadic entry) and a C name two entries share stop it with the entry's file and line, and nothing is written; over the real host
sources (the list dm/host/CMakeLists.txt gives nxhost) it succeeds with every entry. Temporary files only; no game files, keys or compiler needed."""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
GEN = REPO / 'dm/host/tools/gen_hle_thunks.py'
HOST = REPO / 'dm/host'


def host_sources():
    """The nxhost sources of dm/host/CMakeLists.txt: NXHOST_SRC (hle.c loader.c thread.c front_sdl.c ...), the hle_*.c glob without the files its EXCLUDE filters name
    (hle_bigstack.c, hle_table.c, the Apple-only sources), and the NVN frontend (NVN_FRONTEND_SRC)"""
    cm = (HOST / 'CMakeLists.txt').read_text()
    excl = [re.compile(x.replace('\\\\', '\\')) for x in re.findall(r'list\(FILTER HLE_SRC EXCLUDE REGEX "([^"]*)"\)', cm)]
    assert excl, 'no EXCLUDE filter on HLE_SRC in dm/host/CMakeLists.txt'
    m = re.search(r'set\(NXHOST_SRC ([^)]*)\)', cm)
    assert m, 'NXHOST_SRC not found in dm/host/CMakeLists.txt'
    assert 'hle_bigstack' in cm
    f = re.search(r'set\(NVN_FRONTEND_SRC([^)]*)\)', cm)
    assert f, 'NVN_FRONTEND_SRC not found in dm/host/CMakeLists.txt'
    assert 'list(APPEND NXHOST_SRC ${NVN_FRONTEND_SRC}' in cm, 'the NVN frontend is not part of nxhost'
    nvn = [Path(n.replace('${REPO}', str(REPO))) for n in f.group(1).split()]
    hle = sorted(p for p in HOST.glob('hle_*.c') if not any(x.search('/' + p.name) for x in excl))
    return [HOST / n for n in m.group(1).split()] + hle + nvn


NVN_INCLUDE = REPO / 'dm/gfx/nvn/include'   # the -I the CMake rule gives the generator (GEN_HLE_THUNKS_INC)


class Generator(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.out = self.tmp / 'gen/hle_thunks.c'

    def run_gen(self, *sources, incs=()):
        flags = [a for d in incs for a in ('-I', str(d))]
        return subprocess.run([sys.executable, str(GEN), '-o', str(self.out), *flags, *map(str, sources)], capture_output=True, text=True, check=False)

    def source(self, name, text):
        p = self.tmp / name
        p.write_text('#include "hle.h"\n' + text)
        return p

    def fails(self, text, line, *words):
        src = self.source('bad.c', text)
        r = self.run_gen(src)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn(f'{src}:{line}:', r.stderr)
        for w in words:
            self.assertIn(w, r.stderr)
        self.assertFalse(self.out.exists(), 'nothing is written on an error')
        return r.stderr

    def test_struct_parameter(self):   # a struct with a float member is not an aggregate the thunks pass (its host ABI class would differ)
        self.fails('typedef struct { int a; float b; } pair;\nHLE_FORCE("f", "why", void, h_f, void *p, pair v) {}\n', 3, 'struct', 'by value')

    def test_struct_keyword_parameter(self):
        self.fails('HLE_FORCE("f", "why", int, h_f, struct timespec t) { return 0; }\n', 2, 'struct or union by value')

    def test_short_parameter(self):   # AAPCS64 leaves a sub-32-bit argument's upper register bits unspecified: the thunk passes the low bits on
        src = self.source('short.c', 'HLE_FORCE("f", "why", void, h_f, int a, short s, bool b) {}\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = self.out.read_text()
        self.assertIn('extern void h_f(int32_t, int16_t, uint8_t);', text)
        self.assertIn('h_f((int32_t)st->x[0], (int16_t)st->x[1], (uint8_t)st->x[2]);', text)

    def test_aggregates(self):   # AAPCS64 B.3, C.10-C.13: up to 16 bytes in x registers (or stack slots), larger by reference; results in x0 / x1 or through x8
        src = self.source('agg.c', 'typedef struct { uint64_t a, b; } uid;\ntypedef struct { int64_t a, b, c; } trio;\ntypedef struct { uint16_t w, h; uint32_t pad[2]; } pref;\n'
                          'HLE_FORCE("u", "why", uid, h_u, void *p, uid v) { return v; }\n'
                          'HLE_FORCE("t", "why", trio, h_t, trio v, pref q) { (void)q; return v; }\n'
                          'HLE_FORCE("s", "why", int, h_s, int a, int b, int c, int d, int e, int f, int g, uid v, int h) { return a; }\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = self.out.read_text()
        self.assertIn('typedef struct { uint64_t m0; uint64_t m1; } hle_agg_uid;', text)
        self.assertIn('typedef struct { uint16_t m0; uint16_t m1; uint32_t m2[2]; } hle_agg_pref;', text)
        self.assertIn('{ hle_agg_uid r_ = h_u((void *)(uintptr_t)st->x[0], ld_hle_agg_uid((const uint64_t[]){st->x[1], st->x[2]})); uint64_t w_[2] = {0, 0}; '
                      'memcpy(w_, &r_, sizeof r_); st->x[0] = w_[0]; st->x[1] = w_[1]; }', text)
        self.assertIn('{ hle_agg_trio r_ = h_t(ld_hle_agg_trio((void *)(uintptr_t)st->x[0]), ld_hle_agg_pref((const uint64_t[]){st->x[1], st->x[2]})); '
                      'memcpy((void *)(uintptr_t)st->x[8], &r_, sizeof r_); }', text)
        # seven ints take x0..x6; the 16-byte aggregate does not fit x7, so it goes to the stack and x7 stays unused (C.13): h takes the next stack slot
        self.assertIn('ld_hle_agg_uid((const void *)(uintptr_t)(st->sp + 0)), (int32_t)ld_stk(st, 16)', text)

    def test_forced_entry_kinds(self):   # HLE_FORCE_DIRECT gets a thunk as HLE_FORCE does; HLE_FORCE_ALIAS none, and is checked against the forced entries
        src = self.source('kinds.c', '#define WHY "why"\nHLE_FORCE_DIRECT("d", "why", int, h_d, int a) { return a; }\n'
                          'HLE_FORCE_ALIAS("a", "t", WHY);\nHLE_FORCE("f", "why", int, h_f, int a) { return a; }\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('1 forced aliases', r.stdout)
        text = self.out.read_text()
        self.assertIn('{"d", "h_d", hle_thunk_h_d, (const void *)h_d},', text)
        self.assertNotIn('"a"', text)
        self.out.unlink()
        self.fails('HLE_FORCE_ALIAS("x", "f", "why");\nHLE_FORCE("f", "why", int, h_f, int a) { return a; }\n', 2, 'forced alias x: its target f is itself taken over')
        self.fails('HLE_FORCE_ALIAS("f", "t", "why");\nHLE_FORCE("f", "why", int, h_f, int a) { return a; }\n', 2, 'duplicate entry f')

    def test_variadic_entry(self):
        self.fails('HLE("printf", int, h_printf, const char *fmt, ...) { return 0; }\n', 2, 'variadic')

    def test_struct_return(self):
        self.fails('typedef struct { float a; } box;\nHLE_FORCE("f", "why", box, h_f, int a) { box b = {a}; return b; }\n', 3, 'return type', 'by value')

    def test_unknown_typedef(self):
        self.fails('HLE_FORCE("f", "why", void, h_f, some_type_t v) {}\n', 2, "unknown type 'some_type_t'")

    def test_unknown_conditional(self):
        self.fails('#if DM_SOMETHING\nHLE_FORCE("f", "why", void, h_f, int v) {}\n#endif\n', 3, "inside '#if DM_SOMETHING'")

    def test_duplicate_c_name(self):
        a = self.source('a.c', '\nHLE_FORCE("f1", "why", void, h_same, int v) {}\n')
        b = self.source('b.c', '\n\nHLE_FORCE("f2", "why", void, h_same, int v) {}\n')
        r = self.run_gen(a, b)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f'{b}:4: duplicate C name h_same', r.stderr)
        self.assertIn(f'{a}:3', r.stderr)
        self.assertFalse(self.out.exists())

    def test_accepted_shapes(self):
        src = self.source('ok.c', 'typedef uint32_t handle;\n#define W(m, c, ...) HLE_FORCE(m, "why", int, c, __VA_ARGS__)\n'
                          'W("w", h_w, handle h, float f, double d, const void *p)\n{ return 0; }\n#if 0\nHLE_FORCE("x", "why", void, h_x, short s) {}\n#endif\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = self.out.read_text()
        self.assertIn('extern int32_t h_w(uint32_t, float, double, void *);', text)
        self.assertIn('st->x[0] = (uint64_t)(uint32_t)h_w((uint32_t)st->x[0], ld_f(st->v[0]), ld_d(st->v[1]), (void *)(uintptr_t)st->x[1]);', text)
        self.assertNotIn('h_x', text)
        self.assertIn('const size_t hle_thunks_count = 1;', text)

    def test_nvn_entries(self):   # dm/gfx/nvn/hle_nvn.c: NVN(name, ret, params...) and NVN_NOPROTO(...), named by the NVN function, C function h_<name>
        src = self.source('nvn.c', 'typedef struct { uint64_t value; } sep_handle;\ntypedef uint8_t NVNboolean;\n'
                          '#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)\n'
                          'NVN(nvnFoo, NVNboolean, void *d, sep_handle h, float f, uint32_t on) { return 1; }\n'
                          'NVN_NOPROTO(nvnBar, sep_handle, const void *d, int id) { sep_handle s = {id}; return s; }\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('2 entries (1 NVN, 1 NVN_NOPROTO)', r.stdout)
        text = self.out.read_text()
        self.assertIn('extern uint8_t h_nvnFoo(void *, uint64_t, float, uint32_t);', text)   # the one-member struct as its 64-bit integer
        self.assertIn('st->x[0] = (uint64_t)(uint8_t)h_nvnFoo((void *)(uintptr_t)st->x[0], (uint64_t)st->x[1], ld_f(st->v[0]), (uint32_t)st->x[2]);', text)
        self.assertIn('extern uint64_t h_nvnBar(void *, int32_t);', text)
        self.assertIn('{"nvnBar", "h_nvnBar", hle_thunk_h_nvnBar, (const void *)h_nvnBar},', text)
        self.assertLess(text.index('{"nvnBar"'), text.index('{"nvnFoo"'), 'sorted by name')

    def test_nvn_name_not_identifier(self):
        self.fails('\nNVN("nvnFoo", void, int x) {}\n', 3, "NVN function's name is not one identifier")

    def test_nvn_boolean_parameter(self):   # a sub-32-bit parameter is its register's low bits here too
        r = self.run_gen(self.source('nvnb.c', 'typedef uint8_t NVNboolean;\nNVN_NOPROTO(nvnSet, void, NVNboolean on) {}\n'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('h_nvnSet((uint8_t)st->x[0]);', self.out.read_text())

    def test_one_member_struct_only_64_bit(self):   # a two-member one is an aggregate in two registers, not the one-member struct's integer
        r = self.run_gen(self.source('two.c', 'typedef struct { uint64_t a; uint64_t b; } two;\nNVN(nvnF, void, two v) {}\n'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('h_nvnF(ld_hle_agg_two((const uint64_t[]){st->x[0], st->x[1]}));', self.out.read_text())

    def test_include_directory_and_cplusplus_block(self):   # a quoted header found through -I, its typedefs inside an extern "C" { of a __cplusplus block
        inc = self.tmp / 'inc'
        inc.mkdir()
        (inc / 'api.h').write_text('#pragma once\n#ifdef __cplusplus\nextern "C" {\n#endif\ntypedef uint64_t api_addr;\ntypedef enum { A_0, A_1 } api_mode;\n'
                                   '#ifdef __cplusplus\n}\n#endif\n')
        src = self.source('user.c', '#include "api.h"\nHLE_FORCE("f", "why", api_addr, h_f, api_mode m, api_addr a) { return a; }\n')
        self.assertNotEqual(self.run_gen(src).returncode, 0, 'without -I the typedefs are unknown')
        r = self.run_gen(src, incs=[inc])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('extern uint64_t h_f(uint32_t, uint64_t);', self.out.read_text())

    def test_real_host_sources(self):
        srcs = host_sources()
        self.assertGreater(len(srcs), 30)
        r = self.run_gen(*srcs, incs=[NVN_INCLUDE])
        self.assertEqual(r.returncode, 0, r.stderr)
        m = re.search(r'gen_hle_thunks: (\d+) entries', r.stdout)
        assert m, r.stdout
        n = int(m.group(1))
        print(f'\n{r.stdout.strip()}')
        self.assertGreater(n, 700)
        self.assertIn('0 unknown types', r.stdout)
        self.assertIn('0 duplicate C names', r.stdout)
        nvn = re.search(r'(\d+) NVN, (\d+) NVN_NOPROTO', r.stdout)
        assert nvn, r.stdout
        self.assertGreater(int(nvn.group(1)) + int(nvn.group(2)), 400, 'every NVN( entry of dm/gfx/nvn/hle_nvn.c')
        self.assertEqual(int(nvn.group(1)), len(re.findall(r'^NVN\(', (REPO / 'dm/gfx/nvn/hle_nvn.c').read_text(), re.M)))
        text = self.out.read_text()
        self.assertEqual(text.count('\nstatic void hle_thunk_'), n)
        self.assertIn(f'const size_t hle_thunks_count = {n};', text)
        names = re.findall(r'^    \{(".*"), "(\w+)", hle_thunk_\2, ', text, re.M)
        self.assertEqual(len(names), n)
        mangled = [ast.literal_eval(lit) for lit, _ in names]   # adjacent literals (a wrapper macro's # operands) join as in C
        self.assertEqual(mangled, sorted(mangled, key=lambda s: s.encode('latin-1')), 'sorted by mangled name, strcmp order')


if __name__ == '__main__':
    unittest.main()
