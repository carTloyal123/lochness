# Sourced by clangd, clang-tidy and lint.sh: the one place that decides which
# LLVM build the editor and the linter use, so both see the same checks.
#
# Espressif's LLVM (esp-clang) understands the RISC-V ESP targets; install it
# with: $IDF_PATH/tools/idf_tools.py install esp-clang
# Set ESP_CLANG_BIN to a bin/ directory to override.
esp_clang_bin() {
    if [[ -n "${ESP_CLANG_BIN:-}" ]]; then
        echo "$ESP_CLANG_BIN"
        return
    fi
    local bin
    bin="$(ls -d "$HOME"/.espressif/tools/esp-clang/*/esp-clang/bin 2>/dev/null | sort -V | tail -1)"
    if [[ -z "$bin" ]]; then
        echo "esp-clang not installed: \$IDF_PATH/tools/idf_tools.py install esp-clang" >&2
        return 1
    fi
    echo "$bin"
}
