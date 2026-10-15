// Finds the nn::atk WaveSound pool in any game that uses NintendoWare audio and kept its RTTI type names:
// type name -> typeinfo -> vtable for WaveSound and SoundArchivePlayer, then the global pointer to the
// SoundArchivePlayer in the main module's data, its field that points to the WaveSound pool, and the pool's slot
// size and count. Read only, takes a second or two once per game start.
#pragma once
#include <switch.h>
#include "dmntcht.h"

typedef struct {
    u64 sound_archive_player;   // main+ offset of the global SoundArchivePlayer pointer
    u64 sound_pool;             // field in the SoundArchivePlayer that points to the pool
    u64 wave_sound_vtable;      // main+ offset
    u32 slot_size;
    u32 count;
} AtkLocation;

bool AtkFind(const DmntCheatProcessMetadata *meta, AtkLocation *out);
