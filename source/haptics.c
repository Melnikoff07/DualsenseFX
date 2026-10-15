// Haptic patterns for game events. Each pattern is a few voices (sine or low-passed noise with a frequency sweep and
// an attack / hold / decay envelope) that MissionControl synthesises on the DualSense voice coils and mixes with the
// game's own rumble. Left/right amplitudes put an effect in the hand that would feel it: the weapon hand is the right
// one, the shield and the bow are held in the left.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "haptics.h"
#include "mc.h"
#include "joycon.h"

void LogLine(const char *fmt, ...);
void LogVerbose(const char *fmt, ...);

HapticSettings g_haptic_settings = { .output = HapticOutput_DualSense, .enabled = true, .strength = 100, .group = { 100, 100, 100, 100, 50 } };

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
    u8 group;
    u32 count;
    VoiceDef voices[MC_HAPTIC_VOICES_MAX];
} PatternDef;

static const PatternDef DefaultPatterns[HapticPattern_Count] = {
    [HapticPattern_HitSword] = { "hit sword", HapticGroup_Combat, 2, {
        { 0,     120, 230, 220, 130,   0,   2,  12,  70 },  // crisp knock
        { NOISE,  90, 160, 180, 180,   0,   1,   8,  60 },  // body of the impact
    }},
    [HapticPattern_HitHeavy] = { "hit heavy", HapticGroup_Combat, 2, {
        { 0,     255, 255,  75,  40,   0,   4,  40, 200 },  // deep thud
        { NOISE, 180, 200, 140, 140,   0,   2,  30, 160 },
    }},
    [HapticPattern_HitSpear] = { "hit spear", HapticGroup_Combat, 2, {
        { 0,      80, 220, 300, 180,   0,   1,   8,  45 },  // short sharp poke
        { NOISE,  60, 140, 300, 300,   0,   1,   5,  35 },
    }},
    [HapticPattern_ShieldBlock] = { "shield block", HapticGroup_Combat, 2, {
        { 0,     255, 150,  95,  55,   0,   2,  35, 170 },  // heavy knock into the shield hand
        { NOISE, 200, 110, 220, 220,   0,   1,  20, 110 },
    }},
    [HapticPattern_BowNock] = { "bow nock", HapticGroup_Combat, 1, {
        { 0,      60, 140, 280, 280,   0,   1,   6,  25 },  // tiny click of the arrow on the string
    }},
    [HapticPattern_BowRelease] = { "bow release", HapticGroup_Combat, 3, {
        { 0,     180, 255, 240,  90,   0,   1,  10,  80 },  // string snap
        { NOISE, 120, 200, 450, 450,   0,   1,   4,  40 },
        { 0,     140, 100,  60,  60,  15,   3,  20, 120 },  // recoil into the bow hand
    }},
    [HapticPattern_Damage] = { "damage", HapticGroup_Body, 2, {
        { NOISE, 255, 255, 110, 110,   0,   3,  60, 260 },  // rough hit, scaled with the hearts lost
        { 0,     220, 220,  55,  35,   0,   5,  50, 250 },
    }},
    [HapticPattern_Heal] = { "heal", HapticGroup_Body, 1, {
        { 0,      90,  90, 110, 170,   0, 180, 120, 380 },  // soft warm swell
    }},
    [HapticPattern_Heartbeat] = { "heartbeat", HapticGroup_Body, 2, {
        { 0,     200, 200,  58,  58,   0,   6,  45,  90 },  // lub
        { 0,     140, 140,  48,  48, 250,   6,  40, 110 },  // dub
    }},
    [HapticPattern_Daruk] = { "daruk", HapticGroup_Combat, 2, {
        { 0,     255, 255, 180,  35,   0,   3,  90, 450 },  // shockwave of the barrier
        { NOISE, 200, 200, 260,  80,   0,   2,  60, 300 },
    }},
    [HapticPattern_WeaponBreak] = { "weapon break", HapticGroup_Combat, 3, {
        { NOISE, 200, 255, 700, 200,   0,   1,  30, 160 },  // crunch
        { 0,     220, 220, 150,  55,   0,   2,  20, 180 },
        { NOISE, 120, 150, 400, 400,  90,   1,  10,  90 },  // falling pieces
    }},
    [HapticPattern_Landing] = { "landing", HapticGroup_Body, 2, {
        { 0,     255, 255,  70,  35,   0,   2,  20, 170 },  // scaled with the fall time
        { NOISE, 180, 180, 160, 160,   0,   1,  15, 110 },
    }},
    [HapticPattern_SurfBump] = { "surf bump", HapticGroup_Body, 1, {
        { NOISE, 140, 140, 200, 200,   0,   1,  10,  70 },
    }},
    [HapticPattern_StaminaEmpty] = { "stamina empty", HapticGroup_Body, 3, {
        { 0,     110, 110,  50,  50,   0,  20,  40,  80 },  // tired, fading pulses
        { 0,      90,  90,  50,  50, 330,  20,  40,  80 },
        { 0,      70,  70,  50,  50, 660,  20,  40,  80 },
    }},
    [HapticPattern_RuneOn] = { "rune on", HapticGroup_Runes, 1, {
        { 0,      80,  80,  90, 150,   0,  60,  40, 150 },  // soft rising swell of the Sheikah Slate
    }},
    [HapticPattern_RuneLock] = { "rune lock", HapticGroup_Runes, 1, {
        { 0,     110, 110, 220, 260,   0,   1,  10,  40 },  // small click on the target
    }},
    [HapticPattern_MagnesisGrab] = { "magnesis grab", HapticGroup_Runes, 2, {
        { 0,     230, 230,  80,  50,   0,   2,  40, 160 },  // heavy clunk
        { NOISE, 170, 170, 250, 250,   0,   1,  10,  60 },
    }},
    [HapticPattern_MagnesisDrop] = { "magnesis drop", HapticGroup_Runes, 1, {
        { 0,     150, 150,  60,  40,   0,   2,  20, 140 },
    }},
    [HapticPattern_StasisFreeze] = { "stasis freeze", HapticGroup_Runes, 3, {
        { 0,     220, 220, 320, 200,   0,   1,  15,  90 },  // sharp crack
        { NOISE, 200, 200, 500, 500,   0,   1,   5,  40 },
        { 0,     160, 160,  70,  70,  30,   3,  40, 200 },  // weight of the stopped object
    }},
    [HapticPattern_StasisTick] = { "stasis tick", HapticGroup_Runes, 1, {
        { 0,     150, 150, 180, 180,   0,   1,   8,  25 },
    }},
    [HapticPattern_StasisRelease] = { "stasis release", HapticGroup_Runes, 2, {
        { 0,     230, 230,  50,  35,   0,   3,  30, 220 },  // the stored force lets go
        { NOISE, 150, 150, 150, 150,   0,   2,  20, 160 },
    }},
    [HapticPattern_BombSpawn] = { "bomb spawn", HapticGroup_Runes, 1, {
        { 0,      90,  90, 160, 100,   0,   1,  10,  60 },  // light pop
    }},
    [HapticPattern_BombExplode] = { "bomb explode", HapticGroup_Runes, 3, {
        { 0,     255, 255,  60,  25,   0,   2, 120, 600 },  // blast
        { NOISE, 255, 255, 300,  80,   0,   1,  60, 450 },
        { NOISE, 150, 150, 120, 120, 120,  30, 100, 600 },  // rumbling debris
    }},
    [HapticPattern_CryonisRise] = { "cryonis rise", HapticGroup_Runes, 3, {
        { NOISE, 112, 112, 120, 200,   0, 300, 600, 200 },  // grinding ice, growing for about a second
        { 0,      96,  96,  60,  90,   0, 200, 700, 200 },
        { NOISE, 200, 200, 600, 600,1000,   1,  10,  80 },  // crack when the pillar stands
    }},
    [HapticPattern_CryonisBreak] = { "cryonis break", HapticGroup_Runes, 3, {
        { NOISE, 230, 230, 700, 200,   0,   1,  30, 200 },  // shatter
        { 0,     180, 180, 150,  60,   0,   1,  20, 150 },
        { NOISE, 120, 120, 400, 400,  80,   5,  30, 250 },  // falling shards
    }},
    [HapticPattern_Jump] = { "jump", HapticGroup_World, 1, {
        { 0,      70,  70,  80,  60,   0,   1,  10,  60 },  // push off
    }},
    [HapticPattern_LandSoft] = { "land", HapticGroup_Body, 1, {
        { 0,     110, 110,  60,  45,   0,   2,  15, 110 },
    }},
    [HapticPattern_ClimbGrip] = { "climb grip", HapticGroup_World, 1, {
        { NOISE,  45,  45, 300, 300,   0,   1,   5,  40 },
    }},
    [HapticPattern_WaterSplash] = { "water splash", HapticGroup_World, 2, {
        { NOISE, 160, 160, 200,  80,   0,   2,  60, 300 },
        { 0,     110, 110,  55,  40,   0,   3,  30, 200 },
    }},
    [HapticPattern_SwimStroke] = { "swim stroke", HapticGroup_World, 1, {
        { NOISE,  70,  70, 150, 110,   0,  40,  60, 200 },  // water pushed aside
    }},
    [HapticPattern_ChestOpen] = { "chest open", HapticGroup_World, 2, {
        { 0,     120, 120,  90,  60,   0,   5,  80, 200 },  // heavy lid
        { NOISE,  60,  60, 400, 300,  40,  10,  60, 150 },  // creak
    }},
    [HapticPattern_ItemGet] = { "item get", HapticGroup_World, 1, {
        { 0,      70,  70, 300, 500,   0,  10,  30, 120 },  // small rising sparkle
    }},
    [HapticPattern_Roll] = { "roll", HapticGroup_World, 1, {
        { NOISE,  90,  90, 120, 100,   0,  30, 120, 150 },
    }},
    [HapticPattern_CrateBreak] = { "crate break", HapticGroup_World, 2, {
        { NOISE, 180, 180, 600, 200,   0,   1,  20, 150 },
        { 0,     140, 140, 120,  60,   0,   1,  15, 120 },
    }},
};

// Looping textures use fixed slots, one-shot patterns take the remaining slots in turn so that quick events in a row
// don't cut each other off
typedef struct {
    u8 group;
    u32 count;
    u8 slots[2];
    VoiceDef voices[2];
} LoopDef;

static const LoopDef DefaultLoops[HapticLoop_Count] = {
    [HapticLoop_BowTension] = { HapticGroup_Combat, 2, { 0, 1 }, {
        { NOISE | LOOP, 70, 45,  70,  70,   0,  900, 0, 0 },  // faint shaking of the drawn string
        { LOOP,         50, 30,  40,  55,   0, 1200, 0, 0 },  // slowly rising strain
    }},
    [HapticLoop_Surf] = { HapticGroup_Body, 1, { 2 }, {
        { NOISE | LOOP, 75, 75, 110, 110,   0,  150, 0, 0 },  // gravel under the shield
    }},
    [HapticLoop_Magnesis] = { HapticGroup_Runes, 2, { 3, 4 }, {
        { LOOP,         36, 36,  55,  60,   0,  300, 0, 0 },  // magnetic hum (tuned down with the user)
        { NOISE | LOOP, 20, 20,  90,  90,   0,  300, 0, 0 },
    }},
    [HapticLoop_StaminaRefill] = { HapticGroup_Body, 1, { 5 }, {
        { LOOP,         12, 12,  45,  50,   0,  400, 0, 0 },  // barely there, like a calm pulse under the hand
    }},
};

// Footstep taps by ground material (index into Undefined,Soil,Stone,Sand,Metal,WireNet,Grass,Wood,Water,Snow,...)
typedef struct {
    u32 count;
    VoiceDef voices[2];
} StepDef;

static const StepDef DefaultSteps[] = {
    [1] = { 2, {{ 0,     60, 60,  70,  60, 0,  2, 10,  60 }, { NOISE, 30, 30, 150, 150, 0, 1,  5, 40 }}},  // soil: soft thud
    [2] = { 2, {{ 0,     70, 70, 140, 120, 0,  1,  5,  35 }, { NOISE, 40, 40, 600, 600, 0, 1,  2, 25 }}},  // stone: crisp click
    [3] = { 1, {{ NOISE, 55, 55, 250, 200, 0, 10, 20,  80 }}},                                            // sand: soft crunch
    [4] = { 2, {{ 0,     50, 50, 300, 300, 0,  1,  5, 120 }, { 0,     25, 25, 450, 450, 0, 1,  3, 80 }}},  // metal: ringing tap
    [6] = { 2, {{ NOISE, 35, 35, 400, 350, 0, 15, 20,  70 }, { 0,     30, 30,  60,  60, 0, 2,  5, 40 }}},  // grass: swish
    [7] = { 2, {{ 0,     75, 75, 180, 120, 0,  1,  6,  60 }, { 0,     40, 40,  90,  90, 0, 1,  5, 80 }}},  // wood: hollow knock
    [8] = { 1, {{ NOISE, 60, 60, 300, 250, 0,  5, 40, 150 }}},                                            // shallow water: splash
    [9] = { 1, {{ NOISE, 55, 55, 180, 160, 0,  8, 30,  90 }}},                                            // snow: crunch
    [10] = { 2, {{ 0,    60, 60, 260, 220, 0,  1,  4,  40 }, { NOISE, 30, 30, 900, 900, 0, 1,  2, 20 }}},  // ice: hard, glassy
    [14] = { 1, {{ NOISE, 30, 30, 200, 180, 0, 10, 15,  60 }}},                                           // cloth, carpet: muffled
};

#define FIRST_ONESHOT_SLOT 6
#define SLOT_COUNT         12   // MissionControl HapticVoiceSlots

static bool g_available;
static bool g_loop_on[HapticLoop_Count];
static bool g_mc_open;
static u32 g_output_applied = HapticOutput_DualSense;

static Result Send(const McHapticVoice *voices, u32 count) {
    switch (g_haptic_settings.output) {
    case HapticOutput_DualSense:
        return g_mc_open ? mcPlayHaptics(voices, count) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    case HapticOutput_JoyCon:
        JoyConPlay(voices, count);
        return 0;
    default:
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
}

void HapticsSetMcOpen(bool open) {
    g_mc_open = open;
}

void HapticsSendVoices(const McHapticVoice *voices, u32 count) {
    Send(voices, count);
}

void HapticsApplyOutput(void) {
    u32 out = g_haptic_settings.output;
    if (out == HapticOutput_JoyCon) {
        JoyConStart();
    }
    if (out == g_output_applied) {
        return;
    }
    // Silence the old output, loops start again on the new one
    if (g_output_applied == HapticOutput_DualSense && g_mc_open) {
        mcStopHaptics();
    } else if (g_output_applied == HapticOutput_JoyCon) {
        JoyConStopAll();
    }
    memset(g_loop_on, 0, sizeof(g_loop_on));
    LogLine("haptics: output %s", out == HapticOutput_DualSense ? "DualSense" : out == HapticOutput_JoyCon ? "Joy-Con" : "off");
    g_output_applied = out;
}
static u32 g_next_slot = FIRST_ONESHOT_SLOT;

// ---- patterns.ini: the tables above are the defaults, the file can change any of them and add new ones ----

#define STEP_MATERIALS 16

typedef struct {
    char name[32];
    u8 group;
    u8 strength;            // percent, set per pattern from the overlay (strength = in patterns.ini)
    u32 count;
    VoiceDef voices[MC_HAPTIC_VOICES_MAX];
    // The Joy-Con version: from patterns-joycon.ini, else made from the DualSense voices (JoyConVoice)
    bool jc_custom;
    u32 jc_count;
    VoiceDef jc_voices[MC_HAPTIC_VOICES_MAX];
} Pattern;

static Pattern g_patterns[HAPTIC_PATTERN_IDS];  // built-in ids first, then the ones patterns.ini adds
static u32 g_num_patterns;
static Pattern g_loops[HapticLoop_Count];       // slots stay as in DefaultLoops
static Pattern g_steps[STEP_MATERIALS];

static const char *LoopNames[HapticLoop_Count] = { "bow tension", "surf", "magnesis hum", "stamina refill" };
static const char *StepNames[STEP_MATERIALS] = {
    [1] = "soil", [2] = "stone", [3] = "sand", [4] = "metal", [6] = "grass", [7] = "wood", [8] = "water", [9] = "snow",
    [10] = "ice", [14] = "cloth",
};
static const char *GroupNames[HapticGroup_Count] = { "combat", "body", "runes", "world", "footsteps" };

// ---- Joy-Con versions ----
// The DualSense has voice coils with a real mass behind them: 25-80 Hz feels deep and heavy, low-passed noise feels
// like texture. A Joy-Con (and a Pro Controller) has one small linear resonant actuator per hand, driven with two
// sines at once (a low and a high band). It only shakes hard around its resonances, about 160 Hz and 320 Hz (SDL:
// "a fairly narrow frequency range around 100-250Hz that really shakes the controller, everything else is quite
// weak"); 40-60 Hz, the heart of the DualSense patterns, is barely felt. Amplitude and frequency change in 5 ms
// steps at best, so very short voices can fall between two updates. What a Joy-Con does very well: short crisp
// clicks near 320 Hz and punchy knocks near 160 Hz.
// So when a pattern has no hand-made Joy-Con version, its voices are moved where the actuator answers: deep
// frequencies into 100-160 Hz (sweeps keep their direction), very high ones pulled down towards 600-900 Hz, quiet
// voices lifted a little (small amplitudes are rounded away), and every voice lasts at least 15 ms.
static u16 JoyConFreq(u16 f) {
    if (f <= 40) {
        return 100;
    }
    if (f < 120) {
        return (u16)(100 + (f - 40) * 3 / 4);
    }
    if (f > 600) {
        u32 v = 600 + (f - 600) / 4;
        return (u16)(v > 900 ? 900 : v);
    }
    return f;
}

static u8 JoyConAmp(u8 a) {
    if (a == 0) {
        return 0;
    }
    float v = powf(a / 255.0f, 0.8f) * 255.0f;
    return v > 255.0f ? 255 : (u8)v;
}

static VoiceDef JoyConVoice(VoiceDef d) {
    d.freq_start = JoyConFreq(d.freq_start);
    d.freq_end = JoyConFreq(d.freq_end);
    d.left = JoyConAmp(d.left);
    d.right = JoyConAmp(d.right);
    if (d.attack + d.hold < 15) {
        d.hold = 15 - d.attack;
    }
    return d;
}

static void MakeJoyConVersion(Pattern *p) {
    if (p->jc_custom) {
        return;
    }
    p->jc_count = p->count;
    for (u32 i = 0; i < p->count; ++i) {
        p->jc_voices[i] = JoyConVoice(p->voices[i]);
    }
}

// The voices for the current output
static const VoiceDef *VoicesFor(const Pattern *p, u32 *count) {
    if (g_haptic_settings.output == HapticOutput_JoyCon) {
        *count = p->jc_count;
        return p->jc_voices;
    }
    *count = p->count;
    return p->voices;
}

static void SetPattern(Pattern *p, const char *name, u8 group, u32 count, const VoiceDef *voices) {
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->group = group;
    p->strength = 100;
    p->count = count;
    memcpy(p->voices, voices, sizeof(VoiceDef) * count);
    p->jc_custom = false;
}

static void ResetPatterns(void) {
    memset(g_patterns, 0, sizeof(g_patterns));
    for (u32 i = 0; i < HapticPattern_Count; ++i) {
        SetPattern(&g_patterns[i], DefaultPatterns[i].name, DefaultPatterns[i].group, DefaultPatterns[i].count, DefaultPatterns[i].voices);
    }
    g_num_patterns = HapticPattern_Count;
    for (u32 i = 0; i < HapticLoop_Count; ++i) {
        SetPattern(&g_loops[i], LoopNames[i], DefaultLoops[i].group, DefaultLoops[i].count, DefaultLoops[i].voices);
    }
    memset(g_steps, 0, sizeof(g_steps));
    for (u32 i = 0; i < STEP_MATERIALS && i < sizeof(DefaultSteps) / sizeof(DefaultSteps[0]); ++i) {
        if (DefaultSteps[i].count) {
            char name[32];
            snprintf(name, sizeof(name), "step %s", StepNames[i]);
            SetPattern(&g_steps[i], name, HapticGroup_Footsteps, DefaultSteps[i].count, DefaultSteps[i].voices);
        }
    }
}

static void Trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    char *b = s;
    while (*b == ' ' || *b == '\t') {
        ++b;
    }
    memmove(s, b, strlen(b) + 1);
}

// Section [name]: a built-in pattern, a loop, "step <ground>", or a new pattern
static Pattern *SectionFor(const char *name, bool *loop) {
    *loop = false;
    for (u32 i = 0; i < HapticLoop_Count; ++i) {
        if (strcmp(name, LoopNames[i]) == 0) {
            *loop = true;
            return &g_loops[i];
        }
    }
    if (strncmp(name, "step ", 5) == 0) {
        for (u32 i = 0; i < STEP_MATERIALS; ++i) {
            if (StepNames[i] && strcmp(name + 5, StepNames[i]) == 0) {
                snprintf(g_steps[i].name, sizeof(g_steps[i].name), "%.31s", name);
                g_steps[i].group = HapticGroup_Footsteps;
                return &g_steps[i];
            }
        }
        return NULL;
    }
    for (u32 i = 0; i < g_num_patterns; ++i) {
        if (strcmp(name, g_patterns[i].name) == 0) {
            return &g_patterns[i];
        }
    }
    if (g_num_patterns >= HAPTIC_PATTERN_IDS) {
        return NULL;
    }
    Pattern *p = &g_patterns[g_num_patterns++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%.31s", name);
    p->group = HapticGroup_World;
    p->strength = 100;
    return p;
}

static const char *LibraryDefaults =
    "; DualsenseFX pattern library: ready-made patterns every game profile can use (sounds.ini: name = <pattern> [%]).\n"
    "; A game's own patterns.ini can change any of them for that game. Applied within a second after saving.\n"
    ";   voice = sine|noise  left right  freq_start freq_end  delay attack hold decay   (amplitude 0-255, Hz, ms)\n"
    "\n[tap light]\ngroup = world\nvoice = sine   60  60  140  120    0    1    5   35\n"
    "\n[tap]\ngroup = world\nvoice = sine  110 110  160  110    0    1    8   60\n"
    "\n[click]\ngroup = world\nvoice = sine  120 120  260  300    0    1    4   25\nvoice = noise  60  60  800  800    0    1    2   15\n"
    "\n[tick]\ngroup = world\nvoice = sine  150 150  180  180    0    1    8   25\n"
    "\n[pop]\ngroup = world\nvoice = sine   90  90  160  100    0    1   10   60\n"
    "\n[thud]\ngroup = world\nvoice = sine  200 200   70   45    0    2   30  150\nvoice = noise 120 120  140  140    0    1   15   90\n"
    "\n[heavy thud]\ngroup = world\nvoice = sine  255 255   55   30    0    3   60  260\nvoice = noise 200 200  110  110    0    2   40  200\n"
    "\n[explosion]\ngroup = world\nvoice = sine  255 255   60   25    0    2  120  600\nvoice = noise 255 255  300   80    0    1   60  450\nvoice = noise 150 150  120  120  120   30  100  600\n"
    "\n[crack]\ngroup = world\nvoice = noise 220 220  700  300    0    1   20  120\nvoice = sine  160 160  200   90    0    1   10  100\n"
    "\n[splash]\ngroup = world\nvoice = noise 160 160  200   80    0    2   60  300\nvoice = sine  110 110   55   40    0    3   30  200\n"
    "\n[swell]\ngroup = world\nvoice = sine   90  90   90  160    0  150  100  300\n"
    "\n[buzz]\ngroup = world\nvoice = sine  120 120  220  220    0    5  150   60\nvoice = noise  60  60  300  300    0    5  150   60\n"
    "\n[rumble]\ngroup = world\nvoice = noise 150 150   90   70    0   40  300  400\nvoice = sine  120 120   45   45    0   40  300  400\n"
    "\n[sparkle]\ngroup = world\nvoice = sine   70  70  300  500    0   10   30  120\n"
    "\n[whoosh]\ngroup = world\nvoice = noise  70  70  150  110    0   40   60  200\n";

static void WriteVoices(FILE *f, const Pattern *p) {
    for (u32 v = 0; v < p->count; ++v) {
        const VoiceDef *d = &p->voices[v];
        fprintf(f, "voice = %-5s %3u %3u %4u %4u %4u %4u %4u %4u\n", (d->flags & NOISE) ? "noise" : "sine",
                d->left, d->right, d->freq_start, d->freq_end, d->delay, d->attack, d->hold, d->decay);
    }
}

static void WriteDefaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fputs("; Haptic patterns, applied within a second after saving (no reboot).\n"
          "; [name] then group = combat | body | runes | world | footsteps, and up to 4 voices:\n"
          ";   voice = sine|noise  left right  freq_start freq_end  delay attack hold decay\n"
          "; strength = 0-200 scales one pattern (percent; the overlay's per-event sliders set it).\n"
          "; amplitudes 0-255 per hand (the right hand holds the weapon, the left one the shield and the bow), Hz, ms.\n"
          "; Loops (bow tension, surf, magnesis hum, stamina refill) hold while their state lasts; \"step <ground>\" are\n"
          "; the footstep taps. New sections become new patterns that sounds.ini can use.\n", f);
    for (u32 i = 0; i < HapticPattern_Count; ++i) {
        fprintf(f, "\n[%s]\ngroup = %s\n", g_patterns[i].name, GroupNames[g_patterns[i].group]);
        WriteVoices(f, &g_patterns[i]);
    }
    for (u32 i = 0; i < HapticLoop_Count; ++i) {
        fprintf(f, "\n[%s]\ngroup = %s\n", g_loops[i].name, GroupNames[g_loops[i].group]);
        WriteVoices(f, &g_loops[i]);
    }
    for (u32 i = 0; i < STEP_MATERIALS; ++i) {
        if (g_steps[i].count) {
            fprintf(f, "\n[%s]\n", g_steps[i].name);
            WriteVoices(f, &g_steps[i]);
        }
    }
    fclose(f);
    LogLine("haptics: wrote default %s", path);
}

static void WriteText(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
        LogLine("haptics: wrote default %s", path);
    }
}

// One patterns file on top of what is loaded already; false if it is missing. A Joy-Con file (patterns-joycon.ini)
// only sets the Joy-Con voices of its sections; group and strength always come from patterns.ini.
static bool ParsePatternFile(const char *path, bool joycon) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char line[256];
    Pattern *cur = NULL;
    bool loop = false, voices_started = false;
    u32 sections = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        char *c = strchr(line, ';');
        if (c) {
            *c = 0;
        }
        Trim(line);
        if (line[0] == 0) {
            continue;
        }
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) {
                *e = 0;
            }
            Trim(line + 1);
            cur = SectionFor(line + 1, &loop);
            voices_started = false;
            sections += cur != NULL;
            bad += cur == NULL;
            continue;
        }
        if (cur == NULL) {
            continue;
        }
        char type[8];
        unsigned l, r, f0, f1, dl, at, ho, de;
        char gname[16];
        if (sscanf(line, "voice = %7s %u %u %u %u %u %u %u %u", type, &l, &r, &f0, &f1, &dl, &at, &ho, &de) == 9) {
            u32 *count = joycon ? &cur->jc_count : &cur->count;
            if (!voices_started) {
                *count = 0;
                voices_started = true;
                cur->jc_custom |= joycon;
            }
            if (*count < MC_HAPTIC_VOICES_MAX && (loop ? *count < 2 : true)) {
                (joycon ? cur->jc_voices : cur->voices)[(*count)++] = (VoiceDef){
                    (u8)((strcmp(type, "noise") == 0 ? NOISE : 0) | (loop ? LOOP : 0)),
                    (u8)(l > 255 ? 255 : l), (u8)(r > 255 ? 255 : r), (u16)f0, (u16)f1, (u16)dl, (u16)at, (u16)ho, (u16)de };
            }
        } else if (joycon && (strncmp(line, "strength", 8) == 0 || strncmp(line, "group", 5) == 0)) {
            continue;
        } else if (sscanf(line, "strength = %u", &l) == 1) {
            cur->strength = l > 200 ? 200 : l;
        } else if (sscanf(line, "group = %15s", gname) == 1) {
            for (u32 g = 0; g < HapticGroup_Count; ++g) {
                if (strcmp(gname, GroupNames[g]) == 0) {
                    cur->group = g;
                }
            }
        } else {
            ++bad;
        }
    }
    fclose(f);
    LogLine("haptics: %s: %u sections, %u patterns, %u lines not understood", path, sections, g_num_patterns, bad);
    return true;
}

static const char *JoyConHeader =
    "; Joy-Con / Pro Controller versions of the patterns, used while output=2. Same sections and voice lines as\n"
    "; patterns.ini; a pattern missing here is made from its DualSense voices automatically (moved to the frequencies\n"
    "; the Joy-Con actuator answers to: low band ~100-250 Hz, best near 160 Hz; high band best near 320 Hz).\n"
    "; group and strength come from patterns.ini, so the overlay's sliders work for both. Applied within a second.\n";

void HapticsLoadPatterns(const char *library, const char *library_jc, const char *profile, const char *profile_jc,
                         bool builtin_profile) {
    ResetPatterns();
    if (library && !ParsePatternFile(library, false)) {
        WriteText(library, LibraryDefaults);
        ParsePatternFile(library, false);
    }
    if (profile && !ParsePatternFile(profile, false)) {
        // BotW's patterns are built in and written out in full; other games start with an empty file
        if (builtin_profile) {
            WriteDefaults(profile);
        } else {
            WriteText(profile, "; Patterns of this game: change library patterns for it or add new ones, same format as\n"
                               "; config/dualsensefx/patterns.ini. strength = 0-200 scales one pattern.\n");
        }
    }
    if (library_jc && !ParsePatternFile(library_jc, true)) {
        WriteText(library_jc, JoyConHeader);
    }
    if (profile_jc && !ParsePatternFile(profile_jc, true)) {
        WriteText(profile_jc, JoyConHeader);
    }
    for (u32 i = 0; i < g_num_patterns; ++i) {
        MakeJoyConVersion(&g_patterns[i]);
    }
    for (u32 i = 0; i < HapticLoop_Count; ++i) {
        MakeJoyConVersion(&g_loops[i]);
    }
    for (u32 i = 0; i < STEP_MATERIALS; ++i) {
        MakeJoyConVersion(&g_steps[i]);
    }
}

int HapticsFindPattern(const char *name) {
    for (u32 i = 0; i < g_num_patterns; ++i) {
        if (strcmp(name, g_patterns[i].name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

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

const char *HapticsPatternName(u32 pattern) {
    return pattern < g_num_patterns ? g_patterns[pattern].name : "?";
}

void HapticsPlay(u32 pattern, float amount) {
    if (!g_available || !g_haptic_settings.enabled || pattern >= g_num_patterns) {
        return;
    }

    const Pattern *p = &g_patterns[pattern];
    u32 count;
    const VoiceDef *defs = VoicesFor(p, &count);
    if (count == 0) {
        return;
    }
    float factor = 1.0f;
    if (pattern == HapticPattern_Damage || pattern == HapticPattern_Landing) {
        factor = amount < 0.3f ? 0.3f : amount > 1.0f ? 1.0f : amount;
    } else if (pattern >= HapticPattern_Jump) {
        factor = amount > 1.0f ? 1.0f : amount;
    }
    u32 group = g_haptic_settings.group[p->group];
    if (group == 0 || p->strength == 0) {
        return;
    }
    factor *= group / 100.0f * p->strength / 100.0f;

    McHapticVoice voices[MC_HAPTIC_VOICES_MAX];
    for (u32 i = 0; i < count; ++i) {
        voices[i] = MakeVoice(&defs[i], g_next_slot, factor);
        g_next_slot = g_next_slot + 1 >= SLOT_COUNT ? FIRST_ONESHOT_SLOT : g_next_slot + 1;
    }
    Result rc = Send(voices, count);
    LogVerbose("haptics: %s%s", p->name, R_FAILED(rc) ? " (failed)" : "");
}

void HapticsPlayStep(u32 material, bool left, float amount) {
    u32 group = g_haptic_settings.group[HapticGroup_Footsteps];
    if (!g_available || !g_haptic_settings.enabled || group == 0) {
        return;
    }
    const Pattern *s = material < STEP_MATERIALS && g_steps[material].count ? &g_steps[material] : &g_steps[1];
    u32 count;
    const VoiceDef *defs = VoicesFor(s, &count);
    if (s->strength == 0 || count == 0) {
        return;
    }
    amount *= s->strength / 100.0f;
    McHapticVoice voices[MC_HAPTIC_VOICES_MAX];
    for (u32 i = 0; i < count; ++i) {
        voices[i] = MakeVoice(&defs[i], g_next_slot, amount * group / 100.0f);
        // The other hand gets a softer copy, steps alternate between the hands
        u8 *soft = left ? &voices[i].amp_right : &voices[i].amp_left;
        *soft = *soft * 6 / 10;
        g_next_slot = g_next_slot + 1 >= SLOT_COUNT ? FIRST_ONESHOT_SLOT : g_next_slot + 1;
    }
    Send(voices, count);
}

void HapticsSetLoop(HapticLoop loop, bool on) {
    if (!g_available || loop >= HapticLoop_Count) {
        return;
    }
    const Pattern *l = &g_loops[loop];
    on = on && g_haptic_settings.enabled && g_haptic_settings.group[l->group] > 0 && l->strength > 0;
    if (g_loop_on[loop] == on) {
        return;
    }

    u32 count;
    const VoiceDef *defs = VoicesFor(l, &count);
    McHapticVoice voices[MC_HAPTIC_VOICES_MAX];
    for (u32 i = 0; i < count && i < 2; ++i) {
        // Off: amplitude 0 stops the slot
        voices[i] = MakeVoice(&defs[i], DefaultLoops[loop].slots[i],
                              on ? g_haptic_settings.group[l->group] / 100.0f * l->strength / 100.0f : 0.0f);
        if (!on) {
            voices[i].amp_left = voices[i].amp_right = 0;
        }
    }
    if (count > 2) {
        count = 2;
    }
    if (R_SUCCEEDED(Send(voices, count))) {
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
