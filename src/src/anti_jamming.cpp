/* anti_jamming.cpp
 *
 * Integrated anti-jamming + aj_switch (CH5/CH7) + FHSS Dual-Radio Sync (Glock)
 *
 * - Contains the full anti-jamming core (sliding window, BY_COUNT / BY_TIME)
 * - Adds aj_switch RC control (CH5/CH7) to enable/disable anti-jamming
 * - When a hop is recommended and anti-jam is enabled, triggers FHSSBeginHopCycle()
 *   followed by FHSSHopNextSynced(FHSS_RADIO_1) and FHSSHopNextSynced(FHSS_RADIO_2)
 *
 * Assumptions:
 *  - anti_jamming.h defines: aj_ctx_t, aj_config_t, aj_report_t, aj_hop_suggestion_t,
 *    aj_hop_cb_t, aj_timestamp_ms_t, AJ_WINDOW_BY_TIME, AJ_WINDOW_BY_COUNT, AJ_STATE_* etc.
 *  - aj_switch.h provides aj_switch_ctx_t and aj_switch_* APIs.
 *  - FHSS.h (or equivalent) provides FHSSBeginHopCycle(), FHSSHopNextSynced(), FHSS_RADIO_1/2.
 *
 * If your project header names differ, change the include lines accordingly.
 */

#include "anti_jamming.h"
#include "aj_switch.h"
#include "FHSS.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------- Internal types (redeclared as in original) -------- */

typedef struct {
    uint8_t          good;      /* 1 = good (CRC OK), 0 = bad */
    aj_timestamp_ms_t ts;       /* timestamp when observed */
} aj_pkt_entry_t;

/* Opaque context backing structure (flexible array at end) */
struct aj_ctx_s {
    /* Config (current) */
    aj_config_t       cfg;

    /* Packet ring buffer */
    uint16_t          capacity; /* equals cfg.window_size_packets (>=1) */
    uint16_t          count;    /* number of valid entries in ring */
    uint16_t          head;     /* next insert index (0..capacity-1) */
    uint16_t          bad_count;

    /* Time-window bookkeeping */
    aj_timestamp_ms_t window_start_ms; /* start of current time window */
    aj_timestamp_ms_t last_now_ms;     /* last notion of "now" we've seen */

    /* State machine & debounce */
    aj_state_t        state;
    uint8_t           jam_streak;          /* how many consecutive jammy windows */
    aj_timestamp_ms_t last_jam_change_ms;  /* when state last changed to/from JAMMED */

    /* External jam signal */
    uint8_t           ext_jam_recent;      /* sticky until aged by time window */
    aj_timestamp_ms_t ext_jam_since_ms;

    /* Hop recommendation pacing */
    aj_timestamp_ms_t last_reco_ms;

    /* Cached last report (for aj_get_report) */
    aj_report_t       last_report;

    /* Callback */
    aj_hop_cb_t       hop_cb;
    void*             hop_cb_ctx;

    /* Ring storage (flexible) */
    aj_pkt_entry_t    entries[1];
};

/* -------- Helpers -------- */

static uint16_t u16_min(uint16_t a, uint16_t b) { return (a < b) ? a : b; }
static uint32_t u32_clamp(uint32_t x, uint32_t lo, uint32_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
static uint8_t u8_clamp_u32_to_u8(uint32_t v) {
    return (uint8_t)u32_clamp(v, 0u, 255u);
}

/* Drop old packets for BY_TIME windows */
static void prune_old_by_time(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    if (ctx->cfg.window_mode != AJ_WINDOW_BY_TIME) return;

    const uint32_t dur = ctx->cfg.window_duration_ms ? ctx->cfg.window_duration_ms : 1u;
    const aj_timestamp_ms_t cutoff = (now_ms > dur) ? (now_ms - dur) : 0u;

    /* Remove from oldest side (tail) while too old */
    uint16_t removed = 0;
    while (ctx->count > 0) {
        uint16_t tail = (uint16_t)((ctx->head + ctx->capacity - ctx->count) % ctx->capacity);
        aj_pkt_entry_t* e = &ctx->entries[tail];
        if (e->ts >= cutoff) {
            break; /* tail is within window */
        }
        /* remove this tail entry */
        if (!e->good && ctx->bad_count > 0) ctx->bad_count--;
        ctx->count--;
        removed++;
    }

    (void)removed;
}

/* Compute instantaneous bad percentage & score */
static void calc_score(const aj_ctx_t* ctx, uint8_t* out_score, uint16_t* out_total, uint16_t* out_bad)
{
    const uint16_t total = ctx->count;
    const uint16_t bad   = ctx->bad_count;

    uint8_t score = 0;
    if (total > 0) {
        /* base score = bad percentage (0..100) */
        uint32_t pct = (uint32_t)bad * 100u / (uint32_t)total;

        /* If external jam was flagged recently, lift score slightly (capped) */
        if (ctx->ext_jam_recent) {
            pct = u32_clamp(pct + 10u, 0u, 100u);
        }
        score = (uint8_t)pct;
    }

    if (out_score) *out_score = score;
    if (out_total) *out_total = total;
    if (out_bad)   *out_bad   = bad;
}

/* Decide if current window is "jammy" (over threshold) */
static uint8_t is_window_jammy(const aj_ctx_t* ctx)
{
    uint8_t score; uint16_t total, bad;
    calc_score(ctx, &score, &total, &bad);

    if (bad < ctx->cfg.min_bad_packets) return 0;

    return (score >= ctx->cfg.jam_threshold_percent) ? 1u : 0u;
}

/* End-of-window processing for debounce/streak logic */
static void on_window_boundary(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    const uint8_t jammy = is_window_jammy(ctx);

    if (jammy) {
        if (ctx->jam_streak < 255) ctx->jam_streak++;
        if (ctx->jam_streak >= ctx->cfg.consecutive_windows_to_jam) {
            if (ctx->state != AJ_STATE_JAMMED) {
                ctx->state = AJ_STATE_JAMMED;
                ctx->last_jam_change_ms = now_ms;
            }
        } else {
            /* Not yet JAMMED, but we consider this SUSPECT if jammy once */
            if (ctx->state == AJ_STATE_NOT_JAMMED) {
                ctx->state = AJ_STATE_SUSPECT;
                ctx->last_jam_change_ms = now_ms;
            }
        }
    } else {
        /* Not jammy this window */
        ctx->jam_streak = 0;
        if (ctx->state == AJ_STATE_JAMMED) {
            /* Respect hold time before softening state */
            uint32_t hold = ctx->cfg.jam_state_hold_time_ms;
            if ((now_ms - ctx->last_jam_change_ms) >= hold) {
                ctx->state = AJ_STATE_SUSPECT; /* soften */
                ctx->last_jam_change_ms = now_ms;
            }
        } else if (ctx->state == AJ_STATE_SUSPECT) {
            /* If very clean, go back to NOT_JAMMED */
            uint8_t score; uint16_t total, bad;
            calc_score(ctx, &score, &total, &bad);
            if (total == 0 || score < (ctx->cfg.jam_threshold_percent / 2u)) {
                ctx->state = AJ_STATE_NOT_JAMMED;
                ctx->last_jam_change_ms = now_ms;
            }
        } else {
            /* already NOT_JAMMED – nothing */
        }
    }
}

/* Update last_report from current snapshot */
static void update_report(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    uint8_t score; uint16_t total, bad;
    calc_score(ctx, &score, &total, &bad);

    /* Confidence: proportional to evidence amount + how far over threshold */
    uint8_t conf = 0;
    if (total > 0) {
        uint32_t over = (score > ctx->cfg.jam_threshold_percent)
                            ? (uint32_t)(score - ctx->cfg.jam_threshold_percent)
                            : 0u;
        uint32_t base = (uint32_t)u32_clamp(total, 0u, 100u); /* cap the base */
        uint32_t c = base / 2u + over;                        /* 0..150-ish */
        conf = (uint8_t)u32_clamp(c, 0u, 100u);
    }

    /* Hop aggressiveness hint: map score (0..100) to 0..255 */
    uint8_t hint = (uint8_t)((uint32_t)score * 255u / 100u);

    aj_report_t rpt;
    rpt.state = ctx->state;
    rpt.score = score;
    rpt.confidence = conf;
    rpt.when = now_ms;
    rpt.hop_aggressiveness_hint = hint;

    /* Recommend hop if JAMMED, or SUSPECT & significantly above threshold,
       and we've respected min_time_between_reco_ms. */
    uint8_t recommend = 0;
    uint32_t dt = now_ms - ctx->last_reco_ms;
    if (dt >= ctx->cfg.min_time_between_reco_ms) {
        if (ctx->state == AJ_STATE_JAMMED) {
            recommend = 1;
        } else if (ctx->state == AJ_STATE_SUSPECT && score >= (uint8_t)u32_clamp(ctx->cfg.jam_threshold_percent + 10u, 0u, 100u)) {
            recommend = 1;
        }
    }
    rpt.recommend_hop = recommend;

    ctx->last_report = rpt;
}

/* Optionally fire callback if we *now* recommend a hop */
static void maybe_fire_hop_callback(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    if (!ctx->hop_cb) return;

    /* Use the already-updated report */
    const aj_report_t* rpt = &ctx->last_report;
    if (!rpt->recommend_hop) return;

    aj_hop_suggestion_t s;
    s.recommend = 1u;
    s.confidence = rpt->confidence;
    s.hop_aggressiveness_hint = rpt->hop_aggressiveness_hint;

    /* Heuristic: suggest band/group switch when score is very high
       or an external jam was seen recently and user allowed it. */
    s.suggest_group_switch =
        (ctx->cfg.allow_group_switch_suggestions &&
         (rpt->score >= 80u || ctx->ext_jam_recent))
            ? 1u
            : 0u;

    /* No specific slot preference by default */
    s.has_preferred_slot = 0u;
    s.preferred_slot_index = 0u;

    /* Rate-limit by min_time_between_reco_ms (already checked in report) */
    ctx->last_reco_ms = now_ms;

    ctx->hop_cb(&s, ctx->hop_cb_ctx);
}

/* -------- Public API (core anti-jamming) -------- */

size_t aj_context_size_bytes(const aj_config_t* cfg)
{
    if (!cfg) return 0u;

    /* Ensure at least 1 entry to prevent div-by-zero paths */
    uint16_t cap = (cfg->window_size_packets == 0) ? 1u : cfg->window_size_packets;

    const size_t base = sizeof(aj_ctx_t) - sizeof(aj_pkt_entry_t); /* flexible header */
    const size_t ring = (size_t)cap * sizeof(aj_pkt_entry_t);

    /* Return total bytes needed */
    return base + ring;
}

static void aj_internal_apply_cfg(aj_ctx_t* ctx, const aj_config_t* cfg)
{
    ctx->cfg = *cfg;
    ctx->capacity = (ctx->cfg.window_size_packets == 0) ? 1u : ctx->cfg.window_size_packets;

    /* Harden obviously-bad user inputs to safe minima */
    if (ctx->cfg.window_mode == AJ_WINDOW_BY_TIME && ctx->cfg.window_duration_ms == 0) {
        ctx->cfg.window_duration_ms = 1000; /* 1s */
    }
    if (ctx->cfg.min_time_between_reco_ms == 0) {
        ctx->cfg.min_time_between_reco_ms = 500; /* default guard */
    }
    if (ctx->cfg.consecutive_windows_to_jam == 0) {
        ctx->cfg.consecutive_windows_to_jam = 1; /* single-window debounce if user asked 0 */
    }
    if (ctx->cfg.jam_threshold_percent > 100) {
        ctx->cfg.jam_threshold_percent = 100;
    }
    if (ctx->cfg.jam_threshold_percent < 1) {
        ctx->cfg.jam_threshold_percent = 1; /* avoid pathological zero */
    }
}

aj_ctx_t* aj_init(void* buffer, size_t buffer_size, const aj_config_t* cfg)
{
    if (!buffer || !cfg) return (aj_ctx_t*)0;

    const size_t need = aj_context_size_bytes(cfg);
    if (need == 0 || buffer_size < need) return (aj_ctx_t*)0;

    aj_ctx_t* ctx = (aj_ctx_t*)buffer;

    /* Zero essential fields */
    for (size_t i = 0; i < buffer_size; ++i) {
        ((uint8_t*)buffer)[i] = 0;
    }

    aj_internal_apply_cfg(ctx, cfg);

    /* Core ring init */
    ctx->count = 0;
    ctx->head  = 0;
    ctx->bad_count = 0;

    /* Time + state */
    ctx->window_start_ms = 0;
    ctx->last_now_ms = 0;
    ctx->state = AJ_STATE_NOT_JAMMED;
    ctx->jam_streak = 0;
    ctx->last_jam_change_ms = 0;

    ctx->ext_jam_recent = 0;
    ctx->ext_jam_since_ms = 0;

    ctx->last_reco_ms = 0;

    /* Report baseline */
    ctx->last_report.state = AJ_STATE_NOT_JAMMED;
    ctx->last_report.score = 0;
    ctx->last_report.confidence = 0;
    ctx->last_report.when = 0;
    ctx->last_report.recommend_hop = 0;
    ctx->last_report.hop_aggressiveness_hint = 0;

    /* No callback initially */
    ctx->hop_cb = (aj_hop_cb_t)0;
    ctx->hop_cb_ctx = (void*)0;

    return ctx;
}

void aj_configure(aj_ctx_t* ctx, const aj_config_t* cfg)
{
    if (!ctx || !cfg) return;

    /* If capacity (window_size_packets) changes, we must fully reset ring usage.
       Memory was pre-sized for the original maximum. For safety, only allow
       capacity changes that do not exceed the preallocated entries. Since we
       cannot know that here, the recommended way is: size the initial buffer
       to the *maximum* you ever need, and only *reduce* capacity here. */
    uint16_t old_capacity = ctx->capacity;
    aj_internal_apply_cfg(ctx, cfg);
    if (ctx->capacity != old_capacity) {
        /* Reinitialize ring usage; storage is already present in the buffer. */
        ctx->count = 0;
        ctx->head  = 0;
        ctx->bad_count = 0;
    }

    /* Reset timing window start for BY_TIME to avoid stale cutoffs */
    ctx->window_start_ms = ctx->last_now_ms;
    /* Keep state but clear streak to re-debounce with new cfg */
    ctx->jam_streak = 0;
}

void aj_reset(aj_ctx_t* ctx)
{
    if (!ctx) return;

    ctx->count = 0;
    ctx->head  = 0;
    ctx->bad_count = 0;

    ctx->window_start_ms = ctx->last_now_ms;
    ctx->state = AJ_STATE_NOT_JAMMED;
    ctx->jam_streak = 0;
    ctx->last_jam_change_ms = ctx->last_now_ms;

    ctx->ext_jam_recent = 0;
    ctx->ext_jam_since_ms = 0;

    ctx->last_reco_ms = 0;

    ctx->last_report.state = AJ_STATE_NOT_JAMMED;
    ctx->last_report.score = 0;
    ctx->last_report.confidence = 0;
    ctx->last_report.recommend_hop = 0;
    ctx->last_report.when = ctx->last_now_ms;
    ctx->last_report.hop_aggressiveness_hint = 0;
}

void aj_register_packet(aj_ctx_t* ctx, uint8_t good, aj_timestamp_ms_t time_ms)
{
    if (!ctx) return;

    ctx->last_now_ms = time_ms;

    /* BY_TIME: prune first to keep ring within the active time window */
    prune_old_by_time(ctx, time_ms);

    /* If full, we will overwrite at head, so adjust counts for the evicted entry */
    if (ctx->count == ctx->capacity) {
        aj_pkt_entry_t* ev = &ctx->entries[ctx->head];
        if (!ev->good && ctx->bad_count > 0) ctx->bad_count--;
    } else {
        ctx->count++;
    }

    /* Insert at head */
    aj_pkt_entry_t* e = &ctx->entries[ctx->head];
    e->good = (good ? 1u : 0u);
    e->ts   = time_ms;
    if (!e->good) ctx->bad_count++;

    /* Advance head */
    ctx->head = (uint16_t)((ctx->head + 1u) % ctx->capacity);

    /* For BY_COUNT, a window "boundary" is each multiple of window_size_packets */
    if (ctx->cfg.window_mode == AJ_WINDOW_BY_COUNT) {
        if ((ctx->count == ctx->capacity) && (ctx->head == 0)) {
            /* Just wrapped — implies we've ingested exactly capacity packets since last wrap */
            on_window_boundary(ctx, time_ms);
        }
    }

    /* Update report and maybe fire callback */
    update_report(ctx, time_ms);
    maybe_fire_hop_callback(ctx, time_ms);
}

void aj_register_external_jam(aj_ctx_t* ctx, aj_timestamp_ms_t time_ms)
{
    if (!ctx) return;
    ctx->last_now_ms = time_ms;

    ctx->ext_jam_recent = 1;
    ctx->ext_jam_since_ms = time_ms;

    /* External jam influences score via calc_score() and may trigger report */
    prune_old_by_time(ctx, time_ms);
    update_report(ctx, time_ms);
    maybe_fire_hop_callback(ctx, time_ms);
}

void aj_tick(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms)
{
    if (!ctx) return;
    ctx->last_now_ms = now_ms;

    /* BY_TIME: prune old packets and check for window boundary */
    if (ctx->cfg.window_mode == AJ_WINDOW_BY_TIME) {
        prune_old_by_time(ctx, now_ms);

        const uint32_t dur = ctx->cfg.window_duration_ms ? ctx->cfg.window_duration_ms : 1u;
        if ((now_ms - ctx->window_start_ms) >= dur) {
            /* One or more windows elapsed – advance to current boundary and process once */
            uint32_t elapsed = now_ms - ctx->window_start_ms;
            uint32_t steps = elapsed / dur;
            if (steps == 0) steps = 1; /* safety */
            ctx->window_start_ms += steps * dur;
            on_window_boundary(ctx, now_ms);
        }
    }

    /* Age-out external jam flag: if no evidence within a full window duration, clear it */
    if (ctx->ext_jam_recent) {
        uint32_t age = now_ms - ctx->ext_jam_since_ms;
        uint32_t limit = (ctx->cfg.window_mode == AJ_WINDOW_BY_TIME)
                           ? (ctx->cfg.window_duration_ms ? ctx->cfg.window_duration_ms : 1000u)
                           : 1000u; /* default 1s for BY_COUNT */
        if (age >= limit) {
            ctx->ext_jam_recent = 0;
        }
    }

    /* Refresh report (no callback from tick alone; only from register/eval) */
    update_report(ctx, now_ms);
}

void aj_get_report(const aj_ctx_t* ctx, aj_report_t* out_report)
{
    if (!ctx || !out_report) return;
    *out_report = ctx->last_report;
}

uint8_t aj_is_jammed(const aj_ctx_t* ctx)
{
    if (!ctx) return 0u;
    return (ctx->state == AJ_STATE_JAMMED) ? 1u : 0u;
}

void aj_evaluate_hop(const aj_ctx_t* ctx_in, aj_hop_suggestion_t* out_sugg)
{
    if (!ctx_in || !out_sugg) return;

    /* We need a mutable ctx only to respect min_time_between_reco_ms pacing.
       Since the API passes const, we won't mutate timing here. We'll simply
       *compute* the suggestion based on current report and let callers decide
       whether to act. aj_register_packet / aj_register_external_jam already
       rate-limit callbacks. */
    const aj_ctx_t* ctx = ctx_in;

    const aj_report_t* rpt = &ctx->last_report;

    aj_hop_suggestion_t s;
    s.recommend = 0u;
    s.confidence = rpt->confidence;
    s.hop_aggressiveness_hint = rpt->hop_aggressiveness_hint;

    /* Same policy as in update_report() but without pacing mutation */
    if (ctx->state == AJ_STATE_JAMMED) {
        s.recommend = 1u;
    } else if (ctx->state == AJ_STATE_SUSPECT &&
               rpt->score >= (uint8_t)u32_clamp(ctx->cfg.jam_threshold_percent + 10u, 0u, 100u)) {
        s.recommend = 1u;
    }

    s.suggest_group_switch =
        (ctx->cfg.allow_group_switch_suggestions &&
         (rpt->score >= 80u || ctx->ext_jam_recent))
            ? 1u
            : 0u;

    s.has_preferred_slot = 0u;
    s.preferred_slot_index = 0u;

    *out_sugg = s;
}

void aj_set_hop_callback(aj_ctx_t* ctx, aj_hop_cb_t cb, void* user_ctx)
{
    if (!ctx) return;
    ctx->hop_cb = cb;
    ctx->hop_cb_ctx = user_ctx;
}

/* ---------------------- End of anti-jamming core ---------------------- */

/* ---------------------------------------------------------------------------
 * Integration layer: aj_switch control + FHSS glue
 * -------------------------------------------------------------------------*/

/* Local flags & contexts */
static aj_ctx_t* g_aj_ctx = NULL;              /* pointer to anti-jamming ctx */
static aj_switch_ctx_t* g_aj_switch_ctx = NULL;/* pointer to aj_switch ctx */
static uint8_t g_anti_jam_enabled = 0u;        /* controlled by CH5/aj_switch */
static uint8_t g_switch_prev_enabled = 0u;     /* helper to detect transitions */

/* Forward declarations for the internal hop callback (registered with aj_set_hop_callback) */
static void anti_jam_internal_hop_cb(const aj_hop_suggestion_t* s, void* user_ctx);

/* Simple start/stop hooks that run when anti-jam is enabled/disabled via switch. */
static void anti_jamming_start_impl(void)
{
    /* Called once when anti-jamming transitions OFF->ON */
    puts("[ANTIJAM] STARTED (rc)");
    /* Optionally reset aj state to avoid immediate hop */
    if (g_aj_ctx) aj_reset(g_aj_ctx);
}

static void anti_jamming_stop_impl(void)
{
    /* Called when anti-jamming transitions ON->OFF */
    puts("[ANTIJAM] STOPPED (rc)");
}

/* aj_switch notify callback (CH5 enable / CH7 mode) */
static void aj_switch_notify_cb(uint8_t enabled, aj_switch_mode_t mode, aj_timestamp_ms_t when_ms, void* user_ctx)
{
    (void)mode; (void)when_ms; (void)user_ctx;
    /* enabled is 0/1 based on RC channel mapping (CH5 by default) */
    g_anti_jam_enabled = enabled ? 1u : 0u;

    if (g_anti_jam_enabled && !g_switch_prev_enabled) {
        /* ON transition */
        anti_jamming_start_impl();
    } else if (!g_anti_jam_enabled && g_switch_prev_enabled) {
        /* OFF transition */
        anti_jamming_stop_impl();
    }

    g_switch_prev_enabled = g_anti_jam_enabled;
}

/* Internal hop callback invoked by anti-jamming engine when it recommends a hop.
   We respect the RC enable flag here and then trigger FHSS synchronized hop
   (Glock): FHSSBeginHopCycle() followed by FHSSHopNextSynced(RADIO_1/2). */
static void anti_jam_internal_hop_cb(const aj_hop_suggestion_t* s, void* user_ctx)
{
    (void)user_ctx;
    if (!s) return;

    /* If anti-jamming is disabled via RC, ignore recommendations */
    if (!g_anti_jam_enabled) {
        /* still might want to log */
        if (s->recommend) puts("[ANTIJAM] hop recommended but system disabled by RC");
        return;
    }

    if (!s->recommend) return;

    /* Respect hop pacing: aj engine already ensures min_time_between_reco_ms for callback
       so we can call FHSS functions immediately */

    /* 1) Begin synchronized cycle */
    FHSSBeginHopCycle();

    /* 2) Both radios call FHSSHopNextSynced() (first caller increments, second reads same index)
       According to the Glock design: call for RADIO_1 then RADIO_2. */
    uint32_t f1 = FHSSHopNextSynced(FHSS_RADIO_1);
    uint32_t f2 = FHSSHopNextSynced(FHSS_RADIO_2);

    /* Log details */
    printf("[ANTIJAM] Hop fired. R1=%lu R2=%lu conf=%u hint=%u group=%u\n",
           (unsigned long)f1,
           (unsigned long)f2,
           (unsigned int)s->confidence,
           (unsigned int)s->hop_aggressiveness_hint,
           (unsigned int)s->suggest_group_switch);
}

/* Public convenience init that wires everything together.
 * - buffer: caller-provided buffer for aj_ctx (size >= aj_context_size_bytes(cfg))
 * - buffer_size: size of buffer
 * - cfg: configuration for anti-jamming
 *
 * This function:
 *   - calls aj_init(buffer, buffer_size, cfg) -> g_aj_ctx
 *   - registers the internal hop callback which triggers FHSS hops when appropriate
 */
aj_ctx_t* anti_jamming_init_with_buffer(void* buffer, size_t buffer_size, const aj_config_t* cfg)
{
    if (!buffer || !cfg) return (aj_ctx_t*)0;

    aj_ctx_t* ctx = aj_init(buffer, buffer_size, cfg);
    if (!ctx) return (aj_ctx_t*)0;

    g_aj_ctx = ctx;

    /* Register internal hop callback to tie recommendations to FHSS */
    aj_set_hop_callback(g_aj_ctx, (aj_hop_cb_t)anti_jam_internal_hop_cb, NULL);

    return g_aj_ctx;
}

/* Initialize aj_switch and register notify callback.
 * Must be called once at startup if you want RC control (CH5/CH7).
 */
void anti_jamming_switch_init(void)
{
    const size_t ctx_size = aj_switch_context_size_bytes();  // <-- ask the module for its true size
    static uint8_t switch_buf[128];                          // allocate safely

    if (ctx_size > sizeof(switch_buf)) {
        printf("[ANTIJAM] aj_switch_ctx_t too large (%u bytes needed)\n", (unsigned)ctx_size);
        return;
    }

    memset(switch_buf, 0, ctx_size);

    g_aj_switch_ctx = aj_switch_init(switch_buf, ctx_size);
    if (!g_aj_switch_ctx) {
        printf("[ANTIJAM] aj_switch_init failed\n");
        return;
    }

    aj_switch_register_notify_cb(g_aj_switch_ctx, aj_switch_notify_cb, NULL);
    printf("[ANTIJAM] aj_switch initialized (%u bytes)\n", (unsigned)ctx_size);
}
/* Service tick to be called regularly from your main loop (ms ticks).
 * - now_ms: current millisecond timestamp (platform millis())
 *
 * This runs:
 *  - aj_switch_process_from_rc()  (reads RC channels & fires switch callback)
 *  - aj_tick(g_aj_ctx, now_ms)    (prune windows / window boundary processing)
 */
void anti_jamming_service_tick(aj_timestamp_ms_t now_ms)
{
    /* Process RC input for switch (this reads RC channels and triggers notify cb) */
    if (g_aj_switch_ctx) {
        aj_switch_process_from_rc(g_aj_switch_ctx, now_ms);
    }

    /* Step the anti-jamming engine (prune windows, update reports). If anti-jam
       is disabled we still call aj_tick to keep internal timeouts/smoothing sane,
       but register_packet / callback handling won't trigger hops since we check
       g_anti_jam_enabled in the internal callback. */
    if (g_aj_ctx) {
        aj_tick(g_aj_ctx, now_ms);
    }
}

/* Convenience wrapper: register packet into g_aj_ctx */
void anti_jamming_register_packet(uint8_t good, aj_timestamp_ms_t time_ms)
{
    if (!g_aj_ctx) return;
    aj_register_packet(g_aj_ctx, good ? 1u : 0u, time_ms);
}

/* Convenience wrapper: external jam event */
void anti_jamming_register_external_jam(aj_timestamp_ms_t time_ms)
{
    if (!g_aj_ctx) return;
    aj_register_external_jam(g_aj_ctx, time_ms);
}

/* Optional accessor to read last report */
void anti_jamming_get_report(aj_report_t* out)
{
    if (!g_aj_ctx || !out) return;
    aj_get_report(g_aj_ctx, out);
}

/* Optional helper to explicitly trigger a synced hop from code (manual API) */
void anti_jamming_force_synced_hop(void)
{
    /* Only act if enabled */
    if (!g_anti_jam_enabled) {
        puts("[ANTIJAM] forced hop request ignored (disabled)");
        return;
    }

    FHSSBeginHopCycle();
    uint32_t f1 = FHSSHopNextSynced(FHSS_RADIO_1);
    uint32_t f2 = FHSSHopNextSynced(FHSS_RADIO_2);
    printf("[ANTIJAM] Forced hop -> R1=%lu R2=%lu\n", (unsigned long)f1, (unsigned long)f2);
}

#ifdef __cplusplus
} /* extern "C" */
#endif
