idf_build_get_property(matter LOCHNESS_MATTER)

set(srcs main.c lock_task.c lock_bridge.c ble_server.c console.c motor_sim.c door.c status_led.c)
set(requires lock_core lock_link link_proto secure_link platform_esp ln_store ln_rgb bt console esp_driver_gpio esp_timer
             freertos log)
if(matter)
    list(APPEND srcs matter_lock.cpp device_info.cpp)
    list(APPEND requires esp_matter esp_hw_support)
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS . PRIV_REQUIRES ${requires})

if(matter)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LOCHNESS_MATTER=1 CHIP_HAVE_CONFIG_H)
    # matter_lock.cpp overrides weak Door Lock callbacks; keep it in the link.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lochness_retain_door_lock_callbacks")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LOCHNESS_MATTER=0)
endif()
