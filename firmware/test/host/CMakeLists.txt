cmake_minimum_required(VERSION 3.16)
project(lochness_host_tests C)

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

add_executable(test_lock_core
    test_main.c
    fake_platform.c
    test_creds.c
    test_ratelimit.c
    test_log.c
    test_lock.c
)
target_link_libraries(test_lock_core PRIVATE lock_core)

enable_testing()
add_test(NAME lock_core COMMAND test_lock_core)
