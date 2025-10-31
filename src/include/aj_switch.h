#pragma once
/*
 * aj_switch.h
 *
 * On / Off switch for Anti-Jamming FHSS mode.
 * Additional mode switch: HIGH / LOW / AUTO.
 * HIGH/LOW/AUTO mode changes must be connected with "controller-only" logic:
 *   - If controller-only is enabled, only remote/controller-originated commands may change mode.
 *   - Local changes (user/receiver side) must be rejected unless controller-only is disabled,
 *     or unless a controller-authorized request is received.
 *
 * Design goals:
 *  - C ABI (usable from C and C++).
 *  - No dynamic allocation.
 *  - Simple callbacks for state changes.
 *  - Clear return codes for permission/authorization.
 */

#include <stdint.h>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* Common timestamp type (ms) used across aj modules */
typedef uint32_t aj_timestamp_ms_t;

/* Mode enum for AJ operation level */
typedef enum {
    AJ_SWITCH_MODE_AUTO = 0,
    AJ_SWITCH_MODE_LOW  = 1,
    AJ_SWITCH_MODE_HIGH = 2
} aj_switch_mode_t;

/* Return codes for set/request operations */
typedef enum {
    AJ_SW_RET_OK = 0,            /* Operation succeeded */
    AJ_SW_RET_DENIED = 1,        /* Operation denied (e.g., controller-only restriction) */
    AJ_SW_RET_NOCHANGE = 2,      /* Requested mode is already active */
    AJ_SW_RET_INVALID = 3        /* Invalid argument */
} aj_switch_result_t;

/* Callback type for state/mode change notifications.
 * - enabled: 0 = disabled (anti-jamming FHSS off), 1 = enabled (on)
 * - new_mode: the active AJ mode (AUTO/LOW/HIGH)
 * - when_ms: timestamp when change happened (millis)
 * - user_ctx: user-supplied pointer
 */
typedef void (*aj_switch_notify_cb_t)(uint8_t enabled, aj_switch_mode_t new_mode, aj_timestamp_ms_t when_ms, void* user_ctx);

/* Opaque context (implementation may keep state). If not needed, allow NULL usage. */
typedef struct aj_switch_ctx_s aj_switch_ctx_t;

/* === Initialization / context allocation helpers === */

/* Return how many bytes of context are required (so caller can allocate statically). */
size_t aj_switch_context_size_bytes(void);

/* Initialize context in provided buffer (buffer_size must be >= context_size_bytes()). Returns pointer to context or NULL on error. */
aj_switch_ctx_t* aj_switch_init(void* buffer, size_t buffer_size);

/* Reset to defaults: disabled, AUTO mode, controller-only disabled. */
void aj_switch_reset(aj_switch_ctx_t* ctx);

/* === Enable / disable AJ FHSS mode === */

/* Enable or disable anti-jamming FHSS mode locally.
 * If controller-only is enabled, local enable/disable may still be allowed (policy may vary);
 * use aj_switch_request_enable_from_controller() for controller-originated requests.
 *
 * Returns AJ_SW_RET_OK on success or AJ_SW_RET_NOCHANGE if state already matches.
 */
aj_switch_result_t aj_switch_set_enabled(aj_switch_ctx_t* ctx, uint8_t enable, aj_timestamp_ms_t when_ms);

/* Query current enabled state (0/1). */
uint8_t aj_switch_is_enabled(const aj_switch_ctx_t* ctx);

/* === Mode management (AUTO/LOW/HIGH) === */

/* Try to set mode locally. If controller-only lock is active, this returns AJ_SW_RET_DENIED.
 * Returns AJ_SW_RET_OK on success, AJ_SW_RET_NOCHANGE if already set, or AJ_SW_RET_DENIED/AJ_SW_RET_INVALID.
 *
 * when_ms: timestamp (millis) of request/event.
 */
aj_switch_result_t aj_switch_set_mode_local(aj_switch_ctx_t* ctx, aj_switch_mode_t mode, aj_timestamp_ms_t when_ms);

/* Request mode change originating from controller/remote. This bypasses controller-only local lock.
 * Typically called when processing a validated command from the controller.
 *
 * Returns AJ_SW_RET_OK or AJ_SW_RET_NOCHANGE or AJ_SW_RET_INVALID.
 */
aj_switch_result_t aj_switch_set_mode_from_controller(aj_switch_ctx_t* ctx, aj_switch_mode_t mode, aj_timestamp_ms_t when_ms);

/* Query active mode. */
aj_switch_mode_t aj_switch_get_mode(const aj_switch_ctx_t* ctx);

/* === Controller-only policy / authorization === */

/* Enable or disable controller-only enforcement.
 * If enabled (1), local (receiver-side / user) attempts to change AJ mode will be rejected.
 * Controller-originated functions (aj_switch_set_mode_from_controller, aj_switch_request_enable_from_controller)
 * are allowed to change state while controller-only is enabled.
 */
void aj_switch_set_controller_only(aj_switch_ctx_t* ctx, uint8_t controller_only);
uint8_t aj_switch_is_controller_only(const aj_switch_ctx_t* ctx);

/* === Controller-originated enable/disable (bypasses controller-only lock) === */

/* Controller requests to enable/disable AJ mode (should be called only after verifying controller auth).
 * This always applies the change regardless of controller-only flag.
 */
aj_switch_result_t aj_switch_request_enable_from_controller(aj_switch_ctx_t* ctx, uint8_t enable, aj_timestamp_ms_t when_ms);

/* === Callbacks / notifications === */

/* Register notification callback. If cb == NULL, unregisters callback.
 * Callback is called on any successful enable/disable or mode change (whether local or controller-originated).
 */
void aj_switch_register_notify_cb(aj_switch_ctx_t* ctx, aj_switch_notify_cb_t cb, void* user_ctx);

/* === Misc helpers / diagnostics === */

/* Fill a small status struct (caller-provided) for telemetry/UI. */
typedef struct {
    uint8_t enabled;             /* 0/1 */
    aj_switch_mode_t mode;       /* AUTO/LOW/HIGH */
    uint8_t controller_only;     /* 0/1 */
    aj_timestamp_ms_t last_change_ms; /* timestamp of last change */
} aj_switch_status_t;

/* Get current status snapshot. */
void aj_switch_get_status(const aj_switch_ctx_t* ctx, aj_switch_status_t* out_status);

/* === Notes for integrators ===
 * - Controller-originated functions should only be called after verifying that the request was authenticated
 *   (eg validated telemetry/packet signature or known binding). This header does not perform authentication.
 * - The difference between set_mode_local() and set_mode_from_controller() is permission: controller requests
 *   always allowed, local requests are blocked when controller-only is set.
 * - Implementations should call the registered notification callback after state changes (and update last_change_ms).
 * - Typical flow:
 *     - System boots -> aj_switch_init() -> aj_switch_reset()
 *     - UI/user toggles local switch -> aj_switch_set_mode_local() -> may be denied if controller_only
 *     - Telemetry packet from controller requests "HIGH" -> verify -> aj_switch_set_mode_from_controller()
 *     - When AJ enabled and mode changes, anti-jamming module (aj module) should be notified to alter behavior.
 */

/* === Compatibility stub for anti_jamming.cpp ===
 * These are legacy functions used by anti_jamming_switch_init() and anti_jamming_service_tick().
 * They let the anti-jamming layer compile without a real RC reader yet.
 */
/* Dummy function to satisfy anti_jamming.cpp until RC reading is implemented */
static inline void aj_switch_process_from_rc(aj_switch_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    (void)ctx;
    (void)now_ms;
    /* TODO: implement RC polling (CH5 etc.) if needed */
}



#ifdef __cplusplus
} /* extern "C" */
#endif