// SPDX-License-Identifier: GPL-3.0-or-later
// Template cards' data, as the daemon writes it for the worker (tile-<ID>.rec). Providers send
// the same fields as JSON (protocol/PROTOCOL.md); the daemon checks them and writes these.
#pragma once
#include "records.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace kabuki {

// When a position was heard, and whether it was moving on from there.
struct Point { std::uint64_t at = 0; int position = 0; bool moving = false; };

// `media`: a player. Times are milliseconds on the console's monotonic clock.
struct Media {
    std::string title, subtitle, note, art; // note: a line of advice under the subtitle
    int duration = 0;
    bool playing = false, loading = false, shuffle = false;
    int repeat = 0; // 0 off, 1 all, 2 one
    int volume = -1; // -1: no volume control
    std::vector<Point> points; // the heard position and the changes still to come, in time order
    std::uint64_t pending_until = 0; // something asked for isn't heard before this
};

struct Row { std::string key, title, subtitle, art; bool opens = false; };

// A list of rows; a row that `opens` shows a list of its own (`sub`), which Circle closes again.
struct ListLevel {
    std::string title, art, heading; // the header's title and art (empty: the tile's), a heading above the rows
    bool covers = true;              // rows have art
    bool loading = false;            // still loading: placeholder rows
    std::string empty, empty_detail; // shown when there are no rows
    std::vector<Row> rows;
};
struct List {
    std::string busy;      // a row whose action is under way: it shows the buffering light
    ListLevel top;
    std::string sub_key;   // the opened row, if any
    ListLevel sub;
    std::string session; // opaque provider generation; changes reset navigation
};

inline Records media_records(const Media& m) {
    Records r;
    r.add({"media"});
    r.add({"title", m.title}).add({"subtitle", m.subtitle}).add({"note", m.note}).add({"art", m.art});
    r.add({"duration", std::to_string(m.duration)}).add({"playing", m.playing ? "1" : "0"});
    r.add({"loading", m.loading ? "1" : "0"}).add({"shuffle", m.shuffle ? "1" : "0"});
    r.add({"repeat", std::to_string(m.repeat)}).add({"volume", std::to_string(m.volume)});
    r.add({"pending_until", std::to_string(m.pending_until)});
    for (const auto& p : m.points) r.add({"point", std::to_string(p.at), std::to_string(p.position), p.moving ? "1" : "0"});
    return r;
}

inline void level_records(Records& r, const ListLevel& l, const char* prefix) {
    const std::string p = prefix;
    r.add({p + "title", l.title}).add({p + "art", l.art}).add({p + "heading", l.heading});
    r.add({p + "covers", l.covers ? "1" : "0"}).add({p + "loading", l.loading ? "1" : "0"}).add({p + "empty", l.empty, l.empty_detail});
    for (const auto& row : l.rows) r.add({p + "row", row.key, row.title, row.subtitle, row.art, row.opens ? "1" : "0"});
}
inline Records list_records(const List& l) {
    Records r;
    r.add({"list"});
    r.add({"busy", l.busy}).add({"session",l.session});
    level_records(r, l.top, "");
    if (!l.sub_key.empty()) { r.add({"sub", l.sub_key}); level_records(r, l.sub, "sub"); }
    return r;
}

// What a tile's file holds: which template, and its data.
struct Content {
    std::uintptr_t plugin_address=0;
    std::string plugin_data="{}",plugin_error;
    bool media = false, list = false;
    Media m;
    List l;
};

inline bool level_record(ListLevel& l, const std::string& name, const Record& r) {
    if (name == "title") l.title = field(r, 1);
    else if (name == "art") l.art = field(r, 1);
    else if (name == "heading") l.heading = field(r, 1);
    else if (name == "covers") l.covers = number(r, 1) != 0;
    else if (name == "loading") l.loading = number(r, 1) != 0;
    else if (name == "empty") { l.empty = field(r, 1); l.empty_detail = field(r, 2); }
    else if (name == "row") {
        if (l.rows.size() < 500) l.rows.push_back({field(r, 1), field(r, 2), field(r, 3), field(r, 4), number(r, 5) != 0});
    } else return false;
    return true;
}

inline Content parse_content(const std::string& text) {
    Content c;
    for (const auto& r : parse_records(text)) {
        const auto& name = field(r, 0);
        if (name == "plugin") { c.plugin_address=std::uintptr_t(number(r,1)); c.plugin_data=field(r,2); c.plugin_error=field(r,3); continue; }
        if (name == "media") { c.media = true; continue; }
        if (name == "list") { c.list = true; continue; }
        if (c.list&&name=="session") { c.l.session=field(r,1); continue; }
        if (c.media) {
            auto& m = c.m;
            if (name == "title") m.title = field(r, 1);
            else if (name == "subtitle") m.subtitle = field(r, 1);
            else if (name == "note") m.note = field(r, 1);
            else if (name == "art") m.art = field(r, 1);
            else if (name == "duration") m.duration = int(number(r, 1));
            else if (name == "playing") m.playing = number(r, 1) != 0;
            else if (name == "loading") m.loading = number(r, 1) != 0;
            else if (name == "shuffle") m.shuffle = number(r, 1) != 0;
            else if (name == "repeat") m.repeat = int(number(r, 1));
            else if (name == "volume") m.volume = int(number(r, 1, -1));
            else if (name == "pending_until") m.pending_until = std::uint64_t(number(r, 1));
            else if (name == "point" && m.points.size() < 16)
                m.points.push_back({std::uint64_t(number(r, 1)), int(number(r, 2)), number(r, 3) != 0});
        } else if (c.list) {
            auto& l = c.l;
            if (name == "busy") l.busy = field(r, 1);
            else if (name == "sub") l.sub_key = field(r, 1);
            else if (name.compare(0, 3, "sub") == 0) level_record(l.sub, name.substr(3), r);
            else level_record(l.top, name, r);
        }
    }
    return c;
}

// A provider asking Kabuki to show one of its tiles (for example Now Playing once a chosen song
// starts). `seq` grows with each request; the worker acts on each one once.
struct Show { long long seq = 0; std::string tile; };
inline Show parse_show(const std::string& text) {
    Show s;
    for (const auto& r : parse_records(text))
        if (field(r, 0) == "show") { s.seq = number(r, 1); s.tile = field(r, 2); }
    return s;
}

} // namespace kabuki
