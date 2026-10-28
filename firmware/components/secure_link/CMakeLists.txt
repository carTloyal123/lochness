set(SECURE_LINK_SRCS src/sl_sha256.c src/sl_crypto.c src/sl_noise.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${SECURE_LINK_SRCS} INCLUDE_DIRS include PRIV_REQUIRES monocypher)
else()
    add_library(secure_link STATIC ${SECURE_LINK_SRCS})
    target_include_directories(secure_link PUBLIC include)
    target_link_libraries(secure_link PRIVATE monocypher)
endif()
