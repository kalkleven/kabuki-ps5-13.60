// SPDX-License-Identifier: GPL-3.0-or-later
// The daemon's side of Kabuki's tiles: the registry of providers' tiles, the saved settings, and
// the files the ShellUI worker reads. Portable C++ with no console dependencies, so the PC preview
// runs exactly this code. Everything from outside is parsed and checked here; the worker only
// reads the records this writes.
#pragma once
#include "content.hpp"
#include "tiles.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kabuki {

class Service {
public:
    // `dir`: the data folder the worker reads; `clock`: milliseconds on the console's monotonic clock.
    Service(std::string dir, std::function<std::uint64_t()> clock) : dir_(std::move(dir)), clock_(std::move(clock)) {}

    // Actions on a provider's tiles, and its tiles coming on screen, go to whoever registered them.
    using ActionHandler = std::function<void(const std::string& tile, const std::string& action, const std::string& key, int value)>;
    using ShownHandler = std::function<void(const std::string& tile)>;
    struct Provider { ActionHandler action; ShownHandler shown; };

    std::uint64_t now() const { return clock_(); }
    const Settings& settings() const { return settings_; }
    const std::vector<TileInfo>& tiles() const { return tiles_; }

    // settings.json (or the older "nqlys.." form) into settings.rec. Call once at start.
    void load_settings() {
        const auto j = nlohmann::json::parse(read_file(dir_ + "/settings.json"), nullptr, false);
        if (j.is_object()) {
            settings_.chord = j.value("chord", settings_.chord);
            settings_.hold_ms = j.value("hold_ms", settings_.hold_ms);
            settings_.sounds = j.value("sounds", settings_.sounds);
            settings_.librespot_tiles = j.value("librespot_tiles", settings_.librespot_tiles);
            if (j.contains("known") && j["known"].is_array())
                for (const auto& id : j["known"])
                    if (id.is_string() && valid_tile_id(id.get<std::string>()) && !settings_.knows(id.get<std::string>()))
                        settings_.known.push_back(id.get<std::string>());
            if (j.contains("tiles") && j["tiles"].is_string()) {
                migrate_codes(settings_, j["tiles"].get<std::string>());
            } else if (j.contains("tiles") && j["tiles"].is_array() && j["tiles"].size() == kSlots) {
                Settings s = settings_;
                for (int i = 0; i < kSlots; ++i) {
                    const auto& id = j["tiles"][std::size_t(i)];
                    s.slots[i] = id.is_string() && valid_tile_id(id.get<std::string>()) ? id.get<std::string>() : "";
                }
                s.arranged = true;
                if (s.slot_of(kSettingsTile) >= 0) settings_ = s; // never an arrangement without Settings
            }
        }
        if (__builtin_popcount(settings_.chord) < 2) settings_.chord = 6;
        settings_.hold_ms = std::clamp(settings_.hold_ms, 100, 5000);
        save_settings();
    }

    // A command from the worker's ring.
    void command(int kind, int value, const std::string& text) {
        if (kind == kSaveSettings) {
            unsigned chord = 0; int hold = 0, sounds = 1;
            int music = settings_.librespot_tiles ? 1 : 0;
            const int fields = std::sscanf(text.c_str(), "chord=%u hold=%d sounds=%d music=%d", &chord, &hold, &sounds, &music);
            if ((fields == 3 || fields == 4) &&
                __builtin_popcount(chord) >= 2) {
                settings_.chord = chord; settings_.hold_ms = std::clamp(hold, 100, 5000); settings_.sounds = sounds != 0;
                if (fields == 4) settings_.librespot_tiles = music != 0;
                save_settings();
            }
        } else if (kind == kSaveSlots) {
            Settings s = settings_;
            std::size_t from = 0;
            for (int i = 0; i < kSlots; ++i) {
                const auto comma = text.find(',', from);
                const auto id = text.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                s.slots[i] = valid_tile_id(id) ? id : "";
                from = comma == std::string::npos ? text.size() : comma + 1;
            }
            s.arranged = true;
            if (s.slot_of(kSettingsTile) < 0) return; // Settings can never be removed
            settings_ = s;
            save_settings();
        } else if (kind == kTileAction) {
            const auto a = text.find('\t'), b = a == std::string::npos ? a : text.find('\t', a + 1);
            if (a == std::string::npos) return;
            const auto tile = text.substr(0, a);
            const auto action = text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            const auto key = b == std::string::npos ? "" : text.substr(b + 1);
            if (auto* p = provider_of(tile); p && p->action) p->action(tile, action, key, value);
        } else if (kind == kTileShown) {
            if (auto* p = provider_of(text); p && p->shown) p->shown(text);
        }
    }

    // A provider's tiles. Ids must be valid and not taken by another provider; Kabuki's own are reserved.
    bool add_provider(const std::string& name, Provider handlers) {
        if (name.empty() || name == "kabuki") return false;
        providers_[name] = std::move(handlers);
        return true;
    }
    bool register_tile(TileInfo t) {
        t.id = normal_tile_id(t.id);
        if (!valid_tile_id(t.id) || kabuki_tile(t.id) || t.id.compare(0, 3, "KBK") == 0) return false;
        if (t.card != Card::media && t.card != Card::list && t.card != Card::custom) return false;
        if (!providers_.count(t.provider)) return false;
        for (const auto& existing : tiles_)
            if (existing.id == t.id) return existing.provider == t.provider; // the first to register keeps it
        if (t.title.size() > 40) t.title.resize(40);
        t.since = ++registered_;
        tiles_.push_back(std::move(t));
        if (learn_tiles(settings_, tiles_)) save_settings();
        save_tiles();
        return true;
    }
    // A provider going away: its tiles leave the rail; their saved places are kept.
    void remove_provider(const std::string& name) {
        const auto before = tiles_.size();
        for (const auto& t : tiles_) if (t.provider == name) std::remove((dir_ + "/" + content_file(t.id)).c_str());
        tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(), [&](const TileInfo& t) { return t.provider == name; }), tiles_.end());
        providers_.erase(name);
        if (tiles_.size() != before) save_tiles();
    }
    bool has_provider(const std::string& name) const { return providers_.count(name) != 0; }
    bool owns(const std::string& provider, const std::string& tile) const {
        for (const auto& t : tiles_) if (t.id == tile) return t.provider == provider;
        return false;
    }
    void set_plugin(const std::string& tile,std::uintptr_t address,const std::string& data,const std::string& error="") { Records r; r.add({"plugin",std::to_string(address),data,error}); write(content_file(tile),r.text()); }
    void set_media(const std::string& tile, const Media& m) { write(content_file(tile), media_records(m).text()); }
    void set_list(const std::string& tile, const List& l) { write(content_file(tile), list_records(l).text()); }
    // Ask the panel to show one of the provider's tiles (it does if one of that provider's is up).
    void show(const std::string& tile) {
        Records r;
        r.add({"show", std::to_string(++show_seq_), tile});
        write(kShowFile, r.text());
    }
    // Text for one of the System card's lines, set by whatever the daemon knows (the librespot link).
    void set_system_note(std::string note) { system_note_ = std::move(note); }
    const std::string& system_note() const { return system_note_; }

private:
    std::string dir_;
    std::function<std::uint64_t()> clock_;
    Settings settings_;
    std::vector<TileInfo> tiles_;
    std::map<std::string, Provider> providers_;
    long long registered_ = 0, show_seq_ = 0;
    std::string system_note_;

    Provider* provider_of(const std::string& tile) {
        for (const auto& t : tiles_)
            if (t.id == tile) { auto it = providers_.find(t.provider); return it == providers_.end() ? nullptr : &it->second; }
        return nullptr;
    }
    void write(const std::string& name, const std::string& text) { write_file_atomic(dir_ + "/" + name, text); }
    void save_tiles() {
        Records r;
        for (const auto& t : tiles_) add_tile(r, t);
        write(kTilesFile, r.text());
    }
    void save_settings() {
        nlohmann::json j = {{"chord", settings_.chord}, {"hold_ms", settings_.hold_ms}, {"sounds", settings_.sounds},
                            {"librespot_tiles", settings_.librespot_tiles}, {"known", settings_.known}};
        if (settings_.arranged) {
            auto slots = nlohmann::json::array();
            for (const auto& id : settings_.slots) slots.push_back(id.empty() ? nlohmann::json() : nlohmann::json(id));
            j["tiles"] = slots;
        }
        write("settings.json", j.dump());
        write(kSettingsFile, settings_records(settings_).text());
    }
};

} // namespace kabuki
