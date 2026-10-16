#!/usr/bin/env python3
"""Exercise series state transitions in synthetic Git repositories."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

SPEC = importlib.util.spec_from_file_location("apply_susfs", Path(__file__).with_name("apply-susfs.py"))
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


class SeriesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-q")
        (self.root / "file").write_text("old\n")
        (self.root / "deleted").write_text("remove\n")
        (self.root / ".gitignore").write_text("created\n")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "fixture")
        self.index = (self.root / ".git/index").read_bytes()
        self.directory = self.root / ".git/patches"
        self.directory.mkdir()
        self.first = self.directory / "one.patch"
        self.first.write_text("diff --git a/file b/file\n--- a/file\n+++ b/file\n"
                              "@@ -1 +1 @@\n-old\n+middle\n")
        self.second = self.directory / "two.patch"
        self.second.write_text("diff --git a/file b/file\n--- a/file\n+++ b/file\n"
                               "@@ -1 +1 @@\n-middle\n+new\n"
                               "diff --git a/created b/created\nnew file mode 100644\n"
                               "--- /dev/null\n+++ b/created\n@@ -0,0 +1 @@\n+added\n"
                               "diff --git a/deleted b/deleted\ndeleted file mode 100644\n"
                               "--- a/deleted\n+++ /dev/null\n@@ -1 +0,0 @@\n-remove\n")
        self.series = self.directory / "series"
        self.series.write_text("# dependent patches\none.patch\n\ntwo.patch # final\n")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def apply(self):
        HELPER.apply_series(self.root, self.series)
        self.assertEqual((self.root / ".git/index").read_bytes(), self.index)

    def state(self):
        return {p.name: p.read_bytes() for p in self.root.iterdir() if p.is_file()}

    def test_clean_then_repeated(self):
        self.apply()
        self.assertEqual((self.root / "file").read_text(), "new\n")
        self.assertEqual((self.root / "created").read_text(), "added\n")
        self.assertFalse((self.root / "deleted").exists())
        state = self.state()
        self.apply()
        self.assertEqual(self.state(), state)

    def test_partial_is_rejected_without_changes(self):
        self.git("apply", str(self.first))
        state = self.state()
        with self.assertRaisesRegex(RuntimeError, "neither clean nor fully applied"):
            self.apply()
        self.assertEqual(self.state(), state)
        self.assertEqual((self.root / ".git/index").read_bytes(), self.index)

    def test_conflict_is_rejected_without_changes(self):
        (self.root / "deleted").write_text("local edit\n")
        state = self.state()
        with self.assertRaisesRegex(RuntimeError, "neither clean nor fully applied"):
            self.apply()
        self.assertEqual(self.state(), state)

    def test_missing_patch_is_rejected(self):
        self.second.unlink()
        state = self.state()
        with self.assertRaisesRegex(RuntimeError, "patch missing"):
            self.apply()
        self.assertEqual(self.state(), state)

    def test_missing_series_is_rejected(self):
        self.series.unlink()
        with self.assertRaisesRegex(RuntimeError, "series missing"):
            self.apply()

    def test_empty_series_is_rejected(self):
        self.series.write_text("# no patches\n")
        with self.assertRaisesRegex(RuntimeError, "series is empty"):
            self.apply()


if __name__ == "__main__":
    unittest.main()
