// DualSense FX: Tesla / Ultrahand overlay for the adaptive_triggers settings.
// It only edits sdmc:/config/adaptive_triggers/config.ini; the sysmodule notices the change and applies it within
// a second, no reboot needed.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr const char *ConfigPath = "sdmc:/config/adaptive_triggers/config.ini";
constexpr const char *ConfigTmp  = "sdmc:/config/adaptive_triggers/config.ini.tmp";

// Strength steps shown on the sliders, percent
constexpr int Steps[] = { 0, 25, 50, 75, 100, 125, 150, 175, 200 };
constexpr size_t NumSteps = sizeof(Steps) / sizeof(Steps[0]);

class Config {
public:
    void load() {
        lines.clear();
        FILE *f = std::fopen(ConfigPath, "r");
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
        FILE *f = std::fopen(ConfigTmp, "w");
        if (f == nullptr) {
            return;
        }
        for (const auto &line : lines) {
            std::fputs(line.c_str(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
        std::remove(ConfigPath);
        std::rename(ConfigTmp, ConfigPath);
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

// A header with the setting's name and a slider with Off / 25% ... 200%
void AddStrength(tsl::elm::List *list, const char *title, const char *key, int fallback) {
    list->addItem(new tsl::elm::CategoryHeader(title));
    auto *bar = new tsl::elm::NamedStepTrackBar("↔", { "Off", "25%", "50%", "75%", "100%", "125%", "150%", "175%", "200%" });
    bar->setProgress(StepIndex(g_config.get(key, fallback)));
    std::string k = key;
    bar->setValueChangedListener([k](u8 index) {
        g_config.set(k, Steps[index < NumSteps ? index : NumSteps - 1]);
    });
    list->addItem(bar);
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
        auto *frame = new tsl::elm::OverlayFrame("DualSense FX", "BotW 1.6.0 • applied instantly");
        auto *list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Effects"));
        AddToggle(list, "Adaptive triggers", "triggers", 1);
        AddToggle(list, "Haptics", "haptics", 1);
        AddToggle(list, "Low health heartbeat", "low_health_quarters", 4, 4);

        AddStrength(list, "Overall strength", "haptics_strength", 100);
        AddStrength(list, "Footsteps", "footstep_strength", 50);
        AddStrength(list, "Combat (hits, shield, bow)", "combat_strength", 100);
        AddStrength(list, "Body (damage, heal, landing)", "body_strength", 100);
        AddStrength(list, "Runes (bombs, Magnesis, Stasis, Cryonis)", "rune_strength", 100);
        AddStrength(list, "World (water, chests, items, climbing)", "world_strength", 100);

        frame->setContent(list);
        return frame;
    }
};

class DualSenseFx : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();   // stdio access to the SD card (harmless if already mounted)
    }
    void exitServices() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainGui>();
    }
};

}  // namespace

int main(int argc, char **argv) {
    return tsl::loop<DualSenseFx>(argc, argv);
}
