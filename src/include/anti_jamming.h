#pragma once
/*
 * anti_jamming.h  (ExpressLRS-friendly)
 *
 * - Sliding window
 * - Good/bad packet registration
 * - Parameter configuration
 * - Jamming state detection
 * - Issues HOP *recommendations* (does not force FHSS)
 * - Debounce so we don't recommend every window
 *
 * No STL, no exceptions, no dynamic allocation required.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Millisekundi täpsusega ajamärk (nt millis()). */
typedef uint32_t aj_timestamp_ms_t;

/* Jammi olek. */
typedef enum {
    AJ_STATE_NOT_JAMMED = 0,
    AJ_STATE_SUSPECT    = 1,
    AJ_STATE_JAMMED     = 2
} aj_state_t;

/* Aknarežiim. */
typedef enum {
    AJ_WINDOW_BY_COUNT = 0,
    AJ_WINDOW_BY_TIME  = 1
} aj_window_mode_t;

/* Konfiguratsioon. */
typedef struct {
    /* Sliding window (kasutatakse vastavalt window_mode’ile). */
    uint16_t        window_size_packets;          /* nt 100 */
    uint32_t        window_duration_ms;           /* nt 1000 */
    aj_window_mode_t window_mode;                 /* BY_COUNT või BY_TIME */

    /* Jammi lävi (protsent 0..100) ja minimaalne halbade pakettide arv. */
    uint8_t         jam_threshold_percent;        /* nt 30 */
    uint16_t        min_bad_packets;              /* nt 5 */

    /* Debounce: mitu järjestikust akent peab ületama läve, et minna JAMMED. */
    uint8_t         consecutive_windows_to_jam;   /* nt 2 */

    /* Kui kaua hoitakse JAMMED olekut enne pehmenemist (ms). */
    uint32_t        jam_state_hold_time_ms;       /* nt 2000 */

    /* Min aeg kahe hop-soovituse vahel (ms). */
    uint32_t        min_time_between_reco_ms;     /* nt 500 */

    /* Luba tulevikus grupi/bandi vahetuse soovitus (true/false). */
    uint8_t         allow_group_switch_suggestions; /* 0/1 */
} aj_config_t;

/* Hop-soovituse struktuur. */
typedef struct {
    uint8_t  recommend;                /* 0/1 */
    uint8_t  confidence;               /* 0..100 */
    uint8_t  suggest_group_switch;     /* 0/1 */
    uint8_t  hop_aggressiveness_hint;  /* 0..255 (0=leebe) */
    uint32_t preferred_slot_index;     /* kasutusel vaid siis, kui meaningful; muidu ignoreeri */
    uint8_t  has_preferred_slot;       /* 0/1 */
} aj_hop_suggestion_t;

/* Raport (sköör + olek + kas hetkel soovitatakse hop’ida). */
typedef struct {
    aj_state_t      state;             /* NOT_JAMMED / SUSPECT / JAMMED */
    uint8_t         score;             /* 0..100 (0 puhas, 100 tugev jam) */
    uint8_t         recommend_hop;     /* 0/1 – sama loogika, mis evaluate_hop_suggestion() */
    uint8_t         confidence;        /* 0..100 */
    aj_timestamp_ms_t when;            /* ajamärk, millal arvutati */
    uint8_t         hop_aggressiveness_hint; /* 0..255 */
} aj_report_t;

/* Callbacki tüüp hop-soovituse jaoks. */
typedef void (*aj_hop_cb_t)(const aj_hop_suggestion_t* sugg, void* user_ctx);

/* Opaque handle implementatsiooni jaoks. */
typedef struct aj_ctx_s aj_ctx_t;

/* === Elutsükkel / konfiguratsioon === */

/* Arvuta, kui palju mälu kontekst vajab (baiti). Võimaldab staatilist allokeerimist. */
size_t aj_context_size_bytes(const aj_config_t* cfg);

/* Initsialiseeri etteantud puhvris (buffer peab olema vähemalt aj_context_size_bytes()). */
aj_ctx_t* aj_init(void* buffer, size_t buffer_size, const aj_config_t* cfg);

/* Muuda konfiguratsiooni lennult (võib resettida akna arvestust). */
void aj_configure(aj_ctx_t* ctx, const aj_config_t* cfg);

/* Täielik reset. */
void aj_reset(aj_ctx_t* ctx);

/* === Andmesisend === */

/* Registreeri pakett: good=1 kui CRC OK; good=0 kui CRC error vmt. time_ms = millis(). */
void aj_register_packet(aj_ctx_t* ctx, uint8_t good, aj_timestamp_ms_t time_ms);

/* Registreeri väline jam-signaal (nt RF frontend overload). */
void aj_register_external_jam(aj_ctx_t* ctx, aj_timestamp_ms_t time_ms);

/* Perioodiline uuendus (võib kutsuda RX loopis). */
void aj_tick(aj_ctx_t* ctx, aj_timestamp_ms_t now_ms);

/* === Päringud === */

/* Võta viimane raport. */
void aj_get_report(const aj_ctx_t* ctx, aj_report_t* out_report);

/* Kiire kontroll: kas olek on JAMMED. */
uint8_t aj_is_jammed(const aj_ctx_t* ctx);

/* Arvuta hop-soovitus (ilma callbackita). */
void aj_evaluate_hop(const aj_ctx_t* ctx, aj_hop_suggestion_t* out_sugg);

/* === Callback === */

/* Registreeri hop-soovituse callback (anti_jamming EI muuda sagedust ise). */
void aj_set_hop_callback(aj_ctx_t* ctx, aj_hop_cb_t cb, void* user_ctx);


/* --- Integration layer public API --- */
aj_ctx_t* anti_jamming_init_with_buffer(void* buffer, size_t buffer_size, const aj_config_t* cfg);
void anti_jamming_switch_init(void);
void anti_jamming_service_tick(aj_timestamp_ms_t now_ms);
void anti_jamming_register_packet(uint8_t good, aj_timestamp_ms_t time_ms);
void anti_jamming_register_external_jam(aj_timestamp_ms_t time_ms);
void anti_jamming_get_report(aj_report_t* out);
void anti_jamming_force_synced_hop(void);


#ifdef __cplusplus
} /* extern "C" */
#endif
