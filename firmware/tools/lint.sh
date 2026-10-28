#!/usr/bin/env bash
# Run clang-tidy (all checks, see firmware/.clang-tidy) over the firmware.
#
#   firmware/tools/lint.sh                      # everything except vendored code
#   firmware/tools/lint.sh components/lock_core # only paths matching a regex
#   firmware/tools/lint.sh --fix lock_link      # apply suggested fixes
#
# Uses Espressif's clang-tidy (idf_tools.py install esp-clang) so the RISC-V
# target and the clangd in your editor agree. Regenerates the compile database
# when it's missing; run tools/compile_db.py --configure after adding files.
set -euo pipefail

FW="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(ls -d "$HOME"/.espressif/tools/esp-clang/*/esp-clang/bin 2>/dev/null | tail -1)"
if [[ -z "$BIN" ]]; then
    echo "esp-clang not installed: \$IDF_PATH/tools/idf_tools.py install esp-clang" >&2
    exit 1
fi
[[ -f "$FW/compile_commands.json" ]] || "$FW/tools/compile_db.py" --configure

filter="."
fix=false
for arg in "$@"; do
    case "$arg" in
        --fix) fix=true ;;
        *) filter="$arg" ;;
    esac
done
cd "$FW"
if $fix; then
    # esp-clang ships no clang-apply-replacements, so fix one file at a time
    # (serially, so edits to a shared header can't collide).
    python3 -c 'import json; [print(e["file"]) for e in json.load(open("compile_commands.json"))]' |
        grep -v /monocypher/ | grep -E -- "$filter" |
        while read -r f; do "$BIN/clang-tidy" -p . --quiet --fix "$f" || true; done
else
    "$BIN/run-clang-tidy" -clang-tidy-binary "$BIN/clang-tidy" -p . -quiet \
        "^(?!.*/monocypher/).*${filter}"
fi
