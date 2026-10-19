// SPDX-License-Identifier: GPL-3.0-or-later
// The provider broker: what other programs on the console speak to put tiles in Kabuki's panel
// (protocol/PROTOCOL.md). Portable C++ with no sockets: the daemon feeds it the bytes each client
// sends and sends the lines it hands back, and the PC preview does the same over a pipe.
//
// Each client becomes a provider of the Service. Everything it sends is parsed and checked here
// (ids, ownership, lengths, picture paths, rates); the worker in ShellUI only ever reads the
// records the Service writes. A client that misbehaves is dropped, and its tiles leave the rail.
#pragma once
#include "service.hpp"
#include <cmath>
#include <deque>
#include <map>
#include <set>

namespace kabuki {

// The limits in protocol/PROTOCOL.md.
inline constexpr int kBrokerClients = 8;       // providers connected at once
inline constexpr int kBrokerTiles = 6;         // tiles per provider
inline constexpr int kBrokerRows = 500;        // rows per list level
inline constexpr std::size_t kBrokerText = 256;
inline constexpr std::size_t kBrokerTitle = 40;
inline constexpr std::size_t kBrokerLine = 64 * 1024;
inline constexpr std::size_t kBrokerBuffer = 256 * 1024;
inline constexpr std::uint64_t kBrokerUpdateMs = 100; // content: 10 per second per tile
inline constexpr int kBrokerLinesPerSecond = 200;
inline constexpr int kBrokerStrikes = 20;             // refused messages within kBrokerStrikeMs
inline constexpr std::uint64_t kBrokerStrikeMs = 10000;
inline constexpr int kBrokerArtDirs = 4;
inline constexpr int kBrokerChanges = 12;

// Cuts text to at most `max` bytes without splitting a UTF-8 character.
inline std::string clip_text(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

// An absolute path of plain components: no "..", ".", empty parts, control characters or trailing slash.
inline bool plain_path(const std::string& p) {
    if (p.size() < 2 || p.size() > kBrokerText || p[0] != '/' || p.back() == '/') return false;
    std::size_t part = 1;
    for (std::size_t i = 1; i <= p.size(); ++i) {
        if (i < p.size() && static_cast<unsigned char>(p[i]) < 0x20) return false;
        if (i == p.size() || p[i] == '/') {
            const auto name = p.substr(part, i - part);
            if (name.empty() || name == "." || name == "..") return false;
            part = i + 1;
        }
    }
    return true;
}
// A folder a provider may keep pictures in.
inline bool art_dir_valid(const std::string& d) {
    return plain_path(d) && (d.compare(0, 6, "/data/") == 0 || d.compare(0, 11, "/user/data/") == 0);
}
inline bool art_extension(const std::string& p) {
    std::string tail = p.size() > 5 ? p.substr(p.size() - 5) : p;
    for (auto& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto ends = [&](const char* e) { const std::size_t n = std::strlen(e); return tail.size() >= n && tail.compare(tail.size() - n, n, e) == 0; };
    return ends(".png") || ends(".jpg") || ends(".jpeg");
}
inline bool art_inside(const std::string& dir, const std::string& p) {
    return plain_path(p) && p.size() > dir.size() + 1 && p.compare(0, dir.size() + 1, dir + "/") == 0 && art_extension(p);
}

class Broker {
public:
    struct Hooks {
        std::function<void(int client, const std::string& line)> send;   // one line, without the newline
        std::function<void(const std::string& text)> log;                // optional
        // For a picture that passed the path rules: false if it is a symlink, not a JPEG or PNG, or
        // over 2 MiB. A file that isn't there yet passes. Without this only the path is checked.
        std::function<bool(const std::string& dir, const std::string& path)> art_ok;
        std::function<std::uintptr_t(const std::string& path)> plugin_load;
        std::function<void(const std::string&)> native_provider;
    };

    // `art_root`: the folder under which a provider without its own gets "<art_root>/<provider>".
    Broker(Service& service, Hooks hooks, std::string art_root, std::string version)
        : s_(service), hooks_(std::move(hooks)), art_root_(std::move(art_root)), version_(std::move(version)) {}

    // A client connected: its id, or -1 if the broker is full.
    int open() {
        int alive = 0;
        for (const auto& [id, c] : clients_) alive += c.dead ? 0 : 1;
        if (alive >= kBrokerClients) return -1;
        const int id = ++last_id_;
        clients_[id];
        return id;
    }
    // The client's socket closed (or was closed because alive() turned false).
    void closed(int id) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        drop(id, "disconnected");
        clients_.erase(it);
    }
    bool alive(int id) const { auto it = clients_.find(id); return it != clients_.end() && !it->second.dead; }
    int clients() const { int n = 0; for (const auto& [id, c] : clients_) n += c.dead ? 0 : 1; return n; }

    // Bytes from a client.
    void data(int id, const char* bytes, std::size_t size) {
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.dead) return;
        auto& c = it->second;
        c.buffer.append(bytes, size);
        for (;;) {
            const auto end = c.buffer.find('\n');
            if (end == std::string::npos) {
                if (c.buffer.size() > kBrokerLine) return drop(id, "line over 64 KiB");
                break;
            }
            if (end > kBrokerLine) return drop(id, "line over 64 KiB");
            std::string line = c.buffer.substr(0, end);
            c.buffer.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto now = s_.now();
            if (now - c.window >= 1000) { c.window = now; c.window_lines = 0; }
            if (++c.window_lines > kBrokerLinesPerSecond) return drop(id, "too many lines per second");
            handle(id, line);
            if (!alive(id)) return;
        }
        if (c.buffer.size() > kBrokerBuffer) drop(id, "buffer over 256 KiB");
    }
    void data(int id, const std::string& bytes) { data(id, bytes.data(), bytes.size()); }

    // Once in a while: content held back by the rate limit is written.
    void tick() {
        const auto now = s_.now();
        for (auto& [tile, t] : tiles_)
            if (t.dirty && now - t.written >= kBrokerUpdateMs) write(tile, t, now);
    }

private:
    using json = nlohmann::json;

    struct Change { std::uint64_t when; int position; bool moving; };
    // The heard position: where it was at `at`, whether it was moving on, and changes still to come.
    struct Timeline {
        std::uint64_t at = 0; int position = 0; bool moving = false;
        std::vector<Change> changes;
        bool pending = false;
        void settle(std::uint64_t now) {
            std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.when < b.when; });
            std::size_t done = 0;
            while (done < changes.size() && changes[done].when <= now) {
                at = changes[done].when; position = changes[done].position; moving = changes[done].moving; ++done;
            }
            changes.erase(changes.begin(), changes.begin() + std::ptrdiff_t(done));
        }
        int position_at(std::uint64_t now) const {
            if (!moving || now <= at) return position;
            return int(std::min<std::uint64_t>(std::uint64_t(position) + (now - at), 2000000000ULL));
        }
    };
    struct TileState {
        int client = 0;
        std::string provider;
        Card card = Card::none;
        Media media;
        List list;
        Timeline timeline;
        std::uintptr_t plugin_address=0;
        std::string plugin_data="{}",plugin_error;
        bool dirty = false;
        std::uint64_t written = 0;
    };
    struct Client {
        std::string buffer, provider, note;
        bool hello = false, dead = false;
        std::uint64_t window = 0, last_show = 0;
        int window_lines = 0;
        std::deque<std::uint64_t> strikes;
        std::vector<std::string> tiles, dirs;
        std::map<std::string, std::uint64_t> checked; // pictures that passed the file check, until
    };
    // What went wrong in one message, once per kind.
    struct Issues {
        std::vector<std::pair<std::string, std::string>> list;
        void add(const std::string& code, const std::string& message) {
            for (const auto& i : list) if (i.first == code && i.second == message) return;
            list.emplace_back(code, message);
        }
    };

    Service& s_;
    Hooks hooks_;
    std::string art_root_, version_;
    std::map<int, Client> clients_;
    std::map<std::string, TileState> tiles_;
    int last_id_ = 0;

    void log(const std::string& text) { if (hooks_.log) hooks_.log(text); }
    void send(int id, const json& message) {
        auto j = message;
        j["v"] = 1;
        if (hooks_.send) hooks_.send(id, j.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    // Refuses something: tells the client, and drops one that keeps doing it.
    void error(int id, const std::string& code, const std::string& message, const std::string& tile = "") {
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.dead) return;
        json e = {{"code", code}, {"message", message}};
        if (!tile.empty()) e["tile"] = tile;
        send(id, {{"error", e}});
        log("BROKER " + (it->second.provider.empty() ? "client " + std::to_string(id) : it->second.provider) + ": " + code + ": " + message);
        auto& strikes = it->second.strikes;
        const auto now = s_.now();
        strikes.push_back(now);
        while (!strikes.empty() && now - strikes.front() > kBrokerStrikeMs) strikes.pop_front();
        if (int(strikes.size()) >= kBrokerStrikes) drop(id, "too many refused messages");
    }
    void report(int id, const Issues& issues, const std::string& tile) {
        for (const auto& i : issues.list) error(id, i.first, i.second, tile);
    }

    void drop(int id, const std::string& why) {
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.dead) return;
        auto& c = it->second;
        c.dead = true;
        c.buffer.clear();
        if (!c.provider.empty() && c.hello) {
            for (const auto& t : c.tiles) tiles_.erase(t);
            s_.remove_provider(c.provider);
            if (c.provider == "librespot") s_.set_system_note("Not running");
        }
        log("BROKER " + (c.provider.empty() ? "client " + std::to_string(id) : c.provider) + " dropped: " + why);
    }

    void handle(int id, const std::string& line) {
        auto& c = clients_.at(id);
        const auto m = json::parse(line, nullptr, false);
        if (!m.is_object()) return error(id, "bad_json", "not a JSON object");
        const auto v = m.find("v");
        if (v == m.end() || !v->is_number_integer()) return error(id, "bad_message", "missing v");
        if (v->get<long long>() != 1) {
            send(id, {{"error", {{"code", "unsupported_version"}, {"message", "this Kabuki speaks version 1"}, {"versions", {1}}}}});
            return drop(id, "unsupported protocol version");
        }
        std::string kind;
        for (auto i = m.begin(); i != m.end(); ++i) {
            if (i.key() == "v") continue;
            if (!kind.empty()) return error(id, "bad_message", "one message per line");
            kind = i.key();
        }
        if (kind.empty()) return error(id, "bad_message", "no message");
        const auto& body = m[kind];
        if (!body.is_object()) return error(id, "bad_message", kind + " must be an object");
        if (kind == "hello") {
            if (c.hello) return error(id, "bad_message", "hello twice");
            return hello(id, body);
        }
        if (!c.hello) return error(id, "no_hello", "send hello first");
        if (kind == "content" || kind == "data") content(id, body);
        else if (kind == "show") show(id, body);
        else if (kind == "status") status(id, body);
        else error(id, "bad_message", "unknown message " + clip_text(kind, 40));
    }

    static std::string str(const json& j, const char* key) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    }
    static bool provider_name_valid(const std::string& n) {
        if (n.empty() || n.size() > 24 || n == "kabuki") return false;
        for (const char c : n) if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-')) return false;
        return true;
    }

    // A picture path for a card: inside one of the provider's folders and passing the file check.
    // "" if it isn't.
    std::string art(Client& c, const std::string& path, Issues& issues) {
        if (path.empty()) return "";
        for (const auto& dir : c.dirs) {
            if (!art_inside(dir, path)) continue;
            if (hooks_.art_ok) {
                const auto now = s_.now();
                const auto known = c.checked.find(path);
                if (known == c.checked.end() || known->second < now) {
                    if (!hooks_.art_ok(dir, path)) { issues.add("bad_art", "not a PNG or JPEG file of 2 MiB or less, or a symlink"); return ""; }
                    if (c.checked.size() >= 2048) c.checked.clear();
                    c.checked[path] = now + 30000;
                }
            }
            return path;
        }
        issues.add("bad_art", "picture outside the provider's art folders");
        return "";
    }

    void hello(int id, const json& h) {
        auto& c = clients_.at(id);
        const auto name = str(h, "provider");
        if (!provider_name_valid(name)) {
            error(id, "bad_provider", "provider must be 1 to 24 of a-z, 0-9 and -, and not kabuki");
            return drop(id, "bad provider name");
        }
        if(h.contains("native")&&!h["native"].is_boolean()) {error(id,"bad_hello","native must be boolean");return drop(id,"bad native field");}
        if(h.value("native",false)&&hooks_.native_provider)hooks_.native_provider(name);
        if (s_.has_provider(name)) {
            error(id, "bad_provider", "provider " + name + " is already connected");
            return drop(id, "provider name in use");
        }
        const auto tiles = h.find("tiles");
        if (tiles == h.end() || !tiles->is_array() || tiles->empty()) {
            error(id, "bad_message", "hello needs tiles");
            return drop(id, "hello without tiles");
        }
        Issues issues;
        const std::string default_dir = art_root_ + "/" + name;
        if (h.contains("art_dirs") && h["art_dirs"].is_array()) {
            for (const auto& d : h["art_dirs"]) {
                const auto dir = d.is_string() ? d.get<std::string>() : std::string();
                if (!art_dir_valid(dir)) { issues.add("bad_art", "art_dirs: " + clip_text(dir, 60) + " is not under /data/ or /user/data/"); continue; }
                if (int(c.dirs.size()) >= kBrokerArtDirs) { issues.add("too_long", "at most 4 art_dirs"); break; }
                if (std::find(c.dirs.begin(), c.dirs.end(), dir) == c.dirs.end()) c.dirs.push_back(dir);
            }
        }
        if (c.dirs.empty()) c.dirs.push_back(default_dir);
        c.provider = name;
        c.hello = true;

        Service::Provider p;
        p.action = [this, id](const std::string& tile, const std::string& action, const std::string& key, int value) {
            json a = {{"tile", tile}, {"action", action}};
            if (!key.empty()) a["key"] = key;
            if (action == "seek" || action == "volume" || action == "shuffle" || action == "repeat" || (tiles_.count(tile)&&tiles_.at(tile).card==Card::custom)) a["value"] = value;
            send(id, {{"action", a}});
        };
        p.shown = [this, id](const std::string& tile) { send(id, {{"shown", {{"tile", tile}}}}); };
        s_.add_provider(name, p);
        if (name == "librespot") s_.set_system_note("Connected");

        send(id, {{"welcome", {{"kabuki", version_}, {"versions", {1}}, {"art_dir", default_dir}, {"templates", {"media", "list", "plugin"}},
                               {"limits", {{"tiles", kBrokerTiles}, {"rows", kBrokerRows}, {"text", kBrokerText}, {"title", kBrokerTitle},
                                           {"line", kBrokerLine}, {"updates_per_second", int(1000 / kBrokerUpdateMs)}}}}}});
        report(id, issues, "");

        int count = 0;
        for (const auto& t : *tiles) {
            if (!alive(id)) return;
            if (++count > kBrokerTiles) { error(id, "too_long", "at most 6 tiles per provider"); break; }
            register_one(id, c, t);
        }
        log("BROKER " + name + " connected with " + std::to_string(c.tiles.size()) + " tiles");
    }

    void register_one(int id, Client& c, const json& t) {
        if (!t.is_object()) return error(id, "bad_message", "a tile must be an object");
        const auto raw = str(t, "id");
        const auto tile = normal_tile_id(raw);
        if (!valid_tile_id(tile)) return error(id, "bad_tile_id", "id must be 6 letters or digits", clip_text(raw, 12));
        if (tile.compare(0, 3, "KBK") == 0) return error(id, "reserved_id", "KBK ids belong to Kabuki", tile);
        const auto card_name = str(t, "card");
        const auto card = card_from_name(card_name);
        if(card==Card::custom) {const auto path=str(t,"plugin");
            if(!plain_path(path)||path.size()<15||path.compare(0,8,"/mnt/usb")||path[8]<'0'||path[8]>'7'||path[9]!='/'||path.substr(path.size()-4)!=".elf")
                return error(id,"bad_plugin","plugin must name an ELF on a mounted USB",tile);
        }
        if (card != Card::custom && card != Card::media && card != Card::list) return error(id, "bad_card", "card must be media, list or plugin", tile);
        for (const auto& existing : s_.tiles())
            if (existing.id == tile) return error(id, existing.provider == c.provider ? "bad_message" : "tile_taken",
                                                  existing.provider == c.provider ? "tile listed twice" : "id held by another provider", tile);
        Issues issues;
        TileInfo info;
        info.id = tile; info.provider = c.provider; info.card = card;
        const auto title = str(t, "title");
        if (title.size() > kBrokerTitle) issues.add("too_long", "title is cut to 40 bytes");
        info.title = clip_text(title, kBrokerTitle);
        info.icon = "icon:iconid_list";
        const auto icon = str(t, "icon");
        if (!icon.empty()) {
            if (icon.compare(0, 5, "icon:") == 0 && icon.size() > 5 && icon.size() <= 69 &&
                std::all_of(icon.begin() + 5, icon.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.'; }))
                info.icon = icon;
            else if (icon.compare(0, 5, "file:") == 0) {
                const auto path = art(c, icon.substr(5), issues);
                if (!path.empty()) info.icon = "file:" + path;
            } else issues.add("bad_message", "icon must be icon:<id> or file:<path>");
        }
        info.priority = 500;
        if (t.contains("priority") && t["priority"].is_number()) info.priority = int(std::clamp(t["priority"].get<double>(), 0.0, 899.0));
        if (!s_.register_tile(info)) return error(id, "bad_tile_id", "the tile was refused", tile);
        c.tiles.push_back(tile);
        TileState& state = tiles_[tile];
        state = TileState();
        state.client = id; state.provider = c.provider; state.card = card;
        if(card==Card::custom) {
            const auto path=str(t,"plugin");
            if(!plain_path(path)||path.compare(0,8,"/mnt/usb")||path.size()<4||path.substr(path.size()-4)!=".elf")
                state.plugin_error="Plugin must be an ELF on USB";
            else if(hooks_.plugin_load) state.plugin_address=hooks_.plugin_load(path);
            if(!state.plugin_address&&state.plugin_error.empty())state.plugin_error="Plugin failed file or ABI validation";
        }
        if (card == Card::list) state.list.top.loading = true; // until the first content
        write(tile, state, s_.now());
        report(id, issues, tile);
    }

    // ---- merging a provider's data into a tile ----

    static void set_text(const json& d, const char* key, std::string& out, Issues& is, std::size_t max = kBrokerText) {
        const auto it = d.find(key);
        if (it == d.end()) return;
        if (it->is_null()) out.clear();
        else if (it->is_string()) {
            auto s = it->get<std::string>();
            if (s.size() > max) { is.add("too_long", std::string(key) + " is cut to " + std::to_string(max) + " bytes"); s = clip_text(std::move(s), max); }
            out = std::move(s);
        } else is.add("bad_message", std::string(key) + " must be a string");
    }
    static void set_bool(const json& d, const char* key, bool& out, Issues& is) {
        const auto it = d.find(key);
        if (it == d.end()) return;
        if (it->is_boolean()) out = it->get<bool>();
        else if (it->is_null()) out = false;
        else is.add("bad_message", std::string(key) + " must be true or false");
    }
    static void set_int(const json& d, const char* key, int& out, double lo, double hi, Issues& is) {
        const auto it = d.find(key);
        if (it == d.end()) return;
        if (it->is_number()) out = int(std::clamp(std::floor(it->get<double>()), lo, hi));
        else is.add("bad_message", std::string(key) + " must be a number");
    }
    void set_art(Client& c, const json& d, const char* key, std::string& out, Issues& is) {
        const auto it = d.find(key);
        if (it == d.end()) return;
        if (it->is_null()) out.clear();
        else if (it->is_string()) out = art(c, it->get<std::string>(), is);
        else is.add("bad_message", std::string(key) + " must be a string");
    }

    void merge_media(Client& c, TileState& t, const json& d, std::uint64_t now, Issues& is) {
        auto& m = t.media;
        set_text(d, "title", m.title, is); set_text(d, "subtitle", m.subtitle, is); set_text(d, "note", m.note, is);
        set_art(c, d, "art", m.art, is);
        set_int(d, "duration_ms", m.duration, 0, 360000000, is);
        set_bool(d, "playing", m.playing, is); set_bool(d, "loading", m.loading, is); set_bool(d, "shuffle", m.shuffle, is);
        if (const auto it = d.find("repeat"); it != d.end()) {
            const auto mode = it->is_string() ? it->get<std::string>() : std::string();
            if (mode == "off") m.repeat = 0; else if (mode == "all") m.repeat = 1; else if (mode == "one") m.repeat = 2;
            else is.add("bad_message", "repeat must be off, all or one");
        }
        if (const auto it = d.find("volume"); it != d.end()) {
            if (it->is_null()) m.volume = -1;
            else if (it->is_number()) m.volume = int(std::clamp(std::floor(it->get<double>()), 0.0, 100.0));
            else is.add("bad_message", "volume must be a number or null");
        }
        auto& tl = t.timeline;
        if (d.contains("position_ms") || d.contains("moving") || d.contains("changes")) {
            tl.settle(now);
            int position = tl.position_at(now);
            bool moving = tl.moving;
            set_int(d, "position_ms", position, 0, 2000000000, is);
            set_bool(d, "moving", moving, is);
            tl.at = now; tl.position = position; tl.moving = moving;
            if (const auto it = d.find("changes"); it != d.end()) {
                tl.changes.clear();
                if (it->is_array()) {
                    for (const auto& ch : *it) {
                        if (!ch.is_object()) continue;
                        if (int(tl.changes.size()) >= kBrokerChanges) { is.add("too_long", "at most 12 changes"); break; }
                        int in = 0, pos = 0; bool mv = false;
                        set_int(ch, "in_ms", in, 0, 86400000, is); set_int(ch, "position_ms", pos, 0, 2000000000, is);
                        set_bool(ch, "moving", mv, is);
                        tl.changes.push_back({now + std::uint64_t(in), pos, mv});
                    }
                } else if (!it->is_null()) is.add("bad_message", "changes must be a list");
            }
        }
        set_bool(d, "pending", tl.pending, is);
    }

    void merge_level(Client& c, ListLevel& l, const json& d, Issues& is) {
        if (d.is_null()) { l = ListLevel(); return; }
        if (!d.is_object()) return is.add("bad_message", "a level must be an object");
        set_text(d, "title", l.title, is); set_art(c, d, "art", l.art, is); set_text(d, "heading", l.heading, is);
        set_bool(d, "covers", l.covers, is); set_bool(d, "loading", l.loading, is);
        if (const auto it = d.find("empty"); it != d.end()) {
            if (it->is_null()) { l.empty.clear(); l.empty_detail.clear(); }
            else if (it->is_object()) { set_text(*it, "title", l.empty, is); set_text(*it, "detail", l.empty_detail, is); }
            else is.add("bad_message", "empty must be an object");
        }
        if (const auto it = d.find("rows"); it != d.end()) {
            l.rows.clear();
            if (it->is_array()) {
                for (const auto& r : *it) {
                    if (int(l.rows.size()) >= kBrokerRows) { is.add("too_long", "at most 500 rows"); break; }
                    const auto key = r.is_object() ? str(r, "key") : std::string();
                    if (key.empty() || key.size() > kBrokerText) { is.add("bad_message", "a row needs a key of 1 to 256 bytes"); continue; }
                    Row row;
                    row.key = key;
                    set_text(r, "title", row.title, is); set_text(r, "subtitle", row.subtitle, is);
                    set_art(c, r, "art", row.art, is);
                    set_bool(r, "opens", row.opens, is);
                    l.rows.push_back(std::move(row));
                }
            } else if (!it->is_null()) is.add("bad_message", "rows must be a list");
            if (!d.contains("loading")) l.loading = false; // rows arrived: loading is over unless said otherwise
        }
    }

    void merge_list(Client& c, TileState& t, const json& d, Issues& is) {
        auto& l = t.list;
        set_text(d, "busy", l.busy, is);set_text(d,"session",l.session,is);
        if (const auto it = d.find("top"); it != d.end()) merge_level(c, l.top, *it, is);
        if (const auto it = d.find("sub_key"); it != d.end()) {
            if (it->is_null()) { l.sub_key.clear(); l.sub = ListLevel(); }
            else set_text(d, "sub_key", l.sub_key, is);
        }
        if (const auto it = d.find("sub"); it != d.end()) merge_level(c, l.sub, *it, is);
    }

    void content(int id, const json& m) {
        auto& c = clients_.at(id);
        const auto tile = normal_tile_id(str(m, "tile"));
        const auto it = tiles_.find(tile);
        if (it == tiles_.end() || it->second.client != id) return error(id, "not_yours", "not a tile of this provider", clip_text(tile, 12));
        const auto d = m.find("data");
        if (d == m.end() || !d->is_object()) return error(id, "bad_message", "content needs data", tile);
        auto& t = it->second;
        if(t.card==Card::custom) {
            t.plugin_data=d->dump();t.dirty=true;
            if(s_.now()-t.written>=kBrokerUpdateMs)write(tile,t,s_.now());
            return;
        }
        static const char* media_keys[] = {"subtitle", "note", "art", "duration_ms", "position_ms", "moving", "changes", "pending",
                                           "playing", "loading", "shuffle", "repeat", "volume"};
        static const char* list_keys[] = {"busy", "top", "sub_key", "sub"};
        const bool has_media = std::any_of(std::begin(media_keys), std::end(media_keys), [&](const char* k) { return d->contains(k); });
        const bool has_list = std::any_of(std::begin(list_keys), std::end(list_keys), [&](const char* k) { return d->contains(k); });
        if ((t.card == Card::media && has_list) || (t.card == Card::list && has_media))
            return error(id, "bad_card", std::string("this is a ") + card_name(t.card) + " tile", tile);
        Issues issues;
        const auto now = s_.now();
        if (t.card == Card::media) merge_media(c, t, *d, now, issues); else merge_list(c, t, *d, issues);
        t.dirty = true;
        if (now - t.written >= kBrokerUpdateMs) write(tile, t, now);
        report(id, issues, tile);
    }

    // The tile's file, for the worker.
    void write(const std::string& tile, TileState& t, std::uint64_t now) {
        t.dirty = false;
        t.written = now;
        if(t.card==Card::custom) {
            s_.set_plugin(tile,t.plugin_address,t.plugin_data,t.plugin_error);
        } else if (t.card == Card::media) {
            Media m = t.media;
            auto& tl = t.timeline;
            m.points.clear();
            m.points.push_back({tl.at, tl.position, tl.moving});
            std::uint64_t until = tl.pending ? tl.at : 0;
            for (const auto& ch : tl.changes) {
                m.points.push_back({ch.when, ch.position, ch.moving});
                if (tl.pending) until = std::max(until, ch.when);
            }
            std::stable_sort(m.points.begin(), m.points.end(), [](const Point& a, const Point& b) { return a.at < b.at; });
            m.pending_until = until;
            s_.set_media(tile, m);
        } else {
            List l = t.list;
            if (l.sub_key.empty()) l.sub = ListLevel();
            s_.set_list(tile, l);
        }
    }

    void show(int id, const json& m) {
        auto& c = clients_.at(id);
        const auto tile = normal_tile_id(str(m, "tile"));
        const auto it = tiles_.find(tile);
        if (it == tiles_.end() || it->second.client != id) return error(id, "not_yours", "not a tile of this provider", clip_text(tile, 12));
        const auto now = s_.now();
        if (c.last_show && now - c.last_show < 200) return; // asking more often than that does nothing more
        c.last_show = now ? now : 1;
        s_.show(tile);
    }

    void status(int id, const json& m) {
        auto& c = clients_.at(id);
        Issues issues;
        set_text(m, "note", c.note, issues, 60);
        report(id, issues, "");
        if (c.provider == "librespot") s_.set_system_note(c.note.empty() ? "Connected" : c.note);
    }
};

} // namespace kabuki
