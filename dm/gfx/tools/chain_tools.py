#!/usr/bin/env python3
# tool_lock.py's tool fingerprints on any OS (Windows, Linux, the Mac): tools_now() and check_tools() run tool_lock.py's own functions, with one difference: a tool that cannot be
# started here (xcrun and the Metal compilers off a Mac, the translator's macOS executable on Windows and Linux) reads as 'missing' instead of raising. Where every tool starts, as on a
# Mac, the values are tool_lock.py's byte for byte (the same code runs the same commands). The fingerprints moved from dmsh_chain.py to tool_lock.py upstream, outside the chain
# version's FILES as this file is (docs/design/vulkan-backend.md, step M0). There is no --update-lock here: off a Mac it would record the Metal compilers as missing.
# Usage: chain_tools.py --check-tools   as tool_lock.py --check-tools: [RED] lines for every tool that differs from dmsh_chain.lock (a missing one too), exit 1 when any does
import contextlib, os, subprocess, sys, types
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); import tool_lock

def _run(cmd, **kw):   # subprocess.run, except that a program that cannot be started prints 'missing' (tool_lock.py passes text=True and reads stdout only)
    try: return subprocess.run(cmd, **kw)
    except OSError as e: return subprocess.CompletedProcess(cmd, 127, 'missing', str(e))

@contextlib.contextmanager
def _portable():   # tool_lock.py runs its tools through its module's 'subprocess' name (tools_now, xlate_embedded)
    real = tool_lock.subprocess
    tool_lock.subprocess = types.SimpleNamespace(run=_run, SubprocessError=subprocess.SubprocessError, TimeoutExpired=subprocess.TimeoutExpired)
    try: yield
    finally: tool_lock.subprocess = real

def tools_now():
    with _portable(): return tool_lock.tools_now()

def check_tools():
    with _portable(): return tool_lock.check_tools()

if __name__ == '__main__':
    if sys.argv[1:] != ['--check-tools']: sys.exit('usage: chain_tools.py --check-tools')
    bad = check_tools()
    for b in bad: print('[RED] translation chain tools do not match the lock file: ' + b)
    sys.exit(1 if bad else 0)
