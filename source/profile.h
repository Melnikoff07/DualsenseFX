// Game profiles: sdmc:/config/dualsensefx/games/<program id>/ holds game.ini (name, builds, sound pool location),
// patterns.ini and sounds.ini for one game. The module attaches only to games that have an enabled profile.
#pragma once
#include <switch.h>

#define DFX_DIR       "sdmc:/config/dualsensefx"
#define DFX_GAMES_DIR DFX_DIR "/games"
#define DFX_OLD_DIR   "sdmc:/config/adaptive_triggers"    // before 0.4.0, migrated once
#define BOTW_PROGRAM_ID ((u64)0x01007EF00011E000ULL)

#define PROFILE_MAX_BUILDS 8

typedef struct {
    u64 program_id;
    char name[64];
    bool enabled;
    u32 num_builds;                         // 0 = any build
    u8 builds[PROFILE_MAX_BUILDS][8];       // first 8 bytes of the main module's build id
    u64 sound_archive_player;               // main+ offset of the nn::atk SoundArchivePlayer pointer, 0 = none
    u64 sound_pool;                         // offset of the WaveSound pool pointer in it
    u64 wave_sound_vtable;                  // main+ offset of the WaveSound vtable (checked before use)
    char dir[96];
} GameProfile;

// Moves the pre-0.4.0 settings over and writes the built-in profiles that are missing
void ProfileInstallDefaults(void);

// Reads games/<program id>/game.ini; false when there is no profile for this game
bool ProfileLoad(u64 program_id, GameProfile *out);
bool ProfileBuildMatches(const GameProfile *p, const u8 *build_id);

// current_game.txt tells the overlay which profile is in use (NULL: none)
void ProfileSetCurrent(const GameProfile *p);

// Path of a file in the profile's folder
void ProfilePath(const GameProfile *p, const char *file, char *out, size_t size);
