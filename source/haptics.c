// Haptic patterns for game events. Each pattern is a few voices (sine or low-passed noise with a frequency sweep and
// an attack / hold / decay envelope) that MissionControl synthesises on the DualSense voice coils and mixes with the
// game's own rumble. Left/right amplitudes put an effect in the hand that would feel it: the weapon hand is the right
// one, the shield and the bow are held in the left.
#include <string.h>
#include "haptics.h"
#include "mc.h"

void LogVerbose(const char *fmt, ...);

HapticSettings g_haptic_settings = { .enabled = true, .strength = 100 };

#define NOISE McHapticFlag_Noise
#define LOOP  McHapticFlag_Loop

typedef struct {
    u8 flags;
    u8 left, right;
    u16 freq_start, freq_end;
    u16 delay, attack, hold, decay;
} VoiceDef;

typedef struct {
    const char *name;
    u32 count;
    VoiceDef voices[MC_HAPTIC_VOICES_MAX];
} PatternDef;

static const PatternDef Patterns[HapticPattern_Count] = {
    [HapticPattern_HitSword] = { "hit sword", 2, {
        { 0,     120, 230, 220, 130,   0,   2,  12,  70 },  // crisp knock
        { NOISE,  90, 160, 180, 180,   0,   1,   8,  60 },  // body of the impact
    }},
    [HapticPattern_HitHeavy] = { "hit heavy", 2, {
        { 0,     255, 255,  75,  40,   0,   4,  40, 200 },  // deep thud
        { NOISE, 180, 200, 140, 140,   0,   2,  30, 160 },
    }},
    [HapticPattern_HitSpear] = { "hit spear", 2, {
        { 0,      80, 220, 300, 180,   0,   1,   8,  45 },  // short sharp poke
        { NOISE,  60, 140, 300, 300,   0,   1,   5,  35 },
    }},
    [HapticPattern_ShieldBlock] = { "shield block", 2, {
        { 0,     255, 150,  95,  55,   0,   2,  35, 170 },  // heavy knock into the shield hand
        { NOISE, 200, 110, 220, 220,   0,   1,  20, 110 },
    }},
    [HapticPattern_BowNock] = { "bow nock", 1, {
        { 0,      60, 140, 280, 280,   0,   1,   6,  25 },  // tiny click of the arrow on the string
    }},
    [HapticPattern_BowRelease] = { "bow release", 3, {
        { 0,     180, 255, 240,  90,   0,   1,  10,  80 },  // string snap
        { NOISE, 120, 200, 450, 450,   0,   1,   4,  40 },
        { 0,     140, 100,  60,  60,  15,   3,  20, 120 },  // recoil into the bow hand
    }},
    [HapticPattern_Damage] = { "damage", 2, {
        { NOISE, 255, 255, 110, 110,   0,   3,  60, 260 },  // rough hit, scaled with the hearts lost
        { 0,     220, 220,  55,  35,   0,   5,  50, 250 },
    }},
    [HapticPattern_Heal] = { "heal", 1, {
        { 0,      90,  90, 110, 170,   0, 180, 120, 380 },  // soft warm swell
    }},
    [HapticPattern_Heartbeat] = { "heartbeat", 2, {
        { 0,     200, 200,  58,  58,   0,   6,  45,  90 },  // lub
        { 0,     140, 140,  48,  48, 250,   6,  40, 110 },  // dub
    }},
    [HapticPattern_Daruk] = { "daruk", 2, {
        { 0,     255, 255, 180,  35,   0,   3,  90, 450 },  // shockwave of the barrier
        { NOISE, 200, 200, 260,  80,   0,   2,  60, 300 },
    }},
    [HapticPattern_WeaponBreak] = { "weapon break", 3, {
        { NOISE, 200, 255, 700, 200,   0,   1,  30, 160 },  // crunch
        { 0,     220, 220, 150,  55,   0,   2,  20, 180 },
        { NOISE, 120, 150, 400, 400,  90,   1,  10,  90 },  // falling pieces
    }},
    [HapticPattern_Landing] = { "landing", 2, {
        { 0,     255, 255,  70,  35,   0,   2,  20, 170 },  // scaled with the fall time
        { NOISE, 180, 180, 160, 160,   0,   1,  15, 110 },
    }},
    [HapticPattern_SurfBump] = { "surf bump", 1, {
        { NOISE, 140, 140, 200, 200,   0,   1,  10,  70 },
    }},
    [HapticPattern_StaminaEmpty] = { "stamina empty", 3, {
        { 0,     110, 110,  50,  50,   0,  20,  40,  80 },  // tired, fading pulses
        { 0,      90,  90,  50,  50, 330,  20,  40,  80 },
        { 0,      70,  70,  50,  50, 660,  20,  40,  80 },
    }},
};

// Looping textures use fixed slots, one-shot patterns take the remaining slots in turn so that quick events in a row
// don't cut each other off
typedef struct {
    u32 count;
    u8 slots[2];
    VoiceDef voices[2];
} LoopDef;

static const LoopDef Loops[HapticLoop_Count] = {
    [HapticLoop_BowTension] = { 2, { 0, 1 }, {
        { NOISE | LOOP, 70, 45,  70,  70,   0,  900, 0, 0 },  // faint shaking of the drawn string
        { LOOP,         50, 30,  40,  55,   0, 1200, 0, 0 },  // slowly rising strain
    }},
    [HapticLoop_Surf] = { 1, { 2 }, {
        { NOISE | LOOP, 75, 75, 110, 110,   0,  150, 0, 0 },  // gravel under the shield
    }},
};

#define FIRST_ONESHOT_SLOT 3
#define SLOT_COUNT         8

static bool g_available;
static u32 g_next_slot = FIRST_ONESHOT_SLOT;
static bool g_loop_on[HapticLoop_Count];

static u8 Scale(u32 amp, float factor) {
    float v = amp * factor * g_haptic_settings.strength / 100.0f;
    return v > 255.0f ? 255 : (u8)v;
}

static McHapticVoice MakeVoice(const VoiceDef *d, u8 slot, float factor) {
    McHapticVoice v = {
        .slot = slot,
        .flags = d->flags,
        .amp_left = Scale(d->left, factor),
        .amp_right = Scale(d->right, factor),
        .freq_start = d->freq_start,
        .freq_end = d->freq_end,
        .delay_ms = d->delay,
        .attack_ms = d->attack,
        .hold_ms = d->hold,
        .decay_ms = d->decay,
    };
    return v;
}

void HapticsSetAvailable(bool available) {
    g_available = available;
    if (!available) {
        memset(g_loop_on, 0, sizeof(g_loop_on));
    }
}

const char *HapticsPatternName(HapticPattern pattern) {
    return pattern < HapticPattern_Count ? Patterns[pattern].name : "?";
}

void HapticsPlay(HapticPattern pattern, float amount) {
    if (!g_available || !g_haptic_settings.enabled || pattern >= HapticPattern_Count) {
        return;
    }

    const PatternDef *p = &Patterns[pattern];
    float factor = 1.0f;
    if (pattern == HapticPattern_Damage || pattern == HapticPattern_Landing) {
        factor = amount < 0.3f ? 0.3f : amount > 1.0f ? 1.0f : amount;
    }

    McHapticVoice voices[MC_HAPTIC_VOICES_MAX];
    for (u32 i = 0; i < p->count; ++i) {
        voices[i] = MakeVoice(&p->voices[i], g_next_slot, factor);
        g_next_slot = g_next_slot + 1 >= SLOT_COUNT ? FIRST_ONESHOT_SLOT : g_next_slot + 1;
    }
    Result rc = mcPlayHaptics(voices, p->count);
    LogVerbose("haptics: %s%s", p->name, R_FAILED(rc) ? " (failed)" : "");
}

void HapticsSetLoop(HapticLoop loop, bool on) {
    if (!g_available || loop >= HapticLoop_Count) {
        return;
    }
    on = on && g_haptic_settings.enabled;
    if (g_loop_on[loop] == on) {
        return;
    }

    const LoopDef *l = &Loops[loop];
    McHapticVoice voices[MC_HAPTIC_VOICES_MAX];
    for (u32 i = 0; i < l->count; ++i) {
        // Off: amplitude 0 stops the slot
        voices[i] = MakeVoice(&l->voices[i], l->slots[i], on ? 1.0f : 0.0f);
        if (!on) {
            voices[i].amp_left = voices[i].amp_right = 0;
        }
    }
    if (R_SUCCEEDED(mcPlayHaptics(voices, l->count))) {
        g_loop_on[loop] = on;
    }
}

void HapticsStopAll(void) {
    if (!g_available) {
        return;
    }
    for (int i = 0; i < HapticLoop_Count; ++i) {
        HapticsSetLoop(i, false);
    }
}
