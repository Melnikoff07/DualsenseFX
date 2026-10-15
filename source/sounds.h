// Haptics that follow the game's own sounds: every new short sound (nn::atk WaveSound) is looked up by name once,
// footsteps, water, chests and the like then play a pattern at the moment the game plays the sound
#pragma once
#include <switch.h>
#include "dmntcht.h"

// Where the game keeps its sound pool (game profile, offsets from the main module); 0 turns sound haptics off
void SoundsConfigure(u64 sound_archive_player, u64 sound_pool, u64 wave_sound_vtable);

// 20 Hz: finds and checks the sound pool (it moves when the game restarts)
void SoundsUpdate(const DmntCheatProcessMetadata *meta);

// As often as possible (~100 Hz); in_game false keeps quiet (menus, loading)
void SoundsPoll(bool in_game);

// sounds.ini: which sounds play which pattern (writes the defaults when the file is missing). Load patterns first.
void SoundsLoadRules(const char *path, bool forget_names);

// Forget looked-up names (save reload, area change: sound packages get unloaded and their memory reused)
void SoundsReset(void);
