// Correlation searches: memory words in a region of the player actor that change with ZR / ZL held, while
// shield surfing, or while a rune is in use.
#pragma once
#include <switch.h>
#include "dmntcht.h"

typedef enum {
    SearchMode_Triggers = 1, // words that change while ZR / ZL is held
    SearchMode_Surf     = 2, // bits that are set only while shield surfing (anchored on shield wear events)
    SearchMode_Rune     = 3, // words that change 1 s after L is pressed (rune in use) compared with standing idle
    SearchMode_RuneDeep = 4, // like 3, in the first 256 bytes of every heap object the player actor region points to
} SearchMode;

typedef struct {
    bool enabled;
    SearchMode mode;
    bool base_rune; // search_base=rune: the region is relative to the RuneMgr instance instead of the player actor
    u32 offset;     // region start, relative to the player actor (or RuneMgr)
    u32 size;       // region size in bytes
} SearchSettings;

extern SearchSettings g_search_settings;

// Set by the game logic when the player is known to be shield surfing right now
extern bool g_search_surf_event;

// Returns false if it should be retried later (player not loaded yet)
bool SearchStart(const DmntCheatProcessMetadata *meta);
void SearchUpdate(const DmntCheatProcessMetadata *meta);
void SearchStop(void);
