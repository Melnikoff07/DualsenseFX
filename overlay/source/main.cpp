// DualsenseFX: Tesla / Ultrahand overlay for the adaptive_triggers settings.
// It only edits files in sdmc:/config/dualsensefx (config.ini, the games' game.ini and patterns.ini); the sysmodule
// notices the change and applies it within a second, no reboot needed.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
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

// The game's title from its control data (falls back to the program id)
std::string GameTitle(const std::string &id) {
    std::string title = id;
    if (R_FAILED(nsInitialize())) {
        return title;
    }
    auto *data = new NsApplicationControlData;
    u64 size = 0;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, std::strtoull(id.c_str(), nullptr, 16),
                                                data, sizeof(*data), &size))) {
        NacpLanguageEntry *lang = nullptr;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &lang)) && lang && lang->name[0]) {
            title = lang->name;
        }
    }
    delete data;
    nsExit();
    return title;
}

// A new profile: the module attaches within a few seconds (after its usual 30 s wait) and finds the sounds itself
bool CreateProfile(const std::string &id) {
    mkdir(GamesDir, 0777);
    mkdir(GameDir(id).c_str(), 0777);
    FILE *f = std::fopen((GameDir(id) + "/game.ini").c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "; DualsenseFX game profile, created from the overlay\nname = %s\nenabled = 1\n"
                    "; Empty: any build. The nn::atk sound pool is looked for automatically and written below.\nbuilds =\n",
                 GameTitle(id).c_str());
    std::fclose(f);
    return true;
}

constexpr const char *MonitorOn   = "sdmc:/config/dualsensefx/monitor.on";
constexpr const char *MonitorFile = "sdmc:/config/dualsensefx/monitor.txt";
constexpr const char *LibraryPath = "sdmc:/config/dualsensefx/patterns.ini";

// Section names of a patterns file
std::vector<std::string> PatternNames(const std::string &path) {
    std::vector<std::string> names;
    FILE *f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
        return names;
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string l(buf);
        size_t b = l.find_first_not_of(" \t");
        if (b != std::string::npos && l[b] == '[') {
            size_t e = l.find(']', b);
            std::string n = l.substr(b + 1, e == std::string::npos ? std::string::npos : e - b - 1);
            if (n.rfind("step ", 0) != 0) {
                names.push_back(n);
            }
        }
    }
    std::fclose(f);
    return names;
}

// Puts "name = action" first in the game's sounds.ini (the first matching rule wins), replacing an older rule for it
void AssignSound(const std::string &id, const std::string &name, const std::string &action) {
    std::string path = GameDir(id) + "/sounds.ini";
    std::vector<std::string> lines;
    FILE *f = std::fopen(path.c_str(), "r");
    if (f) {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string l(buf);
            l.erase(l.find_last_not_of("\r\n") + 1);
            size_t eq = l.find('=');
            if (eq != std::string::npos && l[0] != ';') {
                std::string mask = l.substr(0, eq);
                mask.erase(mask.find_last_not_of(" \t") + 1);
                if (mask == name) {
                    continue;
                }
            }
            lines.push_back(l);
        }
        std::fclose(f);
    }
    size_t at = 0;
    while (at < lines.size() && (lines[at].empty() || lines[at][0] == ';')) {
        ++at;
    }
    lines.insert(lines.begin() + at, name + " = " + action);
    std::string tmp = path + ".tmp";
    FILE *o = std::fopen(tmp.c_str(), "w");
    if (o == nullptr) {
        return;
    }
    for (const auto &l : lines) {
        std::fputs(l.c_str(), o);
        std::fputc('\n', o);
    }
    std::fclose(o);
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

// What a monitored sound should play
class AssignGui : public tsl::Gui {
public:
    AssignGui(std::string id, std::string name) : m_id(std::move(id)), m_name(std::move(name)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", m_name);
        auto *list = new tsl::elm::List();
        auto add = [&](const std::string &label, const std::string &action) {
            auto *item = new tsl::elm::ListItem(label);
            std::string id = m_id, name = m_name;
            // No goBack() here: it would delete this page while its handler runs (B goes back safely)
            item->setClickListener([item, label, id, name, action](u64 keys) {
                if (keys & HidNpadButton_A) {
                    AssignSound(id, name, action);
                    item->setText("\u2713 " + label + " (saved, B to go back)");
                    return true;
                }
                return false;
            });
            list->addItem(item);
        };
        list->addItem(new tsl::elm::CategoryHeader("Play"));
        add("Nothing (ignore)", "ignore");
        add("Footstep (ground from the name)", "step");
        list->addItem(new tsl::elm::CategoryHeader("Library"));
        for (const auto &n : PatternNames(LibraryPath)) {
            add(n, n);
        }
        auto own = PatternNames(GameDir(m_id) + "/patterns.ini");
        if (!own.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("This game's patterns"));
            for (const auto &n : own) {
                add(n, n);
            }
        }
        frame->setContent(list);
        return frame;
    }

private:
    std::string m_id, m_name;
};

bool Recording() {
    struct stat st;
    return stat(MonitorOn, &st) == 0;
}

void SetRecording(bool on) {
    if (on) {
        if (FILE *f = std::fopen(MonitorOn, "w")) {
            std::fclose(f);
        }
    } else {
        std::remove(MonitorOn);
    }
}

// The recorded sounds as they were when the page opened (nothing moves), newest first, with how long ago they played.
// Refresh and the filter only swap the rows below them: the list removes and adds items between frames, so the
// element whose handler runs is never deleted under it (rebuilding the whole page from its handler crashed).
class RecordedGui : public tsl::Gui {
public:
    explicit RecordedGui(std::string id) : m_id(std::move(id)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", Recording() ? "Recorded sounds (recording)" : "Recorded sounds");
        m_list = new tsl::elm::List();

        auto *refresh = new tsl::elm::ListItem("Refresh", "↻");
        refresh->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                fill();
                return true;
            }
            return false;
        });
        m_list->addItem(refresh);
        auto *filter = new tsl::elm::ToggleListItem("Only sounds without an effect", m_unassigned_only);
        filter->setStateChangedListener([this](bool state) {
            m_unassigned_only = state;
            fill();
        });
        m_list->addItem(filter);
        fill();
        frame->setContent(m_list);
        return frame;
    }

private:
    std::string m_id;
    bool m_unassigned_only = false;
    tsl::elm::List *m_list = nullptr;
    std::vector<tsl::elm::Element *> m_rows;

    void fill() {
        for (auto *row : m_rows) {
            m_list->removeItem(row);
        }
        m_rows.clear();
        FILE *f = std::fopen(MonitorFile, "r");
        if (f != nullptr) {
            u64 now = armGetSystemTick(), freq = armGetSystemTickFreq();
            char buf[256];
            while (m_rows.size() < 120 && std::fgets(buf, sizeof(buf), f)) {
                std::string l(buf);
                l.erase(l.find_last_not_of("\r\n") + 1);
                size_t t1 = l.find('\t'), t2 = l.find('\t', t1 + 1), t3 = l.find('\t', t2 + 1);
                if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) {
                    continue;
                }
                u64 tick = std::strtoull(l.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
                std::string name = l.substr(t2 + 1, t3 - t2 - 1), action = l.substr(t3 + 1);
                bool assigned = action != "-";
                if (m_unassigned_only && assigned) {
                    continue;
                }
                u64 ago = tick && now > tick ? (now - tick) / freq : 0;
                std::string when = ago < 60 ? std::to_string(ago) + "s" : ago < 3600 ? std::to_string(ago / 60) + "m" : ">1h";
                auto *item = new tsl::elm::ListItem(name);
                item->setValue((assigned ? "✓ " : "") + when, assigned);
                std::string id = m_id;
                item->setClickListener([id, name](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        tsl::changeTo<AssignGui>(id, name);
                        return true;
                    }
                    return false;
                });
                m_list->addItem(item);
                m_rows.push_back(item);
            }
            std::fclose(f);
        }
        if (m_rows.empty()) {
            auto *hint = new tsl::elm::CategoryHeader(Recording() ? "Nothing yet: play a bit, then Refresh" :
                                                      "Turn on Record sounds, play, then come back", true);
            m_list->addItem(hint);
            m_rows.push_back(hint);
        }
    }
};

// Per-event sliders edit the running game's patterns.ini, or BotW's while no game runs
std::string PatternsPathFor(const std::string &id) {
    return std::string(GamesDir) + "/" + (id.empty() ? DefaultGame : id) + "/patterns.ini";
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
        CurrentGame game = ReadCurrentGame();
        g_patterns.load(PatternsPathFor(game.id));
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", m_title + (game.name.empty() ? "" : " \u2022 " + game.name));
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

// The installed game profiles, each can be turned off (enabled = in its game.ini)
class GamesGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", "Games");
        auto *list = new tsl::elm::List();
        DIR *dir = opendir(GamesDir);
        if (dir != nullptr) {
            while (struct dirent *e = readdir(dir)) {
                if (e->d_name[0] == '.') {
                    continue;
                }
                std::string ini = std::string(GamesDir) + "/" + e->d_name + "/game.ini";
                Config game;
                game.load(ini);
                if (!game.loaded()) {
                    continue;
                }
                std::string name = game.getString("name", e->d_name);
                auto *toggle = new tsl::elm::ToggleListItem(name, game.get("enabled", 1) != 0);
                toggle->setStateChangedListener([ini](bool state) {
                    Config g;
                    g.load(ini);
                    g.set("enabled", state ? 1 : 0);
                });
                list->addItem(toggle);
            }
            closedir(dir);
        }
        list->addItem(new tsl::elm::CategoryHeader("A game is used when its folder in config/dualsensefx/games has a game.ini", true));
        frame->setContent(list);
        return frame;
    }
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
        CurrentGame game = ReadCurrentGame();
        auto *frame = new tsl::elm::OverlayFrame("DualsenseFX", game.id.empty() ? "No game running" :
                                                  game.has_profile ? game.name : "This game has no profile");
        auto *list = new tsl::elm::List();

        if (!game.id.empty() && !game.has_profile) {
            auto *create = new tsl::elm::ListItem("Create a profile for this game", "\u203A");
            std::string id = game.id;
            create->setClickListener([create, id](u64 keys) {
                if (keys & HidNpadButton_A) {
                    create->setText(CreateProfile(id) ? "Created: starts in about 30 s" : "Could not create the profile");
                    return true;
                }
                return false;
            });
            list->addItem(create);
        }
        if (game.has_profile) {
            std::string status = "unknown";
            if (FILE *sf = std::fopen("sdmc:/config/dualsensefx/sound_status.txt", "r")) {
                char sb[64] = {0};
                if (std::fgets(sb, sizeof(sb), sf)) {
                    status = sb;
                    status.erase(status.find_last_not_of("\r\n ") + 1);
                }
                std::fclose(sf);
            }
            std::string header = status == "active" ? "Sounds of this game (found)" :
                                 status == "searching" ? "Sounds of this game (looking...)" :
                                 status == "not supported" ? "Sounds: this game has its own audio engine, not supported yet" :
                                 "Sounds of this game";
            list->addItem(new tsl::elm::CategoryHeader(header));
            auto *record = new tsl::elm::ToggleListItem("Record sounds", Recording());
            record->setStateChangedListener([](bool state) { SetRecording(state); });
            list->addItem(record);
            auto *recorded = new tsl::elm::ListItem("Recorded sounds", "\u203A");
            std::string id = game.id;
            recorded->setClickListener([id](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<RecordedGui>(id);
                    return true;
                }
                return false;
            });
            list->addItem(recorded);
        }

        auto *games = new tsl::elm::ListItem("Games", "\u203A");
        games->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GamesGui>();
                return true;
            }
            return false;
        });
        list->addItem(games);

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
};

class DualSenseFx : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();   // stdio access to the SD card (harmless if already mounted)
    }
    void exitServices() override {}     // recording goes on while the overlay is closed

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainGui>();
    }
};

}  // namespace

int main(int argc, char **argv) {
    return tsl::loop<DualSenseFx>(argc, argv);
}
