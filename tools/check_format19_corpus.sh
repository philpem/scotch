#!/usr/bin/env bash
set -euo pipefail

verify=${1:-build/replay-verify}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../corpus/format19" && pwd)
manifest=$root/manifest.tsv
count=0

while IFS=$'\t' read -r name width height payload previous expected provenance; do
    [[ -z ${name:-} || $name == \#* ]] && continue
    args=(--codec 19 --size "${width}x${height}"
          --payload "$root/$payload" --expect-6y5uv "$root/$expected")
    if [[ $previous != - ]]; then
        args+=(--previous-6y5uv "$root/$previous")
    fi
    printf 'fixture=%s provenance="%s"\n' "$name" "$provenance"
    "$verify" "${args[@]}"
    ((count += 1))
done < "$manifest"

printf 'format19_corpus fixtures=%d status=ok\n' "$count"
