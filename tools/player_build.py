#!/usr/bin/env python3
"""The player's PC: the port's sources from his own disc (design 7d, M7 launcher) - the package's game folder
(player_package.py) plus his disc image in, the sources the compiler needs out, in minutes, nothing verified
(the developer's lift was).

    python engine/tools/player_build.py --package DIR/game --iso <his image> --work <folder>

In <folder>/game, a copy of the package's game folder:
1. the executable and the IOP modules taken from the image - the executable checked to be the one the package
   was made from (SHA-1);
2. the analysis's strings read back from that executable (the package has their addresses only);
3. the game's own steps (player.json "steps": its run-time code from its archives, say), {ISO} {GAME} filled in;
4. the recorded microprograms' words read from the disc's files (build/vu_refs.txt -> build/vu_programs.txt);
5. the translation (translate.py, the package's forms tables) of the game and of each overlay;
6. port.py's generation: the dispatch tables, the code found at run time, the VU and IOP code.

The compiler then builds src/lifted/*.cpp and build/port/generated/*.cpp (the overlays' sources are included
through the generated ones). Exit code 0 when all of it is there.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

from ps2decomp import game as game_config  # noqa: E402
from ps2decomp.iso9660 import Iso9660  # noqa: E402


WRONG_DISC = 3          # the exit code the launcher tells apart: not the game's disc (or another version)


def say(text: str) -> None:
    print("[player_build] %s" % text, flush=True)


def extract(game, iso_path: str, player: dict) -> None:
    extracted = game.path(game.extracted)
    os.makedirs(extracted, exist_ok=True)
    with Iso9660.open(iso_path) as iso:
        for name in [game.executable] + list(game.iop_modules):
            target = os.path.join(extracted, *name.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            iso.extract_file(iso.find(name), target)
    with open(game.elf_path, "rb") as handle:
        digest = hashlib.sha1(handle.read()).hexdigest()
    if digest != player["executable_sha1"]:
        print("this disc's %s is not the one the port was made from (SHA-1 %s, expected %s): "
              "another version or a modified image" % (game.executable, digest, player["executable_sha1"]))
        raise SystemExit(WRONG_DISC)


def check_disc(iso_path: str, player: dict) -> int:
    """0 when the image boots the package's executable, byte for byte; WRONG_DISC when not (the launcher asks for
    another image)."""
    try:
        with Iso9660.open(iso_path) as iso:
            data = iso.read_file(iso.find(player["executable"]))
    except Exception as exc:                # not an ISO 9660 image, or no such file on it
        print("%s: %s" % (iso_path, exc))
        return WRONG_DISC
    if hashlib.sha1(data).hexdigest() != player["executable_sha1"]:
        print("%s: %s is another version (SHA-1 %s)" % (iso_path, player["executable"], hashlib.sha1(data).hexdigest()))
        return WRONG_DISC
    return 0


def _read_string(sections, address: int) -> str | None:
    """The NUL-terminated text at address in the executable's sections (analyze's rule: printable ASCII)."""
    for data, base in sections:
        if base <= address < base + len(data):
            end = data.find(b"\0", address - base)
            if end < 0:
                return None
            return data[address - base:end].decode("ascii", "replace")
    return None


def refill_strings(part) -> int:
    """The analysis's strings' text, from the player's executable (an overlay's: from its own image)."""
    with open(part.database_path, encoding="utf-8") as handle:
        database = json.load(handle)
    elf = part.load_elf()
    sections = [(elf.section_data(s), s.addr) for s in elf.sections if s.is_alloc and s.size and s.type != 8]
    kept = []
    for entry in database.get("strings", []):
        text = _read_string(sections, entry["addr"])
        if text is not None:
            kept.append({"addr": entry["addr"], "text": text})
    database["strings"] = kept
    with open(part.database_path, "w", encoding="utf-8") as handle:
        json.dump(database, handle, separators=(",", ":"))
    return len(kept)


def resolve_vu(game) -> int:
    """build/vu_refs.txt -> build/vu_programs.txt (vurecomp's manifest): each piece's words from its file."""
    refs = game.path(os.path.join(game.build, "vu_refs.txt"))
    target = game.path(os.path.join(game.build, "vu_programs.txt"))
    if not os.path.exists(refs):
        return 0
    blobs, lines = {}, []
    with open(refs, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 3:
                continue
            runs = []
            for run in parts[2].split(";"):
                base, pieces = run.split(":")
                words = []
                for piece in pieces.split(","):
                    name, rest = piece.rsplit("+", 1)
                    offset, pairs = rest.split("*")
                    if name not in blobs:
                        with open(game.path(name), "rb") as blob:
                            blobs[name] = blob.read()
                    offset, pairs = int(offset, 16), int(pairs)
                    words += struct.unpack_from("<%dI" % (2 * pairs), blobs[name], offset)
                runs.append(base + ":" + ",".join("%X" % w for w in words))
            lines.append("%s %s %s" % (parts[0], parts[1], ";".join(runs)))
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(lines)


def run_steps(game, iso_path: str, player: dict, package_tools: str) -> None:
    for step in player.get("steps", []):
        args = [a.replace("{ISO}", iso_path).replace("{GAME}", game.directory) for a in step]
        script = os.path.join(package_tools, args[0])
        say("step %s" % args[0])
        result = subprocess.run([sys.executable, "-I", "-B", script] + args[1:], cwd=package_tools)
        if result.returncode:
            raise SystemExit("step %s failed (exit %d)" % (args[0], result.returncode))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--package", required=True, help="the package's game folder (player_package.py)")
    parser.add_argument("--iso", required=True, help="the player's disc image")
    parser.add_argument("--work", help="where the game folder is built (written anew)")
    parser.add_argument("--check-only", action="store_true",
                        help="only check the image is the game's disc (exit 0, or 3: not it)")
    parser.add_argument("--tools", help="the folder the game's steps' scripts are relative to "
                                        "(default: the package's folder, the one above its game folder)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    args = parser.parse_args(argv)
    started = time.time()
    iso_path = os.path.abspath(args.iso)
    if args.check_only:
        with open(os.path.join(args.package, "player.json"), encoding="utf-8") as handle:
            return check_disc(iso_path, json.load(handle))
    if not args.work:
        parser.error("--work is needed")
    work = os.path.join(os.path.abspath(args.work), "game")
    # the player's image is read only, wherever it lies (Orta GitHub #6, 2026-11-17: an installer deleted a player's
    # only image inside its folder): an image inside the folder about to be written anew stops the build instead
    try:
        inside = os.path.normcase(os.path.commonpath([work, iso_path])) == os.path.normcase(work)
    except ValueError:   # another drive
        inside = False
    if inside:
        print(f"player_build: the disc image {iso_path} lies inside {work}, which is built anew - move the image "
              f"elsewhere; it is never deleted", file=sys.stderr)
        return 2
    if os.path.isdir(work):
        shutil.rmtree(work)
    shutil.copytree(args.package, work)
    with open(os.path.join(work, "player.json"), encoding="utf-8") as handle:
        player = json.load(handle)
    with open(os.path.join(work, "game.json"), encoding="utf-8") as handle:
        described = json.load(handle)
    described["iso"] = iso_path
    with open(os.path.join(work, "game.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(described, handle, indent=2)
        handle.write("\n")
    game = game_config.load(work)
    say("1. the executable from %s" % iso_path)
    extract(game, iso_path, player)
    run_steps(game, iso_path, player, os.path.abspath(args.tools or os.path.join(args.package, "..")))
    game = game_config.load(work)             # (the steps may have made the overlays' images)
    say("2. strings: %d" % refill_strings(game))
    for relative in game.overlays:
        part = game_config.load(game.path(relative))
        say("   overlay %s strings: %d" % (relative, refill_strings(part)))
    say("4. microprograms: %d" % resolve_vu(game))
    import translate
    for part in [game] + [game_config.load(game.path(r)) for r in game.overlays]:
        out = part.path(os.path.join(os.path.dirname(part.generated), "lifted"))
        say("5. translating %s" % os.path.relpath(part.directory, work))
        translate.translate(part, part.path(os.path.join(part.build, "forms.json")), out, args.jobs)
    import port
    say("6. generating")
    port.generate(game)
    say("done in %d s: %s" % (time.time() - started, work))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
