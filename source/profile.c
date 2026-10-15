#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "profile.h"

void LogLine(const char *fmt, ...);

static const char *BotwGameIni =
    "; DualsenseFX game profile. The module attaches to a game only when its folder has a game.ini with enabled=1.\n"
    "name = The Legend of Zelda: Breath of the Wild\n"
    "enabled = 1\n"
    "; Main module build ids (first 8 bytes, hex) the addresses were found on, comma separated. Empty: any build.\n"
    "; The built-in BotW logic (inventory, bow, runes, surfing) only runs on 8E9978D50BDD20B4 (1.6.0).\n"
    "builds = 8E9978D50BDD20B4\n"
    "; nn::atk sound pool for the sound driven haptics (sounds.ini): [main + sound_archive_player] + sound_pool\n"
    "; points to 64 WaveSound objects whose vtable is main + wave_sound_vtable. Hex offsets.\n"
    "sound_archive_player = 2CC1670\n"
    "sound_pool = 128\n"
    "wave_sound_vtable = 2C17828\n";

static bool Exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void CopyFile(const char *from, const char *to) {
    static char buf[32768];
    FILE *in = fopen(from, "rb");
    if (in == NULL) {
        return;
    }
    size_t n = fread(buf, 1, sizeof(buf), in);
    fclose(in);
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        return;
    }
    fwrite(buf, 1, n, out);
    fclose(out);
    LogLine("profile: copied %s to %s", from, to);
}

void ProfilePath(const GameProfile *p, const char *file, char *out, size_t size) {
    snprintf(out, size, "%s/%s", p->dir, file);
}

void ProfileInstallDefaults(void) {
    char botw[96];
    snprintf(botw, sizeof(botw), "%s/%016lX", DFX_GAMES_DIR, BOTW_PROGRAM_ID);
    mkdir(DFX_DIR, 0777);
    mkdir(DFX_GAMES_DIR, 0777);
    mkdir(botw, 0777);

    // Settings from before the rename
    if (!Exists(DFX_DIR "/config.ini") && Exists(DFX_OLD_DIR "/config.ini")) {
        CopyFile(DFX_OLD_DIR "/config.ini", DFX_DIR "/config.ini");
    }
    static const char *Files[] = { "patterns.ini", "sounds.ini" };
    for (size_t i = 0; i < 2; ++i) {
        char from[128], to[128];
        snprintf(from, sizeof(from), "%s/%s", DFX_OLD_DIR, Files[i]);
        snprintf(to, sizeof(to), "%s/%s", botw, Files[i]);
        if (!Exists(to) && Exists(from)) {
            CopyFile(from, to);
        }
    }

    char ini[128];
    snprintf(ini, sizeof(ini), "%s/game.ini", botw);
    if (!Exists(ini)) {
        FILE *f = fopen(ini, "w");
        if (f) {
            fputs(BotwGameIni, f);
            fclose(f);
            LogLine("profile: wrote %s", ini);
        }
    }
}

static bool ParseHex(const char *s, u64 *out) {
    char *end;
    *out = strtoull(s, &end, 16);
    return end != s;
}

bool ProfileLoad(u64 program_id, GameProfile *out) {
    memset(out, 0, sizeof(*out));
    out->program_id = program_id;
    snprintf(out->dir, sizeof(out->dir), "%s/%016lX", DFX_GAMES_DIR, program_id);
    char path[128];
    ProfilePath(out, "game.ini", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    out->enabled = true;
    out->sound_find = true;
    snprintf(out->name, sizeof(out->name), "%016lX", program_id);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *c = strchr(line, ';');
        if (c) {
            *c = 0;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = 0;
        char *key = line, *value = eq + 1;
        while (*key == ' ' || *key == '\t') ++key;
        char *e = key + strlen(key);
        while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*value == ' ' || *value == '\t') ++value;
        value[strcspn(value, "\r\n")] = 0;
        e = value + strlen(value);
        while (e > value && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;

        u64 v;
        if (strcmp(key, "name") == 0) {
            snprintf(out->name, sizeof(out->name), "%s", value);
        } else if (strcmp(key, "enabled") == 0) {
            out->enabled = atoi(value) != 0;
        } else if (strcmp(key, "builds") == 0) {
            for (char *t = strtok(value, ", "); t && out->num_builds < PROFILE_MAX_BUILDS; t = strtok(NULL, ", ")) {
                if (strlen(t) != 16) {
                    continue;
                }
                for (int i = 0; i < 8; ++i) {
                    char byte[3] = { t[2 * i], t[2 * i + 1], 0 };
                    out->builds[out->num_builds][i] = (u8)strtoul(byte, NULL, 16);
                }
                ++out->num_builds;
            }
        } else if (strcmp(key, "sound_archive_player") == 0 && ParseHex(value, &v)) {
            out->sound_archive_player = v;
        } else if (strcmp(key, "sound_pool") == 0 && ParseHex(value, &v)) {
            out->sound_pool = v;
        } else if (strcmp(key, "wave_sound_vtable") == 0 && ParseHex(value, &v)) {
            out->wave_sound_vtable = v;
        } else if (strcmp(key, "sound_slot_size") == 0 && ParseHex(value, &v)) {
            out->sound_slot_size = (u32)v;
        } else if (strcmp(key, "sound_slots") == 0) {
            out->sound_slots = (u32)atoi(value);
        } else if (strcmp(key, "sound") == 0) {
            out->sound_find = strcmp(value, "off") != 0;
        }
    }
    fclose(f);
    return true;
}

bool ProfileBuildMatches(const GameProfile *p, const u8 *build_id) {
    if (p->num_builds == 0) {
        return true;
    }
    for (u32 i = 0; i < p->num_builds; ++i) {
        if (memcmp(p->builds[i], build_id, 8) == 0) {
            return true;
        }
    }
    return false;
}

void ProfileSaveSoundPool(GameProfile *p, u64 sap, u64 pool, u64 vtable, u32 slot_size, u32 slots) {
    char path[128];
    ProfilePath(p, "game.ini", path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "\n; nn::atk sound pool, found automatically (delete these lines to look again)\n"
               "sound_archive_player = %lX\nsound_pool = %lX\nwave_sound_vtable = %lX\nsound_slot_size = %X\nsound_slots = %u\n",
            sap, pool, vtable, slot_size, slots);
    fclose(f);
    p->sound_archive_player = sap;
    p->sound_pool = pool;
    p->wave_sound_vtable = vtable;
    p->sound_slot_size = slot_size;
    p->sound_slots = slots;
    LogLine("profile: sound pool saved to %s", path);
}

void ProfileSetCurrent(const GameProfile *p) {
    if (p == NULL) {
        remove(DFX_DIR "/current_game.txt");
        return;
    }
    FILE *f = fopen(DFX_DIR "/current_game.txt", "w");
    if (f) {
        fprintf(f, "%016lX\n%s\n", p->program_id, p->name);
        fclose(f);
    }
}
