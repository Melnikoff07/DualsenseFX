#include <stdio.h>
#include <string.h>
#include "mc.h"

enum {
    McCmd_GetBuildVersionString = 1,
    McCmd_SetTriggerEffects     = 6,
    McCmd_ClearTriggerEffects   = 7,
};

static Service g_mc;

Result mcInitialize(void) {
    return smGetService(&g_mc, "mc");
}

void mcExit(void) {
    // Closing the session makes MissionControl restore default effects by itself
    serviceClose(&g_mc);
}

Result mcGetBuildVersionString(char *out, size_t size) {
    char version[64] = {0};
    Result rc = serviceDispatchOut(&g_mc, McCmd_GetBuildVersionString, version);
    snprintf(out, size, "%.63s", version);
    return rc;
}

Result mcSetTriggerEffects(const TriggerEffects *effects) {
    return serviceDispatchIn(&g_mc, McCmd_SetTriggerEffects, *effects);
}

Result mcClearTriggerEffects(void) {
    return serviceDispatch(&g_mc, McCmd_ClearTriggerEffects);
}

TriggerEffect TriggerEffectSimpleFeedback(u8 start, u8 force) {
    TriggerEffect e = { .mode = 0x01, .params = { start, force } };
    return e;
}

TriggerEffect TriggerEffectOff(void) {
    TriggerEffect e = { .mode = 0x05 };
    return e;
}

TriggerEffect TriggerEffectFeedback(const u8 strengths[10]) {
    TriggerEffect e = { .mode = 0x21 };
    u16 active = 0;
    u32 forces = 0;
    for (int i = 0; i < 10; ++i) {
        if (strengths[i] > 0) {
            active |= 1 << i;
            forces |= (u32)((strengths[i] - 1) & 0x07) << (3 * i);
        }
    }
    memcpy(&e.params[0], &active, sizeof(active));
    memcpy(&e.params[2], &forces, sizeof(forces));
    return e;
}

TriggerEffect TriggerEffectWeapon(u8 start, u8 end, u8 strength) {
    TriggerEffect e = { .mode = 0x25 };
    u16 zones = (1 << start) | (1 << end);
    memcpy(&e.params[0], &zones, sizeof(zones));
    e.params[2] = (strength - 1) & 0x07;
    return e;
}

TriggerEffect TriggerEffectVibration(u8 start, u8 amplitude, u8 frequency) {
    TriggerEffect e = { .mode = 0x26 };
    u16 active = 0;
    u32 amps = 0;
    for (int i = start; i < 10; ++i) {
        active |= 1 << i;
        amps |= (u32)((amplitude - 1) & 0x07) << (3 * i);
    }
    memcpy(&e.params[0], &active, sizeof(active));
    memcpy(&e.params[2], &amps, sizeof(amps));
    e.params[8] = frequency;
    return e;
}
