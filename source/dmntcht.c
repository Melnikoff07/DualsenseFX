#include "dmntcht.h"

static Service g_dmntcht;

Result dmntchtInitialize(void) {
    return smGetService(&g_dmntcht, "dmnt:cht");
}

void dmntchtExit(void) {
    serviceClose(&g_dmntcht);
}

Result dmntchtHasCheatProcess(bool *out) {
    u8 tmp = 0;
    Result rc = serviceDispatchOut(&g_dmntcht, 65000, tmp);
    if (R_SUCCEEDED(rc)) {
        *out = tmp & 1;
    }
    return rc;
}

Result dmntchtGetCheatProcessMetadata(DmntCheatProcessMetadata *out) {
    return serviceDispatchOut(&g_dmntcht, 65002, *out);
}

Result dmntchtForceOpenCheatProcess(void) {
    return serviceDispatch(&g_dmntcht, 65003);
}

Result dmntchtReadCheatProcessMemory(u64 address, void *buffer, size_t size) {
    const struct {
        u64 address;
        u64 size;
    } in = { address, size };

    return serviceDispatchIn(&g_dmntcht, 65102, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } },
    );
}
