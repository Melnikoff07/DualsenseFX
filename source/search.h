// Correlation search: finds memory words in a region that change when ZR / ZL is held.
#pragma once
#include <switch.h>
#include "dmntcht.h"

typedef struct {
    bool enabled;
    u32 offset;     // region start, relative to the player actor
    u32 size;       // region size in bytes
} SearchSettings;

extern SearchSettings g_search_settings;

// Returns false if it should be retried later (player not loaded yet)
bool SearchStart(const DmntCheatProcessMetadata *meta);
void SearchUpdate(const DmntCheatProcessMetadata *meta);
void SearchStop(void);
