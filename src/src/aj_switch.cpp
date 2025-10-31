#include "aj_switch.h"
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal context */
struct aj_switch_ctx_s {
    uint8_t enabled;
    aj_switch_mode_t mode;
    uint8_t controller_only;
    aj_timestamp_ms_t last_change_ms;
    aj_switch_notify_cb_t cb;
    void* cb_ctx;
};

/* ---- Internal helper ---- */
static void notify_if_changed(aj_switch_ctx_t* ctx)
{
    if (ctx && ctx->cb) {
        ctx->cb(ctx->enabled, ctx->mode, ctx->last_change_ms, ctx->cb_ctx);
    }
}

/* ---- API implementation ---- */

size_t aj_switch_context_size_bytes(void)
{
    return sizeof(aj_switch_ctx_t);
}

aj_switch_ctx_t* aj_switch_init(void* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < sizeof(aj_switch_ctx_t))
        return NULL;

    aj_switch_ctx_t* ctx = (aj_switch_ctx_t*)buffer;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = AJ_SWITCH_MODE_AUTO;
    ctx->enabled = 0;
    ctx->controller_only = 0;
    ctx->cb = NULL;
    ctx->cb_ctx = NULL;
    ctx->last_change_ms = 0;
    return ctx;
}

void aj_switch_reset(aj_switch_ctx_t* ctx)
{
    if (!ctx) return;
    ctx->enabled = 0;
    ctx->mode = AJ_SWITCH_MODE_AUTO;
    ctx->controller_only = 0;
    ctx->last_change_ms = 0;
}

aj_switch_result_t aj_switch_set_enabled(aj_switch_ctx_t* ctx, uint8_t enable, aj_timestamp_ms_t when_ms)
{
    if (!ctx) return AJ_SW_RET_INVALID;
    if (ctx->enabled == (enable ? 1 : 0)) return AJ_SW_RET_NOCHANGE;

    ctx->enabled = (enable ? 1 : 0);
    ctx->last_change_ms = when_ms;
    notify_if_changed(ctx);
    return AJ_SW_RET_OK;
}

uint8_t aj_switch_is_enabled(const aj_switch_ctx_t* ctx)
{
    return ctx ? ctx->enabled : 0;
}

/* ---- Mode management ---- */
aj_switch_result_t aj_switch_set_mode_local(aj_switch_ctx_t* ctx, aj_switch_mode_t mode, aj_timestamp_ms_t when_ms)
{
    if (!ctx) return AJ_SW_RET_INVALID;
    if (mode > AJ_SWITCH_MODE_HIGH) return AJ_SW_RET_INVALID;
    if (ctx->controller_only) return AJ_SW_RET_DENIED;
    if (ctx->mode == mode) return AJ_SW_RET_NOCHANGE;

    ctx->mode = mode;
    ctx->last_change_ms = when_ms;
    notify_if_changed(ctx);
    return AJ_SW_RET_OK;
}

aj_switch_result_t aj_switch_set_mode_from_controller(aj_switch_ctx_t* ctx, aj_switch_mode_t mode, aj_timestamp_ms_t when_ms)
{
    if (!ctx) return AJ_SW_RET_INVALID;
    if (mode > AJ_SWITCH_MODE_HIGH) return AJ_SW_RET_INVALID;
    if (ctx->mode == mode) return AJ_SW_RET_NOCHANGE;

    ctx->mode = mode;
    ctx->last_change_ms = when_ms;
    notify_if_changed(ctx);
    return AJ_SW_RET_OK;
}

aj_switch_mode_t aj_switch_get_mode(const aj_switch_ctx_t* ctx)
{
    return ctx ? ctx->mode : AJ_SWITCH_MODE_AUTO;
}

/* ---- Controller-only policy ---- */
void aj_switch_set_controller_only(aj_switch_ctx_t* ctx, uint8_t controller_only)
{
    if (!ctx) return;
    ctx->controller_only = controller_only ? 1 : 0;
}

uint8_t aj_switch_is_controller_only(const aj_switch_ctx_t* ctx)
{
    return ctx ? ctx->controller_only : 0;
}

/* ---- Controller-originated enable/disable ---- */
aj_switch_result_t aj_switch_request_enable_from_controller(aj_switch_ctx_t* ctx, uint8_t enable, aj_timestamp_ms_t when_ms)
{
    if (!ctx) return AJ_SW_RET_INVALID;
    if (ctx->enabled == (enable ? 1 : 0)) return AJ_SW_RET_NOCHANGE;

    ctx->enabled = (enable ? 1 : 0);
    ctx->last_change_ms = when_ms;
    notify_if_changed(ctx);
    return AJ_SW_RET_OK;
}

/* ---- Callback registration ---- */
void aj_switch_register_notify_cb(aj_switch_ctx_t* ctx, aj_switch_notify_cb_t cb, void* user_ctx)
{
    if (!ctx) return;
    ctx->cb = cb;
    ctx->cb_ctx = user_ctx;
}

/* ---- Status helper ---- */
void aj_switch_get_status(const aj_switch_ctx_t* ctx, aj_switch_status_t* out_status)
{
    if (!ctx || !out_status) return;
    out_status->enabled = ctx->enabled;
    out_status->mode = ctx->mode;
    out_status->controller_only = ctx->controller_only;
    out_status->last_change_ms = ctx->last_change_ms;
}

#ifdef __cplusplus
}
#endif
