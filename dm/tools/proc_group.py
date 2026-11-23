"""A subprocess in a process group of its own, which the caller owns with every process it starts (the NSP script's run, regress.py's run_heavy).

The caller's interrupt does not reach that group by itself: Spyder's interrupt goes to its kernel alone, a front end's SIGTERM to the script alone, and a terminal's Ctrl-C to
the terminal's foreground group, which this group is not in (a session of its own: no signal from the terminal reaches it, and a member that reads the terminal fails at once
instead of stopping on SIGTTIN). So the group is stopped here:

- on an exception (an interrupt, a SIGTERM the caller turned into one, an error such as a log that cannot be written): stop() sends the group the signal that stopped the caller
  (SIGINT for an interrupt, else SIGTERM), after a SIGCONT for a group a pause left stopped, gives it `grace` seconds, then SIGKILLs what is left and reaps the direct child;
  an interrupt during that wait only shortens it to the SIGKILL;
- after the direct child's normal exit: finish() gives the members left (a descendant still writing, or holding the output pipe) `drain` seconds, then stops them the same
  way and lists them in `stragglers`, which the caller reports as a failure of its stage: their outputs may be incomplete.

A process that leaves the group itself (setsid, setpgid) is out of reach of group supervision: it is not listed, signalled or waited for (only a holder of the output is
seen, by lines(), and fails the step unnamed).

The members: on macOS from libproc, on Linux from /proc (each /proc/<pid>/stat's process group and state, the name its comm), zombies left out on both: a member that has
exited is no straggler (it writes nothing), and a packer that is PID 1 in a container reaps none of the orphans it adopts. Another POSIX system answers only whether the group
has a member, unnamed ('?'), zombies included.

On Windows (the port's packs) Group is JobGroup: the same interface and meaning on a Job Object, which holds the direct child and every process it starts (a process of a job
cannot leave it: the job allows no breakaway), and on a console process group of its own (CREATE_NEW_PROCESS_GROUP), which takes no Ctrl+C from the console, as the POSIX
group takes no signal from the terminal. What Windows does differently:
- the stopping signal, SIGINT or SIGTERM alike, is CTRL_BREAK_EVENT to that console group (Windows delivers no CTRL_C_EVENT to a group, and no SIGTERM from another process):
  a process without a handler for it ends at once (Python's default too), as on SIGTERM. A caller without a console (pythonw) cannot send it: the job is ended at once then;
- SIGKILL is TerminateJobObject; the job is also closed with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so a caller that dies (Task Manager, the launcher's Force stop) takes the
  group with it, which a SIGKILLed caller on POSIX cannot;
- pause() is not available (Windows has no SIGSTOP for a job; regress.py's run_heavy, its one caller, runs on the Mac); resume() has nothing to resume;
- lines() reads the pipe on a thread of its own: Windows' selectors watch sockets only."""
import codecs, ctypes, io, locale, os, selectors, signal, subprocess, sys, time

GRACE = 5.0   # seconds the group has to end after the stopping signal, before SIGKILL
DRAIN = 5.0   # seconds members may outlive the direct child's normal exit
REAP = 2.0    # seconds for the killed members to go (launchd reaps the orphans among them)
TICK = 0.05   # seconds between looks at the group while waiting for it to end

_libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True) if sys.platform == 'darwin' else None
if _libproc is not None:
    _libproc.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    _libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]


def members(pgid):
    """[(pid, name)] of the live processes in process group pgid; zombies are not members (they write nothing; the caller or launchd reaps them)"""
    if _libproc is None and sys.platform.startswith('linux'): return _proc_members(pgid)
    if _libproc is None:   # another system: only whether the group has a member, no names
        try: os.killpg(pgid, 0); return [(pgid, '?')]
        except ProcessLookupError: return []
        except PermissionError: return [(pgid, '?')]
    buf = (ctypes.c_int * 1024)()
    n = _libproc.proc_listpids(2, pgid, buf, ctypes.sizeof(buf)) // ctypes.sizeof(ctypes.c_int)   # PROC_PGRP_ONLY
    out, info = [], ctypes.create_string_buffer(136)   # struct proc_bsdinfo
    for pid in buf[:max(n, 0)]:
        if pid <= 0: continue
        if _libproc.proc_pidinfo(pid, 3, 0, info, 136) != 136: continue   # PROC_PIDTBSDINFO; fails for a zombie or a process gone meanwhile
        status, pg = int.from_bytes(info.raw[4:8], 'little'), int.from_bytes(info.raw[100:104], 'little')
        if status == 5 or pg != pgid: continue   # SZOMB; a pid reused outside the group
        out.append((pid, info.raw[64:96].split(b'\0')[0].decode(errors='replace') or info.raw[48:64].split(b'\0')[0].decode(errors='replace')))
    return out


def _proc_members(pgid):   # members on Linux: /proc/<pid>/stat is "<pid> (<comm>) <state> <ppid> <pgrp> ...", comm itself may hold spaces and parentheses
    out = []
    try: pids = [int(n) for n in os.listdir('/proc') if n.isdigit()]
    except OSError: return out
    for pid in pids:
        try:
            with open(f'/proc/{pid}/stat', 'rb') as f: stat = f.read()
        except OSError: continue   # gone meanwhile
        head, _, rest = stat.rpartition(b')')
        fields = rest.split()
        if len(fields) < 3 or fields[0] in (b'Z', b'X') or int(fields[2]) != pgid: continue   # a zombie or a dead process; another group
        out.append((pid, head.partition(b'(')[2].decode(errors='replace')))
    return out


class Group:
    """The direct child (p, a subprocess.Popen started with these Popen arguments; for lines(), stdout=PIPE in binary mode) leads a new session and process group.
    `with Group(...) as g:` stops the group when the block ends by an exception, and finishes it (finish) when it ends otherwise.
    stragglers: [(pid, name)] that outlived the direct child's normal exit and were stopped (pid None: a holder of the output outside the group), for the caller to report"""

    def __init__(self, cmd, grace=None, drain=None, **popen):   # grace, drain: GRACE and DRAIN when None
        self.grace, self.drain = GRACE if grace is None else grace, DRAIN if drain is None else drain
        self.paused, self.stragglers, self.done = False, [], False
        self.p = subprocess.Popen([str(c) for c in cmd], start_new_session=True, **popen)
        self.pgid = self.p.pid

    def __enter__(self): return self

    def __exit__(self, kind, exc, tb):
        try:
            if kind is not None: self.stop(signal.SIGINT if issubclass(kind, KeyboardInterrupt) else signal.SIGTERM)
            elif not self.done: self.finish()   # a caller that did not wait: the same end, its stragglers in the list
        finally:
            if self.p.stdout: self.p.stdout.close()   # after the stop: a member is not cut off by SIGPIPE before its signal
        return False

    def _signal(self, sig):
        if self.p.returncode is not None and not members(self.pgid): return   # its leader reaped and its members gone: the number may be another group's now
        try: os.killpg(self.pgid, sig)
        except (ProcessLookupError, PermissionError): pass   # every member gone, or only zombies left (macOS answers EPERM for those)

    def pause(self): self._signal(signal.SIGSTOP); self.paused = True

    def resume(self): self._signal(signal.SIGCONT); self.paused = False

    def _gone(self, secs):   # waits up to secs for the group to end (reaping the direct child on the way); → whether it did
        end = time.monotonic() + secs
        while True:
            self.p.poll()
            if not members(self.pgid): return True
            if time.monotonic() >= end: return False
            time.sleep(TICK)

    def stop(self, sig=signal.SIGTERM):
        """sig to the group (resumed first: a stopped process acts on no signal but SIGKILL), up to grace seconds, then SIGKILL; returns once the direct child is reaped and
        no member is left, or REAP seconds after the SIGKILL (a member stuck in the kernel). Re-raises the last interrupt (or SIGTERM, exit) that came while it waited"""
        late, self.done = None, True
        try:
            self.resume(); self._signal(sig)
            if self._gone(self.grace): self.p.wait(); return
        except BaseException as e: late = e   # a second interrupt or SIGTERM: no more grace
        while True:   # each step can be repeated; an interrupt here only repeats it
            try:
                self._signal(signal.SIGKILL); self.p.wait(); self._gone(REAP); break
            except BaseException as e: late = e
        if late is not None: raise late

    def finish(self):
        """After the direct child's exit (waits for it): drain seconds for the other members to go, then stop() those left, listed in stragglers. → the child's exit status"""
        self.p.wait(); self.done = True
        if not self._gone(self.drain):
            self.stragglers += members(self.pgid); self.stop()
        return self.p.returncode

    def lines(self):
        """The output of a group started with stdout=PIPE, a line at a time as text (universal newlines, undecodable bytes replaced: as text=True reads it). Ends at the end of
        the output, or drain seconds after the direct child's exit while a member still holds the pipe: those members are stopped then (stragglers) and the end follows.
        The direct child's exit is seen whether or not output comes; finish() then waits for the rest of the group"""
        out = self.p.stdout
        assert out is not None, 'lines() reads a group started with stdout=PIPE'
        fd = out.fileno()
        dec = io.IncrementalNewlineDecoder(codecs.getincrementaldecoder(locale.getpreferredencoding(False))(errors='replace'), translate=True)
        sel, buf, end, stopped = selectors.DefaultSelector(), '', None, False
        sel.register(fd, selectors.EVENT_READ)
        try:
            while True:
                if sel.select(0.1):
                    data = os.read(fd, 65536)
                    if not data: break
                    buf += dec.decode(data)
                    *done, buf = buf.split('\n')
                    for line in done: yield line + '\n'
                if end is None and self.p.poll() is not None: end = time.monotonic() + self.drain
                if end is not None and time.monotonic() >= end:
                    self.stragglers += members(self.pgid) if not stopped else []
                    if stopped or not self.stragglers:   # a holder outside the group (it left it): stop reading, and fail the stage all the same
                        self.stragglers.append((None, 'a process outside the group holding its output')); break
                    self.stop(); stopped, end = True, time.monotonic() + REAP   # the pipe closes as they go
        finally:
            sel.close()
        buf += dec.decode(b'', final=True)
        if buf: yield buf


if os.name == 'nt':
    import queue, threading
    from ctypes import wintypes
    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    for _name, _res, _args in (
            ('CreateJobObjectW', wintypes.HANDLE, (ctypes.c_void_p, wintypes.LPCWSTR)),
            ('SetInformationJobObject', wintypes.BOOL, (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)),
            ('QueryInformationJobObject', wintypes.BOOL, (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD))),
            ('AssignProcessToJobObject', wintypes.BOOL, (wintypes.HANDLE, wintypes.HANDLE)),
            ('TerminateJobObject', wintypes.BOOL, (wintypes.HANDLE, wintypes.UINT)),
            ('CloseHandle', wintypes.BOOL, (wintypes.HANDLE,)),
            ('CreateToolhelp32Snapshot', wintypes.HANDLE, (wintypes.DWORD, wintypes.DWORD)),
            ('Thread32First', wintypes.BOOL, (wintypes.HANDLE, ctypes.c_void_p)),
            ('Thread32Next', wintypes.BOOL, (wintypes.HANDLE, ctypes.c_void_p)),
            ('OpenThread', wintypes.HANDLE, (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)),
            ('ResumeThread', wintypes.DWORD, (wintypes.HANDLE,)),
            ('CancelSynchronousIo', wintypes.BOOL, (wintypes.HANDLE,)),
            ('OpenProcess', wintypes.HANDLE, (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)),
            ('QueryFullProcessImageNameW', wintypes.BOOL, (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)))):
        getattr(_k32, _name).restype, getattr(_k32, _name).argtypes = _res, _args

    class _BasicLimit(ctypes.Structure):   # JOBOBJECT_BASIC_LIMIT_INFORMATION
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64), ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD), ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                    ('SchedulingClass', wintypes.DWORD)]

    class _ExtendedLimit(ctypes.Structure):   # JOBOBJECT_EXTENDED_LIMIT_INFORMATION (IoInfo: IO_COUNTERS, six counters)
        _fields_ = [('BasicLimitInformation', _BasicLimit), ('IoInfo', ctypes.c_uint64 * 6), ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

    class _ThreadEntry(ctypes.Structure):   # THREADENTRY32
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD), ('th32OwnerProcessID', wintypes.DWORD),
                    ('tpBasePri', ctypes.c_long), ('tpDeltaPri', ctypes.c_long), ('dwFlags', wintypes.DWORD)]

    _EXTENDED_LIMIT, _PROCESS_ID_LIST, _KILL_ON_JOB_CLOSE, _MORE_DATA = 9, 3, 0x2000, 234   # JobObjectExtendedLimitInformation, JobObjectBasicProcessIdList, ERROR_MORE_DATA
    _SUSPENDED, _SNAPTHREAD, _THREAD_ACCESS, _QUERY_LIMITED = 0x4, 0x4, 0x3, 0x1000   # CREATE_SUSPENDED, TH32CS_SNAPTHREAD, THREAD_TERMINATE | THREAD_SUSPEND_RESUME, PROCESS_QUERY_LIMITED_INFORMATION
    _INVALID = wintypes.HANDLE(-1).value

    def _check(ok):
        if not ok: raise ctypes.WinError(ctypes.get_last_error())
        return ok

    def _image_name(pid):   # a process's program file name; '?' when it cannot be opened (gone meanwhile)
        h = _k32.OpenProcess(_QUERY_LIMITED, False, pid)
        if not h: return '?'
        try:
            buf, n = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            return os.path.basename(buf.value[:n.value]) if _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else '?'
        finally: _k32.CloseHandle(h)

    def _resume_process(pid):   # the threads of a process started suspended (one: its first); Popen keeps no handle of it
        snap = _k32.CreateToolhelp32Snapshot(_SNAPTHREAD, 0)
        if snap == _INVALID: raise ctypes.WinError(ctypes.get_last_error())
        try:
            e, n = _ThreadEntry(dwSize=ctypes.sizeof(_ThreadEntry)), 0
            more = _k32.Thread32First(snap, ctypes.byref(e))
            while more:
                if e.th32OwnerProcessID == pid:
                    t = _check(_k32.OpenThread(_THREAD_ACCESS, False, e.th32ThreadID))
                    try: _check(_k32.ResumeThread(t) != 0xFFFFFFFF); n += 1
                    finally: _k32.CloseHandle(t)
                more = _k32.Thread32Next(snap, ctypes.byref(e))
        finally: _k32.CloseHandle(snap)
        if not n: raise OSError(f'no thread of process {pid} to resume')

    def _cancel_read(thread):   # ends a thread's blocking read of a pipe (CancelSynchronousIo); repeated: it may be between two reads
        for _ in range(40):
            if not thread.is_alive(): return
            h = _k32.OpenThread(_THREAD_ACCESS, False, thread.native_id)
            if h:
                _k32.CancelSynchronousIo(h); _k32.CloseHandle(h)
            thread.join(0.05)


class JobGroup:
    """Group on Windows (the module's text says what differs). The direct child starts suspended (CREATE_SUSPENDED in creationflags), goes into a new Job Object, and only then
    runs (its first thread resumed): no process it starts can be created outside the job, so the start has no race. A job that cannot be made or joined stops the start with
    the error, the child ended: a group whose members cannot be listed would pass the drain check unseen. pgid: the direct child's pid, which is also its console process
    group's id. members(): [(pid, program file name)] of the job's live processes (a process that has exited is not listed, whatever handles to it remain)"""

    def __init__(self, cmd, grace=None, drain=None, **popen):   # grace, drain: GRACE and DRAIN when None
        self.grace, self.drain = GRACE if grace is None else grace, DRAIN if drain is None else drain
        self.paused, self.stragglers, self.done, self.job = False, [], False, None
        flags = popen.pop('creationflags', 0) | _SUSPENDED | subprocess.CREATE_NEW_PROCESS_GROUP
        popen.pop('start_new_session', None)
        self.p = subprocess.Popen([str(c) for c in cmd], creationflags=flags, **popen)
        self.pgid = self.p.pid
        try:
            self.job = _check(_k32.CreateJobObjectW(None, None))
            lim = _ExtendedLimit(); lim.BasicLimitInformation.LimitFlags = _KILL_ON_JOB_CLOSE
            _check(_k32.SetInformationJobObject(self.job, _EXTENDED_LIMIT, ctypes.byref(lim), ctypes.sizeof(lim)))
            _check(_k32.AssignProcessToJobObject(self.job, int(self.p._handle)))
            _resume_process(self.p.pid)
        except BaseException:
            self.p.kill(); self.p.wait(); self._close()
            for f in (self.p.stdin, self.p.stdout, self.p.stderr):
                if f: f.close()
            raise

    def __enter__(self): return self

    def __exit__(self, kind, exc, tb):
        try:
            if kind is not None: self.stop(signal.SIGINT if issubclass(kind, KeyboardInterrupt) else signal.SIGTERM)
            elif not self.done: self.finish()
        finally:
            if self.p.stdout: self.p.stdout.close()
            self._close()
        return False

    def _close(self):   # the job's handle: with KILL_ON_JOB_CLOSE, whatever is still in it ends with it
        if self.job: _k32.CloseHandle(self.job); self.job = None

    def members(self):
        if not self.job: return []
        n = 64
        while True:
            class Ids(ctypes.Structure):   # JOBOBJECT_BASIC_PROCESS_ID_LIST with room for n
                _fields_ = [('NumberOfAssignedProcesses', wintypes.DWORD), ('NumberOfProcessIdsInList', wintypes.DWORD), ('ProcessIdList', ctypes.c_size_t * n)]
            ids = Ids()
            if _k32.QueryInformationJobObject(self.job, _PROCESS_ID_LIST, ctypes.byref(ids), ctypes.sizeof(ids), None): break
            if ctypes.get_last_error() != _MORE_DATA: raise ctypes.WinError(ctypes.get_last_error())
            n *= 4
        return [(pid, _image_name(pid)) for pid in ids.ProcessIdList[:ids.NumberOfProcessIdsInList]]

    def _signal(self, sig):   # SIGKILL: the job ended; any other: CTRL_BREAK_EVENT to the console group, or the job ended when this process has no console to send it on
        if not self.members(): return
        if sig != 'kill':
            try: os.kill(self.pgid, signal.CTRL_BREAK_EVENT); return
            except OSError: pass
        _k32.TerminateJobObject(self.job, 1)

    def pause(self): raise NotImplementedError('a job cannot be paused on Windows (no SIGSTOP)')

    def resume(self): self.paused = False   # nothing can be paused

    def _gone(self, secs):
        end = time.monotonic() + secs
        while True:
            self.p.poll()
            if not self.members(): return True
            if time.monotonic() >= end: return False
            time.sleep(TICK)

    def stop(self, sig=signal.SIGTERM):   # Group.stop, TerminateJobObject in place of SIGKILL
        late, self.done = None, True
        try:
            self._signal(sig)
            if self._gone(self.grace): self.p.wait(); return
        except BaseException as e: late = e
        while True:
            try:
                self._signal('kill'); self.p.wait(); self._gone(REAP); break
            except BaseException as e: late = e
        if late is not None: raise late

    def finish(self):
        self.p.wait(); self.done = True
        if not self._gone(self.drain):
            self.stragglers += self.members(); self.stop()
        return self.p.returncode

    def lines(self):   # Group.lines, the pipe read on a thread of its own
        out = self.p.stdout
        assert out is not None, 'lines() reads a group started with stdout=PIPE'
        fd = out.fileno()
        dec = io.IncrementalNewlineDecoder(codecs.getincrementaldecoder(locale.getpreferredencoding(False))(errors='replace'), translate=True)
        got = queue.SimpleQueue()
        def pump():
            try:
                while data := os.read(fd, 65536): got.put(data)
            except OSError: pass   # cancelled (_cancel_read), or the pipe closed under it
            got.put(b'')
        reader = threading.Thread(target=pump, name='proc_group lines', daemon=True); reader.start()
        buf, end, stopped = '', None, False
        try:
            while True:
                try: data = got.get(timeout=0.1)
                except queue.Empty: data = None
                if data is not None:
                    if not data: break
                    buf += dec.decode(data)
                    *done, buf = buf.split('\n')
                    for line in done: yield line + '\n'
                if end is None and self.p.poll() is not None: end = time.monotonic() + self.drain
                if end is not None and time.monotonic() >= end:
                    self.stragglers += self.members() if not stopped else []
                    if stopped or not self.stragglers:
                        self.stragglers.append((None, 'a process outside the group holding its output')); break
                    self.stop(); stopped, end = True, time.monotonic() + REAP
        finally:
            _cancel_read(reader)
        buf += dec.decode(b'', final=True)
        if buf: yield buf


if os.name == 'nt': Group = JobGroup   # noqa: F811


def describe(stragglers):   # for the caller's message
    return ', '.join(name if pid is None else f'{name} (pid {pid})' for pid, name in stragglers)
