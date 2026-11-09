import json
import re
import sys


RELEASES = {
    "5.0": {"socs": ("esp8266", "esp32", "esp32c3"), "rgbwwled_version": "0.10.1"},
    "5.1": {"socs": ("esp32", "esp32c3"), "rgbwwled_version": "0.11.0"},
}
STAGES = {"develop", "testing", "prod"}


def resolve_branch(branch):
    match = re.fullmatch(r"(5\.[01])/(develop|testing|prod)", branch)
    if not match:
        raise ValueError(f"Unsupported firmware release ref: {branch}")

    release_line, stage = match.groups()
    release = RELEASES[release_line]
    return {
        "release_ref": branch,
        "release_line": release_line,
        "stage": stage,
        "artifact_branch": branch.replace("/", "-"),
        "webapp_branch": branch,
        "socs": release["socs"],
        "soc_json": json.dumps(release["socs"], separators=(",", ":")),
        "soc_csv": ",".join(release["socs"]),
        "rgbwwled_version": release["rgbwwled_version"],
    }


def compute_version(tags, release_line, stage):
    if release_line not in RELEASES or stage not in STAGES:
        raise ValueError(f"Unsupported firmware release identity: {release_line}/{stage}")

    version_prefix = f"v{release_line}."
    clean_patches = []
    prerelease_sequences = {}
    for tag in tags:
        if not tag.startswith(version_prefix):
            continue
        patch, separator, suffix = tag[len(version_prefix):].partition("-")
        if not patch.isdigit():
            continue
        patch = int(patch)
        if not separator:
            clean_patches.append(patch)
            continue
        sequence = suffix.partition("-")[0]
        if sequence.isdigit():
            prerelease_sequences.setdefault(patch, []).append(int(sequence))

    latest_clean_patch = max(clean_patches, default=-1)
    candidate_patches = [patch for patch in prerelease_sequences if patch > latest_clean_patch]
    patch = max(candidate_patches) if candidate_patches else latest_clean_patch + 1
    sequence = max(prerelease_sequences.get(patch, ()), default=0) + 1
    version = f"V{release_line}.{patch}-{sequence}-{stage}"
    return {"fw_version": version, "fw_build_tag": f"v{version[1:]}"}


def main(argv):
    if len(argv) == 2 and argv[0] == "resolve":
        try:
            metadata = resolve_branch(argv[1])
        except ValueError as error:
            raise SystemExit(str(error)) from error
        for key, value in metadata.items():
            if key != "socs":
                print(f"{key}={value}")
        return

    if len(argv) == 3 and argv[0] == "version":
        try:
            metadata = compute_version(sys.stdin.read().splitlines(), argv[1], argv[2])
        except ValueError as error:
            raise SystemExit(str(error)) from error
        for key, value in metadata.items():
            print(f"{key}={value}")
        return

    raise SystemExit("usage: release_metadata.py resolve <branch> | version <release-line> <stage>")


if __name__ == "__main__":
    main(sys.argv[1:])