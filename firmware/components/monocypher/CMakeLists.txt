if(ESP_PLATFORM)
    idf_component_register(SRCS src/monocypher.c INCLUDE_DIRS include)
    # X25519 dominates link setup (five scalar multiplications per side per
    # session), and the app's default -Og makes it about 3x slower. Always
    # build the crypto for speed; the later flag wins.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
else()
    add_library(monocypher STATIC src/monocypher.c)
    target_include_directories(monocypher PUBLIC include)
    # Vendored, unmodified: do not apply our stricter warning flags.
    target_compile_options(monocypher PRIVATE -w)
endif()
