#include <stdio.h>
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
