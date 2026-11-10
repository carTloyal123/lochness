/*
 * Matter Door Lock endpoint <-> lock task.
 *
 * Lock/Unlock commands become lock_core requests from the REMOTE source, so
 * the lock's remote policy (remote_unlock_allowed, require_pin_for_remote) and
 * operating modes apply to every Matter controller. LockState follows the lock
 * task's state observer, so keypad, knob and auto-relock operations show up in
 * the controller too.
 *
 * Threads: Matter callbacks run on the CHIP thread and call into the lock task
 * synchronously (lock_task_request). The lock task only ever queues work onto
 * the CHIP thread (ScheduleWork), never waits on it, so the two can't deadlock.
 */
#include "matter_lock.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <vector>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/callback.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/data-model/Nullable.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <clusters/BasicInformation/ClusterId.h>
#include <clusters/DoorLock/ClusterId.h>
#include <clusters/DoorLock/Enums.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_types.h>
#include <host/ble_gatt.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>
#include <lib/core/Optional.h>
#include <lib/support/Span.h>
#include <lib/support/TypeTraits.h>
#include <openthread/instance.h>
#include <openthread/link.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/PlatformManager.h>
#include <platform/ThreadStackManager.h>
#include <platform/internal/BLEManager.h>
#include <protocols/interaction_model/StatusCode.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>
#include <system/SystemClock.h>

#include "ble_server.h"
#include "device_info.h"
#include "link_proto/lp.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_types.h"
#include "lock_task.h"

namespace {

namespace door_lock = chip::app::Clusters::DoorLock;
namespace event_type = chip::DeviceLayer::DeviceEventType;
using chip::EndpointId;
using chip::FabricIndex;
using chip::NodeId;
using chip::app::DataModel::Nullable;

const char *const TAG = "matter";

constexpr uint32_t SECONDS_PER_MINUTE = 60;
/* Matter's limits for a basic commissioning window, in minutes. */
constexpr int WINDOW_MIN_MINUTES = 3;
constexpr int WINDOW_MAX_MINUTES = 15;
/* After the last controller is removed: reopen over DNS-SD only. */
constexpr uint16_t REOPEN_WINDOW_S = 300;
/* OpenThread's netif and task queues (esp-matter's examples use 10). */
constexpr uint8_t THREAD_QUEUE_LEN = 10;

/* Only touched on the CHIP thread. */
struct remote_requester_t {
    bool valid = false;
    Nullable<FabricIndex> fabric;
    Nullable<NodeId> node;
};

struct state_t {
    EndpointId endpoint = chip::kInvalidEndpointId;
    /* Who sent the last remote command, for the LockOperation event that
     * follows when the bolt has moved. */
    remote_requester_t remote{};
};

state_t &state()
{
    static state_t instance;
    return instance;
}

/* ---- lock state -> Matter -------------------------------------------------- */

door_lock::OperationSourceEnum to_source(uint8_t source)
{
    switch (source) {
    case LP_SOURCE_KEYPAD: return door_lock::OperationSourceEnum::kKeypad;
    case LP_SOURCE_REMOTE: return door_lock::OperationSourceEnum::kRemote;
    case LP_SOURCE_APP: return door_lock::OperationSourceEnum::kProprietaryRemote;
    case LP_SOURCE_MANUAL: return door_lock::OperationSourceEnum::kManual;
    case LP_SOURCE_AUTO: return door_lock::OperationSourceEnum::kAuto;
    default: return door_lock::OperationSourceEnum::kUnspecified;
    }
}

/* AutoRelockTime mirrors the lock's own auto-relock setting, but the lock does
 * the relocking (it knows the door and the operating mode). The Door Lock
 * server arms a timer of its own on every unlock, which would report Locked
 * without the bolt moving: cancel it whenever it may have been armed. */
void cancel_server_relock(EndpointId endpoint)
{
    chip::DeviceLayer::SystemLayer().CancelTimer(DoorLockServer::DoorLockOnAutoRelockCallback,
                                                 // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast): the server's own timer context
                                                 reinterpret_cast<void *>(static_cast<uintptr_t>(endpoint)));
}

void cancel_server_relock_work(intptr_t /*arg*/)
{
    if (state().endpoint != chip::kInvalidEndpointId) {
        cancel_server_relock(state().endpoint);
    }
}

/* A move the lock reported with an event: publish it with a LockOperation
 * event. User indices are the lock's own, not Matter users, so leave null. */
void apply_move(EndpointId endpoint, const lp_lock_state_t &lock_state)
{
    const door_lock::DlLockState locked =
        lock_state.bolt == LP_BOLT_LOCKED ? door_lock::DlLockState::kLocked : door_lock::DlLockState::kUnlocked;
    door_lock::OperationSourceEnum source = to_source(lock_state.source);
    Nullable<FabricIndex> fabric;
    Nullable<NodeId> node;
    remote_requester_t &remote = state().remote;
    if (source == door_lock::OperationSourceEnum::kRemote) {
        if (remote.valid && !remote.fabric.IsNull() && !remote.node.IsNull()) {
            fabric = remote.fabric;
            node = remote.node;
        } else {
            /* The server rejects a Remote source without a fabric and node
             * (and then leaves LockState stale). Remote moves we cannot
             * attribute (the console's `remote` stand-in) are published as
             * Unspecified instead. */
            source = door_lock::OperationSourceEnum::kUnspecified;
        }
        remote.valid = false;
    }
    (void)DoorLockServer::Instance().SetLockState(endpoint, locked, source, chip::app::DataModel::NullNullable,
                                                  chip::app::DataModel::NullNullable, fabric, node);
    cancel_server_relock(endpoint);
}

/* A snapshot (no event): settle LockState without a LockOperation event. */
void apply_snapshot(EndpointId endpoint, uint8_t bolt)
{
    DoorLockServer &server = DoorLockServer::Instance();
    switch (bolt) {
    case LP_BOLT_LOCKED: (void)server.SetLockState(endpoint, door_lock::DlLockState::kLocked); break;
    case LP_BOLT_UNLOCKED: (void)server.SetLockState(endpoint, door_lock::DlLockState::kUnlocked); break;
    case LP_BOLT_JAMMED: (void)server.SetLockState(endpoint, door_lock::DlLockState::kNotFullyLocked); break;
    default: break; /* moving or unknown: keep the last settled state */
    }
}

void apply_state(intptr_t arg)
{
    /* Owns the message from on_lock_state. */
    // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast): ScheduleWork passes an intptr_t
    const std::unique_ptr<lp_msg_t> msg(reinterpret_cast<lp_msg_t *>(arg));
    const EndpointId endpoint = state().endpoint;
    if (endpoint == chip::kInvalidEndpointId) {
        return;
    }
    const lp_lock_state_t &lock_state = msg->lock_state;
    const bool settled = lock_state.bolt == LP_BOLT_LOCKED || lock_state.bolt == LP_BOLT_UNLOCKED;
    switch (lock_state.event) {
    case LP_EVENT_JAMMED:
        (void)DoorLockServer::Instance().SetLockState(endpoint, door_lock::DlLockState::kNotFullyLocked);
        (void)DoorLockServer::Instance().SendLockAlarmEvent(endpoint, door_lock::AlarmCodeEnum::kLockJammed);
        break;
    case LP_EVENT_LOCKOUT:
        (void)DoorLockServer::Instance().SendLockAlarmEvent(endpoint, door_lock::AlarmCodeEnum::kWrongCodeEntryLimit);
        break;
    case LP_EVENT_LOCKED:
    case LP_EVENT_UNLOCKED:
        if (settled) {
            apply_move(endpoint, lock_state);
        }
        break;
    case LP_EVENT_NONE: apply_snapshot(endpoint, lock_state.bolt); break;
    default: break;
    }
}

/* Lock task observer: hand a copy to the CHIP thread. */
void on_lock_state(const lp_msg_t *msg)
{
    lp_msg_t *copy = std::unique_ptr<lp_msg_t>(new (std::nothrow) lp_msg_t(*msg)).release();
    if (copy == nullptr) {
        ESP_LOGW(TAG, "dropped a lock state update (out of memory)");
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): ScheduleWork takes an intptr_t
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(apply_state, reinterpret_cast<intptr_t>(copy)) != CHIP_NO_ERROR) {
        const std::unique_ptr<lp_msg_t> reclaim(copy);
        ESP_LOGW(TAG, "dropped a lock state update");
    }
}

/* ---- Matter commands -> lock ----------------------------------------------- */

door_lock::OperationErrorEnum to_error(lc_err_t err)
{
    switch (err) {
    case LC_ERR_DENIED: return door_lock::OperationErrorEnum::kInvalidCredential;
    case LC_ERR_LOCKED_OUT: return door_lock::OperationErrorEnum::kDisabledUserDenied;
    case LC_ERR_SCHEDULE:
    case LC_ERR_MODE: return door_lock::OperationErrorEnum::kRestricted;
    default: return door_lock::OperationErrorEnum::kUnspecified;
    }
}

const char *err_name(lc_err_t err)
{
    switch (err) {
    case LC_OK: return "ok";
    case LC_ERR_DENIED: return "denied";
    case LC_ERR_LOCKED_OUT: return "locked out";
    case LC_ERR_SCHEDULE: return "outside schedule";
    case LC_ERR_MODE: return "mode/policy";
    case LC_ERR_BUSY: return "busy";
    case LC_ERR_DOOR_OPEN: return "door open";
    default: return "error";
    }
}

bool remote_operation(lc_op_t operation, const Nullable<FabricIndex> &fabric, const Nullable<NodeId> &node,
                      const chip::Optional<chip::ByteSpan> &pin, door_lock::OperationErrorEnum &error)
{
    const uint8_t *pin_data = nullptr;
    size_t pin_len = 0;
    if (pin.HasValue()) {
        pin_data = pin.Value().data();
        pin_len = pin.Value().size();
    }
    state().remote = {.valid = true, .fabric = fabric, .node = node};
    /* The lock decides right away (PIN check, policy); the bolt then moves
     * and the LockOperation event follows through on_lock_state. */
    const lc_err_t result = lock_task_request(operation, LC_SOURCE_REMOTE, pin_data, pin_len);
    ESP_LOGI(TAG, "remote %s: %s", operation == LC_OP_LOCK ? "lock" : "unlock", err_name(result));
    if (result != LC_OK) {
        state().remote.valid = false;
        error = to_error(result);
        return false;
    }
    if (operation == LC_OP_UNLOCK) {
        /* The server arms its relock timer after this returns: cancel it
         * from the work queue, which runs after the command. */
        (void)chip::DeviceLayer::PlatformMgr().ScheduleWork(cancel_server_relock_work, 0);
    }
    return true;
}

/* ---- settings: AutoRelockTime <-> the lock's auto_relock_s ----------------- */

void apply_auto_relock(intptr_t seconds)
{
    const EndpointId endpoint = state().endpoint;
    uint32_t current = 0;
    if (endpoint == chip::kInvalidEndpointId ||
        (door_lock::Attributes::AutoRelockTime::Get(endpoint, &current) == chip::Protocols::InteractionModel::Status::Success &&
         current == static_cast<uint32_t>(seconds))) {
        return;
    }
    (void)door_lock::Attributes::AutoRelockTime::Set(endpoint, static_cast<uint32_t>(seconds));
}

/* Lock task observer: the lock's settings were saved (console or Matter). */
void on_lock_config(const lc_config_t *cfg)
{
    (void)chip::DeviceLayer::PlatformMgr().ScheduleWork(apply_auto_relock, static_cast<intptr_t>(cfg->auto_relock_s));
}

/* ---- node ------------------------------------------------------------------ */

void app_event_cb(const ChipDeviceEvent *event, intptr_t /*arg*/)
{
    switch (event->Type) {
    case event_type::kThreadConnectivityChange:
        ESP_LOGI(TAG, "Thread %s", event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established
                                        ? "attached"
                                        : "detached");
        break;
    case event_type::kCommissioningComplete: ESP_LOGI(TAG, "commissioning complete"); break;
    case event_type::kFailSafeTimerExpired: ESP_LOGW(TAG, "commissioning failed: fail-safe timer expired"); break;
    case event_type::kCommissioningWindowOpened: ESP_LOGI(TAG, "commissioning window opened"); break;
    case event_type::kCommissioningWindowClosed: ESP_LOGI(TAG, "commissioning window closed"); break;
    case event_type::kFabricRemoved:
        ESP_LOGI(TAG, "fabric removed");
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
    default: break;
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

bool create_door_lock_endpoint()
{
    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "failed to create Matter node");
        return false;
    }
    esp_matter::endpoint::door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = chip::to_underlying(door_lock::DlLockState::kLocked);
    lock_config.door_lock.actuator_enabled = true;
    lock_config.door_lock.operating_mode = 0;
    esp_matter::endpoint_t *endpoint =
        esp_matter::endpoint::door_lock::create(node, &lock_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "failed to create door lock endpoint");
        return false;
    }
    /* Shown by controllers when present; their values come from
     * device_info.cpp (the attributes are managed by the SDK). */
    esp_matter::cluster_t *basic = esp_matter::cluster::get(esp_matter::endpoint::get(node, 0),
                                                            chip::app::Clusters::BasicInformation::Id);
    esp_matter::cluster_t *lock = esp_matter::cluster::get(endpoint, door_lock::Id);
    if (basic == nullptr || lock == nullptr ||
        esp_matter::cluster::basic_information::attribute::create_serial_number(basic, nullptr, 0) == nullptr ||
        esp_matter::cluster::basic_information::attribute::create_part_number(basic, nullptr, 0) == nullptr ||
        esp_matter::cluster::basic_information::attribute::create_product_label(basic, nullptr, 0) == nullptr ||
        /* Set from the lock's setting once the lock task reports it. */
        esp_matter::cluster::door_lock::attribute::create_auto_relock_time(lock, 0) == nullptr) {
        ESP_LOGE(TAG, "failed to add optional attributes");
        return false;
    }
    ESP_LOGI(TAG, "door lock on endpoint %u", esp_matter::endpoint::get_id(endpoint));
    return true;
}

/* Matter's BLE manager registers its GATT services when it brings up NimBLE;
 * ours must be in the same table. */
bool add_link_service()
{
    std::vector<ble_gatt_svc_def> services;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): NimBLE's table ends with an empty entry
    for (const ble_gatt_svc_def *svc = ble_server_services(); svc->type != 0; svc++) {
        services.push_back(*svc);
    }
    const CHIP_ERROR err = chip::DeviceLayer::Internal::BLEMgrImpl().ConfigureExtraServices(services, false);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "failed to add the link's GATT service: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }
    return true;
}

/* The C6's own 802.15.4 radio, no host processor; OpenThread's settings live
 * in the shared nvs partition. Must be set before esp_matter::start(). */
void configure_thread()
{
    /* esp-matter keeps the pointer. */
    static esp_openthread_platform_config_t config = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs",
                        .netif_queue_size = THREAD_QUEUE_LEN,
                        .task_queue_size = THREAD_QUEUE_LEN},
    };
    set_openthread_platform_config(&config);
}

/* ---- console work (CHIP thread) -------------------------------------------- */

const char *thread_role()
{
    auto &thread = chip::DeviceLayer::ThreadStackMgr();
    if (!thread.IsThreadProvisioned()) {
        return "not provisioned (commission the lock first)";
    }
    if (!thread.IsThreadEnabled()) {
        return "disabled";
    }
    return thread.IsThreadAttached() ? "attached" : "detached (searching)";
}

void print_status_work(intptr_t /*arg*/)
{
    device_info_print();
    auto &server = chip::Server::GetInstance();
    printf("fabrics  %u\n", static_cast<unsigned>(server.GetFabricTable().FabricCount()));
    printf("window   %s\n", server.GetCommissioningWindowManager().IsCommissioningWindowOpen() ? "open" : "closed");
    printf("thread   %s\n", thread_role());
    printf("endpoint %u\n", static_cast<unsigned>(state().endpoint));
}

void print_radio_work(intptr_t reset)
{
    otInstance *instance = esp_openthread_get_instance();
    if (instance == nullptr || !esp_openthread_lock_acquire(portMAX_DELAY)) {
        printf("radio    Thread not running\n");
        return;
    }
    if (reset != 0) {
        otLinkResetCounters(instance);
    }
    const uint8_t channel = otLinkGetChannel(instance);
    const otMacCounters counters = *otLinkGetCounters(instance);
    esp_openthread_lock_release();
    /* A frame that never got the channel (energy above the CCA threshold,
     * or BLE holding the shared radio) counts as a CCA or abort error. */
    printf("radio    channel %u, link advertising %s\n", channel, ble_server_paused() ? "PAUSED" : "on");
    printf("tx       %lu frames, %lu acked, %lu retries\n", static_cast<unsigned long>(counters.mTxTotal),
           static_cast<unsigned long>(counters.mTxAcked), static_cast<unsigned long>(counters.mTxRetry));
    printf("tx fail  %lu channel busy (CCA), %lu aborted, %lu busy, %lu gave up (no ack)\n",
           static_cast<unsigned long>(counters.mTxErrCca), static_cast<unsigned long>(counters.mTxErrAbort),
           static_cast<unsigned long>(counters.mTxErrBusyChannel),
           static_cast<unsigned long>(counters.mTxDirectMaxRetryExpiry));
}

void open_window_work(intptr_t arg)
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
    ESP_LOGI(TAG, "commissioning window open for %lu min (%u controller(s) already added)",
             static_cast<unsigned long>(minutes),
             static_cast<unsigned>(chip::Server::GetInstance().GetFabricTable().FabricCount()));
    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
}

void schedule(chip::DeviceLayer::AsyncWorkFunct work, intptr_t arg)
{
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(work, arg) != CHIP_NO_ERROR) {
        printf("Matter is busy; try again\n");
    }
}

} // namespace

extern "C" esp_err_t matter_lock_start(void)
{
    device_info_init();
    if (!create_door_lock_endpoint() || !add_link_service()) {
        return ESP_FAIL;
    }
    configure_thread();
    const esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Matter: %s", esp_err_to_name(err));
        return err;
    }
    lock_task_set_state_observer(on_lock_state);
    lock_task_set_config_observer(on_lock_config);
    return ble_server_attach();
}

extern "C" void matter_lock_print_status(void) { schedule(print_status_work, 0); }

extern "C" esp_err_t matter_lock_open_window(int minutes)
{
    if (minutes < WINDOW_MIN_MINUTES || minutes > WINDOW_MAX_MINUTES) {
        printf("minutes must be %d..%d (Matter's limits for a basic window)\n", WINDOW_MIN_MINUTES,
               WINDOW_MAX_MINUTES);
        return ESP_ERR_INVALID_ARG;
    }
    schedule(open_window_work, minutes);
    return ESP_OK;
}

extern "C" void matter_lock_print_radio(bool reset) { schedule(print_radio_work, reset ? 1 : 0); }

extern "C" void matter_lock_factory_reset(void) { (void)esp_matter::factory_reset(); }

/* Forced into the link: these replace weak defaults in the door lock server. */
extern "C" void lochness_retain_door_lock_callbacks() {}

// NOLINTBEGIN(readability-identifier-naming): names fixed by the Matter SDK

void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    state().endpoint = endpoint;
    DoorLockServer::Instance().InitServer(endpoint);
    /* Unknown until the lock task's snapshot arrives. */
    (void)door_lock::Attributes::LockState::SetNull(endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId /*endpoint*/, const Nullable<FabricIndex> &fabricIdx,
                                            const Nullable<NodeId> &nodeId,
                                            const chip::Optional<chip::ByteSpan> &pinCode,
                                            door_lock::OperationErrorEnum &err)
{
    return remote_operation(LC_OP_LOCK, fabricIdx, nodeId, pinCode, err);
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId /*endpoint*/, const Nullable<FabricIndex> &fabricIdx,
                                              const Nullable<NodeId> &nodeId,
                                              const chip::Optional<chip::ByteSpan> &pinCode,
                                              door_lock::OperationErrorEnum &err)
{
    return remote_operation(LC_OP_UNLOCK, fabricIdx, nodeId, pinCode, err);
}

/* A controller writes AutoRelockTime: the lock's setting follows (and is
 * saved); the lock then reports it back through on_lock_config. */
chip::Protocols::InteractionModel::Status emberAfPluginDoorLockOnAutoRelockTimeChange(EndpointId /*endpoint*/,
                                                                                    uint32_t newTime)
{
    lock_task_set_auto_relock(newTime);
    return chip::Protocols::InteractionModel::Status::Success;
}

// NOLINTEND(readability-identifier-naming)
