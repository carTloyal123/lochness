if(ESP_PLATFORM)
    idf_component_register(SRCS src/monocypher.c INCLUDE_DIRS include)
else()
    add_library(monocypher STATIC src/monocypher.c)
    target_include_directories(monocypher PUBLIC include)
    # Vendored, unmodified: do not apply our stricter warning flags.
    target_compile_options(monocypher PRIVATE -w)
endif()
