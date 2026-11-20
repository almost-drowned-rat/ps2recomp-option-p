"""Verifying one lifted or structured function against the original.

Kept apart from engine/tools/lift.py, which builds and orchestrates: everything
here decides a verdict, so it is part of the environment fingerprint that
remembered verdicts are keyed by (verdicts.py), and the orchestration is not.
"""

from __future__ import annotations

import random

from . import layout, liftcheck, testmem, testplan

STATE_PARTS = ("registers", "upper register halves", "HI", "LO", "HI1", "LO1",
               "FPU registers", "FPU accumulator", "FPU condition",
               "VU0 vector registers", "VU0 control registers", "VU0 accumulator",
               "shift amount")


def _difference(actual, expected) -> str:
    """Which part of the state differs, naming the register for a register file."""
    for name, a, b in zip(STATE_PARTS, actual, expected):
        if a != b:
            if isinstance(a, tuple):
                index = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
                show = lambda v: "0x%X" % v if isinstance(v, int) else repr(v)
                return "%s (%d: lifted %s, original %s)" % (name, index, show(a[index]),
                                                           show(b[index]))
            return name
    return "nothing"


CALLEE_SAVED_REGS = (16, 17, 18, 19, 20, 21, 22, 23, 30)


def _entry_fill():
    """The registers a case's arguments do not set, filled with distinct values (2026-11-13, planted faults:
    with every register 0 at entry, a function's save and restore of s0-s7 to the stack was invisible - a store
    left out, narrowed to 32 bits or reading the wrong register passed). Each value has upper 32 bits that are
    not the sign extension of the lower ones, and an upper 64-bit half of its own, so narrowed moves show; the
    low halves are no address the test memory maps, so a fill read as a pointer faults instead of hiding.
    The argument registers ($a0-$t3, $f12-$f19) stay the test plan's (0 where it gives no argument)."""
    regs = [0] * 32
    upper = [0] * 32
    for i in range(1, 32):
        if 4 <= i <= 11:
            continue
        regs[i] = ((0x5EED0000 | i) << 32) | 0x7E5A0000 | (i << 8) | i
        upper[i] = (0xC0DE0000 | i) << 32 | (0x1234 * i)
    for i in range(4, 12):
        upper[i] = (0xC0DE0000 | i) << 32 | (0x1234 * i)
    fpr = [0 if 12 <= i <= 19 else 0x3F800000 + (i << 12) for i in range(32)]
    return (tuple(regs), tuple(upper), tuple(fpr), 0x4849000048490001, 0x4C4F00004C4F0002)


ENTRY_FILL = _entry_fill()


def abi_view(state):
    """What a caller can rely on after a call, under the calling convention: v0
    and v1, $gp, $sp, $ra, the callee-saved registers whole (upper halves too),
    the float results and the callee-saved float registers."""
    regs, upper, fpr = state[0], state[1], state[6]
    return (tuple(regs[i] for i in (2, 3, 28, 29, 31) + CALLEE_SAVED_REGS),
            tuple(upper[i] for i in CALLEE_SAVED_REGS),
            tuple(fpr[i] for i in (0, 1) + tuple(range(20, 32))))


# How hard to look for inputs the original completes on, when the standard plan
# finds none: fresh argument sets, each tried under every memory profile, with a
# small budget so that a function no input finishes on costs little.
SEARCH_ATTEMPTS = 12
SEARCH_BUDGET = 20_000
CASES = 8
# Cases that must complete, and agree, for a verdict.
MIN_CASES = 5


def _cases(func, machine):
    """(arguments, profile, memory seed) for each test case, or None.

    The standard plan - eight argument sets under the first profile the original
    runs with on the first of them - comes first. When it has no profile, inputs
    are searched for instead. Any input the original completes on is as valid a
    test as any other; this only decides which inputs are tried.
    """
    plan = testplan.test_plan(func, machine)
    if plan is not None:
        arg_sets, profile = plan
        return [(args, profile, func["addr"] + case) for case, args in enumerate(arg_sets)]
    return _search(func, machine, CASES) or None


def _search(func, machine, want):
    """Up to `want` (arguments, profile, seed) the original completes on, from
    fresh random inputs, each under every profile."""
    rng = random.Random(func["addr"] ^ 0x5EA4C4)
    memory = machine.memory
    found = []
    for attempt in range(SEARCH_ATTEMPTS):
        args = testmem.arguments(rng)
        seed = func["addr"] + 1000 + attempt
        # Random arguments reach random addresses; the pages a failed attempt
        # mapped are given back, or the search would use up the process's
        # mapping budget (emu.MAX_PAGES) and fail the functions verified after.
        mark = memory.page_mark()
        ran = False
        for profile in testmem.PROFILES:
            memory.end_journal()
            memory.write(layout.CURRENT.scratch, testmem.build(profile, seed))
            memory.begin_journal()
            try:
                machine.call(func["addr"], args, max_instructions=SEARCH_BUDGET, fill=ENTRY_FILL)
                ran = True
            except Exception:
                ran = False
            memory.rollback()
            memory.end_journal()
            if ran:
                found.append((args, profile, seed))
                break
        if not ran:
            memory.unmap_to(mark)
        if len(found) >= want:
            break
    return found


def verify(func, machine, library, abi=False) -> str:
    """'verified', or why not."""
    mark = machine.memory.page_mark()
    outcome = _verify(func, machine, library, abi)
    if outcome.startswith("untestable"):
        # Nothing was learnt; nothing it mapped should change what comes after.
        machine.memory.unmap_to(mark)
    return outcome


def _run_original(func, machine, args, profiles, seed) -> bool:
    """Run the original on `args` under the first of `profiles` it completes
    with, leaving memory journalled as it left it; False if none."""
    memory = machine.memory
    for profile in profiles:
        # Pages a failed attempt mapped are given back, as _cases does: they
        # would use up the mapping budget of the functions verified after.
        mark = memory.page_mark()
        memory.end_journal()
        memory.write(layout.CURRENT.scratch, testmem.build(profile, seed))
        memory.begin_journal()
        try:
            machine.call(func["addr"], args, max_instructions=testplan.VERIFY_BUDGET, fill=ENTRY_FILL)
            return True
        except Exception:
            memory.rollback()
            memory.end_journal()
            memory.unmap_to(mark)
    return False


def _verify(func, machine, library, abi) -> str:
    cases = _cases(func, machine)
    if cases is None:
        return "untestable: " + testplan.failure_reason()
    ranges, slots = _coverage_start(func, machine)
    try:
        tested, failure = _check(func, machine, library, abi, cases)
        if failure is None and tested < MIN_CASES and len(cases) == testplan.ARG_SETS:
            # The plan's inputs left too few cases: fresh ones, searched for,
            # make up the rest (a plan's seeds lie below 1000 past the address,
            # a search's from there on, so no input is tried twice).
            more, failure = _check(func, machine, library, abi, _search(func, machine, CASES))
            tested += more
        if failure is None and tested >= MIN_CASES:
            more, failure = _guided(func, machine, library, abi, ranges, slots)
            tested += more
    finally:
        machine.coverage = None
    if failure is not None:
        return failure
    return "verified" if tested >= MIN_CASES else "untestable: only %d cases ran" % tested


# Coverage-guided cases (2026-11-13: a sample of verified ZOE functions showed their cases executing ~37 % of the
# instructions, weighted by size - large functions far less - and most planted faults that survived sat in code
# no case reached). While some instruction of the function has not run in any case, fresh random inputs are
# tried, each a full case (original against lifted) if the original completes on it; seeds from 2000 past the
# address, apart from the plan's (< 1000) and the search's (1000 on).
GUIDED_ATTEMPTS = 12


def _ranges(func):
    return [(func["addr"], func["end"])] + [tuple(chunk) for chunk in func.get("chunks", ())]


def _coverage_start(func, machine):
    """Turn on the machine's coverage over the function; (its ranges, the delay slots a covered branch covers)."""
    import ctypes

    from . import r5900
    ranges = _ranges(func)
    low = min(start for start, _end in ranges)
    high = max(end for _start, end in ranges)
    machine.coverage = (low, (ctypes.c_uint8 * max(1, (high - low) // 4))())
    slots = set()
    for start, end in ranges:
        for pc in range(start, end, 4):
            ins = r5900.decode(machine.memory.read_int(pc, 4, False), pc)
            if ins.has_delay_slot and not ins.is_likely:
                slots.add(pc + 4)       # runs inside its branch's step, never marked on its own
    return ranges, slots


def _uncovered(machine, ranges, slots) -> int:
    low, marks = machine.coverage
    count = 0
    for start, end in ranges:
        for pc in range(start, end, 4):
            if marks[(pc - low) >> 2] or (pc in slots and marks[(pc - 4 - low) >> 2]):
                continue
            count += 1
    return count


_LOADS = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "lwu": 4, "ld": 8, "lwc1": 4}
GLOBALS_MAX = 32


def _global_loads(func, machine):
    """{address: width} of the globals the function loads at a constant address - $gp-relative, or off a register
    a lui (and an addiu on it) set - read from its code; outside the code, the test scratch and the stack."""
    from . import r5900
    gp = layout.CURRENT.gp
    memory = machine.memory
    scratch = layout.CURRENT.scratch
    found = {}
    for start, end in _ranges(func):
        known = {}                      # register -> constant from a lui (+ addiu), within a straight run
        for pc in range(start, end, 4):
            ins = r5900.decode(memory.read_int(pc, 4, False), pc)
            name = ins.mnemonic
            offset = ((ins.imm or 0) & 0xFFFF ^ 0x8000) - 0x8000
            if name == "lui":
                known[ins.rt] = ((ins.imm or 0) & 0xFFFF) << 16
                continue
            if name in _LOADS:
                base = gp if ins.rs == 28 and gp else known.get(ins.rs)
                if base is not None:
                    address, width = (base + offset) & 0xFFFFFFFF, _LOADS[name]
                    if (address % width == 0 and memory.base <= address < memory.base + memory.size - 8
                            and not machine.code_start <= address < machine.code_end
                            and not scratch <= address < scratch + layout.CURRENT.scratch_size
                            and not layout.CURRENT.stack_low <= address < layout.CURRENT.stack_top):
                        found[address] = max(width, found.get(address, 0))
            if name in ("addiu", "daddiu", "ori") and ins.rs in known and ins.rs == ins.rt:
                known[ins.rt] = (known[ins.rs] | (ins.imm & 0xFFFF)) if name == "ori" else known[ins.rs] + offset
            else:
                known.pop(ins.rt, None)
                known.pop(ins.rd, None)
            if ins.is_branch or ins.is_jump:
                known.clear()           # a label may follow; what the registers hold there is not known
    return dict(sorted(found.items())[:GLOBALS_MAX])


def _vary_globals(machine, loads, rng):
    """Give some of the globals `loads` names other values, outside the journal (both forms of the case see
    them); returns what to restore."""
    memory = machine.memory
    saved = []
    for address, width in loads.items():
        if rng.random() < 0.5:
            continue
        try:
            old = memory.read(address, width)
        except Exception:
            continue
        pick = rng.randrange(6)
        if pick == 0:
            value = 0
        elif pick == 1:
            value = rng.randrange(1, testmem.SMALL_LIMIT + 1)
        elif pick == 2:
            value = (1 << (8 * width)) - 1
        elif pick == 3 and width >= 4:
            value = layout.CURRENT.scratch + 16 * rng.randrange(layout.CURRENT.scratch_size // 16)
        else:
            value = rng.getrandbits(8 * width)
        saved.append((address, old))
        memory.write_int(address, value, width)
    return saved


def _code_constants(func, machine):
    """The values the function's code compares and masks with (2026-11-16, planted faults: the guided cases' integer
    arguments were always scratch pointers, so a branch on an argument's value - `n < 5`, `mode == 3` - kept one
    side unrun): slti/sltiu immediates, constants loaded into a register with addiu/ori from $zero, andi masks;
    each with its neighbours (v - 1, v + 1) and 0, -1. Sorted, at most CONSTANTS_MAX."""
    from . import r5900
    found = {0, 0xFFFFFFFFFFFFFFFF}
    for start, end in _ranges(func):
        for pc in range(start, end, 4):
            ins = r5900.decode(machine.memory.read_int(pc, 4, False), pc)
            name = ins.mnemonic
            imm = (ins.imm or 0) & 0xFFFF
            signed = (imm ^ 0x8000) - 0x8000
            if name in ("slti", "sltiu") or (name in ("addiu", "daddiu") and ins.rs == 0):
                value = signed
            elif name in ("ori", "andi") and (name == "andi" or ins.rs == 0):
                value = imm
            else:
                continue
            for v in (value - 1, value, value + 1):
                found.add(v & 0xFFFFFFFFFFFFFFFF)
    return sorted(found)[:CONSTANTS_MAX]


CONSTANTS_MAX = 48


def _guided_args(kind, rng, constants, completed):
    """A guided attempt's arguments: kind 0 fresh (pointers into scratch, testmem.arguments); kind 1 the same with
    one or two integer arguments set to a value the code compares with (or a small one); kind 2 a completed case's
    arguments changed a little (+-1, negated, a bit flipped, 0, two swapped) - what a fuzzer does near inputs that
    already run."""
    args = list(testmem.arguments(rng))
    if kind == 1:
        for _ in range(rng.randrange(1, 3)):
            i = rng.randrange(8)
            args[i] = rng.choice(constants) if constants and rng.random() < 0.75 else rng.randrange(0, testmem.SMALL_LIMIT + 1)
    elif kind == 2 and completed:
        args = list(rng.choice(completed))
        for _ in range(rng.randrange(1, 3)):
            i = rng.randrange(8)
            pick = rng.randrange(6)
            if pick == 0:
                args[i] = (args[i] + 1) & 0xFFFFFFFFFFFFFFFF
            elif pick == 1:
                args[i] = (args[i] - 1) & 0xFFFFFFFFFFFFFFFF
            elif pick == 2:
                args[i] = (-args[i]) & 0xFFFFFFFFFFFFFFFF
            elif pick == 3:
                args[i] ^= 1 << rng.randrange(32)
            elif pick == 4:
                args[i] = 0
            else:
                j = rng.randrange(8)
                args[i], args[j] = args[j], args[i]
    return tuple(args)


def _guided(func, machine, library, abi, ranges, slots):
    """(cases added and agreed on, the first disagreement or None) - see GUIDED_ATTEMPTS."""
    rng = random.Random(func["addr"] ^ 0xC0E5A6E)
    loads = None
    constants = None
    completed = []
    tested = 0
    for attempt in range(4 * GUIDED_ATTEMPTS):
        if not _uncovered(machine, ranges, slots):
            break
        # GUIDED_ATTEMPTS with fresh arguments alone, then as many with some of the globals the function loads
        # given other values too (2026-11-13: what no argument reached mostly hung on the game's global state),
        # then (2026-11-16) as many with the code's constants as arguments, and as many mutating the arguments of
        # guided cases that completed.
        phase = attempt // GUIDED_ATTEMPTS
        saved = ()
        if phase == 1:
            if loads is None:
                loads = _global_loads(func, machine)
            if not loads:
                continue
            saved = _vary_globals(machine, loads, random.Random(func["addr"] * 31 + attempt))
        if phase == 2 and constants is None:
            constants = _code_constants(func, machine)
        if phase == 3 and not completed:
            break
        args = _guided_args({0: 0, 1: 0, 2: 1, 3: 2}[phase], rng, constants or [], completed)
        try:
            more, failure = _guided_case(func, machine, library, abi, args, func["addr"] + 2000 + attempt)
        finally:
            memory = machine.memory
            for address, old in reversed(saved):
                memory.write(address, old)
        tested += more
        if more:
            completed.append(args)
        if failure is not None:
            return tested, failure
    return tested, None


def _guided_case(func, machine, library, abi, args, seed):
    """One guided attempt on `args`: (1, None) for a case that agreed, (0, why) for one that did not, (0, None) if
    the original completed on none of the probes."""
    import ctypes

    memory = machine.memory
    _low, marks = machine.coverage
    # A cheap probe first (a few profiles, the search's budget): most random inputs do not complete, and a gate
    # runs this in Python. What a probe that fails marked is taken back - only completed cases count.
    before = bytes(marks)
    profile = None
    for candidate in GUIDED_PROFILES:
        mark = memory.page_mark()
        memory.end_journal()
        memory.write(layout.CURRENT.scratch, testmem.build(candidate, seed))
        memory.begin_journal()
        try:
            machine.call(func["addr"], args, max_instructions=SEARCH_BUDGET, fill=ENTRY_FILL)
            profile = candidate
        except Exception:
            profile = None
        memory.rollback()
        memory.end_journal()
        if profile is not None:
            break
        memory.unmap_to(mark)
        ctypes.memmove(marks, before, len(before))
    if profile is None:
        return 0, None
    return _check(func, machine, library, abi, [(args, profile, seed)])


GUIDED_PROFILES = testmem.PROFILES[:3]


def _check(func, machine, library, abi, cases):
    """(how many cases the original completed and the lifted code agreed on,
    the first disagreement or None)."""
    memory = machine.memory
    tested = 0
    for args, profile, seed in cases:
        # A case the original does not complete under the profile chosen for
        # the first is tried under the others: the shape that lets one input
        # finish need not suit the next (a count in one, a list in another).
        # Which inputs are tried never changes what a case concludes.
        others = tuple(p for p in testmem.PROFILES if p != profile)
        if not _run_original(func, machine, args, (profile,) + others, seed):
            continue
        expected = liftcheck.machine_state(machine)
        effects = testplan._effects_digest(memory)
        memory.rollback()
        context = liftcheck.entry_context(args, fill=ENTRY_FILL)
        why = library.run(func["addr"], context)
        if why is not None:
            memory.rollback()
            memory.end_journal()
            return tested, "lifted run failed: " + why
        actual = liftcheck.context_state(context)
        replayed = testplan._effects_digest(memory)
        memory.rollback()
        memory.end_journal()
        if abi:
            if abi_view(actual) != abi_view(expected):
                return tested, "disagrees: " + _difference(abi_view(actual), abi_view(expected))
        elif actual != expected:
            return tested, "disagrees: " + _difference(actual, expected)
        if replayed != effects:
            return tested, "disagrees: memory"
        tested += 1
    return tested, None
