import subprocess
from pathlib import Path

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/push_release_tag.sh"
TAG = "v5.1.0-34-develop"


def git(directory, *args):
    location = ["--git-dir", str(directory)] if directory.name.endswith(".git") else ["-C", str(directory)]
    return subprocess.check_output(["git", *location, *args], text=True).strip()


@pytest.fixture
def repository(tmp_path):
    remote = tmp_path / "remote.git"
    local = tmp_path / "local"
    subprocess.run(["git", "init", "--bare", str(remote)], check=True, capture_output=True)
    subprocess.run(["git", "init", str(local)], check=True, capture_output=True)
    git(local, "config", "user.name", "Test")
    git(local, "config", "user.email", "test@example.invalid")
    git(local, "commit", "--allow-empty", "-m", "first")
    git(local, "remote", "add", "origin", str(remote))
    return local, remote


def publish(local):
    return subprocess.run(["bash", str(SCRIPT), TAG], cwd=local, capture_output=True, text=True)


def test_new_tag_and_same_commit_rerun(repository):
    local, remote = repository
    assert publish(local).returncode == 0
    assert git(remote, "rev-parse", TAG) == git(local, "rev-parse", "HEAD")
    assert publish(local).returncode == 0


@pytest.mark.parametrize("annotated", [False, True])
def test_remote_only_matching_tag(repository, annotated):
    local, remote = repository
    args = ("-a", TAG, "-m", "release") if annotated else (TAG,)
    git(local, "tag", *args)
    git(local, "push", "origin", "refs/tags/" + TAG)
    git(local, "tag", "-d", TAG)
    assert publish(local).returncode == 0
    assert git(remote, "rev-parse", TAG + "^{commit}") == git(local, "rev-parse", "HEAD")


def test_conflicting_remote_tag_is_never_overwritten(repository):
    local, remote = repository
    assert publish(local).returncode == 0
    original = git(remote, "rev-parse", TAG)
    git(local, "tag", "-d", TAG)
    git(local, "commit", "--allow-empty", "-m", "second")
    result = publish(local)
    assert result.returncode != 0
    assert "refusing to overwrite" in result.stderr
    assert git(remote, "rev-parse", TAG) == original


def test_remote_query_failure_is_not_success(repository):
    local, _ = repository
    git(local, "remote", "set-url", "origin", str(local / "missing.git"))
    result = publish(local)
    assert result.returncode != 0
    assert "Failed to query remote" in result.stderr
