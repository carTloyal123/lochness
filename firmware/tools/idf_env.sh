# Sourced by the scripts in this repo: loads ESP-IDF (idf.py and toolchains)
# into the current shell, quietly unless it fails.
# Set IDF_PATH to use an ESP-IDF other than ~/esp/esp-idf-v6.0.2.

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v6.0.2}"

idf_env_load() {
    if [[ ! -f "$IDF_PATH/export.sh" ]]; then
        echo "ESP-IDF not found at $IDF_PATH (set IDF_PATH)" >&2
        exit 1
    fi
    local log
    log="$(mktemp)"
    # export.sh calls `exit` on failure, so its log is shown from an EXIT trap.
    # Restore stdout/stderr first, or the trap would append the log to itself.
    exec 3>&1 4>&2 >"$log" 2>&1
    trap 'exec 1>&3 2>&4; cat "$log" >&2; rm -f "$log"' EXIT
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh"
    trap - EXIT
    exec 1>&3 2>&4 3>&- 4>&-
    rm -f "$log"
}
