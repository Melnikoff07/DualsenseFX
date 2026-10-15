// Haptic patterns on the Switch's own controllers (Joy-Con attached or detached, Pro Controller) through the hid
// service: the same voices MissionControl plays on the DualSense, turned into HD rumble values (two frequency bands
// per motor) 200 times a second and mixed with the game's own rumble.
#pragma once
#include <switch.h>
#include "mc.h"

// Starts the hid session and the 5 ms output thread (once)
bool JoyConStart(void);

// Same meaning as mcPlayHaptics: each voice replaces the one in its slot, amplitude 0 stops the slot
void JoyConPlay(const McHapticVoice *voices, u32 count);
void JoyConStopAll(void);
