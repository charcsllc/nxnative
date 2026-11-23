"""Host code calls the game's own functions through GUEST_CALL (dm/host/aot.h DM_GUEST_CALL), never as plain C calls.

On Apple the guest's code is native ARM64, so a host take-over may call a function it looked up in the game's modules (guest_sym, gsym, module_sym, ...) as a
plain C function pointer. On the x86-64 engine (Windows and Linux) that pointer is the address of ARM64 code: a plain call jumps into it and the host dies with
SIGSEGV at that address (Mario vs. Donkey Kong's start after the upstream merge of 90edd532: hle_audren.c called nn::os::GetReadableHandleOfSystemEvent so).
The host files keep Apple's text by defining GUEST_CALL as the plain call under DM_NATIVE_EXEC and as DM_GUEST_CALL elsewhere; this test finds, in dm/host,
dm/gfx/nvn and dm/gfx/hostvk, every function pointer assigned from a guest symbol lookup and called outside GUEST_CALL / DM_GUEST_CALL / dm_gcall, unless the
call is inside a block compiled only where the guest's code is native (#if DM_NATIVE_EXEC, __APPLE__) or a unit test's C stand-ins (DM_TEST_GUEST_C).
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIRS = ('dm/host', 'dm/gfx/nvn', 'dm/gfx/hostvk')
LOOKUPS = r'(?:gsym\w*|guest_sym|module_sym|loaded_sym|sdk_handle_sym|dm_guest_fnptr|lib_sym|resolve_sym)'
WRAPPERS = ('GUEST_CALL', 'DM_GUEST_CALL', 'dm_gcall')
NATIVE = re.compile(r'DM_NATIVE_EXEC|__APPLE__|DM_TEST_GUEST_C')


def strip(text):
    """The C text with comments and string / character literals blanked (newlines kept, so line numbers hold)"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i); j = n if j < 0 else j; out.append(' ' * (j - i)); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2; out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n': j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * (min(j, n) - i - 1) + (c if j < n else '')); i = j + 1
        else:
            out.append(c); i += 1
    return ''.join(out)


def native_only_lines(lines):
    """The 1-based numbers of the lines inside a conditional branch compiled only where the guest's code is native: the #if branch of a condition naming
    DM_NATIVE_EXEC / __APPLE__ / DM_TEST_GUEST_C without negating it, or the #else branch of one that negates it (!DM_NATIVE_EXEC, #ifndef __APPLE__)"""
    stack, out = [], set()   # each entry: whether the current branch is native-only
    for k, line in enumerate(lines, 1):
        s = line.strip()
        m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', s)
        if m:
            kw, cond = m.group(1), m.group(2)
            if kw in ('if', 'ifdef', 'ifndef'):
                named = bool(NATIVE.search(cond))
                negated = kw == 'ifndef' or bool(re.search(r'!\s*(?:defined\s*\(?\s*)?(?:DM_NATIVE_EXEC|__APPLE__)', cond))
                stack.append([named and not negated, named and negated])   # [this branch native-only, the else branch native-only]
            elif kw == 'elif' and stack: stack[-1] = [False, False]
            elif kw == 'else' and stack: stack[-1] = [stack[-1][1], False]
            elif kw == 'endif' and stack: stack.pop()
            continue
        if any(e[0] for e in stack): out.add(k)
    return out


def direct_calls(text):
    """[(line, name, source line)] of the plain calls of guest-looked-up function pointers in one C file's text"""
    clean = strip(text); lines = clean.split('\n'); raw = text.split('\n')
    skip = native_only_lines(lines)
    names = set(re.findall(r'\b([A-Za-z_]\w*)\s*=\s*(?:\([^;=]*?\)\s*)?' + LOOKUPS + r'\s*\(', clean))
    found = []
    for name in sorted(names):
        for m in re.finditer(r'(?<![\w.>&])' + re.escape(name) + r'\s*\(', clean):
            k = clean.count('\n', 0, m.start()) + 1
            if k in skip: continue
            before = clean[max(0, m.start() - 40):m.start()]
            if re.search(r'\(\s*\*\s*$', before): continue   # the declaration (*name)(...)
            if re.search(r'(?:' + '|'.join(WRAPPERS) + r')\s*\(\s*$', before): continue   # GUEST_CALL(name, ...)
            found.append((k, name, raw[k - 1].strip()))
    return found


class GuestCalls(unittest.TestCase):
    def test_no_plain_call_of_a_guest_function(self):
        bad = []
        for d in DIRS:
            for f in sorted((REPO / d).glob('*.c')) + sorted((REPO / d).glob('*.inc')):
                for k, name, line in direct_calls(f.read_text(errors='replace')):
                    bad.append(f'{f.relative_to(REPO)}:{k}: {name}(...) called directly: {line[:160]}')
        self.assertEqual(bad, [], 'guest functions called as plain C functions (use GUEST_CALL; on the x86-64 engine they are ARM64 code):\n' + '\n'.join(bad))

    def test_the_scan_finds_and_spares(self):
        src = '\n'.join((
            '#define GUEST_CALL DM_GUEST_CALL',
            'static void f(void *e) {',
            '    static void (*destroy)(void *); if (!destroy) destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");',
            '    destroy(e);                       // line 4: plain call, found',
            '    GUEST_CALL(destroy, e);           // spared',
            '#if DM_NATIVE_EXEC',
            '    destroy(e);                       // native only: spared',
            '#else',
            '    destroy(e);                       // line 9: the x86-64 branch, found',
            '#endif',
            '    const char *s = "destroy(e)";     // a string: spared',
            '}'))
        self.assertEqual([(k, n) for k, n, _ in direct_calls(src)], [(4, 'destroy'), (9, 'destroy')])


if __name__ == '__main__':
    unittest.main()
