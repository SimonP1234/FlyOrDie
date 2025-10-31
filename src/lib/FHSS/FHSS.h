#pragma once

#include "targets.h"
#include "random.h"

/*
 * Additions (2025-10-27):
 * - Second fhss_config_t for Dual Band FHSS on LR1121 (explicit primary/secondary pointers)
 * - Global lock ("Glock") to sync both radios on the same hop index
 * - If hopping is initiated on Radio 1 then Radio 2 hops to the same index
 * - A single hopping function both radios can call to hop to the next frequency (index advanced once per cycle)
 */

#if defined(RADIO_SX127X)
#define FreqCorrectionMax ((int32_t)(100000/FREQ_STEP))
#elif defined(RADIO_LR1121)
#define FreqCorrectionMax ((int32_t)(100000/FREQ_STEP)) // TODO - This needs checking !!!
#elif defined(RADIO_SX128X)
#define FreqCorrectionMax ((int32_t)(200000/FREQ_STEP))
#endif
#define FreqCorrectionMin (-FreqCorrectionMax)

#if defined(RADIO_LR1121)
#define FREQ_HZ_TO_REG_VAL(freq) (freq)
#define FREQ_SPREAD_SCALE 1
#else
#define FREQ_HZ_TO_REG_VAL(freq) ((uint32_t)((double)freq/(double)FREQ_STEP))
#define FREQ_SPREAD_SCALE 256
#endif

#define FHSS_SEQUENCE_LEN 256

typedef struct {
    const char  *domain;
    uint32_t    freq_start;
    uint32_t    freq_stop;
    uint32_t    freq_count;
    uint32_t    freq_center;
} fhss_config_t;

/* ---------------- Existing globals ---------------- */

extern volatile uint8_t FHSSptr;

#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint8_t FHSSptrSynced;

#ifdef __cplusplus
}
#endif

// Keep compatibility for all radios — required by HandleFreqCorr() in rx_main.cpp
extern int32_t FreqCorrection;
extern int32_t FreqCorrection_2;

// Primary Band
extern uint16_t primaryBandCount;
extern uint32_t freq_spread;
extern uint8_t FHSSsequence[];
extern uint_fast8_t sync_channel;
extern const fhss_config_t *FHSSconfig;

// DualBand Variables
extern bool FHSSusePrimaryFreqBand;
extern bool FHSSuseDualBand;
extern uint16_t secondaryBandCount;
extern uint32_t freq_spread_DualBand;
extern uint8_t FHSSsequence_DualBand[];
extern uint_fast8_t sync_channel_DualBand;
extern const fhss_config_t *FHSSconfigDualBand;

extern uint8_t currentDomainIndex;
extern bool domainSwitchPending;
extern uint32_t lastDomainSwitch;
extern uint8_t consecutiveBadPackets;

// Configuration
#define DOMAIN_SWITCH_THRESHOLD 16      // Switch after 5 bad packets
#define DOMAIN_SWITCH_COOLDOWN 500    // 1 second cooldown between switches

/* ---------------- New: Explicit LR1121 Dual-Band config pointers ---------------- */
/* Teine fhss_config_t struktuur LR1121 jaoks. */
#if defined(RADIO_LR1121)
extern const fhss_config_t *FHSSconfigLR1121_Primary;
extern const fhss_config_t *FHSSconfigLR1121_Secondary;
#endif

/* ---------------- Sequence helpers (existing) ---------------- */

// create and randomise an FHSS sequence
void FHSSrandomiseFHSSsequence(uint32_t seed);
void FHSSrandomiseFHSSsequenceBuild(uint32_t seed, uint32_t freqCount, uint_fast8_t sync_channel, uint8_t *sequence);

static inline uint32_t FHSSgetMinimumFreq(void)
{
    return FHSSconfig->freq_start;
}

static inline uint32_t FHSSgetMaximumFreq(void)
{
    return FHSSconfig->freq_stop;
}

// The number of frequencies for this regulatory domain
static inline uint32_t FHSSgetChannelCount(void)
{
    if (FHSSusePrimaryFreqBand)
    {
        return FHSSconfig->freq_count;
    }
    else
    {
        return FHSSconfigDualBand->freq_count;
    }
}

// get the number of entries in the FHSS sequence
static inline uint16_t FHSSgetSequenceCount()
{
    if (FHSSuseDualBand) // Use the smaller of the 2 bands as not to go beyond the max index for each sequence.
    {
        if (primaryBandCount < secondaryBandCount)
        {
            return primaryBandCount;
        }
        else
        {
            return secondaryBandCount;
        }
    }

    if (FHSSusePrimaryFreqBand)
    {
        return primaryBandCount;
    }
    else
    {
        return secondaryBandCount;
    }
}

// get the initial frequency, which is also the sync channel
static inline uint32_t FHSSgetInitialFreq()
{
    if (FHSSusePrimaryFreqBand)
    {
#if defined(RADIO_SX127X)
        return FHSSconfig->freq_start + (sync_channel * freq_spread / FREQ_SPREAD_SCALE) - FreqCorrection;
#else
        /* LR1121/SX128X: FreqCorrection not used */
        return FHSSconfig->freq_start + (sync_channel * freq_spread / FREQ_SPREAD_SCALE);
#endif
    }
    else
    {
        return FHSSconfigDualBand->freq_start + (sync_channel_DualBand * freq_spread_DualBand / FREQ_SPREAD_SCALE);
    }
}

// Get the current sequence pointer
static inline uint8_t FHSSgetCurrIndex()
{
    return FHSSptr;
}

// Is the current frequency the sync frequency
static inline uint8_t FHSSonSyncChannel()
{
    if (FHSSusePrimaryFreqBand)
    {
        return FHSSsequence[FHSSptr] == sync_channel;
    }
    else
    {
        return FHSSsequence_DualBand[FHSSptr] == sync_channel_DualBand;
    }
}

// Set the sequence pointer, used by RX on SYNC
static inline void FHSSsetCurrIndex(const uint8_t value)
{
    FHSSptr = value % FHSSgetSequenceCount();
    // UUS: Sünkroonitud indeksi uuendamine SYNC-i korral
    FHSSptrSynced = FHSSptr;
}

/* ---------------- Existing next-hop (keep for legacy, prefer synced API below) ---------------- */
/* DEPRECATED in dual-radio synced flow — use FHSSBeginHopCycle + FHSSHopNextSynced(). */
static inline uint32_t FHSSgetNextFreq()
{
    FHSSptr = (FHSSptr + 1) % FHSSgetSequenceCount();

    if (FHSSusePrimaryFreqBand)
    {
#if defined(RADIO_SX127X)
        return FHSSconfig->freq_start + (freq_spread * FHSSsequence[FHSSptr] / FREQ_SPREAD_SCALE) - FreqCorrection;
#else
        return FHSSconfig->freq_start + (freq_spread * FHSSsequence[FHSSptr] / FREQ_SPREAD_SCALE);
#endif
    }
    else
    {
        return FHSSconfigDualBand->freq_start + (freq_spread_DualBand * FHSSsequence_DualBand[FHSSptr] / FREQ_SPREAD_SCALE);
    }
}

static inline const char *FHSSgetRegulatoryDomain()
{
    if (FHSSusePrimaryFreqBand)
    {
        return FHSSconfig->domain;
    }
    else
    {
        return FHSSconfigDualBand->domain;
    }
}

// Get frequency offset by half of the domain frequency range
static inline uint32_t FHSSGeminiFreq(uint8_t FHSSsequenceIdx)
{
    uint32_t freq;
    uint32_t numfhss = FHSSgetChannelCount();
    uint8_t offSetIdx = (FHSSsequenceIdx + (numfhss / 2)) % numfhss; 

    if (FHSSusePrimaryFreqBand)
    {
#if defined(RADIO_SX127X)
        freq = FHSSconfig->freq_start + (freq_spread * offSetIdx / FREQ_SPREAD_SCALE) - FreqCorrection_2;
#else
        freq = FHSSconfig->freq_start + (freq_spread * offSetIdx / FREQ_SPREAD_SCALE);
#endif
    }
    else
    {
        freq = FHSSconfigDualBand->freq_start + (freq_spread_DualBand * offSetIdx / FREQ_SPREAD_SCALE);
    }

    return freq;
}

static inline uint32_t FHSSgetGeminiFreq()
{
    if (FHSSuseDualBand)
    {
        // When using Dual Band there is no need to calculate an offset frequency. Unlike Gemini with 2 frequencies in the same band.
        return FHSSconfigDualBand->freq_start + (FHSSsequence_DualBand[FHSSptr] * freq_spread_DualBand / FREQ_SPREAD_SCALE);
    }
    else
    {
        if (FHSSusePrimaryFreqBand)
        {
            return FHSSGeminiFreq(FHSSsequence[FHSSgetCurrIndex()]);
        }
        else
        {
            return FHSSGeminiFreq(FHSSsequence_DualBand[FHSSgetCurrIndex()]);
        }
    }
}

static inline uint32_t FHSSgetInitialGeminiFreq()
{
    if (FHSSuseDualBand)
    {
        return FHSSconfigDualBand->freq_start + (sync_channel_DualBand * freq_spread_DualBand / FREQ_SPREAD_SCALE);
    }
    else
    {
        if (FHSSusePrimaryFreqBand)
        {
            return FHSSGeminiFreq(sync_channel);
        }
        else
        {
            return FHSSGeminiFreq(sync_channel_DualBand);
        }
    }
}

/* ========================= New: Dual-Radio Sync (“Glock”) API ========================= */

/* Radio ID’d (selguse mõttes). */
#define FHSS_RADIO_1    0u
#define FHSS_RADIO_2    1u

/* Jagatud “glock” olek. */
extern volatile uint8_t     FHSSHopCycleArmed;  /* 1 = index not yet advanced this cycle; 0 = already advanced */
extern volatile uint32_t    FHSSSyncEpoch;      /* loendur diagnostikaks */

/* Alusta uut hop-tsüklit: vabastab “glock’i”, et järgmine FHSSHopNextSynced() saaks indeksi ++ teha. */
void FHSSBeginHopCycle(void);

/* Tagasta jooksva sünkroniseeritud indeksi väärtus. */
static inline uint8_t FHSSGetSyncedIndex(void) { return FHSSptrSynced; }

/*
 * Ühine hüppe-funktsioon mõlemale raadiole.
 * - Esimene kutse käesolevas tsüklis: ++indeks (modulo), lukustab tsükli.
 * - Järgmised kutsed samas tsüklis: ei muuda indeksit.
 * - Tagastab raadiole vastava töö-sageduse.
 */
uint32_t FHSSHopNextSynced(uint8_t radio_id);