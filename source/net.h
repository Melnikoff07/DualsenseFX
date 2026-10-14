// Remote control over the local network for development (net=1 in config.ini): read game memory, see controller
// input, try haptic patterns / trigger effects / lightbar colours live, and move project files. Line based TCP
// protocol, password from net_token; tools/swnet.py is the client. Never writes game memory.
#pragma once
#include <switch.h>
#include "dmntcht.h"
#include "botw.h"
#include "mc.h"

typedef struct {
    bool enabled;
    char token[64];
    u16 port;           // TCP; discovery answers on UDP port + 1
} NetSettings;

extern NetSettings g_net_settings;

// What the game session currently has, filled in by main every poll
typedef struct {
    bool attached;      // dmnt is attached to BotW
    bool mc_open;
    const DmntCheatProcessMetadata *meta;
    BotwState *botw;
} NetContext;

void NetUpdate(const NetContext *ctx);
bool NetClientConnected(void);

// Live overrides set by a client, they expire on their own
bool NetTriggerOverride(TriggerEffects *out);
bool NetLightbarOverride(u8 *r, u8 *g, u8 *b);

// Provided by main
void ReloadConfig(void);
