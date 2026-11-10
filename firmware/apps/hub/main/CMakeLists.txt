idf_component_register(SRCS app_main.cpp device_info.cpp door_lock.cpp hub_link.c wall_clock.c
                       INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
# door_lock.cpp overrides weak Door Lock callbacks; keep it in the link.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lochness_retain_door_lock_callbacks")
