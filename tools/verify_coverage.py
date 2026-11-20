#!/usr/bin/env python3
"""How much of a verified function its verification cases execute.

    python engine/tools/verify_coverage.py --game games/zoe_us --count 500

"Verified" means: on every case the original completed, the lifted code agreed in every register and byte. It
says nothing about instructions no case reached. This runs each sampled verified function's cases (the
verifier's own: liftverify._cases, the filled entry state) on the original with per-address visit counts and
reports the share of the function's instructions executed - a distribution, the least covered functions, and,
with --faults, how the planted-fault survivors (tools/planted_faults.py) relate to coverage.
Runs the Python interpreter (visit counts need every step in Python).
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]
os.environ["PS2_EMU_NATIVE"] = "0"

import lift as lift_tool  # noqa: E402  (tools/lift.py)
from ps2decomp import liftverify, r5900, testmem, testplan  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402
from ps2decomp.emu import Machine  # noqa: E402


def covered(func, machine):
    """(instructions executed by the cases, instructions in the function, cases run)."""
    cases = liftverify._cases(func, machine)
    if not cases:
        return 0, 0, 0
    ranges = [(func["addr"], func["end"])] + [tuple(c) for c in func.get("chunks", ())]
    total = sum((end - start) // 4 for start, end in ranges)
    seen, ran = set(), 0
    memory = machine.memory
    for args, profile, seed in cases:
        machine.visit_counts = {}
        machine.count_visits = True
        others = tuple(p for p in testmem.PROFILES if p != profile)
        try:
            if liftverify._run_original(func, machine, args, (profile,) + others, seed):
                ran += 1
                visited = set(machine.visit_counts)
                # A delay slot runs inside its branch's step and is not counted on its own: a visited branch
                # covers its slot (not a likely branch's - that slot runs only when the branch is taken).
                for pc in list(visited):
                    ins = r5900.decode(int.from_bytes(memory.read(pc, 4), "little"), pc)
                    if ins.has_delay_slot and not ins.is_likely:
                        visited.add(pc + 4)
                seen |= {pc for pc in visited if any(s <= pc < e for s, e in ranges)}
                memory.rollback()
                memory.end_journal()
        finally:
            machine.count_visits = False
    return len(seen), total, ran


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--count", type=int, default=500)
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--faults", action="store_true",
                        help="also relate build/planted_faults/report.txt's survivors to coverage")
    args = parser.parse_args(argv)

    game = game_config.from_args(args)
    with open(game.path(os.path.join(game.build, "lift_outcomes.json")), encoding="utf-8") as handle:
        outcomes = json.load(handle)
    verified = sorted(int(a, 16) for a, o in outcomes.items() if o == "verified")
    database = game.load_database()
    by_address = {f["addr"]: f for f in database["functions"]}
    rng = random.Random(args.seed)
    chosen = set(rng.sample(verified, min(args.count, len(verified))))
    survivors = {}
    report = game.path(os.path.join(game.build, "planted_faults", "report.txt"))
    if args.faults and os.path.exists(report):
        with open(report, encoding="utf-8") as handle:
            for line in handle:
                m = re.match(r"(survived|DISAGREE) (\w+) 0x([0-9A-F]{8})", line)
                if m:
                    survivors.setdefault(int(m.group(3), 16), []).append(m.group(2))
        chosen |= {a for a in survivors if a in by_address}

    elf = game.load_elf()
    section = elf.exec_sections[0]
    image = elf.section_data(section)
    machine = Machine.from_elf(elf)
    testplan.prepare_test_memory(machine, database, image, section.addr,
                                 [f for f in database["functions"] if f["end"] > f["addr"]])
    rows = []
    for number, address in enumerate(sorted(chosen), 1):
        seen, total, ran = covered(by_address[address], machine)
        if total:
            rows.append((address, seen, total, ran))
        if number % 100 == 0:
            print("  %d of %d" % (number, len(chosen)), flush=True)

    out = game.path(os.path.join(game.build, "verify_coverage.txt"))
    shares = sorted(seen / total for _a, seen, total, _r in rows)
    weighted = sum(s for _a, s, _t, _r in rows) / max(1, sum(t for _a, _s, t, _r in rows))
    lines = ["%d verified functions sampled (+ planted-fault functions: %d)" % (len(rows), len(survivors)),
             "instructions executed by the cases: %.1f %% of all sampled instructions" % (100 * weighted)]
    for limit in (0.25, 0.5, 0.75, 0.9, 1.0):
        lines.append("  functions with coverage < %3d %%: %d" % (100 * limit, sum(1 for s in shares if s < limit)))
    lines.append("  fully covered: %d" % sum(1 for s in shares if s >= 1.0))
    if survivors:
        cov = {a: s / t for a, s, t, _r in rows}
        with_s = [cov[a] for a in survivors if a in cov]
        without = [cov[a] for a in cov if a not in survivors]
        lines.append("planted-fault functions with a survivor: mean coverage %.1f %% (%d); without: %.1f %% (%d)"
                     % (100 * sum(with_s) / max(1, len(with_s)), len(with_s),
                        100 * sum(without) / max(1, len(without)), len(without)))
    lines.append("least covered:")
    for address, seen, total, ran in sorted(rows, key=lambda r: r[1] / r[2])[:25]:
        lines.append("  0x%08X %4d of %4d instructions (%3.0f %%), %d cases" % (address, seen, total,
                                                                               100.0 * seen / total, ran))
    with open(out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines[:9]))
    print("-> %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
