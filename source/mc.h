// Client for MissionControl's mc service (trigger effect commands added in our fork)
#pragma once
#include <switch.h>

// One trigger's adaptive effect block as sent to the DualSense (mode + 10 parameters)
typedef struct {
    u8 mode;
    u8 params[10];
} __attribute__((packed)) TriggerEffect;

typedef struct {
    TriggerEffect right;
    TriggerEffect left;
} __attribute__((packed)) TriggerEffects;

Result mcInitialize(void);
void mcExit(void);

Result mcGetBuildVersionString(char *out, size_t size);
Result mcSetTriggerEffects(const TriggerEffects *effects);
Result mcClearTriggerEffects(void);

typedef struct {
    u8 left;
    u8 right;
    u8 valid;
    u8 reserved;
} McAnalogTriggers;

Result mcGetAnalogTriggers(McAnalogTriggers *out);
Result mcSetLightbarColour(u8 r, u8 g, u8 b);
Result mcClearLightbarColour(void);

// Effect builders, same encodings as dualsense_controller.cpp (Nielk1's TriggerEffectGenerator)
TriggerEffect TriggerEffectOff(void);
TriggerEffect TriggerEffectSimpleFeedback(u8 start, u8 force);
TriggerEffect TriggerEffectFeedback(const u8 strengths[10]);
TriggerEffect TriggerEffectWeapon(u8 start, u8 end, u8 strength);
TriggerEffect TriggerEffectVibration(u8 start, u8 amplitude, u8 frequency);
