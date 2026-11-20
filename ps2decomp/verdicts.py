"""Verification verdicts remembered between runs.

A lifted or structured function is verified by compiling its C++ and running it
against the original on test cases. Its callees run in the interpreter, from the
original machine code, so the verdict depends on nothing but

* the function's own C++ text, and whether it is checked whole or at the
  calling-convention level;
* the environment: the interpreter, the test memory and its seeding, the
  runtime it is compiled with, the verification code, and the game's image
  and analysis.

The text goes into each key; the environment is one fingerprint over the files
that make it up. Change any of them and every verdict is recomputed. Change only
code that produces text - structuring, folding, naming - and only the functions
whose text changed are verified again.

What the key cannot see: the compiler's version, and the order functions run in
within a process (a function needing more fresh pages than
emu.Machine.FAULT_RETRIES can pass after others mapped them). A run with --no-cache recomputes everything
and is the check on both.
"""

from __future__ import annotations

import ast
import hashlib
import json
import os

# Modules that only produce the text being verified: the text is in every key, so
# a change to them invalidates exactly the verdicts whose text changes.
TEXT_ONLY = {"structure.py", "liveness.py", "naming.py", "signatures.py"}

# Modules of the environment with one marked text-only region: the region (from the
# BEGIN line to the END line, both included) is left out of the fingerprint, the rest
# of the file stays in. lift.py's region is the C++ of single instructions; function
# boundaries, jump targets and dispatch stay outside it. A file whose markers are
# missing, doubled or out of order counts whole - when in doubt, not text-only.
PARTLY_TEXT_ONLY = {
    "lift.py": ("# ==== TEXT-ONLY BEGIN", "# ==== TEXT-ONLY END"),
}


def _environment_bytes(path: str) -> bytes:
    """A file's bytes as the fingerprint takes them: line endings made the same, and
    a PARTLY_TEXT_ONLY file's marked region left out."""
    with open(path, "rb") as handle:
        data = handle.read().replace(b"\r\n", b"\n")
    markers = PARTLY_TEXT_ONLY.get(os.path.basename(path))
    if not markers:
        return data
    begin, end = (m.encode() for m in markers)
    lines = data.split(b"\n")
    starts = [i for i, line in enumerate(lines) if line.startswith(begin)]
    ends = [i for i, line in enumerate(lines) if line.startswith(end)]
    if len(starts) != 1 or len(ends) != 1 or ends[0] <= starts[0]:
        return data
    return b"\n".join(lines[:starts[0]] + [b"# (text-only region)"] + lines[ends[0] + 1:])

# Where verification starts; everything they import from the engine is part of
# the environment, and nothing else is - the symbolic deriver, for one.
ROOTS = ("liftverify.py", "testplan.py", "liftcheck.py", "lift.py")


def _imports(path: str) -> set:
    """The engine modules a module imports, as file names."""
    with open(path, encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.level == 1:
            if node.module:
                found.add(node.module.split(".")[0] + ".py")
            else:
                found.update(alias.name + ".py" for alias in node.names)
    return found


def modules(package: str) -> set:
    """The import closure of ROOTS within the package, less TEXT_ONLY."""
    seen, work = set(), list(ROOTS)
    while work:
        name = work.pop()
        path = os.path.join(package, name)
        if name in seen or not os.path.exists(path):
            continue
        seen.add(name)
        work.extend(_imports(path))
    return seen - TEXT_ONLY


def _files(repository: str) -> list:
    engine = os.path.join(repository, "engine")
    package = os.path.join(engine, "ps2decomp")
    files = [os.path.join(package, name) for name in modules(package)]
    # The native core of the interpreter (emu_native.py builds it from native/emu_core.c): it decides
    # verdicts as emu.py does, so a change to it is a change of the environment (2026-11-13).
    for folder, _dirs, names in os.walk(os.path.join(package, "native")):
        files += [os.path.join(folder, name) for name in names if name.endswith((".c", ".h"))]
    # engine/tools/ is left out: lift.py and derive.py only orchestrate. What
    # decides a verdict - liftverify.py, testplan.py - is among the modules above.
    for folder, _dirs, names in os.walk(os.path.join(engine, "runtime")):
        files += [os.path.join(folder, name) for name in names]
    return sorted(files)


def fingerprint(repository: str, inputs=()) -> str:
    """The environment's fingerprint: the source files verification depends on,
    plus `inputs` - bytes or anything with a stable repr (image, analysis, seeds)."""
    digest = hashlib.sha256()
    for path in _files(repository):
        digest.update(os.path.relpath(path, repository).replace(os.sep, "/").encode())
        # Line endings differ between checkouts; the code does not.
        digest.update(_environment_bytes(path))
    for item in inputs:
        digest.update(item if isinstance(item, bytes) else repr(item).encode())
    return digest.hexdigest()


# Verdicts of this many environments are kept: the current one, and a few before
# it - a full run and a subset run seed test memory differently, so they are
# different environments, and switching between them should not lose either.
KEEP_ENVIRONMENTS = 4


class Verdicts:
    """{key: outcome} on disk, per environment; a key covers form and text."""

    def __init__(self, path: str, environment: str, use: bool = True) -> None:
        self.path = path
        self.environment = environment
        self.use = use
        self.hits = self.misses = 0
        self.stored = []                 # [[environment, {key: outcome}]], newest last
        if os.path.exists(path):
            try:
                with open(path, encoding="utf-8") as handle:
                    self.stored = [list(pair) for pair in json.load(handle)]
            except (OSError, ValueError, TypeError):
                self.stored = []
        current = [pair for pair in self.stored if pair[0] == environment]
        self.stored = [pair for pair in self.stored if pair[0] != environment]
        self.entries = current[0][1] if current else {}
        # Safeguard (c): a share of the remembered verdicts verified again (sample()).
        self.sample_share = 0.0
        self._random = None
        self.sampled = {}               # key -> the remembered outcome, until put() compares
        self.sample_count = 0
        self.mismatches = []            # (key, remembered, fresh)
        self.from_cache = set()         # addresses whose verdict was reused (the caller fills it)

    def sample(self, share: float, seed) -> None:
        """From now on, a remembered verdict is handed out only with probability
        1 - share; otherwise get() answers None, as if unknown, and the fresh
        outcome put() later receives is compared with the remembered one."""
        import random
        self.sample_share = share
        self._random = random.Random(seed)

    def key(self, text: str, abi: bool) -> str:
        digest = hashlib.sha256(self.environment.encode())
        digest.update(b"abi" if abi else b"whole")
        digest.update(text.encode("utf-8"))
        return digest.hexdigest()

    def get(self, text: str, abi: bool):
        """The remembered outcome, or None (also when not using remembered ones,
        and for a remembered one picked for the sample)."""
        key = self.key(text, abi)
        outcome = self.entries.get(key) if self.use else None
        if outcome is not None and self.sample_share and self._random.random() < self.sample_share:
            self.sampled[key] = outcome
            self.sample_count += 1
            outcome = None
        if outcome is None:
            self.misses += 1
        else:
            self.hits += 1
        return outcome

    def put(self, text: str, abi: bool, outcome: str) -> None:
        key = self.key(text, abi)
        if key in self.sampled:
            remembered = self.sampled.pop(key)
            if remembered != outcome:
                self.mismatches.append((key, remembered, outcome))
        self.entries[key] = outcome

    def save(self) -> None:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        kept = self.stored[-(KEEP_ENVIRONMENTS - 1):] + [[self.environment, self.entries]]
        temporary = self.path + ".tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(kept, handle)
        os.replace(temporary, self.path)


# ---------------------------------------------------------------- safeguards
# Partial verification (only changed texts verified again) is allowed only with these
# safeguards (maintainer's order 2026-11-06), for every Option P game:
#   (a) a fault traced to a function whose verdict came from the cache -> the next run
#       of that version is a full run;
#   (b) FAULTS_FOR_FULL unexplained faults (crash, hang, replay mismatch vs tier E)
#       since the last full run -> full run;
#   (c) every partial run verifies SAMPLE_SHARE of the reused verdicts again; any
#       mismatch -> a full run at once, and the change that let it through is to be
#       reviewed (its text-only classification);
#   (d) a full run before every gate and release (lift.py --gate).
# They cannot be switched off, except --no-safeguards for tests of the cache itself,
# which every run says loudly and the run log records.
SAMPLE_SHARE = 0.05
FAULTS_FOR_FULL = 2


class Safeguards:
    """The per-version state of the safeguards (<build>/verify_state.json) and the run
    log (<build>/verify_runs.log): one line a run - kind, trigger, counts, sample,
    duration."""

    def __init__(self, folder: str) -> None:
        self.state_path = os.path.join(folder, "verify_state.json")
        self.log_path = os.path.join(folder, "verify_runs.log")
        self.state = {"last_full": None, "faults": [], "force_full": None, "from_cache": []}
        if os.path.exists(self.state_path):
            try:
                with open(self.state_path, encoding="utf-8") as handle:
                    self.state.update(json.load(handle))
            except (OSError, ValueError, TypeError):
                # A state that cannot be read cannot vouch for the cache.
                self.state["force_full"] = "the safeguard state could not be read"

    def settings(self) -> str:
        return ("safeguards: sample %d%% of reused verdicts, full run after %d unexplained faults "
                "or one traced to a reused verdict, full run before gates/releases (--gate)"
                % (round(SAMPLE_SHARE * 100), FAULTS_FOR_FULL))

    def decide(self, no_cache: bool, gate: bool, environment_changed: bool):
        """(kind, trigger): "full" or "partial", and why."""
        if no_cache:
            return "full", "--no-cache"
        if gate:
            return "full", "gate or release (--gate)"
        if self.state.get("force_full"):
            return "full", self.state["force_full"]
        if not self.state.get("last_full"):
            return "full", "no full run recorded for this version yet"
        if environment_changed:
            return "full", "the environment changed (verification code, runtime, image or analysis)"
        return "partial", "only changed texts (cache reused, %d%% sampled)" % round(SAMPLE_SHARE * 100)

    def record_fault(self, description: str, function=None) -> str:
        """A fault seen in a run of this version: traced to `function` (an address)
        or unexplained. Returns what follows from it."""
        from_cache = set(self.state.get("from_cache", ()))
        self.state.setdefault("faults", []).append(
            {"what": description, "function": None if function is None else "0x%08X" % function})
        if function is not None and function in from_cache:
            self.state["force_full"] = ("fault traced to 0x%08X, whose verdict came from the cache: %s"
                                        % (function, description))
        elif function is None:
            unexplained = [f for f in self.state["faults"] if f["function"] is None]
            if len(unexplained) >= FAULTS_FOR_FULL:
                self.state["force_full"] = "%d unexplained faults since the last full run" % len(unexplained)
        self._save()
        return self.state.get("force_full") or "recorded; no full run needed yet"

    def finish(self, kind: str, trigger: str, verified: int, reused: int, sampled: int,
               mismatches: int, seconds: float, from_cache, safeguards_off: bool = False,
               counts_as_full: bool = True) -> None:
        """One run done: a full run (of every function, safeguards on) resets the fault
        count and any forced full run; the reused functions are remembered for (a)."""
        import time
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        if kind == "full" and counts_as_full and not safeguards_off:
            self.state["last_full"] = stamp
            self.state["faults"] = []
            self.state["force_full"] = None
        if counts_as_full:                  # a subset run (--only, --limit) saw only some functions
            self.state["from_cache"] = sorted(from_cache)
        self._save()
        line =("%s | %s | trigger: %s | verified afresh %d, reused %d | sample %d, mismatches %d | %.0f s%s\n"
                % (stamp, kind, trigger, verified, reused, sampled, mismatches, seconds,
                   " | SAFEGUARDS OFF (--no-safeguards, cache test)" if safeguards_off else ""))
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        with open(self.log_path, "a", encoding="utf-8") as handle:
            handle.write(line)

    def force_full(self, reason: str) -> None:
        self.state["force_full"] = reason
        self._save()

    def _save(self) -> None:
        os.makedirs(os.path.dirname(self.state_path), exist_ok=True)
        temporary = self.state_path + ".tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(self.state, handle, indent=1)
        os.replace(temporary, self.state_path)
