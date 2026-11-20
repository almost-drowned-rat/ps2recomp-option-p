"""Remembered verification verdicts: a verdict is reused only for the same text,
form and environment."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import verdicts  # noqa: E402

REPOSITORY = os.path.join(os.path.dirname(__file__), "..", "..")


class TestVerdicts(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.path = os.path.join(self.folder, "verdicts.json")

    def test_same_text_form_and_environment_is_remembered(self):
        cache = verdicts.Verdicts(self.path, "env")
        cache.put("void F() {}", False, "verified")
        cache.save()
        again = verdicts.Verdicts(self.path, "env")
        self.assertEqual(again.get("void F() {}", False), "verified")

    def test_other_text_form_or_environment_is_not(self):
        cache = verdicts.Verdicts(self.path, "env")
        cache.put("void F() {}", False, "verified")
        cache.save()
        again = verdicts.Verdicts(self.path, "env")
        self.assertIsNone(again.get("void F() { }", False))
        self.assertIsNone(again.get("void F() {}", True))
        self.assertIsNone(verdicts.Verdicts(self.path, "other").get("void F() {}", False))

    def test_no_cache_ignores_but_still_records(self):
        cache = verdicts.Verdicts(self.path, "env")
        cache.put("t", False, "verified")
        cache.save()
        fresh = verdicts.Verdicts(self.path, "env", use=False)
        self.assertIsNone(fresh.get("t", False))
        fresh.put("t", False, "disagrees: memory")
        fresh.save()
        self.assertEqual(verdicts.Verdicts(self.path, "env").get("t", False), "disagrees: memory")

    def test_a_few_environments_are_kept(self):
        for index in range(verdicts.KEEP_ENVIRONMENTS + 2):
            cache = verdicts.Verdicts(self.path, "env%d" % index)
            cache.put("t", False, str(index))
            cache.save()
        last = verdicts.KEEP_ENVIRONMENTS + 1
        self.assertEqual(verdicts.Verdicts(self.path, "env%d" % last).get("t", False), str(last))
        self.assertEqual(verdicts.Verdicts(self.path, "env%d" % (last - 1)).get("t", False),
                         str(last - 1))
        self.assertIsNone(verdicts.Verdicts(self.path, "env0").get("t", False))

    def test_fingerprint_follows_inputs_not_text_only_modules(self):
        base = verdicts.fingerprint(REPOSITORY, (b"image",))
        self.assertEqual(base, verdicts.fingerprint(REPOSITORY, (b"image",)))
        self.assertNotEqual(base, verdicts.fingerprint(REPOSITORY, (b"other image",)))
        names = {os.path.basename(path) for path in verdicts._files(REPOSITORY)}
        self.assertIn("emu.py", names)
        self.assertIn("lifted.h", names)
        self.assertFalse(names & verdicts.TEXT_ONLY)

    def test_the_symbolic_deriver_is_not_part_of_the_environment(self):
        # Lift verification never runs it; a change to it must not cost a cold run.
        package = os.path.join(REPOSITORY, "engine", "ps2decomp")
        names = verdicts.modules(package)
        self.assertIn("liftverify.py", names)
        self.assertIn("testplan.py", names)
        self.assertFalse(names & {"paths.py", "symexec.py", "calls.py"})


class TestPartialVerification(unittest.TestCase):
    """The text-only region of lift.py, the sample, and the safeguards (2026-11-06 order)."""

    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.path = os.path.join(self.folder, "verdicts.json")

    def _lift_py(self, inside: str, outside: str) -> str:
        path = os.path.join(self.folder, "lift.py")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("import x\n# ==== TEXT-ONLY BEGIN\ndef statement(): return %r\n"
                         "# ==== TEXT-ONLY END\ndef lift_function(): return %r\n" % (inside, outside))
        return path

    def test_a_change_inside_the_text_only_region_keeps_the_environment(self):
        before = verdicts._environment_bytes(self._lift_py("a + b", "boundaries"))
        self.assertEqual(before, verdicts._environment_bytes(self._lift_py("a - b", "boundaries")))
        self.assertNotEqual(before, verdicts._environment_bytes(self._lift_py("a + b", "other boundaries")))

    def test_broken_markers_count_the_whole_file(self):
        path = os.path.join(self.folder, "lift.py")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("# ==== TEXT-ONLY BEGIN\nx = 1\n")        # no END: when in doubt, not text-only
        with open(path, "rb") as handle:
            self.assertEqual(verdicts._environment_bytes(path), handle.read())

    def test_the_real_lift_py_has_one_region(self):
        path = os.path.join(REPOSITORY, "engine", "ps2decomp", "lift.py")
        if not os.path.exists(path):
            path = os.path.join(os.path.dirname(__file__), "..", "ps2decomp", "lift.py")
        reduced = verdicts._environment_bytes(path)
        self.assertIn(b"# (text-only region)", reduced)
        self.assertNotIn(b"def statement(", reduced)          # the instruction text is in the region
        self.assertIn(b"def lift_function(", reduced)         # function assembly is not

    def test_a_real_text_only_change_reverifies_only_the_changed_function(self):
        cache = verdicts.Verdicts(self.path, "env")
        cache.put("void A() { x + y; }", False, "verified")
        cache.put("void B() { z; }", False, "verified")
        cache.save()
        again = verdicts.Verdicts(self.path, "env")
        self.assertIsNone(again.get("void A() { x - y; }", False))     # changed text: verified afresh
        self.assertEqual(again.get("void B() { z; }", False), "verified")
        self.assertEqual((again.hits, again.misses), (1, 1))

    def test_a_misclassified_change_is_caught_by_the_sample(self):
        # A change that alters behaviour without altering texts (wrongly marked text-only):
        # every remembered verdict is now wrong. The 5% sample must see it.
        cache = verdicts.Verdicts(self.path, "env")
        texts = ["void F%d() {}" % i for i in range(400)]
        for text in texts:
            cache.put(text, False, "verified")
        cache.save()
        again = verdicts.Verdicts(self.path, "env")
        again.sample(verdicts.SAMPLE_SHARE, 12345)
        for text in texts:
            if again.get(text, False) is None:
                again.put(text, False, "disagrees: register 2")     # what verifying afresh finds
        self.assertGreater(again.sample_count, 5)
        self.assertEqual(len(again.mismatches), again.sample_count)

    def test_a_sample_that_agrees_finds_nothing(self):
        cache = verdicts.Verdicts(self.path, "env")
        for i in range(200):
            cache.put("t%d" % i, False, "verified")
        again = verdicts.Verdicts(self.path, "env")
        again.entries = cache.entries
        again.sample(1.0, 1)
        for i in range(200):
            self.assertIsNone(again.get("t%d" % i, False))
            again.put("t%d" % i, False, "verified")
        self.assertEqual((again.sample_count, again.mismatches), (200, []))

    def test_safeguards_force_full_runs(self):
        guards = verdicts.Safeguards(self.folder)
        self.assertEqual(guards.decide(False, False, False)[0], "full")      # none recorded yet
        guards.finish("full", "first", 10, 0, 0, 0, 1.0, {0x100})
        self.assertEqual(guards.decide(False, False, False)[0], "partial")
        self.assertEqual(guards.decide(False, True, False)[0], "full")       # (d) gate
        self.assertEqual(guards.decide(True, False, False)[0], "full")       # --no-cache
        self.assertEqual(guards.decide(False, False, True)[0], "full")       # new environment
        # (b): one unexplained fault is not enough, two are
        guards.record_fault("hang in stage 1")
        self.assertEqual(guards.decide(False, False, False)[0], "partial")
        guards.record_fault("crash in a movie")
        self.assertEqual(verdicts.Safeguards(self.folder).decide(False, False, False)[0], "full")
        guards.finish("full", "faults", 10, 0, 0, 0, 1.0, {0x100})
        self.assertEqual(guards.decide(False, False, False)[0], "partial")   # a full run resets
        # (a): a fault traced to a function whose verdict was reused
        guards.record_fault("wrong picture", 0x200)                        # not reused: no full run yet
        self.assertEqual(guards.decide(False, False, False)[0], "partial")
        guards.record_fault("wrong picture", 0x100)
        kind, trigger = guards.decide(False, False, False)
        self.assertEqual(kind, "full")
        self.assertIn("0x00000100", trigger)

    def test_a_subset_or_safeguards_off_run_does_not_count_as_full(self):
        guards = verdicts.Safeguards(self.folder)
        guards.finish("full", "subset", 5, 0, 0, 0, 1.0, set(), counts_as_full=False)
        self.assertEqual(guards.decide(False, False, False)[0], "full")
        guards.finish("full", "off", 5, 0, 0, 0, 1.0, set(), safeguards_off=True)
        self.assertEqual(guards.decide(False, False, False)[0], "full")
        with open(guards.log_path, encoding="utf-8") as handle:
            log = handle.read()
        self.assertIn("SAFEGUARDS OFF", log)
        self.assertEqual(len(log.splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
