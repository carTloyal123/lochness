idf_component_register(
    SRCS main.c lock_task.c lock_bridge.c ble_server.c console.c motor_sim.c door.c
    INCLUDE_DIRS .
    PRIV_REQUIRES lock_core lock_link link_proto secure_link platform_esp ln_store bt console esp_driver_gpio esp_timer freertos log
)
