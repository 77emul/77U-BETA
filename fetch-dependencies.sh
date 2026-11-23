#!/bin/sh
set -eu
cd "$(dirname "$0")"
tab=$(printf '\t')
{
    IFS= read -r header
    while IFS="$tab" read -r path revision upstream; do
        if [ ! -e "$path" ]; then
            git init -q "$path"
            git -C "$path" remote add origin "$upstream"
        elif [ ! -e "$path/.git" ]; then
            printf 'Expected a dedicated dependency checkout at %s\n' "$path" >&2
            exit 1
        fi
        if [ "$(git -C "$path" rev-parse --verify HEAD 2>/dev/null || true)" != "$revision" ]; then
            git -C "$path" fetch --no-tags --depth=1 "$upstream" "$revision"
            git -C "$path" checkout --detach "$revision"
        fi
        git -C "$path" submodule update --init --recursive
    done
} < DEPENDENCY-REVISIONS.tsv
