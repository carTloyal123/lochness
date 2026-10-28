set(LOCK_CORE_SRCS
    src/lc_creds.c
    src/lc_lock.c
    src/lc_log.c
    src/lc_ratelimit.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${LOCK_CORE_SRCS} INCLUDE_DIRS include)
else()
    add_library(lock_core STATIC ${LOCK_CORE_SRCS})
    target_include_directories(lock_core PUBLIC include)
    target_compile_features(lock_core PUBLIC c_std_11)
endif()
