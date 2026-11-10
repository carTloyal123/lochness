cmake_minimum_required(VERSION 3.22)

# Portable components, then the ESP-only ones (platform_esp, ln_store, ln_central).
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components ${CMAKE_CURRENT_LIST_DIR}/../components)

# Build only main and what it requires, not every component in those dirs
# (e.g. the lock's QEMU build has no Bluetooth for the keypad's ln_central).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lochness_lock_unit)
