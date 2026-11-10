cmake_minimum_required(VERSION 3.22)

# Matter over Thread is on by default. -DLOCHNESS_MATTER=OFF builds the lock
# without it: QEMU smoke tests use the ESP32-C3, which has no Thread radio.
if(NOT DEFINED LOCHNESS_MATTER)
    set(LOCHNESS_MATTER ON)
endif()

# Portable components, then the ESP-only ones (platform_esp, ln_store, ln_central).
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components ${CMAKE_CURRENT_LIST_DIR}/../components)

if(LOCHNESS_MATTER)
    if(NOT DEFINED ENV{ESP_MATTER_PATH})
        message(FATAL_ERROR "Set ESP_MATTER_PATH to esp-matter (./esp.sh does this for ~/esp/esp-matter-v1.6.1), "
                            "or build without Matter with -DLOCHNESS_MATTER=OFF")
    endif()
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    # esp-matter's component scripts read MATTER_SDK_PATH.
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
    # Matter and Thread settings on top of the lock's own, unless the caller
    # picked the defaults files (tools/qemu_console.sh does).
    if(NOT DEFINED SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.matter")
    endif()
endif()

# Build only main and what it requires, not every component in those dirs
# (e.g. the lock's QEMU build has no Bluetooth for the keypad's ln_central).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Read by main/CMakeLists.txt (build properties reach component scripts).
idf_build_set_property(LOCHNESS_MATTER ${LOCHNESS_MATTER})

# Matter's SoftwareVersionString; without this it is the checkout's git describe.
set(PROJECT_VER "0.1.0")
project(lochness_lock_unit)

if(LOCHNESS_MATTER)
    # What esp-matter's examples set for the Matter SDK's C++.
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
    # RISC-V: project_include.cmake sets -Wno-format but not the flags that depend on it.
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
endif()

if(LOCHNESS_MATTER)
    # esp-matter v1.6.1 bug: `static constexpr char *TAG = "icd"` (a string
    # literal as char *). Downgrade that one warning in that one SDK file only.
    idf_component_get_property(esp_matter_lib esp_matter COMPONENT_LIB)
    set_source_files_properties("${ESP_MATTER_PATH}/components/esp_matter/esp_matter_icd_configuration.cpp"
        TARGET_DIRECTORY ${esp_matter_lib} PROPERTIES COMPILE_OPTIONS "-Wno-error=write-strings")
endif()
