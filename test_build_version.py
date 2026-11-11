"""Exercise the firmware's Make version block without compiling Sming."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BuildVersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "-q", "-b", "5.1/develop")
        (self.root / "tracked").write_text("initial\n")
        self.git("add", "tracked")
        self.commit()
        component = (Path(__file__).parent / "component.mk").read_text()
        self.version_block = component.split("#### GIT VERSION Information #####", 1)[1].split("GIT_DATE =", 1)[0]

    def git(self, *args):
        return subprocess.check_output(
            ["git", *args], cwd=self.root, text=True, stderr=subprocess.STDOUT
        ).strip()

    def commit(self):
        self.git("-c", "user.name=Version Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm", "test")

    def version(self, *args):
        environment = dict(os.environ)
        environment.pop("GITHUB_RUN_NUMBER", None)
        environment.pop("GIT_VERSION", None)
        return subprocess.check_output(
            ["make", "--no-print-directory", "-s", "-f", "-", *args, "version"],
            input=self.version_block + "\n.PHONY: version\nversion:\n\t@echo '$(GIT_VERSION)'\n",
            cwd=self.root, env=environment, text=True,
        ).strip()

    def test_versioned_ci_tag_preserves_entire_branch_and_numeric_run(self):
        self.git("tag", "ci/5.1/develop/1199")
        self.assertRegex(self.version(), r"^V5\.1-1199-5\.1/develop-.+-0$")
        (self.root / "tracked").write_text("changed\n")
        self.assertRegex(self.version(), r"^V5\.1-1199-5\.1/develop-.+-0-dirty$")
        self.git("add", "tracked")
        self.commit()
        self.assertRegex(self.version(), r"^V5\.1-1199-5\.1/develop-.+-1$")

    def test_legacy_ci_tag_does_not_override_source_release_line(self):
        self.git("tag", "ci/develop/42")
        self.assertRegex(self.version(), r"^V5\.1-42-develop-.+-0$")

    def test_ci_run_uses_source_release_line(self):
        self.assertEqual(self.version("GITHUB_RUN_NUMBER=1200"), "V5.1-1200-5.1/develop")

    def test_untagged_and_old_release_tag_fallbacks_have_correct_prefix(self):
        self.assertRegex(self.version(), r"^V5\.1-local-[0-9a-f]+-5\.1/develop$")
        self.git("tag", "v5.0.0")
        self.assertRegex(self.version(), r"^V5\.1-local-[0-9a-f]+-5\.1/develop$")

    def test_explicit_ci_version_is_unchanged(self):
        self.assertEqual(
            self.version("GIT_VERSION=V5.1.0-1200-develop", "GITHUB_RUN_NUMBER=1200"),
            "V5.1.0-1200-develop",
        )


if __name__ == "__main__":
    unittest.main()
