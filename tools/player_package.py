#!/usr/bin/env python3
"""The player package's game part (design 7d, M7 launcher): what a player's PC needs to build the port from his
own disc - metadata only, no game code or data.

    python engine/tools/player_package.py --game games/zoe_us --out DIR [--steps port/release/player_steps.json]

Writes DIR/game, a game folder with only:

* game.json (no disc path) and each overlay's game.json (their code ranges);
* build/functions.json - the analysis: functions, regions; the strings by address only (their text is read
  back from the player's executable: player_build.py);
* build/forms.json - which form each function took in the verified lift (translate.py --derive-forms);
* build/vu_refs.txt - each microprogram a run recorded, as references into the disc's files (file, offset,
  pairs), not its words (vurecomp's manifest is written from them on the player's PC);
* build/port/discovered.txt - the run addresses of code found at run time (addresses only);
* config/names.csv where the game has one;
* player.json - the executable's SHA-1 (the player's disc must hold the same one) and the game's own extra
  steps (--steps: a game's extraction of run-time code from its archives, say) for player_build.py.

Every recorded microprogram must be found on the disc, or the package is refused (a program only a run has would
have to ship as code).
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

from ps2decomp import game as game_config  # noqa: E402

import translate  # noqa: E402
import vurecomp  # noqa: E402


def strip_analysis(source: str, target: str) -> dict:
    """The analysis without text from the executable: strings keep their address, functions lose their string
    hints, cross-references (not needed to translate) go."""
    with open(source, encoding="utf-8") as handle:
        database = json.load(handle)
    database["strings"] = [{"addr": entry["addr"]} for entry in database.get("strings", [])]
    for func in database["functions"]:
        func["string_hint"] = ""
    database["xrefs"] = []
    database.pop("renderware", None)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(database, handle, separators=(",", ":"))
    return database


def disc_files(game) -> list:
    """The disc's files a recorded microprogram is looked for in (relative to the game folder): the executable,
    then what the game's extraction steps put in extracted/ (its stage files, its IOP modules)."""
    extracted = game.path(game.extracted)
    files = [os.path.join(game.extracted, game.executable)]
    for path in sorted(glob.glob(os.path.join(extracted, "**", "*"), recursive=True)):
        rel = os.path.relpath(path, game.directory).replace("\\", "/")
        if os.path.isfile(path) and rel != files[0] and "/overlays/" not in "/" + rel:
            files.append(rel)
    return [f.replace("\\", "/") for f in files]


def _find_pieces(data: bytes, blobs: dict):
    """data as pieces of whole pairs found in the files: [(file, offset, pairs)], longest first; None if a part
    is on no file."""
    out, at = [], 0
    while at < len(data):
        best = None
        for n in range(len(data) - at, 0, -8):
            chunk = data[at:at + n]
            for name, blob in blobs.items():
                k = blob.find(chunk)
                if k >= 0:
                    best = (name, k, n // 8)
                    break
            if best:
                break
        if not best:
            return None
        out.append(best)
        at += best[2] * 8
    return out


def vu_refs(game, manifest: str, target: str) -> int:
    """build/vu_programs.txt as references (vurecomp.resolve_refs reads them back)."""
    if not os.path.exists(manifest):
        open(target, "w").close()
        return 0
    blobs = {}
    for rel in disc_files(game):
        with open(game.path(rel), "rb") as handle:
            blobs[rel] = handle.read()
    lines, missing = [], []
    with open(manifest, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 3 or not parts[0].startswith("vu"):
                continue
            runs = []
            for run in parts[2].split(";"):
                base, words = run.split(":")
                words = [int(w, 16) for w in words.split(",")]
                pieces = _find_pieces(struct.pack("<%dI" % len(words), *words), blobs)
                if pieces is None:
                    missing.append("%s %s at %s" % (parts[0], parts[1], base))
                    break
                runs.append(base + ":" + ",".join("%s+%X*%d" % p for p in pieces))
            else:
                lines.append("%s %s %s" % (parts[0], parts[1], ";".join(runs)))
    if missing:
        raise SystemExit("microprograms on no disc file (they would have to ship as code): " + ", ".join(missing[:8]))
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(lines)


def sha1(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha1(handle.read()).hexdigest()


def package_part(part, out_dir: str, top: bool) -> None:
    """One game folder (the game, or an overlay) into out_dir."""
    os.makedirs(out_dir, exist_ok=True)
    with open(part.path("game.json"), encoding="utf-8") as handle:
        described = json.load(handle)
    if top:
        described["iso"] = None
    with open(os.path.join(out_dir, "game.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(described, handle, indent=2)
        handle.write("\n")
    strip_analysis(part.database_path, os.path.join(out_dir, part.database))
    forms = os.path.join(out_dir, part.build, "forms.json")
    if translate.derive_forms(part, forms):
        raise SystemExit("%s: functions of no known form - not packaged" % part.directory)
    names = part.path(os.path.join(os.path.dirname(part.status), "names.csv"))
    if os.path.exists(names):
        os.makedirs(os.path.join(out_dir, os.path.dirname(part.status)), exist_ok=True)
        shutil.copyfile(names, os.path.join(out_dir, os.path.dirname(part.status), "names.csv"))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--out", required=True, help="the package's folder (its game/ is written anew)")
    parser.add_argument("--steps", help="the game's own player steps (json list of [script, args...])")
    args = parser.parse_args(argv)
    game = game_config.from_args(args)
    out = os.path.join(args.out, "game")
    if os.path.isdir(out):
        shutil.rmtree(out)
    package_part(game, out, top=True)
    for relative in game.overlays:
        package_part(game_config.load(game.path(relative)), os.path.join(out, relative), top=False)
    os.makedirs(os.path.join(out, game.build, "port"), exist_ok=True)
    count = vu_refs(game, game.path(os.path.join(game.build, "vu_programs.txt")),
                    os.path.join(out, game.build, "vu_refs.txt"))
    discovered = game.path(os.path.join(game.build, "port", "discovered.txt"))
    if os.path.exists(discovered):
        shutil.copyfile(discovered, os.path.join(out, game.build, "port", "discovered.txt"))
    steps = []
    if args.steps:
        with open(game.path(args.steps), encoding="utf-8") as handle:
            steps = json.load(handle)
    player = {"executable": game.executable, "executable_sha1": sha1(game.elf_path),
              "executable_size": os.path.getsize(game.elf_path), "steps": steps}
    with open(os.path.join(out, "player.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(player, handle, indent=2)
        handle.write("\n")
    print("package %s: %d microprograms by reference, %d overlay(s), steps %d" % (out, count, len(game.overlays),
                                                                                  len(steps)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
