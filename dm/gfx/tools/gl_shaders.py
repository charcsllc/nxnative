#!/usr/bin/env python3
# OpenGL games' GLSL shaders → one Metal library + a table for the host's GL layer (dm/gfx/gl/hle_gl.m), or with --vk a SPIR-V library + its table for the Vulkan
# backend's GL layer (dm/gfx/hostvk/guest_gl.c, Windows and Linux). Digimon Story Cyber Sleuth: Complete Edition renders with NVIDIA's desktop GL through EGL and compiles
# GLSL ES 3.20 source at run time (glShaderSource / glCompileShader); the host finds each shader again by the hash of its source.
# Sources, all searched in every input path:
#   · MVGL archives (*.mvgl, dm/gfx/tools/mvgl.py): every .shad file, named <material>_vp / <material>_fp (1,682 files, 615 distinct sources in Digimon);
#   · an NSO (a file that starts with NSO0, the game's main): every NUL-terminated string that starts with '#version' (its built-in post effects, movie and shadow shaders);
#   · capture folders (the Mac host's DM_GL_CAPTURE): <key>.vert / <key>.frag, what the game really compiled, for sources it puts together at run time.
#   · loose source files: *.vert / *.frag, and *.glsl, whose name says nothing of its stage (Mega Man Legacy Collection's shaders/nx/*.glsl, read and compiled as they are).
#     libGDX's *.vs / *.fs are read where the directory has a complete source of each stage (Slay the Spire).
# Per shader:
#   · key = sha256(the source text up to its first NUL)[:16] (hex), as the host hashes the strings glShaderSource gets; the Metal function is gl_<key>_v or gl_<key>_f;
#   · the stage: a capture's extension, an archive file's _vp / _fp; a string in main or a .glsl file says nothing of its stage, so it is translated as each stage that compiles (one of
#     Digimon's built-in fragment shaders names gl_Position in a #define);
#   · GLSL → SPIR-V with glslang (-V -R: relaxed Vulkan rules, which gather the loose uniforms into one block, gl_DefaultUniformBlock; --aml / --amb number the inputs,
#     outputs and bindings), SPIR-V → MSL with spirv-cross (one Tier 2 argument buffer per stage; vertex shaders --fixup-clipspace: GL's z from −w to w → 0 to w);
#   · varyings are matched by name, as GL links them: --aml numbers each stage's outputs in its own declaration order, so [[user(locnN)]] becomes [[user(<variable name>)]];
#   · the host renders in GL's orientation (row 0 at the bottom, the NVN layer's lower-left origin), so nothing is flipped here.
# --vk (docs/design/vulkan-backend.md 8.2): the same SPIR-V, without the MSL step and without spirv-cross (the in-repo build is a Mac binary): the table comes from the module
#   itself (reflect below gives table_fields_vk what spirv-cross --reflect gives the MSL path), then three passes on the binary with dmsh_spv.py's reader: the fragment
#   stage's resources move to descriptor set 1 (--amb numbers each stage's bindings from 0, so the vertex stage keeps set 0), a vertex shader's position gets z = (z + w) / 2
#   before each return (what --fixup-clipspace writes into the MSL), and FMin / FMax / FClamp become NMin / NMax / NClamp (dmsh_spv.py precise: the MSL path's precise::).
#   Varyings keep --aml's locations: the host matches them by name when a program links (guest_gl.c), as GL does. spirv-val checks each module where it is installed.
# Output: <out>/gl/gl_shaders.metallib and <out>/gl/gl_shaders.tsv; --ios <dir> also writes <dir>/gl/ with an iPhone library (same table); --vk writes <out>/gl-vk/ instead:
#   gl_<key>_v.spv / gl_<key>_f.spv and gl_shaders.tsv. With <out> the shader cache folder (DM_MSL_DIR), gl-vk/ stands beside the Metal library gl/ and the NVN chain's Vulkan
#   library vk-<DMSH_CHAIN_VK>/ (dmsh_build_vk.py); it is not versioned by the chain, as gl/ is not: this tool is not part of it
# Table rows: key <TAB> stage (vert / frag) <TAB> fields, space-separated (a key can have a row per stage):
#   a:<name>=<slot>:<type>                       a vertex input, [[attribute(slot)]] (--vk: its location)
#   u:<name>=<offset>:<type>:<count>:<stride>    a member of the default uniform block (count 0 = not an array)
#   r:<slot>:<count>                             the argument buffer's Metal buffer slot (-1 when absent), and number of argument IDs
#   b:<id>:<size>                                the default uniform block: its argument ID and size in bytes
#   n:<name>=<id>:<size>                         a named uniform block (DOOM 3's matrices_ubo): its argument ID and size in bytes
#   t:<name>=<texture id>:<sampler id>:<type>[:<unit>]  a combined sampler (-1 sampler ID if MSL omits it; optional initial unit from explicit layout(binding))
#   o:<name>=<location>                          a fragment output, [[color(location)]]
#   --vk's table (gl-vk/gl_shaders.tsv, which guest_gl.c reads) keeps the direct form of the Metal table before its ABI 2: b:<binding>:<size> for the default block,
#   t:<name>=<binding>:<type> for a combined sampler, no r: or n: rows (a named uniform block is refused there)
# <TAB> where it came from
# Usage: gl_shaders.py <input path>... <out dir> [--ios <dir>] [--fixed-function] [--vk]   (input paths: the RomFS, the main NSO, capture folders)
#        gl_shaders.py --capture-shader <vert|frag> <source file> <out dir>   (one Mac shader for the capture host)
import concurrent.futures as cf, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvgl
from dmsh_chain import SPIRV_CROSS, MAC_MIN_FLAG   # the in-repo spirv-cross build the NVN chain uses too (spirv_cross_bin)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); from ios_metal import IOS_FLAGS   # the iPhone's deployment target and math mode
import dmsh_spv   # the SPIR-V reader and passes of the NVN chain's Vulkan target (--vk)

VEC_TYPES = {'float', 'vec2', 'vec3', 'vec4', 'int', 'ivec2', 'ivec3', 'ivec4', 'uint', 'uvec2', 'uvec3', 'uvec4', 'bool', 'mat2', 'mat3', 'mat4'}
TABLE_ABI = '# GL shader ABI 2'   # direct-slot tables cannot be consumed by the argument-buffer host

def red(msg): sys.exit('[RED] ' + msg)

def text_of(b):
    b = bytes(b); z = b.find(b'\0')
    return b[:z] if z >= 0 else b

# A whole shader defines its entry point. DOOM 3's main holds two '#version 150' strings that are not shaders but the prefix its renderer puts before the GLSL it
# generates from its render programs at run time (helper functions, no main): those are left to the capture (dm/games/profile_guess.py GL_ENTRY, the same rule)
GL_ENTRY = re.compile(rb'\bvoid\s+main\s*\(')

def nso_strings(path):
    """'#version <number>' strings of an NSO's read-only segment (dm/tools/nso.py) that are whole shaders (GL_ENTRY); a main that links glslang (Balatro's LÖVE) also has its
    messages that start with '#version:'. nso.Error (a ValueError) on a damaged file"""
    import nso
    with open(path, 'rb') as f: m = nso.load(f.read(), path)
    (mo, sz) = m.segments[1]; img = bytes(m.image[mo:mo + sz])
    texts = (text_of(img[m.start():m.start() + 0x10000]) for m in re.finditer(rb'(?:^|(?<=\0))#version [0-9]', img))
    return [t for t in texts if GL_ENTRY.search(t)]


PAIR_STAGES = {'.vs': 'vert', '.fs': 'frag'}
GL_COMMENTS = re.compile(rb'/\*.*?\*/|//[^\r\n]*', re.DOTALL)
GL_MAIN_DEF = re.compile(rb'\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{')


def is_standalone_glsl(source):
    """True when source is text GLSL defining a main() entry point (not a header/snippet or binary)."""
    return bool(source) and not re.search(rb'[\x00-\x08\x0e-\x1f\x7f]', source) and bool(GL_MAIN_DEF.search(GL_COMMENTS.sub(b' ', source)))


def paired_sources(folder, names):
    """Find standalone vertex and fragment sources in one directory. The packer uses the same evidence before skipping capture."""
    candidates = {name: PAIR_STAGES[ext] for name in names if (ext := os.path.splitext(name)[1].lower()) in PAIR_STAGES}
    if len(set(candidates.values())) != 2: return {}   # LÖVE's .fs files, for example, are assembled at run time
    found = {}
    for name, stage in candidates.items():
        with open(os.path.join(folder, name), 'rb') as f: source = text_of(f.read())
        if is_standalone_glsl(source):
            found[name] = (source, stage)
    return found if len({stage for _, stage in found.values()}) == 2 else {}


def sources(roots):
    """(source bytes, stage or None, where) for every shader in the input paths"""
    for r in roots:
        if os.path.isfile(r):
            with open(r, 'rb') as f: head = f.read(4)
            if head == b'NSO0':
                for k, s in enumerate(nso_strings(r)): yield s, None, f'{os.path.basename(r)}#{k}'
            continue
        for d, _, fs in os.walk(r):
            paired = paired_sources(d, fs)
            for fn in sorted(fs):
                p = os.path.join(d, fn)
                low = fn.lower()
                if low.endswith('.mvgl'):
                    try: a = mvgl.Archive(p)
                    except ValueError: continue   # the sound archives are CRI AFS2 under the same extension
                    for nm in sorted(a.files):
                        if not nm.endswith('.shad'): continue
                        st = 'vert' if nm.endswith('_vp.shad') else 'frag' if nm.endswith('_fp.shad') else None
                        yield text_of(a.read(nm)), st, f'/{os.path.relpath(p, r)}:{nm}'
                elif low.endswith(('.vert', '.frag', '.glsl')):
                    with open(p, 'rb') as f: source = text_of(f.read())
                    ext = low.rsplit('.', 1)[1]
                    if ext == 'glsl' and not is_standalone_glsl(source): continue
                    yield source, None if ext == 'glsl' else ext, '/' + os.path.relpath(p, r)
                elif fn in paired:
                    source, stage = paired[fn]
                    yield source, stage, '/' + os.path.relpath(p, r)

# NVIDIA's GLSL compiler accepts more than the GLSL ES 3.20 specification, and glslang does not; these fix-ups run on the preprocessed text (glslang -E, so macros are expanded):
#   · the GLSL ES 1.0 texture functions (89 of Digimon's fragment shaders call textureCube; with -V glslang reads textureCube as Vulkan's texture type, so a macro cannot rename it);
#   · a redeclaration of gl_Position as a plain output (the built-in sprite shader of main declares `out float4 vPosition;` with vPosition defined as gl_Position);
#   · textureOffset with an offset that is not a constant (the built-in FXAA shader): Metal's sample offsets are constants too, so it becomes texture at P + offset / level 0 size;
#   · built-ins redeclared as plain inputs and outputs, which DOOM 3's renderer writes into every GLSL 1.50 shader it generates (`out vec4 gl_Position;`, `in vec4 gl_FragCoord;`,
#     `out float gl_FragDepth;`): the declarations go, the built-ins stay; `out vec4 gl_FragColor;` becomes a plain output of the shader's own (Vulkan GLSL has no gl_FragColor);
#   · the compatibility colour declared as a plain varying, `out vec4 gl_FrontColor;` in the vertex shader and `in vec4 gl_Color;` in the fragment shader (DOOM 3): NVIDIA links the two
#     as GL's fixed pipeline does, so both become one varying, dm_Color, which the host matches by name;
#   · `sampler` as a name (DOOM 3's helpers take `sampler2D sampler`): a reserved word in Vulkan GLSL, an ordinary name in OpenGL's;
#   · a function body closed with `};` (Mega Man Legacy Collection's shaders/nx/*.glsl end main that way): GLSL ES has no empty declaration at file scope, so the `;` goes.
#     A struct's or a block's `};` ends its declaration and stays: only a `{` opened right after a `)` at file scope is a function body
OLD_TEX = [(r'\btexture2DLod\s*\(', 'textureLod('), (r'\btextureCubeLod\s*\(', 'textureLod('), (r'\btexture3DLod\s*\(', 'textureLod('),
           (r'\btexture2DProj\s*\(', 'textureProj('), (r'\btexture3DProj\s*\(', 'textureProj('),
           (r'\btexture2D\s*\(', 'texture('), (r'\btexture3D\s*\(', 'texture('), (r'\btextureCube\s*\(', 'texture(')]

def call_args(t, i):
    """the arguments of the call whose '(' is at t[i], and the index after its ')'"""
    depth, args, start = 0, [], i + 1
    for k in range(i, len(t)):
        c = t[k]
        if c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: args.append(t[start:k]); return args, k + 1
        elif c == ',' and depth == 1: args.append(t[start:k]); start = k + 1
    raise ValueError('unbalanced parentheses')

BUILTIN_DECL = r'^[ \t]*(?:invariant\s+)?(?:in|out)\s+(?:highp\s+|mediump\s+|lowp\s+)?(?:vec4|float)\s+gl_(?:Position|FragCoord|FragDepth|PointSize)\s*;'

def own_name(t, decl, name, new, out=None):
    """t with a built-in the shader declares as a plain varying (decl matches the declaration) renamed to a variable of its own; out: the declaration to put in its place"""
    if not re.search(decl, t, flags=re.M): return t
    t = re.sub(decl, out or '', t, flags=re.M)
    return re.sub(rf'\b{name}\b', new, t)

def function_semicolons(t):
    """t without the `;` right after the `}` that closes a function body at file scope"""
    out, depth, body, i, k = [], 0, False, 0, 0
    while k < len(t):
        c = t[k]
        if c == '{':
            if depth == 0: body = t[:k].rstrip().endswith(')')
            depth += 1
        elif c == '}' and depth:
            depth -= 1
            if depth == 0 and body:
                m = re.match(r'\s*;', t[k + 1:])
                if m: out.append(t[i:k + 1]); i = k + 1 + m.end(); k = i; continue
        k += 1
    return ''.join(out) + t[i:]

def rename_identifier(t, name, base, renamed, keep_calls=False):
    """Rename an identifier into a free name, recording only replacements made in this translation."""
    pattern = r'\b' + re.escape(name) + r'\b' + (r'(?!\s*\()' if keep_calls else '')
    if not re.search(pattern, t): return t
    used = set(re.findall(r'\b\w+\b', t))
    new, i = base, 0
    while new in used: i += 1; new = f'{base}_{i}'
    renamed[new] = name
    return re.sub(pattern, new, t)

def fixups(t, stage, renamed=None):
    if renamed is None: renamed = {}
    t = function_semicolons(t)
    for a, b in OLD_TEX: t = re.sub(a, b, t)
    t = re.sub(BUILTIN_DECL, '', t, flags=re.M)
    t = rename_identifier(t, 'sampler', 'dm_sampler', renamed)
    if stage == 'vert': t = own_name(t, r'^[ \t]*out\s+vec4\s+gl_FrontColor\s*;', 'gl_FrontColor', 'dm_Color', 'out vec4 dm_Color;')
    else:
        t = own_name(t, r'^[ \t]*in\s+vec4\s+gl_Color\s*;', 'gl_Color', 'dm_Color', 'in vec4 dm_Color;')
        t = own_name(t, r'^[ \t]*out\s+vec4\s+gl_FragColor\s*;', 'gl_FragColor', 'dm_FragColor', 'out vec4 dm_FragColor;')
    out, i = [], 0
    for m in re.finditer(r'\btextureOffset\s*\(', t):
        if m.start() < i: continue
        args, end = call_args(t, m.end() - 1)
        if len(args) == 3 and re.search(r'[A-Za-z_]\w*', re.sub(r'\b[iu]?vec[234]\b', '', args[2])):   # an offset with a variable in it
            s_, p_, o_ = (x.strip() for x in args)
            out.append(t[i:m.start()] + f'texture({s_}, ({p_}) + vec2({o_}) / vec2(textureSize({s_}, 0)))'); i = end
    return ''.join(out) + t[i:]

def glsl_type(t, refl):
    return refl['types'][t]['name'] if t in refl.get('types', {}) else t

def active_default_uniforms(log):
    """Read the active names in glslang -q's default block. The SPIRV-Cross layout includes declared but inactive members."""
    _, uniform_header, data = log.partition('Uniform reflection:\n')
    uniforms, block_header, blocks = data.partition('\nUniform block reflection:\n')
    if not uniform_header or not block_header: return None
    block = re.search(r'^gl_DefaultUniformBlock: [^\n]*\bindex (\d+),', blocks.split('\n\n', 1)[0], re.M)
    if not block: return set()
    return {name for name, index in re.findall(r'^(\S+): [^\n]*\bindex (-?\d+),', uniforms, re.M) if index == block[1]}

def uni_members(refl, ms, prefix, base, f, bools, active):
    """Table rows for default block members. A struct's members recurse with their dotted name, which is the name the game passes to GL (u_LightEnv.m_dirLights_dir[0]); the
    reflection gives a nested member's offset relative to its parent, so the parent's offset is added"""
    for mb in ms:
        arr = mb.get('array', [])
        name = prefix + mb['name']
        if len(arr) > 1: return f'uniform {name}: arrays of arrays'
        if 'members' in refl['types'].get(mb['type'], {}):
            if arr: return f'uniform {name}: an array of structs is not implemented'
            e = uni_members(refl, refl['types'][mb['type']]['members'], name + '.', base + mb['offset'], f, bools, active)
            if e: return e
            continue
        if name not in active: continue
        ty = glsl_type(mb['type'], refl)
        if ty == 'uint' and mb['name'] in bools: ty = 'bool'
        if ty not in VEC_TYPES: return f'uniform {name}: type {ty} (structs are not implemented)'
        f.append(f'u:{name}={base + mb["offset"]}:{ty}:{arr[0] if arr else 0}:{mb.get("array_stride", 0)}')

# GLSL ES 1.00 (#version 100 or unversioned) and legacy desktop GLSL (#version 110 / 120 / 130): `attribute`, `varying`, `texture2D`/`texture3D`, `gl_FragColor`,
# precision statements and lowp / mediump / highp qualifiers. ES 1.00 promotes to `#version 310 es`; legacy desktop GLSL promotes to `#version 450 core`, preserving
# desktop implicit int-to-float conversions and sampler types without ES precision qualifiers (such as `sampler3D`). The texture functions stay for fixups() after preprocessing.
def promote_legacy(t, stage, renamed=None, desktop=False):
    if renamed is None: renamed = {}
    t = re.sub(r'^[ \t]*precision[ \t]+\w+[ \t]+\w+[ \t]*;\r?[ \t]*\r?\n', '', t, flags=re.M)
    t = re.sub(r'\b(?:lowp|mediump|highp)\b[ \t]*', '', t)
    t = re.sub(r'\battribute\b', 'in', t)
    t = re.sub(r'\bvarying\b', 'out' if stage == 'vert' else 'in', t)
    if stage == 'frag' and 'gl_FragColor' in t:
        t = 'out vec4 gm_FragColor;\n' + t.replace('gl_FragColor', 'gm_FragColor')
    t = rename_identifier(t, 'sample', 'gm_sample', renamed)   # `sample` became a reserved word after ES 1.00 (GameMaker's sprite shaders use it as a variable)
    # `texture` became a built-in function after ES 1.00, and a name declared `texture` redefines it (Fate/hollow ataraxia REMASTERED: `uniform sampler2D texture;`); a call stays
    t = rename_identifier(t, 'texture', 'dm_texture', renamed, keep_calls=True)
    header = '#version 450 core\n' if desktop else '#version 310 es\nprecision highp float;\nprecision highp int;\n'
    return header + t

def es100(t, stage, renamed=None):
    return promote_legacy(t, stage, renamed, desktop=False)

def legacy_desktop_io(t, stage):
    """Lower the compatibility varyings used by programmable GLSL 1.10/1.20/130 stages after expanding preprocessor macros."""
    direction = 'out' if stage == 'vert' else 'in'
    color = 'gl_FrontColor' if stage == 'vert' else 'gl_Color'
    secondary = 'gl_FrontSecondaryColor' if stage == 'vert' else 'gl_SecondaryColor'
    decl = []
    for old, name, ty in ((color, 'dm_LegacyColor', 'vec4'), (secondary, 'dm_LegacySecondaryColor', 'vec4'),
                          ('gl_FogFragCoord', 'dm_LegacyFog', 'float')):
        t, n = re.subn(rf'\b{old}\b', name, t)
        if n: decl.append(f'{direction} {ty} {name};')
    arrays = [('gl_TexCoord', 'dm_LegacyTexCoord', f'{direction} vec4')]
    if stage == 'frag': arrays.append(('gl_FragData', 'dm_LegacyFragData', 'out vec4'))
    for old, name, qualifier in arrays:
        pattern = rf'\b{old}\s*\[\s*(0|[1-9][0-9]*)\s*\]'
        for i in sorted(set(re.findall(pattern, t)), key=int):
            layout = f'layout(location = {i}) ' if old == 'gl_FragData' else ''
            decl.append(f'{layout}{qualifier} {name}{i};')
        t = re.sub(pattern, lambda m, prefix=name: prefix + m[1], t)
    if not decl: return t
    lines = t.splitlines()
    at = 1
    while at < len(lines) and (not lines[at].strip() or lines[at].lstrip().startswith(('#extension', '#line'))):
        at += 1
    return '\n'.join((*lines[:at], *decl, *lines[at:])) + ('\n' if t.endswith('\n') else '')

def explicit_sampler_bindings(text):
    """Initial texture units from explicit GLSL layout(... binding = N ...) sampler declarations in preprocessed text."""
    out = {}
    pat = (r'(?:\blayout\s*\(([^)]*)\)\s*uniform\b|\buniform\s+layout\s*\(([^)]*)\))'
           r'\s+(?:(?:lowp|mediump|highp)\s+)?[iu]?sampler\w+\s+([^;]+);')
    for m in re.finditer(pat, text):
        qual = m.group(1) if m.group(1) is not None else m.group(2)
        b = re.search(r'\bbinding\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\b', qual)
        if not b: continue
        unit = int(b.group(1), 0)
        for decl in m.group(3).split(','):
            nm = re.match(r'\s*([A-Za-z_]\w*)', decl)
            if nm: out[nm.group(1)] = unit
    return out

# The game's names that the rewrite renames (es100, fixups): the table rows give them back as the game wrote them, since the host finds attributes, uniforms and samplers by the
# game's name (glGetAttribLocation, glGetUniformLocation); the key is the hash of the game's own source
def game_names(f, renamed):
    """table fields with the renamed names in their name part (a:, o:, u:, t:, n: up to the '=') given back"""
    def back(x):
        k, sep, rest = x.partition('=')
        return re.sub(r'\b\w+\b', lambda m: renamed.get(m.group(0), m.group(0)), k) + sep + rest
    return [back(x) for x in f]

def argument_names(spv):
    """Give bound SPIR-V variables collision-free names, so MSL reflection can identify even an unnamed default block. Keep the original GL names in the table.
    Descriptor sets are Vulkan's namespace, not GL's: all resources go into one argument buffer, whose IDs spirv-cross allocates independently of the Binding decorations."""
    data = open(spv, 'rb').read(); words = list(struct.unpack(f'<{len(data) // 4}I', data))
    ins, names, bound, pointers, variables, i = [], {}, set(), {}, {}, 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if not n: raise ValueError('SPIR-V instruction length 0')
        w = words[i:i + n]; ins.append(w)
        if op == 5: names[w[1]] = struct.pack(f'<{n - 2}I', *w[2:]).split(b'\0')[0].decode()
        elif op == 71 and w[2] == 33: bound.add(w[1])   # OpDecorate Binding
        elif op == 32: pointers[w[1]] = w[3]   # OpTypePointer
        elif op == 59: variables[w[2]] = (w[1], w[3])   # OpVariable: pointer type, storage class
        i += n
    prefix = 'dm_gl_resource_'
    while any(prefix in s for s in names.values()): prefix += 'x'
    renamed = {v: prefix + str(v) for v in bound}
    new_names = []
    for v, name in sorted(renamed.items()):
        b = name.encode() + b'\0'; b += b'\0' * (-len(b) % 4)
        new_names += [((2 + len(b) // 4) << 16) | 5, v, *struct.unpack(f'<{len(b) // 4}I', b)]
    out, added = words[:5], False
    for w in ins:
        op = w[0] & 0xffff
        if not added and op in (5, 6, 71, 72): out += new_names; added = True
        if op == 5 and w[1] in renamed: continue
        if op == 71 and w[1] in renamed and w[2] == 34: w[3] = 0   # DescriptorSet
        out += w
    open(spv, 'wb').write(struct.pack(f'<{len(out)}I', *out))
    textures = {names.get(v, ''): name for v, name in renamed.items() if variables[v][1] == 0}   # UniformConstant
    buffers = {f'_{pointers[variables[v][0]]}': name for v, name in renamed.items() if variables[v][1] == 2}   # Uniform
    return textures, buffers

def spirv_of(src, stage, key, tmp):
    """GLSL → SPIR-V, the front end of both targets (translate: MSL, translate_vk: the Vulkan GL layer): legacy GLSL promoted, glslang -E, the fix-ups, glslang -V -R -q;
    returns (spv path, the preprocessed text, the fixed-up text, the renamed names, the active default-block uniforms) or an error string"""
    t = src.decode('latin1'); renamed = {}
    v = re.search(r'^[ \t]*#version[ \t]+(\d+)[^\n]*\n?', t, re.M)
    desktop_legacy = bool(v and v.group(1) in ('110', '120', '130'))
    if not v or v.group(1) == '100' or desktop_legacy:
        src = promote_legacy(t.replace(v.group(0), '', 1) if v else t, stage, renamed, desktop=desktop_legacy).encode('latin1')
    g = os.path.join(tmp, f'{key}.{stage}'); spv = g + '.spv'
    open(g + '.in', 'wb').write(src)
    r = subprocess.run(['glslangValidator', '-E', '-S', stage, g + '.in'], capture_output=True, text=True)
    if r.returncode: return 'glslang -E: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    pre = r.stdout[r.stdout.find('#version'):]   # warnings come first
    if desktop_legacy: pre = legacy_desktop_io(pre, stage)
    fixed = fixups(pre, stage, renamed)
    open(g, 'w').write(fixed)
    r = subprocess.run(['glslangValidator', '-V', '-R', '--aml', '--amb', '-q', '-S', stage, g, '-o', spv], capture_output=True, text=True)
    if r.returncode: return 'glslang: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l and 'compilation' not in l)[:400]
    active = active_default_uniforms(r.stdout)
    if active is None: return 'glslang: could not read active uniform reflection'
    return spv, pre, fixed, renamed, active

def translate(src, stage, key, tmp):
    """GLSL → MSL; returns (msl path, table fields) or an error string"""
    r = spirv_of(src, stage, key, tmp)
    if isinstance(r, str): return r
    (spv, pre, fixed, renamed, active), msl = r, os.path.join(tmp, f'{key}_{stage[0]}.metal')
    refl = json.loads(subprocess.run([SPIRV_CROSS, spv, '--reflect'], capture_output=True, text=True, check=True).stdout)
    for k in ('separate_images', 'separate_samplers', 'images', 'ssbos', 'push_constants'):
        if refl.get(k): return f'{k} are not implemented'
    for k in ('textures', 'ubos'):
        for r in refl.get(k, []):
            if r.get('array'): return f'{"sampler" if k == "textures" else "uniform block"} array {r["name"]}'
    textures, buffers = argument_names(spv)
    cmd = [SPIRV_CROSS, spv, '--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-argument-buffer-tier', '1',
           '--msl-texture-buffer-native', '--rename-entry-point', 'main', f'gl_{key}_{stage[0]}', stage, '--output', msl]
    if stage == 'vert': cmd.append('--fixup-clipspace')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: return 'spirv-cross: ' + r.stderr.strip()[:300]
    m = open(msl).read()
    # Varyings by name: the out struct of a vertex shader and the in struct of a fragment shader (stage_in members of a vertex shader are [[attribute(n)]])
    m = re.sub(r'(\b(\w+)\s+\[\[)user\(locn\d+\)', lambda x: f'{x.group(1)}user({renamed.get(x.group(2), x.group(2))})', m)
    m = re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', m)   # a NaN operand gives the other one, as on the Switch's GPU (vk_shaders.py PRECISE)
    open(msl, 'w').write(m)
    ids = {n: int(i) for n, i in re.findall(r'\b(\w+)\s+\[\[id\((\d+)\)\]\]', m)}
    f = [f'r:{0 if ids else -1}:{max(ids.values(), default=-1) + 1}']
    if stage == 'vert':
        for v in refl.get('inputs', []): f.append(f'a:{v["name"]}={v["location"]}:{v["type"]}')
    else:
        for v in refl.get('outputs', []): f.append(f'o:{v["name"]}={v["location"]}')
    # a bool uniform is a uint in the SPIR-V block (it has no bool members), but the game asks GL for its declared type: LÖVE's Shader:send takes only a boolean for a GLSL bool
    bools = {n for n in re.findall(r'\buniform\s+(?:(?:lowp|mediump|highp)\s+)?bool\s+(\w+)\s*;', pre)}
    for u in refl.get('ubos', []):
        aid = ids.get(buffers[u['type']])
        if aid is None: continue
        t = refl['types'][u['type']]
        if t['name'] != 'gl_DefaultUniformBlock':   # a named block: the buffer the game binds to its binding point (glUniformBlockBinding, glBindBufferRange)
            f.append(f'n:{t["name"]}={aid}:{u["block_size"]}'); continue
        f.append(f'b:{aid}:{u["block_size"]}')
        e = uni_members(refl, t['members'], '', 0, f, bools, active)
        if e: return e
    # the samplers the MSL takes: spirv-cross leaves out one the shader never reads, but its reflection still lists it, and glslang gives it the binding of a used one (Mega Man Legacy
    # Collection's default_ps.glsl declares T1 for code under #if 0: T0 and T1 both at 0). Listed, the host bound the unit T1 names, an empty one, over T0's texture: every sprite sampled black
    bindings = explicit_sampler_bindings(fixed)
    for t in refl.get('textures', []):
        name = textures[t['name']]
        if name not in ids: continue
        unit = bindings.get(t['name'], 0)
        suffix = f':{unit}' if unit else ''
        f.append(f't:{t["name"]}={ids[name]}:{ids.get(name + "Smplr", -1)}:{t["type"]}{suffix}')
    return msl, game_names(f, renamed)

def table_fields_vk(refl, pre, stage, active):
    """--vk: the table's fields of a shader from its reflection (reflect below, in spirv-cross --reflect's form), in the direct form guest_gl.c reads (b: and t: by
    binding); the default block's members as the Metal table has them (uni_members: active ones only, a struct's members by their dotted name); a list, or an error string"""
    f = []
    if stage == 'vert':
        for v in refl.get('inputs', []): f.append(f'a:{v["name"]}={v["location"]}:{v["type"]}')
    else:
        for v in refl.get('outputs', []): f.append(f'o:{v["name"]}={v["location"]}')
    # a bool uniform is a uint in the SPIR-V block (it has no bool members), but the game asks GL for its declared type: LÖVE's Shader:send takes only a boolean for a GLSL bool
    bools = {n for n in re.findall(r'\buniform\s+(?:(?:lowp|mediump|highp)\s+)?bool\s+(\w+)\s*;', pre)}
    for u in refl.get('ubos', []):
        t = refl['types'][u['type']]
        if t['name'] != 'gl_DefaultUniformBlock': return f'uniform block {t["name"]} is not the default block'
        f.append(f'b:{u["binding"]}:{u["block_size"]}')
        e = uni_members(refl, t['members'], '', 0, f, bools, active)
        if e: return e
    for t in refl.get('textures', []):
        if t.get('array'): return f'sampler array {t["name"]}'
        f.append(f't:{t["name"]}={t["binding"]}:{t["type"]}')
    for k in ('separate_images', 'separate_samplers', 'images', 'ssbos', 'push_constants'):
        if refl.get(k): return f'{k} are not implemented'
    return f

# ---------- --vk: the reflection read from the module, and the passes ----------
T_MATRIX, D_BLOCK, D_ARRAY_STRIDE, D_MATRIX_STRIDE, D_ROW_MAJOR, D_OFFSET, SC_PUSH_CONSTANT = 24, 2, 6, 7, 4, 35, 9
OP_RETURN, OP_COMPOSITE_INSERT, OP_FADD, OP_FMUL = 253, 82, 129, 133
DIM_NAMES = {0: '1D', 1: '2D', 2: '3D', 3: 'Cube', 4: '2DRect', 5: 'Buffer', 6: 'SubpassInput'}
ARRAYS = (dmsh_spv.T_ARRAY, dmsh_spv.T_RUNTIME_ARRAY)

def reflect(m):
    """What spirv-cross --reflect gives table_fields, read from module m (dmsh_spv.Module): the entry point's inputs and outputs (built-ins left out; by location), the uniform
    blocks with their struct types (members' offsets, arrays and strides; the block's declared size as spirv-cross computes it: the member at the highest offset, plus its
    size), the combined image samplers, and the resource kinds the table refuses. Types by GLSL name; a struct as '_<id>', its name in 'types', as spirv-cross writes them"""
    d, names, dec, mdec = m.defs(), m.names(), m.decorations(), m.member_decorations()
    mnames = {(i[1], i[2]): dmsh_spv.read_string(i[3:])[0] for i in m.of(dmsh_spv.MEMBER_NAME)}
    iface = set(m.entry_points()[0][3])
    consts = {i[2]: i[3] for i in m.of(dmsh_spv.CONSTANT) if len(i) > 3}
    out = {'types': {}}

    def op(t): return d[t][0] & 0xffff if t in d else None
    def scalar(t):
        i, o = d[t], op(t)
        if o == dmsh_spv.T_FLOAT: return {32: 'float', 64: 'double'}.get(i[2], 'float16_t')
        if o == dmsh_spv.T_INT: return ('int' if i[3] else 'uint') if i[2] == 32 else f'{"" if i[3] else "u"}int{i[2]}_t'
        return 'bool' if o == dmsh_spv.T_BOOL else f'_{t}'
    def name_of(t):
        i, o = d[t], op(t)
        if o in ARRAYS: return name_of(i[2])
        if o == dmsh_spv.T_VECTOR:
            s = scalar(i[2])
            return {'float': '', 'double': 'd', 'int': 'i', 'uint': 'u', 'bool': 'b'}.get(s, s) + f'vec{i[3]}'
        if o == T_MATRIX:
            col = d[i[2]]; rows, cols = col[3], i[3]; p = 'd' if scalar(col[2]) == 'double' else ''
            return f'{p}mat{cols}' if cols == rows else f'{p}mat{cols}x{rows}'
        if o == dmsh_spv.T_SAMPLED_IMAGE:
            img = d[i[2]]   # OpTypeImage: result, sampled type, dim, depth, arrayed, multisampled, sampled, format
            return ({'int': 'i', 'uint': 'u'}.get(scalar(img[2]), '') + 'sampler' + DIM_NAMES.get(img[3], str(img[3])) + ('MS' if img[6] else '') + ('Array' if img[5] else '')
                    + ('Shadow' if img[4] == 1 else ''))
        if o == dmsh_spv.T_STRUCT: struct_type(t); return f'_{t}'
        return scalar(t)
    def dims_of(t):
        n = []
        while op(t) in ARRAYS: n.append(consts.get(d[t][3], 0) if op(t) == dmsh_spv.T_ARRAY else 0); t = d[t][2]
        return n
    def member_size(t, md):   # spirv-cross get_declared_struct_member_size
        o = op(t)
        if o in ARRAYS: return dec.get(t, {}).get(D_ARRAY_STRIDE, [0])[0] * (consts.get(d[t][3], 0) if o == dmsh_spv.T_ARRAY else 0)
        if o == dmsh_spv.T_STRUCT: return struct_size(t)
        if o == T_MATRIX: return (d[d[t][2]][3] if D_ROW_MAJOR in md else d[t][3]) * md.get(D_MATRIX_STRIDE, [0])[0]
        _, n, width = m.type_shape(t, d)
        return n * width // 8
    def struct_size(t):
        best, size = -1, 0
        for k, mt in enumerate(d[t][2:]):
            md = mdec.get((t, k), {}); off = md.get(D_OFFSET, [0])[0]
            if off > best: best, size = off, member_size(mt, md)
        return max(best, 0) + size
    def struct_type(t):
        key = f'_{t}'
        if key in out['types']: return
        out['types'][key] = e = {'name': names.get(t, key), 'members': []}
        for k, mt in enumerate(d[t][2:]):
            md = mdec.get((t, k), {}); mb = {'name': mnames.get((t, k), f'_m{k}'), 'type': name_of(mt), 'offset': md.get(D_OFFSET, [0])[0]}
            if dims_of(mt): mb['array'] = dims_of(mt); mb['array_stride'] = dec.get(mt, {}).get(D_ARRAY_STRIDE, [0])[0]
            if D_MATRIX_STRIDE in md: mb['matrix_stride'] = md[D_MATRIX_STRIDE][0]
            e['members'].append(mb)
    def builtin(var, t):
        if dmsh_spv.D_BUILTIN in dec.get(var, {}): return True
        while op(t) in ARRAYS: t = d[t][2]
        return op(t) == dmsh_spv.T_STRUCT and any(dmsh_spv.D_BUILTIN in mdec.get((t, k), {}) for k in range(len(d[t]) - 2))
    def entry(var, t):
        e = {'type': name_of(t), 'name': names.get(var, f'_{var}')}
        if dims_of(t): e['array'] = dims_of(t)
        for key, dk in (('location', dmsh_spv.D_LOCATION), ('set', dmsh_spv.D_DESCRIPTOR_SET), ('binding', dmsh_spv.D_BINDING)):
            if dk in dec.get(var, {}): e[key] = dec[var][dk][0]
        return e
    for var, ptr, sc in m.variables():
        t = base = d[ptr][3]
        while op(base) in ARRAYS: base = d[base][2]
        bo = op(base)
        if sc in (dmsh_spv.SC_INPUT, dmsh_spv.SC_OUTPUT):
            if var in iface and not builtin(var, t): out.setdefault('inputs' if sc == dmsh_spv.SC_INPUT else 'outputs', []).append(entry(var, t))
        elif sc == dmsh_spv.SC_UNIFORM and bo == dmsh_spv.T_STRUCT and D_BLOCK in dec.get(base, {}):
            e = entry(var, t); e['type'] = name_of(base); e['block_size'] = struct_size(base); out.setdefault('ubos', []).append(e)
        elif sc in (dmsh_spv.SC_UNIFORM, dmsh_spv.SC_STORAGE_BUFFER): out.setdefault('ssbos', []).append(entry(var, t))   # a BufferBlock, or a storage buffer
        elif sc == SC_PUSH_CONSTANT: out.setdefault('push_constants', []).append(entry(var, t))
        elif sc == dmsh_spv.SC_UNIFORM_CONSTANT:
            kind = ('textures' if bo == dmsh_spv.T_SAMPLED_IMAGE else 'separate_samplers' if bo == dmsh_spv.T_SAMPLER else
                    'images' if bo == dmsh_spv.T_IMAGE and d[base][7] == 2 else 'separate_images' if bo == dmsh_spv.T_IMAGE else None)
            if kind: out.setdefault(kind, []).append(entry(var, t))
    for k in ('inputs', 'outputs'):
        if k in out: out[k].sort(key=lambda e: (e.get('location', 1 << 30), e['name']))
    return out

def set_descriptor_set(m, s):
    """Every resource of module m in descriptor set s (the fragment stage's: 1); → the decorations written"""
    n, dec = 0, m.decorations()
    for i in m.ins:
        if i[0] & 0xffff == dmsh_spv.DECORATE and i[2] == dmsh_spv.D_DESCRIPTOR_SET: i[3] = s; n += 1
    for var, _, sc in m.variables():
        if sc in (dmsh_spv.SC_UNIFORM, dmsh_spv.SC_UNIFORM_CONSTANT, dmsh_spv.SC_STORAGE_BUFFER) and dmsh_spv.D_DESCRIPTOR_SET not in dec.get(var, {}):
            m.decorate(var, dmsh_spv.D_DESCRIPTOR_SET, s); n += 1
    return n

def clip_z(m):
    """GL's clip z (−w to w) → Vulkan's (0 to w): position.z = (position.z + position.w) * 0.5 before each return of the vertex entry point, the formula spirv-cross's
    --fixup-clipspace writes into the MSL; → the returns changed (0 when the module writes no position)"""
    eps = [e for e in m.entry_points() if e[0] == 'vs']
    if not eps: return 0
    fn, iface = eps[0][2], set(eps[0][3])
    d, dec, mdec = m.defs(), m.decorations(), m.member_decorations()
    target = None   # (the output variable, the position's member index in it or None)
    for var, ptr, _ in m.variables(dmsh_spv.SC_OUTPUT):
        if var not in iface: continue
        t = d[ptr][3]
        if dec.get(var, {}).get(dmsh_spv.D_BUILTIN, [None])[0] == dmsh_spv.BUILTIN_POSITION: target = (var, None); break
        if d[t][0] & 0xffff == dmsh_spv.T_STRUCT:
            k = next((k for k in range(len(d[t]) - 2) if mdec.get((t, k), {}).get(dmsh_spv.D_BUILTIN, [None])[0] == dmsh_spv.BUILTIN_POSITION), None)
            if k is not None: target = (var, k); break
    if not target: return 0
    f32 = m.float_type(); v4 = m.vector_type(f32, 4); pv4 = m.pointer_type(dmsh_spv.SC_OUTPUT, v4)
    half = m.constant(f32, struct.unpack('<I', struct.pack('<f', 0.5))[0])
    idx = m.constant(m.int_type(True), target[1]) if target[1] is not None else None
    n, k, inside = 0, 0, False
    while k < len(m.ins):
        i = m.ins[k]; o = i[0] & 0xffff
        if o == dmsh_spv.FUNCTION: inside = i[2] == fn
        elif o == dmsh_spv.FUNCTION_END: inside = False
        elif o == OP_RETURN and inside:
            code, ptr = [], target[0]
            if idx is not None: ptr = m.new_id(); code.append([5 << 16 | dmsh_spv.ACCESS_CHAIN, pv4, ptr, target[0], idx])
            v, z, w, s, h, nv = (m.new_id() for _ in range(6))
            code += [[4 << 16 | dmsh_spv.LOAD, v4, v, ptr], [5 << 16 | dmsh_spv.COMPOSITE_EXTRACT, f32, z, v, 2], [5 << 16 | dmsh_spv.COMPOSITE_EXTRACT, f32, w, v, 3],
                     [5 << 16 | OP_FADD, f32, s, z, w], [5 << 16 | OP_FMUL, f32, h, s, half], [6 << 16 | OP_COMPOSITE_INSERT, v4, nv, h, v, 2], [3 << 16 | dmsh_spv.STORE, ptr, nv]]
            m.ins[k:k] = code; k += len(code); n += 1
        k += 1
    return n

def translate_vk(src, stage, key, tmp):
    """GLSL → SPIR-V for the Vulkan backend's GL layer (--vk); returns (spv path, table fields) or an error string"""
    r = spirv_of(src, stage, key, tmp)
    if isinstance(r, str): return r
    spv, pre, _, renamed, active = r
    try: m = dmsh_spv.Module.read(spv)
    except ValueError as e: return f'glslang output: {e}'
    f = table_fields_vk(reflect(m), pre, stage, active)
    if isinstance(f, str): return f
    dmsh_spv.precise(m)
    if stage == 'vert': clip_z(m)
    else: set_descriptor_set(m, 1)
    out = os.path.join(tmp, f'gl_{key}_{stage[0]}.spv'); m.write(out)
    val = shutil.which('spirv-val')
    if val:
        r = subprocess.run([val, '--target-env', 'vulkan1.0', out], capture_output=True, text=True)
        if r.returncode: return 'spirv-val: ' + ' | '.join((r.stdout + r.stderr).split('\n'))[:400]
    return out, game_names(f, renamed)

def one(item, tmp, tr=translate):
    """[(key, stage, fields, msl or spv, where)], or [(key, None, errors, None, where)] when no stage translates"""
    src, stage, where = item
    key = hashlib.sha256(src).hexdigest()[:16]
    out, errs = [], []
    for st in [stage] if stage else ['vert', 'frag']:
        r = tr(src, st, key, tmp)
        if isinstance(r, tuple): out.append((key, st, r[1], r[0], where))
        else: errs.append(f'{st}: {r}')
    return out or [(key, None, errs, None, where)]

def metallib(msls, out, sdk, extra):
    os.makedirs(out, exist_ok=True)
    def comp(m):
        a = m[:-6] + f'.{sdk}.air'
        # -fwrapv: signed integer overflow wraps, as GLSL's (and SPIR-V's) integer arithmetic does; in MSL it is undefined and the compiler folds around it (dmsh_chain.py MAC_METAL)
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', '-fwrapv', *extra, m, '-o', a], capture_output=True, text=True)
        if r.returncode: red(f'{m}: metal compile failed: {r.stderr.strip()[:400]}')
        return a
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: airs = list(ex.map(comp, msls))
    lib = os.path.join(out, 'gl_shaders.metallib')
    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', *airs, '-o', lib], check=True)
    return lib

def capture_shader(stage, path, out):
    if stage not in ('vert', 'frag'):
        red(f'invalid capture stage: {stage}')
    with open(path, 'rb') as f:
        src = text_of(f.read())
    key = hashlib.sha256(src).hexdigest()[:16]
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        result = translate(src, stage, key, tmp)
        if isinstance(result, str):
            red(f'{key}.{stage}: {result}')
        msl, fields = result
        metallib([msl], out, 'macosx', [MAC_MIN_FLAG])
        with open(os.path.join(out, 'gl_shaders.tsv'), 'w') as f:
            f.write(TABLE_ABI + '\n' + '\t'.join((key, stage, ' '.join(fields))) + '\n')

def main():
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == '--capture-shader':
        return capture_shader(*a[1:])
    ios = None
    if '--ios' in a: i = a.index('--ios'); ios = a[i + 1]; del a[i:i + 2]
    fixed = '--fixed-function' in a
    if fixed: a.remove('--fixed-function')
    vk = '--vk' in a
    if vk: a.remove('--vk')
    if vk and ios: sys.exit('gl_shaders.py: --vk and --ios make one library each: run it twice')
    if vk and fixed: sys.exit('gl_shaders.py: --fixed-function is the Metal host\'s fixed-function shaders; the Vulkan GL layer has no --vk form of it')
    if len(a) < 2: sys.exit('usage: gl_shaders.py <input path>... <out dir> [--ios <dir>] [--fixed-function] [--vk]')
    roots, out = a[:-1], a[-1]
    items, seen_src = [], set()
    for s, st, where in sources(roots):
        if (s, st) in seen_src: continue
        seen_src.add((s, st)); items.append((s, st, where))
    if fixed and items: red('GLSL sources found in a fixed-function build')
    if not fixed and not items: red(f'no GLSL shaders in {" ".join(roots)}')
    with tempfile.TemporaryDirectory() as tmp:
        tr = translate_vk if vk else translate
        with cf.ThreadPoolExecutor(os.cpu_count()) as ex: rows = [r for rs in ex.map(lambda it: one(it, tmp, tr), items) for r in rs]
        seen, failed = {}, []
        for key, stage, f, msl, where in rows:
            if not stage: failed.append((key, where, f)); continue
            if (key, stage) not in seen: seen[(key, stage)] = (stage, f, msl, where)
        for key, where, errs in failed:
            if not any(k == key for k, _ in seen): print(f'[RED] {where} ({key}) does not translate: {" / ".join(errs)}')
        if not fixed and not seen: red('no GLSL shaders translated')
        if vk: return write_vk(seen, failed, out)
        tsv = [TABLE_ABI, '# GL shaders (dm/gfx/tools/gl_shaders.py): key = sha256(GLSL source)[:16], Metal function gl_<key>_v / gl_<key>_f; a: vertex inputs, u: default uniform block members '
               '(offset:type:count:stride), r: argument buffer (slot:count), b: the block (id:size), n: named blocks (name=id:size), t: combined samplers (texture-id:sampler-id:type), o: fragment outputs']
        if fixed:
            tsv.append('# Fixed-function OpenGL uses the host Metal shaders')
        else:
            spirv = subprocess.run([SPIRV_CROSS, '--revision'], capture_output=True, text=True)
            tsv.append('# ' + ' '.join(subprocess.run(['glslangValidator', '--version'], capture_output=True, text=True).stdout.split()[:3]) + '; spirv-cross '
                       + ' '.join((spirv.stdout + spirv.stderr).split()))
        tsv += [f'{k[0]}\t{v[0]}\t{" ".join(v[1])}\t{v[3]}' for k, v in sorted(seen.items())]
        dirs = [(os.path.join(out, 'gl'), 'macosx', [MAC_MIN_FLAG])]   # the Mac host's minimum macOS, as the NVN chain's libraries (dmsh_chain.py MAC_MIN)
        if ios: dirs.append((os.path.join(ios, 'gl'), 'iphoneos', list(IOS_FLAGS)))   # relaxed as in vk_shaders.py and ios_metal.py: no NaN / Inf assumption on the iPhone's GPU compiler
        msls = [v[2] for v in seen.values()]
        if fixed:
            placeholder = os.path.join(tmp, 'empty.metal')
            with open(placeholder, 'w') as f: f.write('#include <metal_stdlib>\n')
            msls.append(placeholder)
        for d, sdk, extra in dirs:
            lib = metallib(msls, d, sdk, extra)
            open(os.path.join(d, 'gl_shaders.tsv'), 'w').write('\n'.join(tsv) + '\n')
            print(f'{sdk}: {len(seen)} shaders ({sum(1 for v in seen.values() if v[0] == "vert")} vertex) → {lib}' + (f'; {len(failed)} failed' if failed else ''))

def write_vk(seen, failed, out):
    """--vk: <out>/gl-vk/gl_<key>_v.spv / gl_<key>_f.spv and gl_shaders.tsv, rows as in the Metal library's table (the host's lookup is the same: the key and the stage)"""
    d = os.path.join(out, 'gl-vk'); os.makedirs(d, exist_ok=True)
    for (key, stage), v in seen.items():
        with open(v[2], 'rb') as f: data = f.read()
        dst = os.path.join(d, f'gl_{key}_{stage[0]}.spv'); tmp = f'{dst}.{os.getpid()}.tmp'
        with open(tmp, 'wb') as f: f.write(data)
        os.replace(tmp, dst)   # whole files only
    tsv = ['# GL shaders for Vulkan (dm/gfx/tools/gl_shaders.py --vk): key = sha256(GLSL source)[:16], module gl_<key>_v.spv / gl_<key>_f.spv (entry point main; the vertex '
           "stage's resources in descriptor set 0, the fragment stage's in set 1); a: vertex inputs (location:type), u: default uniform block members (offset:type:count:stride), "
           'b: the block (binding:size), t: combined samplers (binding:type), o: fragment outputs',
           '# ' + ' '.join(subprocess.run(['glslangValidator', '--version'], capture_output=True, text=True).stdout.split()[:3]) + '; no spirv-cross (reflected from the module)']
    tsv += [f'{k[0]}\t{v[0]}\t{" ".join(v[1])}\t{v[3]}' for k, v in sorted(seen.items())]
    with open(os.path.join(d, 'gl_shaders.tsv'), 'w') as f: f.write('\n'.join(tsv) + '\n')
    print(f'vk: {len(seen)} shaders ({sum(1 for v in seen.values() if v[0] == "vert")} vertex) → {d}' + (f'; {len(failed)} failed' if failed else ''))

if __name__ == '__main__':
    import nso
    try: main()
    except nso.Error as e: red(str(e))   # a damaged main
