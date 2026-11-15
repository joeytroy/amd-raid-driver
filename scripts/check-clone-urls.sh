#!/bin/bash
#
# check-clone-urls.sh — every documented `git clone` must fetch THIS repository.
#
# Why: the install instructions are followed verbatim on a live USB, and three
# of them (docs/README.md, docs/INSTALL.md x2) still said `joeytroy/amd-raid-driver`
# after the work moved here.  Following one built the upstream tree, which has
# neither this branch's installer nor the array-builder removal — the failure
# looks like the docs being wrong about the driver, not the clone being stale.
#
# The expected repository is whatever this checkout IS: GitHub Actions reports
# it in GITHUB_REPOSITORY, and locally it is `origin`.  A tarball checkout has
# neither, and guessing there would make the gate pass vacuously, so it fails.
#
# Run locally:  scripts/check-clone-urls.sh [file ...]
# With no arguments it checks every tracked *.md and *.sh.
#
# Only lines that carry a URL are treated as clone commands: prose that says
# "every `git clone`" is not a claim about where a tree comes from, and a
# command with no URL (`git clone "$REPO"`) has nothing to check.
#
# If a genuinely third-party clone belongs in the tree (a vendored README, say),
# add its path to PATH_EXCLUDE with a comment saying whose tree it documents.
# This gate deliberately has no exclusion list today: nothing tracked clones
# anything but this repository.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PATH_EXCLUDE=(
    # Reference artifacts, not instructions we author (mirrors the whitespace gate).
    'docs/ghidra_output/*'
    'drivers/windows/*'
)

normalize() {
    local u="$1"
    u="${u%"${u##*[!/]}"}"      # trailing slashes
    u="${u%.git}"
    u="${u#https://}"
    u="${u#http://}"
    u="${u#ssh://}"
    u="${u#git://}"
    u="${u#git@}"
    # scp-style git@host:owner/repo -> host/owner/repo
    if [[ "$u" == *:* && "${u%%:*}" != */* ]]; then
        u="${u%%:*}/${u#*:}"
    fi
    printf '%s' "${u,,}"
}

if [ -n "${GITHUB_REPOSITORY:-}" ]; then
    expected="$(normalize "github.com/${GITHUB_REPOSITORY}")"
else
    origin="$(git remote get-url origin 2>/dev/null || true)"
    if [ -z "$origin" ]; then
        echo "::error::no GITHUB_REPOSITORY and no origin remote — cannot tell which repository this tree is" >&2
        exit 1
    fi
    expected="$(normalize "$origin")"
fi

excluded() {
    local path="$1" pat
    for pat in "${PATH_EXCLUDE[@]}"; do
        # shellcheck disable=SC2053  # glob match on purpose
        [[ "$path" == $pat ]] && return 0
    done
    return 1
}

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(git ls-files '*.md' '*.sh')
fi

if [ "${#files[@]}" -eq 0 ]; then
    echo "::error::no files to check — the gate would pass vacuously" >&2
    exit 1
fi

checked=0
clones=0
rc=0
for f in "${files[@]}"; do
    [ -f "$f" ] || continue
    excluded "$f" && continue
    checked=$((checked + 1))
    while IFS= read -r hit; do
        lineno="${hit%%:*}"
        line="${hit#*:}"
        # The URL is the first http(s)/scp-style token on the clone line.
        url="$(printf '%s\n' "$line" | grep -oE '(https?://[^ )`"]+|git@[^ )`"]+)' | head -1 || true)"
        [ -n "$url" ] || continue      # prose, or a URL held in a variable
        clones=$((clones + 1))
        got="$(normalize "$url")"
        if [ "$got" != "$expected" ]; then
            echo "::error file=$f,line=$lineno::clone URL '$url' is '$got', expected '$expected'"
            rc=1
        fi
    done < <(grep -n 'git clone' "$f" || true)
done

if [ "$clones" -eq 0 ]; then
    echo "::error::no 'git clone' found in $checked files — the gate would pass vacuously" >&2
    exit 1
fi

if [ "$rc" -eq 0 ]; then
    echo "All $clones documented clone URL(s) in $checked file(s) point at $expected."
fi
exit "$rc"
