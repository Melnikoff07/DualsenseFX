// BotW 1.6.0 game state readers (memory layout notes in botw.c)
#pragma once
#include <switch.h>
#include "dmntcht.h"

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
} BotwState;

void BotwInit(BotwState *state, const DmntCheatProcessMetadata *meta);
void BotwUpdate(BotwState *state, const DmntCheatProcessMetadata *meta);
