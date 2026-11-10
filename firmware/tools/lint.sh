#!/usr/bin/env bash
# Strict clang-tidy over the firmware. Exits non-zero on any finding.
#
#   firmware/tools/lint.sh                      # everything
#   firmware/tools/lint.sh components/lock_core # only paths matching a regex
#   firmware/tools/lint.sh --fix lock_link      # apply suggested fixes
#
# What runs where is decided in exactly one place each, shared with clangd:
#   rules   .clang-tidy files (root, plus nested opt-outs such as
#           components/monocypher/.clang-tidy)
#   tool    tools/esp_clang.sh (same LLVM build as tools/clangd)
#   flags   compile_commands.json from tools/compile_db.py, refreshed here on
#           every run; it fails if any source is in no build
set -euo pipefail

FW="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=firmware/tools/esp_clang.sh
. "$FW/tools/esp_clang.sh"
BIN="$(esp_clang_bin)"

filter="."
fix=false
for arg in "$@"; do
    case "$arg" in
        --fix) fix=true ;;
        -h | --help)
            sed -n '2,13p' "$0"
            exit 0
            ;;
        *) filter="$arg" ;;
    esac
done

cd "$FW"
# Configure the builds the first time; afterwards a merge is enough (and
# cheap), so new files and flag changes reach the linter and the editor.
configure=()
ls apps/*/build/compile_commands.json >/dev/null 2>&1 || configure=(--configure)
"$FW/tools/compile_db.py" ${configure[@]+"${configure[@]}"} # bash 3.2-safe empty array

# Files in the database matching the filter, minus those whose .clang-tidy
# enables no checks (vendored code). Decided per directory by clang-tidy
# itself, so the opt-out lives only in the .clang-tidy file. (Python, not
# bash: macOS ships bash 3.2.)
files=()
while IFS= read -r f; do files+=("$f"); done < <(python3 - "$BIN/clang-tidy" "$filter" <<'PY'
import json, os, re, subprocess, sys
tidy, pattern = sys.argv[1], re.compile(sys.argv[2])
enabled = {}
for e in json.load(open("compile_commands.json")):
    f = e["file"]
    if not pattern.search(f):
        continue
    d = os.path.dirname(f)
    if d not in enabled:
        out = subprocess.run([tidy, "--list-checks", os.path.join(d, "_.c"), "--"],
                             capture_output=True, text=True).stdout
        enabled[d] = any(line.startswith("    ") for line in out.splitlines())
    if enabled[d]:
        print(f)
PY
)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no files match '$filter'" >&2
    exit 1
fi
echo "clang-tidy: ${#files[@]} files" >&2

if $fix; then
    # esp-clang ships no clang-apply-replacements, so fix one file at a time
    # (serially, so edits to a shared header can't collide).
    status=0
    for f in "${files[@]}"; do
        "$BIN/clang-tidy" -p . --quiet --fix "$f" || status=1
    done
    exit $status
fi

regex="^($(printf '%s\n' "${files[@]}" | python3 -c 'import re,sys; print("|".join(re.escape(l.strip()) for l in sys.stdin))'))$"
exec "$BIN/run-clang-tidy" -clang-tidy-binary "$BIN/clang-tidy" -p . -quiet "$regex"
