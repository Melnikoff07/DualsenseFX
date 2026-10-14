// Minimal client for Atmosphère's dmnt:cht service (same interface EdiZon/Breeze use).
// Command ids from Atmosphere/stratosphere/dmnt/source/cheat/dmnt_cheat_service.hpp
#pragma once
#include <switch.h>

typedef struct {
    u64 base;
    u64 size;
} DmntMemoryRegionExtents;

typedef struct {
    u64 process_id;
    u64 program_id;
    DmntMemoryRegionExtents main_nso_extents;
    DmntMemoryRegionExtents heap_extents;
    DmntMemoryRegionExtents alias_extents;
    DmntMemoryRegionExtents aslr_extents;
    u8 main_nso_build_id[0x20];
} DmntCheatProcessMetadata;

_Static_assert(sizeof(DmntCheatProcessMetadata) == 0x70, "DmntCheatProcessMetadata");

Result dmntchtInitialize(void);
void dmntchtExit(void);

Result dmntchtHasCheatProcess(bool *out);
Result dmntchtGetCheatProcessMetadata(DmntCheatProcessMetadata *out);
Result dmntchtForceOpenCheatProcess(void);
Result dmntchtReadCheatProcessMemory(u64 address, void *buffer, size_t size);
