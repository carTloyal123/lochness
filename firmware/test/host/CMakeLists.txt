cmake_minimum_required(VERSION 3.16)
project(lochness_host_tests C)

# For clangd and clang-tidy (merged by tools/compile_db.py).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
option(LOCHNESS_SANITIZE "Build with ASan/UBSan" ON)
if(LOCHNESS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(../../components/lock_core lock_core)
add_subdirectory(../../components/monocypher monocypher)
add_subdirectory(../../components/secure_link secure_link)
add_subdirectory(../../components/link_proto link_proto)
add_subdirectory(../../components/lock_link lock_link)
add_subdirectory(../../components/keypad_core keypad_core)

add_executable(test_lock_core
    test_main.c
    fake_platform.c
    test_creds.c
    test_ratelimit.c
    test_log.c
    test_lock.c
    test_secure_link.c
    test_link_proto.c
    test_link.c
    test_state.c
    ../../apps/lock_unit/main/lock_bridge.c
)
target_link_libraries(test_lock_core PRIVATE lock_core lock_link keypad_core)
target_include_directories(test_lock_core PRIVATE ../../apps/lock_unit/main)

enable_testing()
add_test(NAME lock_core COMMAND test_lock_core)
