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

// Effect builders, same encodings as dualsense_controller.cpp
TriggerEffect TriggerEffectSimpleFeedback(u8 start, u8 force);
