"""The native core of the verification interpreter (native/emu_core.c), and its bridge to emu.Machine.

Machine._call_once and Machine.run_to_return hand their loops to the core while it is enabled: it runs every
step it can do exactly as emu.py would, and stops before any step it cannot (emu.py then runs that one step
itself and hands back). Enabled by PS2_EMU_NATIVE=1 (off by default until the full comparison against the Python
interpreter on every function of a game has passed - docs/option_p/verify_cpp_plan.md). The DLL is built once
per source version with the Visual C++ compiler (cl), next to the toolkit's other build output.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "native", "emu_core.c")
BUILD = os.path.join(os.path.dirname(HERE), "build", "emu_native")

STOP_RETURNED, NEED_PYTHON, OVER_BUDGET, JOURNAL_FULL = 0, 1, 2, 3
JOURNAL_CAP = 4096


class State(ctypes.Structure):
    """Mirror of EmuState in native/emu_core.c."""
    _fields_ = [
        ("regs", ctypes.c_uint64 * 32), ("regs_hi", ctypes.c_uint64 * 32),
        ("hi", ctypes.c_uint64), ("lo", ctypes.c_uint64), ("hi1", ctypes.c_uint64), ("lo1", ctypes.c_uint64),
        ("pc", ctypes.c_uint32), ("mode", ctypes.c_uint32),
        ("return_address", ctypes.c_uint32), ("entry_sp", ctypes.c_uint32),
        ("executed", ctypes.c_int64), ("budget", ctypes.c_int64),
        ("code_start", ctypes.c_uint32), ("code_end", ctypes.c_uint32),
        ("mem", ctypes.POINTER(ctypes.c_uint8)), ("mem_len", ctypes.c_uint64),
        ("ram_base", ctypes.c_uint32), ("pad0", ctypes.c_uint32), ("ram_size", ctypes.c_uint64),
        ("win_start", ctypes.c_uint32 * 2), ("win_end", ctypes.c_uint32 * 2), ("win_at", ctypes.c_uint64 * 2),
        ("nwin", ctypes.c_uint32), ("npages", ctypes.c_uint32),
        ("page_addr", ctypes.POINTER(ctypes.c_uint32)), ("page_at", ctypes.POINTER(ctypes.c_uint64)),
        ("journal_on", ctypes.c_uint32), ("jr_count", ctypes.c_uint32), ("jr_cap", ctypes.c_uint32),
        ("pad1", ctypes.c_uint32),
        ("journal_bytes", ctypes.c_int64), ("journal_limit", ctypes.c_int64),
        ("jr_off", ctypes.POINTER(ctypes.c_uint64)), ("jr_len", ctypes.POINTER(ctypes.c_uint8)),
        ("jr_prev", ctypes.POINTER(ctypes.c_uint8)),
        ("watch_zero", ctypes.c_uint32), ("last_zero_valid", ctypes.c_uint32),
        ("last_zero_read", ctypes.c_uint32), ("pad2", ctypes.c_uint32),
        ("fpr", ctypes.c_uint32 * 32), ("fp_cond", ctypes.c_uint32), ("fp_acc", ctypes.c_uint32),
        ("vf", (ctypes.c_uint32 * 4) * 32), ("vc", ctypes.c_uint32 * 32), ("vacc", ctypes.c_uint32 * 4),
        ("cov", ctypes.POINTER(ctypes.c_uint8)), ("cov_start", ctypes.c_uint32), ("cov_len", ctypes.c_uint32),
        ("fcr31", ctypes.c_uint32), ("pad3", ctypes.c_uint32),
    ]


_LIBRARY = None
_TRIED = False


def _source_hash() -> str:
    with open(SOURCE, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()[:16]


def _build(dll: str) -> None:
    from . import cppcheck
    os.makedirs(BUILD, exist_ok=True)
    work = tempfile.mkdtemp(prefix="emu_core_", dir=BUILD)
    out = os.path.join(work, "emu_core.dll")
    script = os.path.join(work, "build.bat")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write('@echo off\ncall "%s" >nul\n' % cppcheck.find_vcvars())
        handle.write('cd /d "%s"\n' % work)
        handle.write('cl /nologo /O2 /LD /W3 "%s" /Fe"%s" >build.log 2>&1\n' % (SOURCE, out))
    result = subprocess.run(["cmd", "/c", script], capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        with open(os.path.join(work, "build.log"), encoding="utf-8", errors="replace") as handle:
            raise RuntimeError("emu_core.c did not build:\n" + handle.read()[-3000:])
    try:
        os.replace(out, dll)            # atomic: a worker building at the same time loses nothing
    except OSError:
        if not os.path.exists(dll):
            raise


def library():
    """The loaded core, built first if need be; None when it cannot be."""
    global _LIBRARY, _TRIED
    if _TRIED:
        return _LIBRARY
    _TRIED = True
    dll = os.path.join(BUILD, "emu_core_%s.dll" % _source_hash())
    if not os.path.exists(dll):
        _build(dll)
    lib = ctypes.CDLL(dll)
    lib.emu_core_run.argtypes = [ctypes.POINTER(State)]
    lib.emu_core_run.restype = ctypes.c_int
    lib.emu_core_step.argtypes = [ctypes.POINTER(State)]
    lib.emu_core_step.restype = ctypes.c_int
    lib.emu_core_state_size.restype = ctypes.c_int
    if lib.emu_core_state_size() != ctypes.sizeof(State):
        raise RuntimeError("emu_core.c's EmuState (%d bytes) and emu_native.State (%d) differ"
                           % (lib.emu_core_state_size(), ctypes.sizeof(State)))
    _LIBRARY = lib
    return lib


def enabled() -> bool:
    """On by default since 2026-11-13 (ZOE US: every one of 6,831 verdicts and all 33 emitted sources identical
    to the Python interpreter's). PS2_EMU_NATIVE=0 turns it off; lift.py --gate does, so a gate's verdicts come
    from the independent Python interpreter."""
    return os.environ.get("PS2_EMU_NATIVE", "1") != "0"


class Bridge:
    """One machine's state on the native side, and its synchronisation with the Python one."""

    def __init__(self, machine) -> None:
        self.machine = machine
        self.state = State()
        self.jr_off = (ctypes.c_uint64 * JOURNAL_CAP)()
        self.jr_len = (ctypes.c_uint8 * JOURNAL_CAP)()
        self.jr_prev = (ctypes.c_uint8 * (16 * JOURNAL_CAP))()     # 16 bytes an entry (sqc2)
        self.page_generation = None
        self.page_addr = (ctypes.c_uint32 * 1)()
        self.page_at = (ctypes.c_uint64 * 1)()
        s = self.state
        s.jr_off, s.jr_len, s.jr_prev = self.jr_off, self.jr_len, self.jr_prev
        s.jr_cap = JOURNAL_CAP
        self.buffer_key = None
        self.lib = library()
        self.ref = ctypes.byref(s)

    def _pages(self, memory) -> None:
        generation = (len(memory.pages), getattr(memory, "page_generation", 0))
        if generation == self.page_generation:
            return
        self.page_generation = generation
        items = sorted(memory.pages.items())
        n = max(1, len(items))
        self.page_addr = (ctypes.c_uint32 * n)(*[a for a, _ in items])
        self.page_at = (ctypes.c_uint64 * n)(*[o for _, o in items])
        self.state.page_addr = self.page_addr
        self.state.page_at = self.page_at
        self.state.npages = len(items)

    def run(self, mode: int, executed: int, budget: int, return_address: int = 0, entry_sp: int = 0,
            single: bool = False) -> tuple:
        """(reason, executed) after the core ran from the machine's state; the machine has the result."""
        from .emu import Memory
        m = self.machine
        memory = m.memory
        s = self.state
        s.regs[:] = m.regs
        s.regs_hi[:] = m.regs_hi
        s.hi, s.lo, s.hi1, s.lo1 = m.hi, m.lo, m.hi1, m.lo1
        s.fpr[:] = m.fpr
        s.fp_cond = 1 if m.fp_cond else 0
        s.fcr31 = m.fcr31
        s.fp_acc = m.fp_acc
        for i, lanes in enumerate(m.vf):
            s.vf[i][:] = lanes
        s.vc[:] = m.vc
        s.vacc[:] = m.vacc
        s.pc = m.pc & 0xFFFFFFFF
        s.mode = mode
        s.return_address = return_address & 0xFFFFFFFF
        s.entry_sp = entry_sp & 0xFFFFFFFF
        s.executed = executed
        s.budget = budget
        s.code_start, s.code_end = m.code_start, m.code_end
        s.ram_base, s.ram_size = memory.base, memory.size
        s.nwin = len(memory.windows)
        for i, (start, end, at) in enumerate(memory.windows[:2]):
            s.win_start[i], s.win_end[i], s.win_at[i] = start, end, at
        self._pages(memory)
        journal = memory._journal
        s.journal_on = 1 if journal is not None else 0
        s.journal_bytes = getattr(memory, "_journal_bytes", 0) if journal is not None else 0
        s.journal_limit = Memory.JOURNAL_LIMIT
        s.jr_count = 0
        coverage = m.coverage
        if coverage is None:
            s.cov_len = 0
        else:
            # (start, a ctypes uint8 array): marked in place by both interpreters (emu.Machine.coverage).
            s.cov = ctypes.cast(coverage[1], ctypes.POINTER(ctypes.c_uint8))
            s.cov_start, s.cov_len = coverage[0], len(coverage[1])
        s.watch_zero = 1 if memory.watch_zero else 0
        s.last_zero_valid = 0
        # The buffer's address, taken without keeping an export on it (one would stop Memory.map_page from
        # growing it: "BufferError: Existing exports of data"). It moves only when the buffer is resized - by
        # map_page / unmap_to, which bump page_generation - so it is taken again only then.
        key = (id(memory.bytes), len(memory.bytes), memory.page_generation)
        if key != self.buffer_key:
            view = (ctypes.c_uint8 * len(memory.bytes)).from_buffer(memory.bytes)
            address = ctypes.addressof(view)
            del view
            self.buffer_key = key
            s.mem = ctypes.cast(ctypes.c_void_p(address), ctypes.POINTER(ctypes.c_uint8))
            s.mem_len = len(memory.bytes)
        lib = self.lib
        reason = (STOP_RETURNED if lib.emu_core_step(self.ref) else NEED_PYTHON) if single \
            else lib.emu_core_run(self.ref)
        # The journal entries, in order, before anything else can write.
        if s.jr_count:
            raw = bytes(self.jr_prev)
            for i in range(s.jr_count):
                length = self.jr_len[i]
                journal.append((self.jr_off[i], raw[16 * i: 16 * i + length]))
            memory._journal_bytes = s.journal_bytes
        m.regs[:] = s.regs              # in place: a holder of the list sees the result
        m.regs_hi[:] = s.regs_hi
        m.hi, m.lo, m.hi1, m.lo1 = s.hi, s.lo, s.hi1, s.lo1
        m.fpr[:] = s.fpr
        m.fp_cond = bool(s.fp_cond)
        m.fcr31 = s.fcr31
        m.fp_acc = s.fp_acc
        for i in range(32):
            m.vf[i] = list(s.vf[i])
        m.vc[:] = s.vc
        m.vacc[:] = s.vacc
        m.pc = s.pc
        if s.last_zero_valid:
            memory.last_zero_read = s.last_zero_read
        return reason, s.executed
