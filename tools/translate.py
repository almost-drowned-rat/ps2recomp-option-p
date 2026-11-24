#!/usr/bin/env python3
"""The player's build path (design 7d, M6b): translate the game's code without verifying it.

    python engine/tools/translate.py --game games/zoe_us --forms forms.json [--out DIR]
    python engine/tools/translate.py --game games/zoe_us --derive-forms forms.json

The developer's lift (lift.py) lifts every function, verifies each against the original and keeps, per function,
the most readable form that verified (structured: folded / saves elided / saves kept; else the plain lifted
form). That takes hours. A player builds the port from his own disc: he gets the analysis (the function list) and
the forms table - which form each function took, metadata, no code - and this tool writes the same translated
source from his disc's executable in minutes: lift, structure where the table says, emit.

--derive-forms (developer side) reads the game's emitted src/lifted, works out each function's form by
translating it every way and comparing (calls' name comments and NOT VERIFIED notes ignored), and writes the
table. --out with --forms writes the source elsewhere (to compare with the developer's: --check).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

from ps2decomp import game as game_config
from ps2decomp import lift

import lift as lift_tool

FORMS = {
    "folded": dict(elide=True, readable=True),
    "elided": dict(elide=True),
    "kept": dict(elide=False),
}

_COMMENT = re.compile(r"[ \t]*/\*.*?\*/", re.S)   # (with the blanks before it: "0x...u /* name */, x" == "0x...u, x")
_FUNCTION = re.compile(r"^void F_([0-9A-F]{8})\(lifted::Context& c\) \{$", re.M)


def _normal(text: str) -> str:
    """A function's text without comments and trailing blanks: what the forms are compared by."""
    return "\n".join(line.rstrip() for line in _COMMENT.sub("", text).splitlines() if line.strip())


def _setup(game):
    database = game.load_database()
    elf = game.load_elf()
    section = elf.exec_sections[0]
    image = elf.section_data(section)
    lift.use_sections(lift_tool._other_sections(elf, section))
    lift.use_function_starts(f["addr"] for f in database["functions"])
    lift.use_immediates(game.immediates)
    functions = [f for f in database["functions"] if f["end"] > f["addr"]]
    strings = {entry["addr"]: entry["text"] for entry in database.get("strings", [])}
    return functions, image, section.addr, strings


def _structured(func, image, base, strings, form):
    from ps2decomp import structure
    return structure.structure_function(image, base, func["addr"], func["end"], strings=strings,
                                        chunks=func.get("chunks", ()), **FORMS[form])


def emitted_functions(directory: str) -> dict:
    """{address: text} of the functions in a lifted/ directory's files."""
    found = {}
    for name in sorted(os.listdir(directory)):
        if not (name.startswith("lifted") and name.endswith(".cpp")):
            continue
        with open(os.path.join(directory, name), encoding="utf-8") as handle:
            text = handle.read()
        starts = [(m.start(), int(m.group(1), 16)) for m in _FUNCTION.finditer(text)]
        for i, (at, address) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            found[address] = text[at:end]
    return found


def derive_forms(game, out_path: str) -> int:
    functions, image, base, strings = _setup(game)
    by_address = {f["addr"]: f for f in functions}
    emitted = emitted_functions(game.path(os.path.join(os.path.dirname(game.generated), "lifted")))
    plain, _refused = lift_tool.lift_all(functions, image, base)
    forms, unknown = {}, []
    for address, text in emitted.items():
        want = _normal(text)
        if address in plain and _normal(plain[address]) == want:
            forms["0x%08X" % address] = "plain"
            continue
        for form in FORMS:
            try:
                if _normal(_structured(by_address[address], image, base, strings, form)) == want:
                    forms["0x%08X" % address] = form
                    break
            except (lift.Refused, KeyError):
                pass
        else:
            unknown.append(address)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump({"forms": forms}, handle, indent=0, sort_keys=True)
    print("forms of %d emitted functions: %s; %d matched no form%s"
          % (len(emitted), {f: list(forms.values()).count(f) for f in ["plain"] + list(FORMS)}, len(unknown),
             (" (first: %s)" % ", ".join("0x%08X" % a for a in unknown[:8])) if unknown else ""))
    return 0 if not unknown else 1


_WORKER = None


def _worker_init(game_dir: str) -> None:
    """A worker process's own analysis and image (the lifter's module state is per process)."""
    global _WORKER
    functions, image, base, strings = _setup(game_config.load(game_dir))
    _WORKER = ({f["addr"]: f for f in functions}, image, base, strings)


def _worker_structure(job):
    address, form = job
    by_address, image, base, strings = _WORKER
    return address, _structured(by_address[address], image, base, strings, form)


def _have_multiprocessing() -> bool:
    """Python for Android (python.org's package) has no _multiprocessing (Android has no POSIX named semaphores):
    multiprocessing.Pool cannot start there."""
    try:
        import _multiprocessing  # noqa: F401
        return True
    except ImportError:
        return False


def _structure_forked(functions, image, base, strings, work, jobs: int) -> dict:
    """The structuring in `jobs` forked children, without multiprocessing (agent L 2026-11-23, Android: the player's
    first-start translation on the device, 189 s -> ~1/jobs). Each child inherits the analysis already loaded,
    takes every jobs-th function (sizes mixed evenly) and sends its texts back through a pipe, pickled."""
    import pickle
    by_address = {f["addr"]: f for f in functions}
    children = []
    for k in range(jobs):
        part = work[k::jobs]
        read_fd, write_fd = os.pipe()
        pid = os.fork()
        if pid == 0:                                    # the child
            os.close(read_fd)
            code = 0
            try:
                out = {a: _structured(by_address[a], image, base, strings, form) for a, form in part}
                with os.fdopen(write_fd, "wb") as pipe:
                    pickle.dump(out, pipe, protocol=pickle.HIGHEST_PROTOCOL)
            except BaseException as error:              # (reported by the parent: a missing part)
                sys.stderr.write("translate: worker %d failed: %r\n" % (k, error))
                code = 1
            finally:
                os._exit(code)
        os.close(write_fd)
        children.append((pid, read_fd, len(part)))
    chosen, failed = {}, 0
    for pid, read_fd, count in children:
        with os.fdopen(read_fd, "rb") as pipe:
            data = pipe.read()
        _, status = os.waitpid(pid, 0)
        if status != 0 or not data:
            failed += 1
            continue
        chosen.update(pickle.loads(data))
    if failed:
        raise SystemExit("translate: %d of %d workers failed" % (failed, jobs))
    return chosen


def translate(game, forms_path: str, out_dir: str, jobs: int) -> dict:
    started = time.time()
    with open(forms_path, encoding="utf-8") as handle:
        forms = {int(k, 16): v for k, v in json.load(handle)["forms"].items()}
    functions, image, base, strings = _setup(game)
    stage = time.time()
    plain, refused = lift_tool.lift_all([f for f in functions if f["addr"] in forms], image, base)
    lifted_s = time.time() - stage
    stage = time.time()
    chosen = {a: plain[a] for a, form in forms.items() if form == "plain" and a in plain}
    work = sorted((a, form) for a, form in forms.items() if form != "plain")
    if jobs > 1 and len(work) >= 200 and _have_multiprocessing():  # (a few functions: one process, no start-up)
        import multiprocessing
        with multiprocessing.Pool(jobs, _worker_init, (game.directory,)) as pool:
            for address, text in pool.imap_unordered(_worker_structure, work, chunksize=32):
                chosen[address] = text
    elif jobs > 1 and len(work) >= 200 and hasattr(os, "fork"):
        chosen.update(_structure_forked(functions, image, base, strings, work, jobs))
    else:
        by_address = {f["addr"]: f for f in functions}
        for address, form in work:
            chosen[address] = _structured(by_address[address], image, base, strings, form)
    structured_s = time.time() - stage
    stage = time.time()
    paths = lift_tool.write_chunks(chosen, out_dir, "lifted")
    written_s = time.time() - stage
    times = {"functions": len(chosen), "lift_s": round(lifted_s, 1), "structure_s": round(structured_s, 1),
             "write_s": round(written_s, 1), "total_s": round(time.time() - started, 1), "files": len(paths)}
    print("translated %(functions)d functions into %(files)d files: lift %(lift_s)ss, structure %(structure_s)ss, "
          "write %(write_s)ss, %(total_s)ss in all" % times)
    return times


def check(game, out_dir: str) -> int:
    """The translated source against the developer's: the same functions, the same text but comments."""
    mine = emitted_functions(out_dir)
    theirs = emitted_functions(game.path(os.path.join(os.path.dirname(game.generated), "lifted")))
    differ = [a for a in theirs if a not in mine or _normal(mine[a]) != _normal(theirs[a])]
    extra = [a for a in mine if a not in theirs]
    print("check: %d functions, %d differ, %d missing, %d extra%s"
          % (len(theirs), len([a for a in differ if a in mine]), len([a for a in differ if a not in mine]),
             len(extra), (" (first: %s)" % ", ".join("0x%08X" % a for a in differ[:8])) if differ else ""))
    return 0 if not differ and not extra else 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--forms", help="the forms table (developer's, shipped with the port)")
    parser.add_argument("--derive-forms", metavar="OUT", help="write the forms table from the emitted src/lifted")
    parser.add_argument("--out", help="where to write the translated source (default the game's src/lifted)")
    parser.add_argument("--check", action="store_true", help="compare --out with the game's src/lifted")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2),
                        help="processes structuring in parallel (default: all but two)")
    args = parser.parse_args(argv)
    game = game_config.from_args(args)
    if args.derive_forms:
        return derive_forms(game, args.derive_forms)
    if not args.forms:
        parser.error("--forms or --derive-forms")
    out = args.out or game.path(os.path.join(os.path.dirname(game.generated), "lifted"))
    translate(game, args.forms, out, args.jobs)
    return check(game, out) if args.check else 0


if __name__ == "__main__":
    raise SystemExit(main())
