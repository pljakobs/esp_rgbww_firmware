#!/usr/bin/env bash
set -euo pipefail

tag_name=${1:?Release tag is required}
git check-ref-format "refs/tags/$tag_name"
expected=$(git rev-parse HEAD)

check_remote() {
    local refs actual
    if ! refs=$(git ls-remote origin "refs/tags/$tag_name" "refs/tags/$tag_name^{}"); then
        echo "Failed to query remote release tag $tag_name." >&2
        exit 1
    fi
    if [[ -z "$refs" ]]; then
        return 1
    fi
    actual=$(printf '%s\n' "$refs" | awk '$2 ~ /\^\{\}$/ { print $1; found=1 } END { if (!found) print first } NR == 1 { first=$1 }')
    if [[ "$actual" != "$expected" ]]; then
        echo "Release tag $tag_name points to $actual, expected $expected; refusing to overwrite it." >&2
        exit 1
    fi
    echo "Release tag $tag_name already points to $expected."
}

if check_remote; then
    exit 0
fi
if git show-ref --verify --quiet "refs/tags/$tag_name"; then
    actual=$(git rev-parse "refs/tags/$tag_name^{commit}")
    if [[ "$actual" != "$expected" ]]; then
        echo "Local release tag $tag_name points to $actual, expected $expected." >&2
        exit 1
    fi
else
    git tag "$tag_name" "$expected"
fi
if ! git push origin "refs/tags/$tag_name"; then
    if check_remote; then
        exit 0
    fi
    echo "Failed to publish release tag $tag_name." >&2
    exit 1
fi
