idf_component_register(
    SRCS main.c lock_task.c console.c motor_sim.c door.c
    INCLUDE_DIRS .
    PRIV_REQUIRES lock_core platform_esp console esp_driver_gpio esp_timer freertos log
)
