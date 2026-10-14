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
    bool menu_open;         // pause menu open (mEquippedWeapons is only filled then)
    u64 settle_until_tick;  // effects stay off until then (death, save reload)
    u64 pulse_until_tick[2];// short event effects: [0] right trigger, [1] left trigger
    TriggerEffect pulse_effect[2];
    bool effects_off_logged;
} BotwState;

void BotwInit(BotwState *state, const DmntCheatProcessMetadata *meta);
void BotwUpdate(BotwState *state, const DmntCheatProcessMetadata *meta);

// Trigger effects for the current game state. Returns false if the build is not supported.
bool BotwComputeEffects(BotwState *state, TriggerEffects *out);
