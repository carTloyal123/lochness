cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Set ESP_MATTER_PATH to esp-matter (./esp.sh does this for ~/esp/esp-matter-v1.6.1)")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
set(LOCHNESS_COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(LOCHNESS_ESP_COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only the lochness components the hub uses (the lock's are not built here).
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${LOCHNESS_COMPONENTS}/monocypher"
    "${LOCHNESS_COMPONENTS}/secure_link"
    "${LOCHNESS_COMPONENTS}/link_proto"
    "${LOCHNESS_COMPONENTS}/hub_core"
    "${LOCHNESS_ESP_COMPONENTS}/ln_central"
    "${LOCHNESS_ESP_COMPONENTS}/ln_store")

# Matter SoftwareVersionString comes from the app version; without this it is
# the git describe of the checkout.
set(PROJECT_VER "0.1.0")
project(lochness_hub)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
# RISC-V: project_include.cmake sets -Wno-format but not the flags that depend on it.
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
