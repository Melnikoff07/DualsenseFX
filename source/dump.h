// Research helper: dump of the game's main module to the SD card
#pragma once
#include <switch.h>
#include "dmntcht.h"

extern bool g_dump_main;    // dump_main=1 in config.ini

void DumpStart(const DmntCheatProcessMetadata *meta);
void DumpUpdate(void);      // writes one chunk per call
void DumpStop(void);
