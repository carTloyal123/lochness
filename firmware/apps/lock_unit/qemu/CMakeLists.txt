cmake_minimum_required(VERSION 3.22)

# QEMU smoke-test build of the lock (tools/qemu_console.sh, `./esp.sh qemu`):
# the lock's own main, as an ESP32-C3 (the closest chip QEMU emulates) with no
# Bluetooth and no Matter (the C3 has no Thread radio).
#
# A project of its own because the ESP-IDF component manager deletes whatever
# the current build doesn't use from <project>/managed_components: sharing the
# lock's folder, this build and the Matter build would delete each other's
# downloads (and rewrite each other's dependencies.lock) on every switch.
set(LOCK ${CMAKE_CURRENT_LIST_DIR}/..)
set(IDF_TARGET esp32c3)
set(SDKCONFIG_DEFAULTS "${LOCK}/sdkconfig.defaults;${LOCK}/sdkconfig.qemu")
set(EXTRA_COMPONENT_DIRS ${LOCK}/main ${LOCK}/../../components ${LOCK}/../components)
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(LOCHNESS_MATTER OFF)
project(lochness_lock_unit_qemu)
