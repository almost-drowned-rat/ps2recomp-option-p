#!/usr/bin/env python3
"""A PS2 port's release: the launcher with its payload (launcher/launcher.h) - built from the developer's real
llvm-mingw build of the port, so the player's compile is that build's.

    python engine/tools/player_release.py --game games/zoe_us --release v0.1.0 --launcher <launcher.exe>
        --out <folder> --mingw <slim llvm-mingw zip> --python <python-3.12-embed-amd64.zip>
        [--steps port/release/player_steps.json] [--script tools/m3/zoe_dat.py ...] [--game-exe ZOE.exe]

The payload (a zip appended to the launcher, the Xbox launcher's trailer) holds no game code or data:
* engine/  the toolkit's Python (ps2decomp, tools) - the translator;
* game/    the game's metadata (player_package.py);
* <script> the port's own scripts its steps run (tools/m3/zoe_dat.py, ...), at the same relative paths;
* include/ the headers the translated code is compiled against; lib/ the engine's libraries and the port's own
           objects (main, overrides), prebuilt with llvm-mingw;
* python/  the embedded Python; bundle/ the compiler (a zip, unpacked once at the first start);
* lift.txt build.txt toolchain.txt release.txt, LICENSES/.
The compile and link lines are read from the build's build.ninja (the port's target), its paths mapped into the
payload. Nothing is downloaded.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
sys.path[:0] = [HERE, ENGINE]

from ps2decomp import game as game_config  # noqa: E402

TRAILER_MAGIC = b"XRPAYLD1"


def ninja_block(text: str, marker: str) -> dict:
    """The variables of the first build statement whose outputs contain marker."""
    flat = re.sub(r"\$\r?\n\s*", "", text)
    for match in re.finditer(r"^build ([^\n]*)\n((?:  [^\n]*\n)*)", flat, re.M):
        if marker in match.group(1).split(":")[0]:
            values = {}
            for line in match.group(2).splitlines():
                key, _, value = line.strip().partition(" = ")
                values[key] = value
            return values
    raise SystemExit("build.ninja: no build statement for %s" % marker)


def ninja_split(value: str) -> list:
    """A command line fragment split as the shell would (Ninja writes it for cmd: quotes and backslashes)."""
    out, cur, quoted, i = [], "", False, 0
    while i < len(value):
        ch = value[i]
        if ch == "\\" and i + 1 < len(value) and value[i + 1] == '"':
            cur += '"'
            i += 2
            continue
        if ch == '"':
            quoted = not quoted
        elif ch == " " and not quoted:
            if cur:
                out.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    if cur:
        out.append(cur)
    return out


def make_payload(args, game, stage: str) -> None:
    tree = os.path.abspath(args.tree or game.path(os.path.join(game.build, "port", "cmake-mingw")))
    target = os.path.basename(os.path.normpath(game.directory))
    ninja = open(os.path.join(tree, "build.ninja"), encoding="utf-8").read()
    compile_vars = ninja_block(ninja, "/generated/dispatch.cpp.obj")
    link_vars = ninja_block(ninja, target + ".exe")

    # headers: each include folder of the compile line, copied (headers only) to include/<n>
    cc = ninja_split(compile_vars["FLAGS"]) + ninja_split(compile_vars["DEFINES"])
    for n, inc in enumerate(ninja_split(compile_vars.get("INCLUDES", ""))):
        source = inc[2:] if inc.startswith("-I") else inc
        dest = os.path.join(stage, "include", str(n))
        for path in glob.glob(os.path.join(source, "**", "*"), recursive=True):
            if os.path.isfile(path) and os.path.splitext(path)[1].lower() in (".h", ".hpp", ".inc", ".inl"):
                rel = os.path.relpath(path, source)
                os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
                shutil.copyfile(path, os.path.join(dest, rel))
        cc.append("-I{TOOLS}/include/%d" % n)

    # the prebuilt part: the port target's own objects that are not translated or generated code, the libraries
    lib = os.path.join(stage, "lib")
    os.makedirs(lib, exist_ok=True)
    statement = re.search(r"^build %s\.exe: \S+ ([^\n|]*)" % re.escape(target), re.sub(r"\$\r?\n\s*", "", ninja), re.M)
    objects = []
    for obj in statement.group(1).split():
        obj = obj.replace("$:", ":")
        if "/generated/" in obj or "/src/lifted/" in obj:
            continue
        name = os.path.basename(obj).replace(".cpp.obj", ".o")
        shutil.copyfile(os.path.join(tree, obj), os.path.join(lib, name))
        objects.append(name)
    link = ninja_split(link_vars.get("FLAGS", "")) + ninja_split(link_vars.get("LINK_FLAGS", "")) + ["{OBJECTS}"]
    for item in ninja_split(link_vars["LINK_LIBRARIES"]):
        if item.endswith(".a") and not item.startswith("-"):
            shutil.copyfile(os.path.join(tree, item), os.path.join(lib, os.path.basename(item)))
            link.append("{TOOLS}/lib/" + os.path.basename(item))
        else:
            link.append(item)
    link += ["-o", "{OUT}"]
    with open(os.path.join(stage, "build.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# the port's compile and link, from its llvm-mingw build (player_release.py)\n")
        handle.write("\t".join(["cc"] + cc) + "\n")
        handle.write("glob\t{WORK}/game/src/lifted\t.cpp\n")
        handle.write("glob\t{WORK}/game/build/port/generated\t.cpp\n")
        for name in objects:
            handle.write("obj\t{TOOLS}/lib/%s\n" % name)
        handle.write("\t".join(["link"] + link) + "\n")

    # the translator: the toolkit's Python, the game's metadata, the port's scripts
    for part in ("ps2decomp", "tools"):
        shutil.copytree(os.path.join(ENGINE, part), os.path.join(stage, "engine", part),
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    import player_package
    player_package.main(["--game", game.directory, "--out", stage] + (["--steps", args.steps] if args.steps else []))
    root = os.path.dirname(os.path.dirname(os.path.abspath(game.directory)))     # the project (games/<name>/..)
    for script in args.script:
        os.makedirs(os.path.dirname(os.path.join(stage, script)), exist_ok=True)
        shutil.copyfile(os.path.join(root, script), os.path.join(stage, script))
    with open(os.path.join(stage, "lift.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# the translation (player_build.py): check = the disc only (exit 3: not the game's)\n")
        handle.write("check\t{TOOLS}/engine/tools/player_build.py\t--package\t{TOOLS}/game\t--iso\t{ISO}\t"
                     "--check-only\n")
        handle.write("translate\t{TOOLS}/engine/tools/player_build.py\t--package\t{TOOLS}/game\t--iso\t{ISO}\t"
                     "--work\t{WORK}\t--jobs\t{JOBS}\n")

    # Python and the compiler
    with zipfile.ZipFile(args.python) as z:
        z.extractall(os.path.join(stage, "python"))
    pth = glob.glob(os.path.join(stage, "python", "python3*._pth"))
    os.makedirs(os.path.join(stage, "bundle"), exist_ok=True)
    zip_name = os.path.basename(args.mingw)
    shutil.copyfile(args.mingw, os.path.join(stage, "bundle", zip_name))
    with zipfile.ZipFile(args.mingw) as z:
        top = {n.split("/", 1)[0] for n in z.namelist() if n}.pop()
    digest = hashlib.sha256(open(args.mingw, "rb").read()).hexdigest()
    with open(os.path.join(stage, "toolchain.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("format\t2\n")
        # (the slim zip keeps clang++.exe only; its default target is x86_64-w64-windows-gnu, its .cfg loaded)
        handle.write("clang\tllvm-mingw/bin/clang++.exe\n")
        handle.write("python\tpython/python.exe\n")
        handle.write("bundle\tbundle/%s\t%s\t%d\t%s\tllvm-mingw\n" % (zip_name, digest, os.path.getsize(args.mingw), top))
    with open(os.path.join(stage, "release.txt"), "w", encoding="utf-8") as handle:
        handle.write(args.release)
    licences = os.path.join(ENGINE, "LICENSES")
    if os.path.isdir(licences):
        shutil.copytree(licences, os.path.join(stage, "LICENSES"))
    print("payload staged in %s (python ._pth: %s)" % (stage, [os.path.basename(p) for p in pth]))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--release", required=True)
    parser.add_argument("--launcher", required=True, help="the built launcher exe (launcher/CMakeLists.txt)")
    parser.add_argument("--out", required=True, help="the release folder (written anew)")
    parser.add_argument("--mingw", required=True, help="the slim llvm-mingw zip the player compiles with")
    parser.add_argument("--python", required=True, help="the embeddable Python zip")
    parser.add_argument("--tree", help="the port's llvm-mingw build folder (default build/port/cmake-mingw)")
    parser.add_argument("--steps", help="the game's player steps (player_package.py --steps)")
    parser.add_argument("--script", action="append", default=[], help="a port script its steps run (repeat)")
    parser.add_argument("--game-exe", help="the launcher's exe name in the release (default <game>.exe)")
    args = parser.parse_args(argv)
    if len(args.release.encode()) > 32:
        parser.error("--release: at most 32 bytes")
    game = game_config.from_args(args)
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    stage = os.path.join(args.out, "_payload")
    os.makedirs(stage)
    make_payload(args, game, stage)
    payload = os.path.join(args.out, "_payload.zip")
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(glob.glob(os.path.join(stage, "**", "*"), recursive=True)):
            if os.path.isfile(path):
                rel = os.path.relpath(path, stage).replace("\\", "/")
                # the compiler zip is already compressed
                z.write(path, rel, zipfile.ZIP_STORED if rel.startswith("bundle/") else zipfile.ZIP_DEFLATED)
    exe = os.path.join(args.out, args.game_exe or "launcher.exe")
    with open(exe, "wb") as out:
        out.write(open(args.launcher, "rb").read())
        offset = out.tell()
        with open(payload, "rb") as handle:
            shutil.copyfileobj(handle, out, 1 << 20)
        out.write(TRAILER_MAGIC + struct.pack("<Q", offset) + args.release.encode().ljust(32, b"\0"))
    shutil.rmtree(stage)
    os.remove(payload)
    print("release %s: %s, %.1f MB" % (args.release, exe, os.path.getsize(exe) / 2 ** 20))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
