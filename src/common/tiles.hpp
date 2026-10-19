// SPDX-License-Identifier: GPL-3.0-or-later
// Tiles: what the rail shows, who they belong to, and the order they go in. Shared by the daemon
// (which keeps the registry and the saved settings) and the ShellUI worker (which shows them).
#pragma once
#include "records.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kabuki {

constexpr int kSlots = 7; // tiles on the rail at most
constexpr const char* kSystemTile = "KBKSYS";   // Kabuki's own tiles: always registered, always last
constexpr const char* kSettingsTile = "KBKSET"; // by default; Settings can never be removed

// Files the daemon writes for the worker, in its data folder.
constexpr const char* kTilesFile = "tiles.rec";       // the registry (providers' tiles)
constexpr const char* kSettingsFile = "settings.rec"; // shortcut, sounds, the arrangement
constexpr const char* kShowFile = "show.rec";         // a provider asking for one of its tiles
inline std::string content_file(const std::string& id) { return "tile-" + id + ".rec"; }

// Command kinds on the shared ring, worker to daemon.
enum Command : int {
    kSaveSettings = 12, // text: chord=%u hold=%d sounds=%d
    kSaveSlots = 13,    // text: the 7 slot ids, comma separated ("" for an empty slot)
    kTileAction = 20,   // text: tile \t action \t key; value: the action's number
    kTileShown = 21,    // text: tile; the tile's card is now on screen
    kTerminateLibrespot = 22, // stop the exact librespot-ps5.elf process with SIGTERM
};

// A tile id: exactly 6 letters or digits, kept uppercase.
inline bool valid_tile_id(const std::string& id) {
    if (id.size() != 6) return false;
    for (const char c : id) if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}
inline std::string normal_tile_id(std::string id) {
    for (auto& c : id) c = char(std::toupper(static_cast<unsigned char>(c)));
    return id;
}
inline bool kabuki_tile(const std::string& id) { return id == kSystemTile || id == kSettingsTile; }

// What kind of card a tile opens. Template cards are drawn by Kabuki from the provider's data;
// custom cards are the provider's own widget tree.
enum class Card { media, list, stats, settings, custom, none };
inline const char* card_name(Card c) {
    switch (c) {
    case Card::media: return "media"; case Card::list: return "list"; case Card::stats: return "stats";
    case Card::settings: return "settings"; case Card::custom: return "custom"; default: return "none";
    }
}
inline Card card_from_name(const std::string& s) {
    if (s == "media") return Card::media;
    if (s == "list") return Card::list;
    if (s == "stats") return Card::stats;
    if (s == "settings") return Card::settings;
    if (s == "custom" || s == "plugin") return Card::custom;
    return Card::none;
}

struct TileInfo {
    std::string id;       // 6 letters or digits
    std::string provider; // who registered it ("kabuki" for Kabuki's own)
    Card card = Card::none;
    std::string title;    // on the rail and the card's header
    std::string icon;     // "icon:<console icon id>", "file:<png path>" or "chip"
    int priority = 500;   // lower comes first in the default order
    long long since = 0;  // registration order, for ties
};

inline TileInfo system_tile() { return {kSystemTile, "kabuki", Card::stats, "System", "chip", 900, 0}; }
inline TileInfo settings_tile() { return {kSettingsTile, "kabuki", Card::settings, "Settings", "icon:iconid_settings", 901, 0}; }

inline Records& add_tile(Records& r, const TileInfo& t) {
    return r.add({"tile", t.id, t.provider, card_name(t.card), t.title, t.icon, std::to_string(t.priority), std::to_string(t.since)});
}
// The providers' tiles from tiles.rec (Kabuki's own two aren't in it).
inline std::vector<TileInfo> parse_tiles(const std::string& text) {
    std::vector<TileInfo> out;
    for (const auto& r : parse_records(text)) {
        if (field(r, 0) != "tile" || !valid_tile_id(field(r, 1)) || kabuki_tile(field(r, 1))) continue;
        TileInfo t{field(r, 1), field(r, 2), card_from_name(field(r, 3)), field(r, 4), field(r, 5),
                   int(number(r, 6, 500)), number(r, 7)};
        if (t.card == Card::none || t.card == Card::settings) continue;
        out.push_back(std::move(t));
    }
    return out;
}

// Kabuki's settings. `slots` is the user's arrangement once they've changed the Sidebar
// (`arranged`); until then the default order applies. A slot keeps a tile's id while its
// provider isn't running, so the tile comes back to the same place.
struct Settings {
    std::uint32_t chord = 6; // L3 + R3
    int hold_ms = 500;
    bool sounds = true;
    bool librespot_tiles = true; // show the librespot-ps5 provider's tiles on Kabuki's rail
    bool arranged = false;
    std::string slots[kSlots];
    std::vector<std::string> known; // every tile ever seen: a new one is placed once, then left alone

    bool knows(const std::string& id) const { return std::find(known.begin(), known.end(), id) != known.end(); }
    int slot_of(const std::string& id) const {
        for (int i = 0; i < kSlots; ++i) if (slots[i] == id) return i;
        return -1;
    }
};

inline Records settings_records(const Settings& s) {
    Records r;
    r.add({"settings", std::to_string(s.chord), std::to_string(s.hold_ms), s.sounds ? "1" : "0",
           s.librespot_tiles ? "1" : "0"});
    if (s.arranged) {
        Record slots{"slots"};
        for (const auto& id : s.slots) slots.push_back(id);
        r.add(slots);
    }
    Record known{"known"};
    for (const auto& id : s.known) known.push_back(id);
    r.add(known);
    return r;
}
inline Settings parse_settings(const std::string& text) {
    Settings s;
    for (const auto& r : parse_records(text)) {
        if (field(r, 0) == "settings") {
            s.chord = std::uint32_t(number(r, 1, 6)); s.hold_ms = int(number(r, 2, 500)); s.sounds = number(r, 3, 1) != 0;
            s.librespot_tiles = number(r, 4, 1) != 0;
        } else if (field(r, 0) == "slots") {
            s.arranged = true;
            for (int i = 0; i < kSlots; ++i) {
                const auto& id = field(r, std::size_t(i) + 1);
                s.slots[i] = valid_tile_id(id) ? id : "";
            }
        } else if (field(r, 0) == "known") {
            for (std::size_t i = 1; i < r.size(); ++i) if (valid_tile_id(r[i])) s.known.push_back(r[i]);
        }
    }
    if (__builtin_popcount(s.chord) < 2) s.chord = 6;
    s.hold_ms = std::clamp(s.hold_ms, 100, 5000);
    if (s.arranged && s.slot_of(kSettingsTile) < 0) s.arranged = false; // Settings can't be lost
    return s;
}

// Settings from before tile ids: "nqlys.." codes, one per slot.
inline bool migrate_codes(Settings& s, const std::string& codes) {
    static const char* ids[] = {"LSPNOW", "LSPQUE", "LSPLIB", kSystemTile, kSettingsTile};
    std::string slots[kSlots];
    bool settings = false;
    for (int i = 0; i < kSlots; ++i) {
        const char c = i < int(codes.size()) ? codes[std::size_t(i)] : '.';
        const char* at = c ? std::strchr("nqlys", c) : nullptr;
        if (at) { slots[i] = ids[at - "nqlys"]; settings |= slots[i] == kSettingsTile; }
    }
    if (!settings) return false;
    for (int i = 0; i < kSlots; ++i) s.slots[i] = slots[i];
    s.arranged = true;
    for (const auto& id : slots) if (!id.empty() && !s.knows(id)) s.known.push_back(id);
    return true;
}

// The default order: providers' tiles by priority, then registration, then Kabuki's own two.
inline std::vector<std::string> default_order(const std::vector<TileInfo>& online) {
    std::vector<const TileInfo*> theirs;
    for (const auto& t : online) if (!kabuki_tile(t.id)) theirs.push_back(&t);
    std::stable_sort(theirs.begin(), theirs.end(), [](const TileInfo* a, const TileInfo* b) {
        return a->priority != b->priority ? a->priority < b->priority : a->since < b->since;
    });
    std::vector<std::string> order;
    for (const auto* t : theirs) if (int(order.size()) < kSlots - 2) order.push_back(t->id);
    order.push_back(kSystemTile);
    order.push_back(kSettingsTile);
    return order;
}

// The Sidebar page's 7 slots: the saved arrangement, or the default order padded with empties.
inline void current_slots(const Settings& s, const std::vector<TileInfo>& online, std::string out[kSlots]) {
    if (s.arranged) { for (int i = 0; i < kSlots; ++i) out[i] = s.slots[i]; return; }
    const auto order = default_order(online);
    for (int i = 0; i < kSlots; ++i) out[i] = i < int(order.size()) ? order[std::size_t(i)] : "";
}

// The tiles on the rail, in order: saved slots whose tiles are running, or the default order.
inline std::vector<std::string> arrange(const Settings& s, const std::vector<TileInfo>& online) {
    const auto running = [&](const std::string& id) {
        return kabuki_tile(id) || std::any_of(online.begin(), online.end(), [&](const TileInfo& t) { return t.id == id; });
    };
    const auto visible = [&](const std::string& id) {
        if (id == kSystemTile || id == kSettingsTile) return true;
        const auto it = std::find_if(online.begin(), online.end(), [&](const TileInfo& t) { return t.id == id; });
        return it != online.end() && (s.librespot_tiles || it->provider != "librespot");
    };
    if (!s.arranged) {
        auto order = default_order(online);
        order.erase(std::remove_if(order.begin(), order.end(), [&](const std::string& id) { return !visible(id); }), order.end());
        return order;
    }
    std::vector<std::string> order;
    for (const auto& id : s.slots) if (!id.empty() && running(id) && visible(id)) order.push_back(id);
    return order;
}

// The daemon calls this as tiles register: a tile never seen before is remembered, and if the
// user has arranged the Sidebar it goes in just before Kabuki's own tiles (they move down a
// slot). With no slot free it isn't placed, and stays in the picker. Returns whether anything changed.
inline bool learn_tiles(Settings& s, const std::vector<TileInfo>& online) {
    bool changed = false;
    for (const auto& t : online) {
        if (kabuki_tile(t.id) || s.knows(t.id)) continue;
        s.known.push_back(t.id);
        changed = true;
        if (!s.arranged || s.slot_of(t.id) >= 0) continue;
        int at = kSlots;
        for (int i = 0; i < kSlots; ++i) if (kabuki_tile(s.slots[i])) { at = i; break; }
        int empty = -1;
        for (int i = at; i < kSlots && empty < 0; ++i) if (s.slots[i].empty()) empty = i;
        if (empty >= 0) {
            for (int i = empty; i > at; --i) s.slots[i] = s.slots[i - 1];
            s.slots[at] = t.id;
        } else {
            for (int i = std::min(at, kSlots) - 1; i >= 0; --i) if (s.slots[i].empty()) { s.slots[i] = t.id; break; }
        }
    }
    return changed;
}

} // namespace kabuki
