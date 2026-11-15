import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/push_release_tag.sh"
TAG = "v5.1.0-34-develop"


def git(directory, *args):
    location = ["--git-dir", str(directory)] if directory.name.endswith(".git") else ["-C", str(directory)]
    return subprocess.check_output(["git", *location, *args], text=True).strip()


def publish(local):
    return subprocess.run(["bash", str(SCRIPT), TAG], cwd=local, capture_output=True, text=True)


class ReleaseTagTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="release-tag-")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        self.remote = directory / "remote.git"
        self.local = directory / "local"
        subprocess.run(["git", "init", "--bare", str(self.remote)], check=True, capture_output=True)
        subprocess.run(["git", "init", str(self.local)], check=True, capture_output=True)
        git(self.local, "config", "user.name", "Test")
        git(self.local, "config", "user.email", "test@example.invalid")
        git(self.local, "commit", "--allow-empty", "-m", "first")
        git(self.local, "remote", "add", "origin", str(self.remote))

    def test_new_tag_and_same_commit_rerun(self):
        self.assertEqual(publish(self.local).returncode, 0)
        self.assertEqual(git(self.remote, "rev-parse", TAG), git(self.local, "rev-parse", "HEAD"))
        self.assertEqual(publish(self.local).returncode, 0)

    def check_remote_only_matching_tag(self, annotated):
        args = ("-a", TAG, "-m", "release") if annotated else (TAG,)
        git(self.local, "tag", *args)
        git(self.local, "push", "origin", "refs/tags/" + TAG)
        git(self.local, "tag", "-d", TAG)
        self.assertEqual(publish(self.local).returncode, 0)
        self.assertEqual(git(self.remote, "rev-parse", TAG + "^{commit}"),
                         git(self.local, "rev-parse", "HEAD"))

    def test_remote_only_matching_lightweight_tag(self):
        self.check_remote_only_matching_tag(False)

    def test_remote_only_matching_annotated_tag(self):
        self.check_remote_only_matching_tag(True)

    def test_conflicting_remote_tag_is_never_overwritten(self):
        self.assertEqual(publish(self.local).returncode, 0)
        original = git(self.remote, "rev-parse", TAG)
        git(self.local, "tag", "-d", TAG)
        git(self.local, "commit", "--allow-empty", "-m", "second")
        result = publish(self.local)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(git(self.remote, "rev-parse", TAG), original)

    def test_remote_query_failure_is_not_success(self):
        git(self.local, "remote", "set-url", "origin", str(self.local / "missing.git"))
        result = publish(self.local)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to query remote", result.stderr)
