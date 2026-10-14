// Haptic patterns for game events, played on the DualSense voice coils through MissionControl
#pragma once
#include <switch.h>

typedef enum {
    HapticPattern_HitSword,     // one-handed weapon hit something
    HapticPattern_HitHeavy,     // two-handed weapon
    HapticPattern_HitSpear,
    HapticPattern_ShieldBlock,
    HapticPattern_BowNock,
    HapticPattern_BowRelease,
    HapticPattern_Damage,       // strength scales with the hearts lost
    HapticPattern_Heal,
    HapticPattern_Heartbeat,    // one "lub-dub", in step with the lightbar
    HapticPattern_Daruk,
    HapticPattern_WeaponBreak,
    HapticPattern_Landing,      // strength scales with the fall time
    HapticPattern_SurfBump,
    HapticPattern_StaminaEmpty,
    HapticPattern_RuneOn,       // a rune is raised to aim (Magnesis, Stasis)
    HapticPattern_RuneLock,     // the aimed rune locked on a target
    HapticPattern_MagnesisGrab,
    HapticPattern_MagnesisDrop,
    HapticPattern_StasisFreeze,
    HapticPattern_StasisTick,   // in step with the flashing of the frozen target
    HapticPattern_StasisRelease,
    HapticPattern_BombSpawn,
    HapticPattern_BombExplode,
    HapticPattern_CryonisRise,  // a pillar grows out of the water (~1 s)
    HapticPattern_CryonisBreak,
    HapticPattern_Count,
} HapticPattern;

typedef enum {
    HapticLoop_BowTension,
    HapticLoop_Surf,
    HapticLoop_Magnesis,        // hum while an object is held
    HapticLoop_Count,
} HapticLoop;

typedef struct {
    bool enabled;       // haptics=1 in config.ini
    u32 strength;       // percent, scales every pattern
    u32 footsteps;      // percent, 0 disables footstep taps
} HapticSettings;

extern HapticSettings g_haptic_settings;

// Set by main while the mc session is open
void HapticsSetAvailable(bool available);

// amount: 0.0-1.0 for patterns that scale (damage, landing), ignored by the others
void HapticsPlay(HapticPattern pattern, float amount);

// One footstep on the given ground material (BotW material index), in the hand of the foot that stepped;
// strength in percent of the footstep_strength setting
void HapticsPlayStep(u32 material, bool left);

// Continuous textures, only sent when the state changes
void HapticsSetLoop(HapticLoop loop, bool on);
void HapticsStopAll(void);

const char *HapticsPatternName(HapticPattern pattern);
