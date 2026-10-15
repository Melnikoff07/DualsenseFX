// Haptics that follow the game's own sounds: every new short sound (nn::atk WaveSound) is looked up by name once,
// footsteps, water, chests and the like then play a pattern at the moment the game plays the sound
#pragma once
#include <switch.h>
#include "dmntcht.h"
#include "botw.h"

// 20 Hz: finds and checks the sound pool (it moves when the game restarts)
void SoundsUpdate(const DmntCheatProcessMetadata *meta);

// As often as possible (~100 Hz)
void SoundsPoll(BotwState *state);

// sounds.ini: which sounds play which pattern (writes the defaults when the file is missing). Load patterns first.
void SoundsLoadRules(const char *path, bool forget_names);

// Forget looked-up names (save reload, area change: sound packages get unloaded and their memory reused)
void SoundsReset(void);
