#!/usr/bin/env python3
"""Planted faults: the verifier must reject lifted functions with a fault planted in them.

    python engine/tools/planted_faults.py --game games/zoe_us --count 300 --jobs 4

Takes `--count` functions the last lift verified (build/lift_outcomes.json), lifts each again and plants one
fault per kind - a changed immediate, a wrong source register, a dropped statement, a flipped branch condition,
a narrower memory access (docs/option_p/verify_cpp_plan.md, design point 4) - then verifies every mutant, by
default with the native core AND with emu.py alone. Reported per kind: rejected, survived. A survivor is either
a fault the checker cannot see or a mutant that is truly the same (a dead store, a constant that does not
matter); the two interpreters must give every mutant the same verdict, and every survivor is listed for review.
Exit 1 when the two interpreters disagree on any mutant.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

import lift as lift_tool  # noqa: E402  (tools/lift.py)
from ps2decomp import lift, testplan  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402
from ps2decomp.emu import Machine  # noqa: E402

_REG = re.compile(r"\bc\.r\[(\d+)\]")          # the plain form's registers (one statement per instruction)
_CONSTANT = re.compile(r"\b0x([0-9A-F]+)(ull|u)\b")
_WIDTH = [("Write64(", "Write32("), ("Write32(", "Write16("), ("Write16(", "Write8("),
          ("ReadS32(", "ReadS16("), ("ReadU32(", "ReadU16("), ("ReadS16(", "ReadS8("), ("ReadU16(", "ReadU8("),
          ("ReadU64(", "ReadU32(")]
_FLIP = [(" == ", " != "), (" != ", " == "), (" < ", " >= "), (" >= ", " < "), (" > ", " <= "), (" <= ", " > ")]


class NoCache:
    """verify_texts' cache, remembering nothing: every mutant is verified."""
    def __init__(self):
        self.from_cache = set()

    def get(self, text, abi):
        return None

    def put(self, text, abi, outcome):
        pass

    def save(self):
        pass


def _body(lines):
    """Indices of the statements a fault may go into (after the prologue's `(void)entry_ra;` line)."""
    start = next((i + 1 for i, line in enumerate(lines) if line.strip().startswith("(void)entry_ra;")), None)
    if start is None:
        return []
    return [i for i in range(start, len(lines)) if lines[i].strip() and lines[i].strip() not in ("}", "{")
            and not lines[i].lstrip().startswith(("L_", "//", "leave()", "case ", "default:", "switch"))]


def plant(text: str, kind: str, rng: random.Random):
    """(mutant text, the changed line) or None when the function has no place for this kind."""
    lines = text.split("\n")
    body = _body(lines)
    places = []
    for i in body:
        line = lines[i]
        stripped = line.strip()
        calls = "Call(" in line or "goto " in line or "Dispatch" in line
        if kind == "immediate" and not calls:
            for m in _CONSTANT.finditer(line):
                places.append((i, m))
        elif kind == "register" and not calls and " = " in stripped and not stripped.startswith(("if", "{")):
            target, _, source = line.partition(" = ")
            for m in _REG.finditer(source):
                if m.group(1) != "0":
                    places.append((i, m))
        elif kind == "drop" and not calls and stripped.endswith(";") and not stripped.startswith(
                ("if", "return", "out()", "in()", "}")) and "return" not in stripped:
            places.append((i, None))
        elif kind == "branch" and ("if (" in stripped or re.search(r"const bool t = ", stripped)):
            for old, new in _FLIP:
                if old in line:
                    places.append((i, (old, new)))
                    break
        elif kind == "width":
            for old, new in _WIDTH:
                if old in line:
                    places.append((i, (old, new)))
                    break
    if not places:
        return None
    i, what = rng.choice(places)
    line = lines[i]
    if kind == "immediate":
        value = int(what.group(1), 16)
        changed = value ^ (1 << rng.randrange(0, max(1, min(16, value.bit_length() + 2))))
        line = line[:what.start()] + "0x%X%s" % (changed, what.group(2)) + line[what.end():]
    elif kind == "register":
        others = [r for r in range(1, 32) if r != int(what.group(1)) and r not in (26, 27)]
        target, eq, source = line.partition(" = ")
        source = source[:what.start()] + "c.r[%d]" % rng.choice(others) + source[what.end():]
        line = target + eq + source
    elif kind == "drop":
        line = ""
    else:
        line = line.replace(what[0], what[1], 1)
    if line == lines[i]:
        return None
    original = lines[i].strip()
    lines[i] = line
    return "\n".join(lines), original


KINDS = ("immediate", "register", "drop", "branch", "width")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--count", type=int, default=300, help="functions to plant faults in")
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--interpreters", default="native,python",
                        help="which verifications to run: native, python or both (the default)")
    args = parser.parse_args(argv)

    game = game_config.from_args(args)
    with open(game.path(os.path.join(game.build, "lift_outcomes.json")), encoding="utf-8") as handle:
        outcomes = json.load(handle)
    verified = sorted(int(a, 16) for a, o in outcomes.items() if o == "verified")
    rng = random.Random(args.seed)
    chosen = sorted(rng.sample(verified, min(args.count, len(verified))))

    database = game.load_database()
    elf = game.load_elf()
    section = elf.exec_sections[0]
    image = elf.section_data(section)
    lift.use_sections(lift_tool._other_sections(elf, section))
    lift.use_function_starts(f["addr"] for f in database["functions"])
    machine = Machine.from_elf(elf)
    by_address = {f["addr"]: f for f in database["functions"]}
    functions = [by_address[a] for a in chosen]
    seeds = testplan.prepare_test_memory(machine, database, image, section.addr,
                                         [f for f in database["functions"] if f["end"] > f["addr"]])
    texts, _refused = lift_tool.lift_all(functions, image, section.addr)

    mutants = {}                    # kind -> {address: (text, original line)}
    for kind in KINDS:
        mutants[kind] = {}
        for address in sorted(texts):
            planted = plant(texts[address], kind, random.Random("%d %s %x" % (args.seed, kind, address)))
            if planted:
                mutants[kind][address] = planted
    out = game.path(os.path.join(game.build, "planted_faults"))
    os.makedirs(out, exist_ok=True)

    verdicts = {}                   # (interpreter, kind) -> {address: outcome}
    for interpreter in args.interpreters.split(","):
        os.environ["PS2_EMU_NATIVE"] = "1" if interpreter == "native" else "0"
        for kind in KINDS:
            started = time.time()
            work = os.path.join(out, "work_%s" % kind)
            result = lift_tool.verify_texts({a: m[0] for a, m in mutants[kind].items()}, work, game, seeds,
                                            args.jobs, NoCache(), False, machine, by_address)
            verdicts[(interpreter, kind)] = result
            survived = sorted(a for a, o in result.items() if o == "verified")
            print("%-6s %-9s %4d mutants: %4d rejected, %3d survived (%.0fs)"
                  % (interpreter, kind, len(result), len(result) - len(survived), len(survived),
                     time.time() - started), flush=True)

    report, disagree = [], 0
    interpreters = args.interpreters.split(",")
    for kind in KINDS:
        for address, (_text, original) in sorted(mutants[kind].items()):
            seen = [verdicts[(i, kind)].get(address) for i in interpreters]
            if len(set(seen)) > 1:
                disagree += 1
                report.append("DISAGREE %s 0x%08X `%s`: %s" % (kind, address, original, " | ".join(map(str, seen))))
            elif seen[0] == "verified":
                report.append("survived %s 0x%08X `%s`" % (kind, address, original))
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(report) + "\n")
    with open(os.path.join(out, "verdicts.json"), "w", encoding="utf-8") as handle:
        json.dump({"%s %s" % key: {"0x%08X" % a: o for a, o in value.items()}
                   for key, value in verdicts.items()}, handle, indent=0)
    print("%d mutants of %d functions; %d survivors listed, interpreters disagree on %d -> %s"
          % (sum(len(m) for m in mutants.values()), len(texts), len(report) - disagree, disagree,
             os.path.join(out, "report.txt")))
    return 1 if disagree else 0


if __name__ == "__main__":
    sys.exit(main())
