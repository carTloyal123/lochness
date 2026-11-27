# Sourced by the scripts in this repo: loads ESP-IDF (idf.py and toolchains)
# into the current shell, quietly unless it fails.
# Set IDF_PATH to use an ESP-IDF other than ~/esp/esp-idf-v6.0.2, and
# ESP_MATTER_PATH for an esp-matter other than ~/esp/esp-matter-v1.6.1 (for
# the lock's Matter-over-Thread build; it is loaded when present).
#
# The Python environment is this project's own: uv builds firmware/.venv on a
# uv-managed Python (version in firmware/.python-version) from ESP-IDF's and
# esp-matter's requirement files, and rebuilds it when any of those change.
# The system python3 only starts ESP-IDF's export script.

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v6.0.2}"
ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter-v1.6.1}"
_idf_env_firmware="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PYTHON_ENV_PATH="$_idf_env_firmware/.venv"

# Create or refresh firmware/.venv. A stamp of every input makes this a no-op
# when nothing changed.
idf_env_venv() {
    local venv="$IDF_PYTHON_ENV_PATH" tools="$_idf_env_firmware/tools"
    local major minor idf_version constraints stamp
    major="$(sed -n 's/^set(IDF_VERSION_MAJOR \([0-9]*\))/\1/p' "$IDF_PATH/tools/cmake/version.cmake")"
    minor="$(sed -n 's/^set(IDF_VERSION_MINOR \([0-9]*\))/\1/p' "$IDF_PATH/tools/cmake/version.cmake")"
    idf_version="$major.$minor"
    # install.sh downloads the constraints; fall back to Espressif's copy.
    constraints="${IDF_TOOLS_PATH:-$HOME/.espressif}/espidf.constraints.v$idf_version.txt"
    [[ -f "$constraints" ]] || constraints="https://dl.espressif.com/dl/esp-idf/espidf.constraints.v$idf_version.txt"

    local -a inputs=("$_idf_env_firmware/.python-version" "$tools/py-overrides.txt"
        "$IDF_PATH/tools/requirements/requirements.core.txt")
    [[ -f "$constraints" ]] && inputs+=("$constraints")
    local -a reqs=(-r "$IDF_PATH/tools/requirements/requirements.core.txt")
    if [[ -f "$ESP_MATTER_PATH/requirements.txt" ]]; then
        inputs+=("$ESP_MATTER_PATH/requirements.txt")
        reqs+=(-r "$ESP_MATTER_PATH/requirements.txt")
    fi
    stamp="$(echo "$IDF_PATH $ESP_MATTER_PATH" | cat - "${inputs[@]}" | shasum | cut -d' ' -f1)"
    [[ -x "$venv/bin/python" && "$(cat "$venv/.stamp" 2>/dev/null)" == "$stamp" ]] && return 0

    if ! command -v uv >/dev/null; then
        echo "uv not found: install it (https://docs.astral.sh/uv/) to build $venv" >&2
        exit 1
    fi
    echo "Building the Python environment in $venv ..." >&2
    (
        set -e
        cd "$_idf_env_firmware" # for .python-version
        uv venv --quiet --clear --managed-python "$venv"
        uv pip install --quiet --python "$venv/bin/python" "${reqs[@]}" \
            --constraint "$constraints" --override "$tools/py-overrides.txt"
    ) >&2 || exit 1
    # ESP-IDF checks that the environment was made for its version.
    printf '%s' "$idf_version" >"$venv/idf_version.txt"
    echo "$stamp" >"$venv/.stamp"
}

idf_env_load() {
    if [[ ! -f "$IDF_PATH/export.sh" ]]; then
        echo "ESP-IDF not found at $IDF_PATH (set IDF_PATH)" >&2
        exit 1
    fi
    idf_env_venv
    local log
    log="$(mktemp)"
    # export.sh calls `exit` on failure, so its log is shown from an EXIT trap.
    # Restore stdout/stderr first, or the trap would append the log to itself.
    exec 3>&1 4>&2 >"$log" 2>&1
    trap 'exec 1>&3 2>&4; cat "$log" >&2; rm -f "$log"' EXIT
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh"
    if [[ -f "$ESP_MATTER_PATH/export.sh" ]]; then
        export ESP_MATTER_PATH
        # shellcheck disable=SC1091
        . "$ESP_MATTER_PATH/export.sh"
    fi
    trap - EXIT
    exec 1>&3 2>&4 3>&- 4>&-
    rm -f "$log"
}
