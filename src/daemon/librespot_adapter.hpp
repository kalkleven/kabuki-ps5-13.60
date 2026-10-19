// SPDX-License-Identifier: GPL-3.0-or-later
// librespot-ps5's tiles, until librespot-ps5 registers them itself.
//
// This stands in for librespot's own provider: while its control channel (127.0.0.1:38574,
// protocol/LIBRESPOT-CONTROL-V1.md) answers, it registers Now Playing, Queue and Library as
// template cards, turns librespot's lines into their data, and turns actions on them back into
// librespot's commands. Without librespot there are no Spotify tiles at all.
#pragma once
#include "service.hpp"
#include <cctype>

namespace kabuki {

class LibrespotAdapter {
public:
    static constexpr const char* kProvider = "librespot";
    static constexpr const char* kNowPlaying = "LSPNOW";
    static constexpr const char* kQueue = "LSPQUE";
    static constexpr const char* kLibrary = "LSPLIB";

    // `send`: one line to librespot's control channel.
    LibrespotAdapter(Service& service, std::function<void(const std::string&)> send)
        : s_(service), send_(std::move(send)) {}

    bool linked() const { return linked_; }
    void native_takeover(){if(linked_){s_.remove_provider(kProvider);linked_=false;}native_=true;}

    void connected() {
        if (linked_||native_) return;
        linked_ = true;
        Service::Provider p;
        p.action = [this](const std::string& tile, const std::string& action, const std::string& key, int value) { act(tile, action, key, value); };
        p.shown = [this](const std::string& tile) { if (tile == kLibrary) request_library(false); };
        s_.add_provider(kProvider, p);
        s_.register_tile({kNowPlaying, kProvider, Card::media, "Now Playing", "icon:iconid_music", 100});
        s_.register_tile({kQueue, kProvider, Card::list, "Queue", "icon:iconid_list", 100});
        s_.register_tile({kLibrary, kProvider, Card::list, "Library", "icon:iconid_app_library_media", 100});
        state_ = nlohmann::json(); library_ = nlohmann::json(); tracks_ = nlohmann::json();
        library_known_ = false; library_asked_ = 0; open_.clear(); busy_.clear(); frozen_ = nlohmann::json();
        publish_all();
        s_.set_system_note("Connected");
    }
    void disconnected() {
        if (!linked_) return;
        linked_ = false;
        s_.remove_provider(kProvider);
        s_.set_system_note("Not running");
    }

    // One line from librespot: its state, its library, or an open list's tracks.
    void line(const std::string& text) {
        if(native_)return;
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return;
        if (j.contains("library") && j["library"].is_object()) {
            library_ = std::move(j["library"]); library_known_ = true; publish_library();
        } else if (j.contains("tracks") && j["tracks"].is_object()) {
            tracks_ = std::move(j["tracks"]); publish_library();
        } else if (j.contains("state") && j["state"].is_object()) {
            state_ = std::move(j["state"]); state_at_ = s_.now();
            check_started();
            publish_media(); publish_queue();
        }
    }

    // Once in a while: a chosen song that never starts stops showing as starting.
    void tick() {
        if (!busy_.empty() && s_.now() - busy_since_ >= 15000) { clear_busy(); publish_queue(); publish_library(); }
    }

private:
    Service& s_;
    std::function<void(const std::string&)> send_;
    bool native_=false;
    bool linked_ = false, library_known_ = false, saw_pending_ = false;
    nlohmann::json state_, library_, tracks_, frozen_;
    std::uint64_t state_at_ = 0, library_asked_ = 0, busy_since_ = 0;
    std::string open_, busy_, busy_tile_, starting_title_, starting_artist_;

    static std::string str(const nlohmann::json& j, const char* key) {
        return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
    }
    template <class T> static T num(const nlohmann::json& j, const char* key, T fallback) {
        return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<T>() : fallback;
    }
    static bool flag(const nlohmann::json& j, const char* key) {
        return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
    }
    // Spotify URIs only: "spotify:" and plain characters.
    static bool plain_uri(const std::string& text) {
        if (text.compare(0, 8, "spotify:") || text.size() > 120) return false;
        for (const char c : text) if (!std::isalnum(static_cast<unsigned char>(c)) && c != ':' && c != '_' && c != '-') return false;
        return true;
    }
    void command(nlohmann::json j) { j["v"] = 1; send_(j.dump()); }

    void request_library(bool force) {
        const auto now = s_.now();
        if (!force && library_asked_ && now - library_asked_ < 60000 && str(library_, "error").empty()) return;
        library_asked_ = now;
        command({{"cmd", "library"}});
    }

    void act(const std::string& tile, const std::string& action, const std::string& key, int value) {
        if (tile == kNowPlaying) {
            if (action == "play_pause" || action == "previous" || action == "next") command({{"cmd", action}});
            else if (action == "volume") command({{"cmd", "volume"}, {"value", std::clamp(value, 0, 100)}});
            else if (action == "shuffle") command({{"cmd", "shuffle"}, {"on", value != 0}});
            else if (action == "repeat") command({{"cmd", "repeat"}, {"mode", value == 2 ? "one" : value == 1 ? "all" : "off"}});
            else if (action == "seek") command({{"cmd", "seek"}, {"ms", std::max(0, value)}});
        } else if (tile == kQueue && action == "select") {
            // Queue keys are "<index in librespot's queue>|<uri>".
            const auto bar = key.find('|');
            if (bar == std::string::npos) return;
            const auto uri = key.substr(bar + 1);
            if (!plain_uri(uri)) return;
            const auto& queue = state_.contains("queue") && state_["queue"].is_array() ? state_["queue"] : nlohmann::json::array();
            for (const auto& q : queue)
                if (str(q, "uri") == uri) { start_busy(kQueue, key, str(q, "title"), str(q, "artist")); break; }
            frozen_ = queue;
            command({{"cmd", "play_queued"}, {"uri", uri}, {"index", std::max(0, std::atoi(key.c_str()))}});
            publish_queue();
        } else if (tile == kLibrary && action == "open") {
            if (!plain_uri(key)) return;
            open_ = key; tracks_ = nlohmann::json();
            command({{"cmd", "open_list"}, {"uri", key}});
            publish_library();
        } else if (tile == kLibrary && action == "close") {
            open_.clear(); clear_busy(); publish_library();
        } else if (tile == kLibrary && action == "select") {
            if (!plain_uri(key) || open_.empty()) return;
            const auto& items = tracks_.contains("items") && tracks_["items"].is_array() ? tracks_["items"] : nlohmann::json::array();
            for (const auto& t : items)
                if (str(t, "uri") == key) { start_busy(kLibrary, key, str(t, "title"), str(t, "artist")); break; }
            command({{"cmd", "play"}, {"context", open_}, {"track", key}});
            publish_library();
        }
    }

    void start_busy(const char* tile, const std::string& key, std::string title, std::string artist) {
        busy_ = key; busy_tile_ = tile; busy_since_ = s_.now(); saw_pending_ = false;
        starting_title_ = std::move(title); starting_artist_ = std::move(artist);
    }
    void clear_busy() { busy_.clear(); busy_tile_.clear(); frozen_ = nlohmann::json(); }
    // The chosen song is heard: Now Playing is shown, as when it's chosen on the phone.
    void check_started() {
        if (busy_.empty()) return;
        saw_pending_ |= flag(state_, "pending");
        const bool heard = !flag(state_, "pending") && flag(state_, "playing") && str(state_, "title") == starting_title_ &&
                           str(state_, "artist") == starting_artist_ && (saw_pending_ || s_.now() - busy_since_ > 2500);
        if (!heard) return;
        clear_busy();
        s_.show(kNowPlaying);
        publish_library();
    }

    void publish_all() { publish_media(); publish_queue(); publish_library(); }

    void publish_media() {
        Media m;
        const auto& st = state_;
        if (st.is_object()) {
            m.title = str(st, "title"); m.subtitle = str(st, "artist"); m.art = str(st, "cover");
            m.duration = num(st, "duration_ms", 0); m.playing = flag(st, "playing"); m.loading = flag(st, "loading");
            m.shuffle = flag(st, "shuffle"); m.volume = std::clamp(num(st, "volume", 100), 0, 100);
            const auto mode = str(st, "repeat");
            m.repeat = mode == "one" ? 2 : mode == "all" ? 1 : 0;
            m.points.push_back({state_at_, num(st, "position_ms", 0), flag(st, "moving")});
            std::uint64_t until = flag(st, "pending") ? state_at_ : 0;
            if (st.contains("changes") && st["changes"].is_array())
                for (const auto& c : st["changes"]) {
                    if (!c.is_object() || m.points.size() >= 12) continue;
                    const auto when = state_at_ + num(c, "in_ms", std::uint64_t(0));
                    m.points.push_back({when, num(c, "position_ms", 0), flag(c, "moving")});
                    if (until) until = std::max(until, when);
                }
            std::stable_sort(m.points.begin(), m.points.end(), [](const Point& a, const Point& b) { return a.at < b.at; });
            m.pending_until = until;
            if (m.title.empty()) {
                m.title = flag(st, "connected") ? "Nothing playing" : "Waiting for Spotify";
                m.subtitle = "Pick this console in the Spotify app";
            } else if (st.contains("streaming") && !flag(st, "streaming")) {
                m.note = "Play Librespot Streamer in Music to hear it";
            }
        } else {
            m.title = "Connecting to librespot-ps5";
        }
        s_.set_media(kNowPlaying, m);
    }

    void publish_queue() {
        List l;
        // While a chosen song starts, the queue holds as it was, so the row doesn't move.
        const auto& queue = !frozen_.is_null() ? frozen_
                          : state_.is_object() && state_.contains("queue") && state_["queue"].is_array() ? state_["queue"] : nlohmann::json();
        l.top.loading = !queue.is_array();
        if (queue.is_array()) {
            int i = 0;
            for (const auto& q : queue) {
                if (!q.is_object() || l.top.rows.size() >= 20) continue;
                const int index = num(q, "index", -1);
                l.top.rows.push_back({std::to_string(index < 0 ? i : index) + "|" + str(q, "uri"), str(q, "title"), str(q, "artist"), str(q, "cover")});
                ++i;
            }
        }
        if (!l.top.rows.empty()) l.top.heading = "Up next";
        else { l.top.empty = "Nothing queued"; l.top.empty_detail = "Add songs from the Spotify app"; }
        if (busy_tile_ == kQueue) l.busy = busy_;
        s_.set_list(kQueue, l);
    }

    void publish_library() {
        List l;
        l.top.loading = !library_known_;
        const auto error = str(library_, "error");
        std::string open_name, open_cover;
        if (library_.contains("items") && library_["items"].is_array())
            for (const auto& item : library_["items"]) {
                if (!item.is_object()) continue;
                Row r{str(item, "uri"), str(item, "name"), str(item, "subtitle"), str(item, "cover"), true};
                if (r.key == open_) { open_name = r.title; open_cover = r.art; }
                l.top.rows.push_back(std::move(r));
            }
        if (library_known_ && l.top.rows.empty()) {
            l.top.empty = error.empty() ? "Nothing saved yet" : "Couldn't load your library";
            l.top.empty_detail = error.empty() ? "Save playlists and albums in Spotify" : error;
        } else l.top.heading = error;
        if (!open_.empty()) {
            l.sub_key = open_;
            l.sub.title = open_name; l.sub.art = open_cover; l.sub.covers = false;
            const bool known = str(tracks_, "uri") == open_ && tracks_.contains("items") && tracks_["items"].is_array();
            l.sub.loading = !known;
            if (known) {
                for (const auto& t : tracks_["items"])
                    if (t.is_object()) l.sub.rows.push_back({str(t, "uri"), str(t, "title"), str(t, "artist"), ""});
                const auto terror = str(tracks_, "error");
                if (l.sub.rows.empty()) {
                    l.sub.empty = terror.empty() ? "No songs here" : "Couldn't load these songs";
                    l.sub.empty_detail = terror;
                }
            }
        }
        if (busy_tile_ == kLibrary) l.busy = busy_;
        s_.set_list(kLibrary, l);
    }
};

} // namespace kabuki
