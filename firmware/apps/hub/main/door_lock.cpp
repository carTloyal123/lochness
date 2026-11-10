/*
 * Matter Door Lock cluster <-> lock link.
 *
 * Lock/Unlock commands are forwarded to the lock and answered with its
 * decision; the lock-side remote policy (remote_unlock_allowed,
 * require_pin_for_remote) is the authority, not this hub. LockState follows
 * the LOCK_STATE messages the lock pushes, so operations from the keypad,
 * the knob or auto-relock show up in the controller too.
 */
#include "door_lock.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/callback.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/data-model/Nullable.h>
#include <clusters/DoorLock/Enums.h>
#include <esp_log.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>
#include <lib/core/Optional.h>
#include <lib/support/Span.h>
#include <platform/PlatformManager.h>

#include "hub_link.h"
#include "link_proto/lp.h"

namespace {

namespace door_lock = chip::app::Clusters::DoorLock;
using chip::EndpointId;
using chip::FabricIndex;
using chip::NodeId;
using chip::app::DataModel::Nullable;

const char *const TAG = "door_lock";

constexpr uint32_t REQUEST_TIMEOUT_MS = 4000;

/* Only touched in the CHIP context. */
struct remote_requester_t {
    bool valid = false;
    Nullable<FabricIndex> fabric;
    Nullable<NodeId> node;
};

struct state_t {
    EndpointId endpoint = chip::kInvalidEndpointId;
    /* Who sent the last remote command, for the LockOperation event that
     * follows when the lock reports the move. */
    remote_requester_t remote{};
};

state_t &state()
{
    static state_t instance;
    return instance;
}

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

door_lock::OperationErrorEnum to_error(uint8_t result)
{
    switch (result) {
    case LP_RESULT_DENIED: return door_lock::OperationErrorEnum::kInvalidCredential;
    case LP_RESULT_LOCKED_OUT: return door_lock::OperationErrorEnum::kDisabledUserDenied;
    case LP_RESULT_SCHEDULE:
    case LP_RESULT_MODE:
    case LP_RESULT_NOT_PERMITTED: return door_lock::OperationErrorEnum::kRestricted;
    default: return door_lock::OperationErrorEnum::kUnspecified;
    }
}

const char *result_name(uint8_t result)
{
    static constexpr std::array<const char *, LP_RESULT_ERROR + 1> NAMES = {
        "ok", "denied", "locked out", "outside schedule", "mode/policy", "busy", "door open", "not permitted", "error"};
    return result < NAMES.size() ? NAMES.at(result) : "?";
}

bool remote_operation(lp_op_t operation, const Nullable<FabricIndex> &fabric, const Nullable<NodeId> &node,
                      const chip::Optional<chip::ByteSpan> &pin, door_lock::OperationErrorEnum &error)
{
    const uint8_t *pin_data = nullptr;
    size_t pin_len = 0;
    if (pin.HasValue()) {
        pin_data = pin.Value().data();
        pin_len = pin.Value().size();
        if (pin_len > LP_PIN_FIELD_LEN) {
            error = door_lock::OperationErrorEnum::kInvalidCredential;
            return false;
        }
    }
    state().remote = {.valid = true, .fabric = fabric, .node = node};
    /* Blocks the CHIP thread until the lock answers: usually well under a
     * second over the open link, bounded by REQUEST_TIMEOUT_MS. */
    const uint8_t result = hub_link_request(operation, pin_data, pin_len, REQUEST_TIMEOUT_MS, nullptr);
    ESP_LOGI(TAG, "remote %s: %s", operation == LP_OP_LOCK ? "lock" : "unlock", result_name(result));
    if (result != LP_RESULT_OK) {
        state().remote.valid = false;
        error = to_error(result);
        return false;
    }
    return true;
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
             * attribute (hub console, another lock controller) are
             * published as Unspecified instead. */
            source = door_lock::OperationSourceEnum::kUnspecified;
        }
        remote.valid = false;
    }
    (void)DoorLockServer::Instance().SetLockState(endpoint, locked, source, chip::app::DataModel::NullNullable,
                                                  chip::app::DataModel::NullNullable, fabric, node);
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
    /* Owns the message from door_lock_on_state. */
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

void apply_link_down(intptr_t /*arg*/)
{
    const EndpointId endpoint = state().endpoint;
    if (endpoint != chip::kInvalidEndpointId) {
        /* Null LockState: the controller shows the state as unknown. */
        (void)door_lock::Attributes::LockState::SetNull(endpoint);
    }
}

} // namespace

extern "C" void door_lock_on_state(const lp_msg_t *msg)
{
    /* apply_state takes ownership; reclaimed here if it never runs. */
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

extern "C" void door_lock_on_link(bool is_up)
{
    /* On up, the lock's snapshot sets LockState. */
    if (!is_up && chip::DeviceLayer::PlatformMgr().ScheduleWork(apply_link_down, 0) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "dropped a link-down update");
    }
}

/* Forced into the link: these replace weak defaults in the door lock server. */
extern "C" void lochness_retain_door_lock_callbacks() {}

// NOLINTBEGIN(readability-identifier-naming): names fixed by the Matter SDK

void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    state().endpoint = endpoint;
    DoorLockServer::Instance().InitServer(endpoint);
    /* Unknown until the lock reports in. */
    (void)door_lock::Attributes::LockState::SetNull(endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId /*endpoint*/, const Nullable<FabricIndex> &fabricIdx,
                                            const Nullable<NodeId> &nodeId,
                                            const chip::Optional<chip::ByteSpan> &pinCode,
                                            door_lock::OperationErrorEnum &err)
{
    return remote_operation(LP_OP_LOCK, fabricIdx, nodeId, pinCode, err);
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId /*endpoint*/, const Nullable<FabricIndex> &fabricIdx,
                                              const Nullable<NodeId> &nodeId,
                                              const chip::Optional<chip::ByteSpan> &pinCode,
                                              door_lock::OperationErrorEnum &err)
{
    return remote_operation(LP_OP_UNLOCK, fabricIdx, nodeId, pinCode, err);
}

// NOLINTEND(readability-identifier-naming)
