#include "console.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "keypad_core/kp.h"
#include "keypad_task.h"

static int cmd_keys(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: keys <sequence>   e.g. keys 1234#   (* clear, # unlock, L lock)\n");
        return 1;
    }
    for (const char *k = argv[1]; *k; k++) {
        keypad_task_post_key(*k);
    }
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    keypad_task_post_pair();
    printf("searching for a lock with an open pairing window (run 'pair keypad' on the lock)\n");
    return 0;
}

static int cmd_forget(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("forgets the paired lock. usage: forget confirm\n");
        return 1;
    }
    keypad_task_post_forget();
    return 0;
}

enum { BLE_ADDR_BYTES = 6 };

static void do_status(kp_t *kp, void *arg)
{
    (void)arg;
    static const char *links[] = {"down", "connecting", "handshake", "ready"};
    printf("key      %02x%02x%02x%02x…\n", kp->identity.pub[0], kp->identity.pub[1], kp->identity.pub[2],
           kp->identity.pub[3]);
    if (kp->pairing.paired) {
        /* lock_addr is the address type, then the address little-endian. */
        const uint8_t *addr = kp->pairing.lock_addr + 1;
        printf("lock     %02x%02x%02x%02x… at ", kp->pairing.lock_pub[0], kp->pairing.lock_pub[1],
               kp->pairing.lock_pub[2], kp->pairing.lock_pub[3]);
        for (int i = BLE_ADDR_BYTES - 1; i >= 0; i--) {
            printf(i > 0 ? "%02x:" : "%02x\n", addr[i]);
        }
    } else {
        printf("lock     not paired\n");
    }
    printf("link     %s%s\n", links[kp->link], kp->pairing_mode ? " (pairing)" : "");
    printf("entry    %u digit(s)\n", kp->ndigits);
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    keypad_task_call(do_status, NULL);
    return 0;
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {.command = name, .help = help, .func = func};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "keypad>";

    ESP_ERROR_CHECK(esp_console_register_help_command());
    reg("keys", "Type keys as if pressed (digits, * clear, # unlock, L lock)", cmd_keys);
    reg("pair", "Pair with a lock that has an open pairing window", cmd_pair);
    reg("forget", "Forget the paired lock", cmd_forget);
    reg("status", "Show pairing and link state", cmd_status);

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
