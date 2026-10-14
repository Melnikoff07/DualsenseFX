// BotW 1.6.0 game state readers (memory layout notes in botw.c)
#pragma once
#include <switch.h>
#include "dmntcht.h"
#include "mc.h"

typedef enum {
    BotwSlot_Sword  = 0,
    BotwSlot_Bow    = 1,
    BotwSlot_Arrow  = 2,
    BotwSlot_Shield = 3,
    BotwSlot_Count,
} BotwSlot;

typedef struct {
    bool present;
    u32 slot;       // index in the pouch item buffer
    s32 value;      // durability x100 for weapons, count for arrows
    char name[64];
} BotwEquippedItem;

typedef struct {
    bool supported;     // running build matches the offsets below
    u64 pmdm;           // PauseMenuDataMgr instance
    u64 next_scan_tick;
    BotwEquippedItem equipped[BotwSlot_Count];
    bool dumped[BotwSlot_Count];

    s32 hearts;             // quarter hearts, -1 if unreadable
    s32 last_hearts;
    u64 player;             // player actor, refreshed every scan
    u32 bow_state;          // player +1F88: 0 none, 1 nocking, 2 drawn, 3 shot
    u64 bow_active_until_tick; // keep light bow tension shortly after a shot, for rapid fire
    u32 action_flags;       // player +C40
    u64 flash_until_tick;   // lightbar flash (Daruk's Protection)
    u64 hearts_changed_tick;
    bool menu_open;         // pause menu open (mEquippedWeapons is only filled then)
    u64 settle_until_tick;  // effects stay off until then (death, save reload)
    u64 pulse_until_tick[2];// short event effects: [0] right trigger, [1] left trigger
    TriggerEffect pulse_effect[2];
    bool effects_off_logged;
} BotwState;

// Tunables from config.ini
typedef struct {
    u8 shield_hit_strength; // 0-8, 0 disables the block impact pulse
    u8 shield_hit_style;    // 1 = trigger stiffens (push back), 2 = vibration
    u32 shield_hit_ms;
    u32 low_health_quarters; // heartbeat on the lightbar at or below this many quarter hearts, 0 disables
    u8 bow_release_strength; // 0-8, pulse on R2 when an arrow is shot, 0 disables (not felt: the finger is already off the trigger)
    u8 bow_drawn_strength;   // 1-8, R2 resistance once the bow is drawn (bow state 2)
    u8 surf_strength;        // 0-8, L2 vibration while shield surfing (hypothesis: player +C40 bit 0x2), 0 disables
    u8 daruk_strength;       // 0-8, vibration on both triggers when Daruk's Protection blocks a hit, 0 disables (lightbar flash stays)
} BotwSettings;

extern BotwSettings g_botw_settings;

void BotwInit(BotwState *state, const DmntCheatProcessMetadata *meta);
void BotwUpdate(BotwState *state, const DmntCheatProcessMetadata *meta);

// Trigger effects for the current game state. Returns false if the build is not supported.
bool BotwComputeEffects(BotwState *state, TriggerEffects *out);

// Lightbar colour for the current game state. Returns false when the normal player colour should show.
bool BotwComputeLightbar(BotwState *state, u8 *r, u8 *g, u8 *b);

// Player actor (PlayerInfo::mPlayerActor), 0 if not available
u64 BotwGetPlayerActor(const DmntCheatProcessMetadata *meta);
