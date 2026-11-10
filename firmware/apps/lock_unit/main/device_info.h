/* Basic Information (names, serial, versions) shown by Matter controllers. */
#pragma once

/* Call before esp_matter::start(). */
void device_info_init();
/* One line for the console's `matter status`. */
void device_info_print();
