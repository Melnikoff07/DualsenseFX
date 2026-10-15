// DualsenseFX: Tesla / Ultrahand overlay for the DualsenseFX settings.
// It only edits files in sdmc:/config/dualsensefx (config.ini and the games' patterns.ini); the sysmodule notices the
// change and applies it within a second, no reboot needed.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

namespace {

constexpr const char *ConfigPath = "sdmc:/config/dualsensefx/config.ini";
constexpr const char *GamesDir   = "sdmc:/config/dualsensefx/games";
constexpr const char *CurrentGamePath = "sdmc:/config/dualsensefx/current_game.txt";
constexpr const char *DefaultGame = "01007EF00011E000";   // Zelda: Breath of the Wild

// Strength steps shown on the sliders, percent
constexpr int Steps[] = { 0, 25, 50, 75, 100, 125, 150, 175, 200 };
constexpr size_t NumSteps = sizeof(Steps) / sizeof(Steps[0]);

// A "key = value" file (config.ini, game.ini) that keeps its comments when a value changes
class Config {
public:
    void load(const std::string &path = ConfigPath) {
        m_path = path;
        m_loaded = false;
        lines.clear();
        FILE *f = std::fopen(path.c_str(), "r");
        if (f == nullptr) {
            return;
        }
        m_loaded = true;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            lines.push_back(line);
        }
        std::fclose(f);
    }

    int get(const std::string &key, int fallback) const {
        for (const auto &line : lines) {
            size_t n;
            if (isKey(line, key, n)) {
                return std::atoi(line.c_str() + n);
            }
        }
        return fallback;
    }

    std::string getString(const std::string &key, const std::string &fallback) const {
        for (const auto &line : lines) {
            size_t n;
            if (isKey(line, key, n)) {
                std::string v = line.substr(n);
                size_t c = v.find(';');
                if (c != std::string::npos) {
                    v.erase(c);
                }
                v.erase(0, v.find_first_not_of(" \t"));
                v.erase(v.find_last_not_of(" \t\r") + 1);
                return v;
            }
        }
        return fallback;
    }

    bool loaded() const {
        return m_loaded;
    }

    // Changes or adds "key=value" and writes the file (temporary file first, so the module never sees half of it)
    void set(const std::string &key, int value) {
        std::string entry = key + "=" + std::to_string(value);
        bool found = false;
        for (auto &line : lines) {
            size_t n;
            if (isKey(line, key, n)) {
                line = entry;
                found = true;
                break;
            }
        }
        if (!found) {
            lines.push_back(entry);
        }
        save();
    }

private:
    std::vector<std::string> lines;
    std::string m_path = ConfigPath;
    bool m_loaded = false;

    // "key = value" or "key=value" outside a comment; n gets the index of the value
    static bool isKey(const std::string &line, const std::string &key, size_t &n) {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] == ';' || line.compare(i, key.size(), key) != 0) {
            return false;
        }
        i += key.size();
        i = line.find_first_not_of(" \t", i);
        if (i == std::string::npos || line[i] != '=') {
            return false;
        }
        n = i + 1;
        return true;
    }

    void save() {
        std::string tmp = m_path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (f == nullptr) {
            return;
        }
        for (const auto &line : lines) {
            std::fputs(line.c_str(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
        std::remove(m_path.c_str());
        std::rename(tmp.c_str(), m_path.c_str());
    }
};

Config g_config;

size_t StepIndex(int percent) {
    size_t best = 0;
    for (size_t i = 0; i < NumSteps; ++i) {
        if (std::abs(Steps[i] - percent) < std::abs(Steps[best] - percent)) {
            best = i;
        }
    }
    return best;
}

// The game the module is attached to (it writes current_game.txt: program id, name), or none
struct CurrentGame {
    std::string id, name;
    bool has_profile = false;
};

CurrentGame ReadCurrentGame() {
    CurrentGame g;
    FILE *f = std::fopen(CurrentGamePath, "r");
    if (f == nullptr) {
        return g;
    }
    char buf[128];
    if (std::fgets(buf, sizeof(buf), f)) {
        g.id = buf;
        g.id.erase(g.id.find_last_not_of("\r\n ") + 1);
    }
    if (std::fgets(buf, sizeof(buf), f)) {
        g.name = buf;
        g.name.erase(g.name.find_last_not_of("\r\n ") + 1);
    }
    g.has_profile = !g.id.empty();
    if (std::fgets(buf, sizeof(buf), f)) {
        g.has_profile = std::string(buf).rfind("profile", 0) == 0;
    }
    std::fclose(f);
    return g;
}

std::string GameDir(const std::string &id) {
    return std::string(GamesDir) + "/" + id;
}

// The installed game profiles (a folder in config/dualsensefx/games with a game.ini)
struct GameEntry {
    std::string id, name;
};

std::vector<GameEntry> InstalledGames() {
    std::vector<GameEntry> games;
    DIR *dir = opendir(GamesDir);
    if (dir == nullptr) {
        return games;
    }
    while (struct dirent *e = readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        Config game;
        game.load(GameDir(e->d_name) + "/game.ini");
        if (game.loaded()) {
            games.push_back({ e->d_name, game.getString("name", e->d_name) });
        }
    }
    closedir(dir);
    return games;
}

// The game whose events the per-event sliders show: the running one if it has a profile, else BotW, until picked
GameEntry g_game;

void PickDefaultGame() {
    CurrentGame running = ReadCurrentGame();
    std::string want = running.has_profile ? running.id : DefaultGame;
    auto games = InstalledGames();
    g_game = { DefaultGame, "Zelda: Breath of the Wild" };
    for (const auto &g : games) {
        if (g.id == want) {
            g_game = g;
            return;
        }
    }
    if (!games.empty()) {
        g_game = games.front();
    }
}

// Per-event sliders edit the picked game's patterns.ini (the strength applies to the DualSense and the Joy-Con set)
std::string PatternsPath() {
    return GameDir(g_game.id) + "/patterns.ini";
}

// patterns.ini sections and their per-pattern "strength =" lines (the module applies them within a second)
class Patterns {
public:
    struct Section {
        std::string name;
        std::string group;
        int strength = 100;
        int header = -1;            // line index of [name]
        int strength_line = -1;     // line index of "strength =", -1 if missing
    };
    std::vector<Section> sections;

    void load(const std::string &path) {
        m_path = path;
        lines.clear();
        sections.clear();
        FILE *f = std::fopen(path.c_str(), "r");
        if (f == nullptr) {
            return;
        }
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            lines.push_back(line);
        }
        std::fclose(f);
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string &l = lines[i];
            size_t b = l.find_first_not_of(" \t");
            if (b == std::string::npos || l[b] == ';') {
                continue;
            }
            if (l[b] == '[') {
                size_t e = l.find(']', b);
                Section s;
                s.name = l.substr(b + 1, e == std::string::npos ? std::string::npos : e - b - 1);
                s.header = i;
                s.group = s.name.rfind("step ", 0) == 0 ? "footsteps" : "world";
                sections.push_back(s);
            } else if (!sections.empty()) {
                int v;
                char g[16];
                if (std::sscanf(l.c_str() + b, "strength = %d", &v) == 1) {
                    sections.back().strength = v;
                    sections.back().strength_line = i;
                } else if (std::sscanf(l.c_str() + b, "group = %15s", g) == 1) {
                    sections.back().group = g;
                }
            }
        }
    }

    void setStrength(const std::string &name, int value) {
        for (auto &s : sections) {
            if (s.name != name) {
                continue;
            }
            std::string entry = "strength = " + std::to_string(value);
            if (s.strength_line >= 0) {
                lines[s.strength_line] = entry;
            } else {
                int at = s.header + 1;
                lines.insert(lines.begin() + at, entry);
                for (auto &o : sections) {
                    if (o.header >= at) ++o.header;
                    if (o.strength_line >= at) ++o.strength_line;
                }
                s.strength_line = at;
            }
            s.strength = value;
            save();
            return;
        }
    }

private:
    std::vector<std::string> lines;
    std::string m_path;

    void save() {
        std::string tmp = m_path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (f == nullptr) {
            return;
        }
        for (const auto &line : lines) {
            std::fputs(line.c_str(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
        std::remove(m_path.c_str());
        std::rename(tmp.c_str(), m_path.c_str());
    }
};

Patterns g_patterns;

tsl::elm::NamedStepTrackBar *StrengthBar(int percent) {
    auto *bar = new tsl::elm::NamedStepTrackBar("↔", { "Off", "25%", "50%", "75%", "100%", "125%", "150%", "175%", "200%" });
    bar->setProgress(StepIndex(percent));
    return bar;
}

// "step grass" -> "Grass", "bomb explode" -> "Bomb explode"
std::string DisplayName(const std::string &name) {
    std::string n = name.rfind("step ", 0) == 0 ? name.substr(5) : name;
    if (!n.empty()) {
        n[0] = std::toupper(n[0]);
    }
    return n;
}

// Every event of one group with its own slider
class GroupGui : public tsl::Gui {
public:
    GroupGui(std::string group, std::string title) : m_group(std::move(group)), m_title(std::move(title)) {}

    tsl::elm::Element *createUI() override {
        g_patterns.load(PatternsPath());
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", m_title + " \u2022 " + g_game.name);
        auto *list = new tsl::elm::List();
        for (const auto &s : g_patterns.sections) {
            if (s.group != m_group) {
                continue;
            }
            list->addItem(new tsl::elm::CategoryHeader(DisplayName(s.name)));
            auto *bar = StrengthBar(s.strength);
            std::string name = s.name;
            bar->setValueChangedListener([name](u8 index) {
                g_patterns.setStrength(name, Steps[index < NumSteps ? index : NumSteps - 1]);
            });
            list->addItem(bar);
        }
        frame->setContent(list);
        return frame;
    }

private:
    std::string m_group, m_title;
};

// Picks the game for the per-event sliders. Only the marks change here (a page must not delete itself from a handler),
// B goes back.
class GamesGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", "Game");
        auto *list = new tsl::elm::List();
        for (const auto &g : InstalledGames()) {
            auto *item = new tsl::elm::ListItem(g.name, g.id == g_game.id ? "\u2713" : "");
            m_items.push_back({ item, g });
            item->setClickListener([this, g](u64 keys) {
                if (keys & HidNpadButton_A) {
                    g_game = g;
                    for (auto &it : m_items) {
                        it.first->setValue(it.second.id == g_game.id ? "\u2713" : "");
                    }
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }
        list->addItem(new tsl::elm::CategoryHeader("More games will come as profiles in config/dualsensefx/games", true));
        frame->setContent(list);
        return frame;
    }

private:
    std::vector<std::pair<tsl::elm::ListItem *, GameEntry>> m_items;
};

// A header with the setting's name and a slider with Off / 25% ... 200%
void AddStrength(tsl::elm::List *list, const char *title, const char *key, int fallback, const char *group = nullptr) {
    list->addItem(new tsl::elm::CategoryHeader(title));
    auto *bar = StrengthBar(g_config.get(key, fallback));
    std::string k = key;
    bar->setValueChangedListener([k](u8 index) {
        g_config.set(k, Steps[index < NumSteps ? index : NumSteps - 1]);
    });
    list->addItem(bar);
    if (group != nullptr) {
        auto *more = new tsl::elm::ListItem("Each event", "›");
        std::string g = group, t = title;
        more->setClickListener([g, t](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GroupGui>(g, t);
                return true;
            }
            return false;
        });
        list->addItem(more);
    }
}

void AddToggle(tsl::elm::List *list, const char *title, const char *key, int fallback, int on_value = 1) {
    auto *toggle = new tsl::elm::ToggleListItem(title, g_config.get(key, fallback) != 0);
    std::string k = key;
    toggle->setStateChangedListener([k, on_value](bool state) {
        g_config.set(k, state ? on_value : 0);
    });
    list->addItem(toggle);
}

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        g_config.load();
        CurrentGame running = ReadCurrentGame();
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", running.has_profile ? running.name : "No supported game running");
        auto *list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Game"));
        m_game = new tsl::elm::ListItem(g_game.name, "\u203A");
        m_game->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GamesGui>();
                return true;
            }
            return false;
        });
        list->addItem(m_game);
        m_shown = g_game.id;

        list->addItem(new tsl::elm::CategoryHeader("Output"));
        auto *output = new tsl::elm::NamedStepTrackBar("↔", { "Off", "DualSense", "Joy-Con" });
        output->setProgress(std::min(g_config.get("output", 1), 2));
        output->setValueChangedListener([](u8 index) { g_config.set("output", index); });
        list->addItem(output);

        list->addItem(new tsl::elm::CategoryHeader("Effects"));
        AddToggle(list, "Adaptive triggers (DualSense)", "triggers", 1);
        AddToggle(list, "Haptics", "haptics", 1);
        AddToggle(list, "Low health heartbeat", "low_health_quarters", 4, 4);

        AddStrength(list, "Overall strength", "haptics_strength", 100);
        AddStrength(list, "Footsteps", "footstep_strength", 50, "footsteps");
        AddStrength(list, "Combat (hits, shield, bow)", "combat_strength", 100, "combat");
        AddStrength(list, "Body (damage, heal, landing)", "body_strength", 100, "body");
        AddStrength(list, "Runes (bombs, Magnesis, Stasis, Cryonis)", "rune_strength", 100, "runes");
        AddStrength(list, "World (water, chests, items, climbing)", "world_strength", 100, "world");

        frame->setContent(list);
        return frame;
    }

    // Back from the Game page: show the new pick
    void update() override {
        if (m_game != nullptr && m_shown != g_game.id) {
            m_game->setText(g_game.name);
            m_shown = g_game.id;
        }
    }

private:
    tsl::elm::ListItem *m_game = nullptr;
    std::string m_shown;
};

class DualSenseFx : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();   // stdio access to the SD card (harmless if already mounted)
    }
    void exitServices() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        PickDefaultGame();
        return initially<MainGui>();
    }
};

}  // namespace

int main(int argc, char **argv) {
    return tsl::loop<DualSenseFx>(argc, argv);
}
