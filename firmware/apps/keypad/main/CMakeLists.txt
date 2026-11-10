idf_component_register(
    SRCS main.c keypad_task.c input.c feedback.c console.c battery.c status_led.c
    INCLUDE_DIRS .
    PRIV_REQUIRES keypad_core link_proto secure_link ln_store ln_central ln_rgb bt console
                  esp_driver_gpio esp_driver_ledc esp_adc esp_timer freertos log
)
