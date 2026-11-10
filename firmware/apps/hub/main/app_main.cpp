/*
 * Lochness hub: the Matter node (Door Lock device over Wi-Fi) that drives
 * the lock unit over the lock link. See docs/ARCHITECTURE.md, "Why the hub
 * is the Matter node".
 *
 * Console (dev): matter esp hub status|commission [min]|pair|forget|lock|unlock [pin]
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <clusters/DoorLock/Enums.h>
#include <lib/core/CHIPError.h>
#include <lib/support/TypeTraits.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/ConfigurationManager.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/PlatformManager.h>
#include <system/SystemClock.h>

#include "device_info.h"
#include "door_lock.h"
#include "hub_link.h"
#include "link_proto/lp.h"
#include "ln_store.h"
#include "secure_link/sl_crypto.h"
#include "wall_clock.h"

namespace {

const char *const TAG = "hub";

namespace event_type = chip::DeviceLayer::DeviceEventType;

constexpr uint32_t CONSOLE_REQUEST_TIMEOUT_MS = 4000;
constexpr uint32_t SECONDS_PER_MINUTE = 60;
/* Matter's limits for a basic commissioning window, in minutes. */
constexpr int32_t WINDOW_MIN_MINUTES = 3;
constexpr int32_t WINDOW_MAX_MINUTES = 15;
constexpr int DECIMAL = 10;
/* After the last controller is removed: reopen over DNS-SD only. */
constexpr uint16_t REOPEN_WINDOW_S = 300;
/* Basic Information string limits (+ NUL): names 32, version string 64. */
constexpr size_t NAME_LEN = 33;
constexpr size_t VERSION_LEN = 65;

/* ESP_ERROR_CHECK without its macro (whose temporary trips const checks). */
void require_ok(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
        std::abort();
    }
}

void warn_if_failed(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
}

void app_event_cb(const ChipDeviceEvent *event, intptr_t /*arg*/)
{
    switch (event->Type) {
    case event_type::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address changed");
        wall_clock_network_up();
        break;
    case event_type::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case event_type::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Commissioning failed: fail-safe timer expired");
        break;
    case event_type::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;
    case event_type::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;
    case event_type::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            auto &window = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!window.IsCommissioningWindowOpen()) {
                const CHIP_ERROR err = window.OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds16(REOPEN_WINDOW_S), chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "failed to reopen commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    default:
        break;
    }
}

esp_err_t app_identification_cb(esp_matter::identification::callback_type_t type, uint16_t /*endpoint_id*/,
                                uint8_t effect_id, uint8_t /*effect_variant*/, void * /*priv_data*/)
{
    ESP_LOGI(TAG, "identify: type %u, effect %u", type, effect_id);
    return ESP_OK;
}

esp_err_t app_attribute_update_cb(esp_matter::attribute::callback_type_t /*type*/, uint16_t /*endpoint_id*/,
                                  uint32_t /*cluster_id*/, uint32_t /*attribute_id*/, esp_matter_attr_val_t * /*val*/,
                                  void * /*priv_data*/)
{
    return ESP_OK;
}

/* What controllers read from Basic Information. */
void print_device_info()
{
    std::array<char, NAME_LEN> vendor{"?"};
    std::array<char, NAME_LEN> product{"?"};
    std::array<char, NAME_LEN> serial{"?"};
    std::array<char, VERSION_LEN> version{"?"};
    chip::DeviceLayer::DeviceInstanceInfoProvider *info = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    if (info != nullptr) {
        (void)info->GetVendorName(vendor.data(), vendor.size());
        (void)info->GetProductName(product.data(), product.size());
        (void)info->GetSerialNumber(serial.data(), serial.size());
    }
    (void)chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersionString(version.data(), version.size());
    printf("device   %s %s, serial %s, sw %s\n", vendor.data(), product.data(), serial.data(), version.data());
}

/* Reopen the Matter commissioning window (BLE and DNS-SD) so a controller
 * can add the hub, e.g. after the boot-time window has timed out. Also works
 * once commissioned, for adding a second controller (multi-admin).
 *
 * Runs on the CHIP thread: holding the stack lock from the console task while
 * the window restarts its BLE advertising deadlocks. */
void open_commissioning_window_work(intptr_t arg)
{
    const auto minutes = static_cast<uint32_t>(arg);
    auto &window = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (window.IsCommissioningWindowOpen()) {
        window.CloseCommissioningWindow(); /* restart so the new timeout applies */
    }
    const CHIP_ERROR err = window.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(minutes * SECONDS_PER_MINUTE), chip::CommissioningWindowAdvertisement::kAllSupported);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "failed to open the commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    ESP_LOGI(TAG,
             "commissioning window open for %lu min (%u controller(s) already added); "
             "QR: 'matter onboardingcodes ble', code 3497-011-2332",
             static_cast<unsigned long>(minutes),
             static_cast<unsigned>(chip::Server::GetInstance().GetFabricTable().FabricCount()));
}

esp_err_t open_commissioning_window(const char *arg)
{
    int32_t minutes = WINDOW_MAX_MINUTES;
    if (arg != nullptr) {
        char *end = nullptr;
        const auto parsed = std::strtol(arg, &end, DECIMAL);
        const bool numeric = end != arg && *end == '\0';
        minutes = numeric && parsed >= WINDOW_MIN_MINUTES && parsed <= WINDOW_MAX_MINUTES ? static_cast<int32_t>(parsed)
                                                                                           : -1;
    }
    /* Validate before the work closes an already-open window. */
    if (minutes < WINDOW_MIN_MINUTES) {
        printf("minutes must be %d..%d (Matter's limits for a basic window)\n", static_cast<int>(WINDOW_MIN_MINUTES),
               static_cast<int>(WINDOW_MAX_MINUTES));
        return ESP_ERR_INVALID_ARG;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(open_commissioning_window_work, minutes) != CHIP_NO_ERROR) {
        printf("could not schedule the commissioning window\n");
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *result_name(uint8_t result)
{
    static constexpr std::array<const char *, LP_RESULT_ERROR + 1> NAMES = {
        "ok",   "denied",    "locked out",    "outside schedule", "mode/policy",
        "busy", "door open", "not permitted", "error (link down or no answer)"};
    return result < NAMES.size() ? NAMES.at(result) : "?";
}

void print_status()
{
    print_device_info();
    hub_link_print_status();
    uint32_t unix_s = 0;
    int16_t offset = 0;
    if (wall_clock_now(&unix_s, &offset)) {
        printf("time     %lu, UTC%+d min\n", static_cast<unsigned long>(unix_s), offset);
    } else {
        printf("time     not synced\n");
    }
}

esp_err_t remote_request(lp_op_t operation, const char *pin)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the PIN's characters as bytes
    const uint8_t result = hub_link_request(operation, reinterpret_cast<const uint8_t *>(pin), strlen(pin),
                                            CONSOLE_REQUEST_TIMEOUT_MS, nullptr);
    printf("%s\n", result_name(result));
    return result == LP_RESULT_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t hub_console(int argc, char **argv)
{
    const char *command = argc < 1 ? "status" : argv[0];
    const char *arg = argc > 1 ? argv[1] : nullptr;
    if (strcmp(command, "status") == 0) {
        print_status();
        return ESP_OK;
    }
    if (strcmp(command, "commission") == 0) {
        return open_commissioning_window(arg);
    }
    if (strcmp(command, "pair") == 0) {
        hub_link_pair();
        printf("searching for a lock with an open pairing window (run 'pair hub' on the lock)\n");
        return ESP_OK;
    }
    if (strcmp(command, "forget") == 0) {
        if (arg == nullptr || strcmp(arg, "confirm") != 0) {
            printf("forgets the paired lock. usage: matter esp hub forget confirm\n");
            return ESP_ERR_INVALID_ARG;
        }
        hub_link_forget();
        return ESP_OK;
    }
    if (strcmp(command, "lock") == 0) {
        return remote_request(LP_OP_LOCK, arg != nullptr ? arg : "");
    }
    if (strcmp(command, "unlock") == 0) {
        return remote_request(LP_OP_UNLOCK, arg != nullptr ? arg : "");
    }
    printf("usage: matter esp hub status|commission [min]|pair|forget confirm|lock [pin]|unlock [pin]\n");
    return ESP_ERR_INVALID_ARG;
}

/* The link identity must exist before Matter starts the radio: the bootloader
 * entropy source it is drawn from cannot run alongside Wi-Fi or BLE.
 * hub_link_start loads it later. */
void create_link_identity()
{
    sl_keypair_t identity;
    require_ok(ln_secret_get_or_create("link_id", identity.priv, sizeof(identity.priv)), "link identity");
    sl_wipe(&identity, sizeof(identity));
}

bool create_door_lock_endpoint()
{
    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "failed to create Matter node");
        return false;
    }
    esp_matter::endpoint::door_lock::config_t lock_config;
    lock_config.door_lock.lock_state =
        chip::to_underlying(chip::app::Clusters::DoorLock::DlLockState::kLocked);
    lock_config.door_lock.actuator_enabled = true;
    lock_config.door_lock.operating_mode = 0;
    esp_matter::endpoint_t *endpoint =
        esp_matter::endpoint::door_lock::create(node, &lock_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "failed to create door lock endpoint");
        return false;
    }
    ESP_LOGI(TAG, "door lock on endpoint %u", esp_matter::endpoint::get_id(endpoint));
    return true;
}

void start_console()
{
#if CONFIG_ENABLE_CHIP_SHELL
    static const esp_matter::console::command_t HUB_COMMAND = {
        .name = "hub",
        .description =
            "Lock link. Usage: matter esp hub status|commission [min]|pair|forget confirm|lock [pin]|unlock [pin]",
        .handler = hub_console,
    };
    warn_if_failed(esp_matter::console::add_commands(&HUB_COMMAND, 1), "hub console command");
    warn_if_failed(esp_matter::console::diagnostics_register_commands(), "diagnostics commands");
    warn_if_failed(esp_matter::console::wifi_register_commands(), "wifi commands");
    warn_if_failed(esp_matter::console::factoryreset_register_commands(), "factory reset commands");
    warn_if_failed(esp_matter::console::init(), "console");
#endif
}

} // namespace

extern "C" void app_main()
{
    require_ok(ln_store_init(), "storage");
    create_link_identity();
    wall_clock_init();
    device_info_init();

    if (!create_door_lock_endpoint()) {
        return;
    }
    if (esp_matter::start(app_event_cb) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Matter");
        return;
    }

    static const hub_link_cbs_t LINK_CBS = {
        .lock_state = door_lock_on_state,
        .link_changed = door_lock_on_link,
        .wall_time = wall_clock_now,
    };
    hub_link_start(&LINK_CBS);
    start_console();
    ESP_LOGI(TAG, "lochness hub ready");
}
