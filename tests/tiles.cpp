// SPDX-License-Identifier: GPL-3.0-or-later
// Host checks: tiles and their order, settings, the librespot adapter, the broker, and the panel's model.
#include "broker.hpp"
#include "librespot_adapter.hpp"
#include "native_model.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace kabuki;

int main() {
    // Records survive escaping.
    Records r; r.add({"row", "a\tb", "line\nbreak", "back\\slash"});
    const auto parsed = parse_records(r.text());
    assert(parsed.size() == 1 && parsed[0][1] == "a\tb" && parsed[0][2] == "line\nbreak" && parsed[0][3] == "back\\slash");
    assert(parse_records("nonsense\n").empty());

    // Without providers: System, Settings. With librespot: its three first, in its order.
    Settings s;
    assert((arrange(s, {}) == std::vector<std::string>{kSystemTile, kSettingsTile}));
    std::vector<TileInfo> lsp{{"LSPNOW", "librespot", Card::media, "Now Playing", "", 100, 1},
                              {"LSPQUE", "librespot", Card::list, "Queue", "", 100, 2},
                              {"LSPLIB", "librespot", Card::list, "Library", "", 100, 3}};
    assert((arrange(s, lsp) == std::vector<std::string>{"LSPNOW", "LSPQUE", "LSPLIB", kSystemTile, kSettingsTile}));
    Settings music_hidden; music_hidden.librespot_tiles = false;
    assert((arrange(music_hidden, lsp) == std::vector<std::string>{kSystemTile, kSettingsTile}));
    music_hidden.arranged = true;
    music_hidden.slots[0] = "LSPNOW"; music_hidden.slots[1] = kSystemTile; music_hidden.slots[2] = kSettingsTile;
    assert((arrange(music_hidden, lsp) == std::vector<std::string>{kSystemTile, kSettingsTile}));
    assert(!parse_settings(settings_records(music_hidden).text()).librespot_tiles);

    // The old "nqlys.." settings, and offline tiles keeping their slots.
    assert(migrate_codes(s, "sqlyn.."));
    assert(s.slots[0] == kSettingsTile && s.slots[4] == "LSPNOW");
    assert((arrange(s, {}) == std::vector<std::string>{kSettingsTile, kSystemTile}));
    assert(arrange(s, lsp).size() == 5 && arrange(s, lsp)[4] == "LSPNOW");
    assert(!migrate_codes(s, "nqly..."));  // no Settings: refused

    // An arrangement without Settings never takes effect.
    auto bad = parse_settings("kabuki\t1\nslots\tLSPNOW\t\t\t\t\t\t\n");
    assert(!bad.arranged);

    // A new tile goes in before Kabuki's own, which move down.
    Settings a; a.arranged = true;
    std::string base[kSlots] = {"LSPNOW", kSystemTile, kSettingsTile, "", "", "", ""};
    for (int i = 0; i < kSlots; ++i) a.slots[i] = base[i];
    a.known = {"LSPNOW"};
    assert(learn_tiles(a, {{"HELLO1", "hello", Card::list, "Hello", "", 500, 4}}));
    assert(a.slots[1] == "HELLO1" && a.slots[2] == kSystemTile && a.slots[3] == kSettingsTile);

    // The daemon's service and adapter: tiles exist only while librespot answers.
    const auto dir = (std::filesystem::temp_directory_path() / "kabuki-tests").string();
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    std::uint64_t now = 1000;
    Service service(dir, [&] { return now; });
    service.load_settings();
    service.command(kSaveSettings, 0, "chord=6 hold=500 sounds=1 music=0");
    assert(!service.settings().librespot_tiles);
    service.command(kSaveSettings, 0, "chord=6 hold=750 sounds=0"); // an older UI keeps this preference
    assert(!service.settings().librespot_tiles && service.settings().hold_ms == 750 && !service.settings().sounds);
    service.command(kSaveSettings, 0, "chord=6 hold=500 sounds=1 music=1");
    assert(service.settings().librespot_tiles && service.settings().sounds);
    std::vector<std::string> sent;
    LibrespotAdapter adapter(service, [&](const std::string& line) { sent.push_back(line); });
    assert(parse_tiles(read_file(dir + "/" + kTilesFile)).empty());
    adapter.connected();
    assert(parse_tiles(read_file(dir + "/" + kTilesFile)).size() == 3);
    adapter.line(R"({"v":1,"state":{"title":"Song","artist":"Artist","playing":true,"duration_ms":1000,"queue":[{"title":"Next","artist":"A","uri":"spotify:track:x","index":7}]}})");
    const auto np = parse_content(read_file(dir + "/" + content_file("LSPNOW")));
    assert(np.media && np.m.title == "Song" && np.m.playing);
    const auto q = parse_content(read_file(dir + "/" + content_file("LSPQUE")));
    assert(q.list && q.l.top.rows.size() == 1 && q.l.top.rows[0].key == "7|spotify:track:x");
    // A queued song chosen: librespot's own command, with its original index.
    service.command(kTileAction, 0, "LSPQUE\tselect\t7|spotify:track:x");
    assert(!sent.empty() && sent.back().find("play_queued") != std::string::npos && sent.back().find("\"index\":7") != std::string::npos);
    // Settings can't be saved away.
    service.command(kSaveSlots, 0, "LSPNOW,LSPQUE,,,,,");
    assert(!service.settings().arranged);
    adapter.disconnected();
    assert(parse_tiles(read_file(dir + "/" + kTilesFile)).empty());

    // Settings exposes live Librespot status, a saved tile-visibility switch, and a stop action.
    const auto ui_dir = dir + "/ui-settings";
    std::filesystem::create_directories(ui_dir);
    Settings ui_defaults;
    write_file_atomic(ui_dir + "/" + kSettingsFile, settings_records(ui_defaults).text());
    write_file_atomic(ui_dir + "/" + kTilesFile, Records{}.text());
    std::vector<std::tuple<int, int, std::string>> ui_commands;
    overlay::NativeModel settings_model([&](int k, int v, const char* t) { ui_commands.emplace_back(k, v, t ? t : ""); });
    settings_model.data_dir(ui_dir);
    auto ui_now = now + 1000;
    settings_model.tick(ui_now, 99, 0, 0, false);                 // opens on System
    settings_model.tick(++ui_now, 99, 0x40, 0x40, false);        // move to Settings
    settings_model.tick(++ui_now, 99, 0x4000, 0x4000, false);    // enter Settings
    for (int i = 0; i < 4; ++i) settings_model.tick(++ui_now, 99, 0, 0x40, false);
    assert(settings_model.view().setting == 4);
    settings_model.tick(++ui_now, 99, 0x4000, 0x4000, false);    // open Librespot page
    assert(settings_model.view().page == 2 && settings_model.view().header == "Librespot");
    settings_model.tick(++ui_now, 99, 0x40, 0x40, false);        // focus Show music tiles
    settings_model.tick(++ui_now, 99, 0x4000, 0x4000, false);    // hide the tiles
    assert(!settings_model.view().settings.librespot_tiles);
    assert(std::get<0>(ui_commands.back()) == kSaveSettings && std::get<2>(ui_commands.back()).find("music=0") != std::string::npos);
    assert((settings_model.view().rail.size() == 2));
    settings_model.tick(++ui_now, 99, 0x40, 0x40, false);        // focus Stop Librespot
    settings_model.tick(++ui_now, 99, 0x4000, 0x4000, false);
    assert(std::get<0>(ui_commands.back()) == kTerminateLibrespot);
    Records system_record; system_record.add({"info", "librespot", "Connected"});
    write_file_atomic(ui_dir + "/system.rec", system_record.text());
    settings_model.poll_files(ui_now + 500);
    assert(settings_model.view().system.librespot == "Connected");

    // The panel's model on those files: opens on the first tile; without librespot, System.
    std::vector<std::tuple<int, int, std::string>> commands;
    overlay::NativeModel model([&](int k, int v, const char* t) { commands.emplace_back(k, v, t ? t : ""); });
    model.data_dir(dir);
    adapter.connected();
    adapter.line(R"({"v":1,"state":{"title":"Song","artist":"Artist","playing":true,"duration_ms":1000}})");
    model.poll_files(now); model.tick(now, 1, 0, 0, false);
    assert(model.view().tile == "LSPNOW" && model.view().focused && model.view().rail.size() == 5);
    now += 50; model.tick(now, 1, 0x8000, 0x8000, false); // Square: play/pause, to librespot's tile
    assert(std::get<0>(commands.back()) == kTileAction && std::get<2>(commands.back()) == "LSPNOW\tplay_pause\t");
    // A provider/account restart must cancel a pending sublist in the UI too.
    List library;library.session="account-1";library.top.rows.push_back({"list-a","List A","","",true});
    service.set_list("LSPLIB",library);now+=1000;model.poll_files(now);
    model.tick(++now,1,0,0,true);
    model.tick(++now,1,0,0x40,false);model.tick(++now,1,0,0x40,false);
    model.tick(++now,1,0,0x4000,false);model.tick(++now,1,0,0x4000,false);
    assert(model.view().tile=="LSPLIB"&&model.view().list.level==1&&model.view().list.loading);
    library.top.rows[0].title="Updated";service.set_list("LSPLIB",library);
    now+=1000;model.poll_files(now);assert(model.view().list.level==1&&model.view().list.loading);
    library.session="account-2";library.top.rows[0].key="list-b";service.set_list("LSPLIB",library);
    now+=1000;model.poll_files(now);
    assert(model.view().list.level==0&&!model.view().list.loading&&model.view().list.rows.size()==1);
    assert(parse_content(read_file(dir+"/"+content_file("LSPLIB"))).l.session=="account-2");
    adapter.disconnected();
    now += 1000; model.poll_files(now); model.tick(now, 2, 0, 0, false);
    assert(model.view().tile == kSystemTile && model.view().rail.size() == 2);
    std::filesystem::remove_all(dir);

    // ---- The broker: other programs' tiles over the provider protocol ----
    {
    const auto bdir = (std::filesystem::temp_directory_path() / "kabuki-broker-tests").string();
    std::filesystem::remove_all(bdir); std::filesystem::create_directories(bdir);
    std::uint64_t bnow = 5000;
    Service bservice(bdir, [&] { return bnow; });
    bservice.load_settings();
    std::map<int, std::vector<nlohmann::json>> heard; // what each client was sent
    std::vector<std::string> checked_art;
    Broker::Hooks hooks;
    hooks.send = [&](int client, const std::string& line) { heard[client].push_back(nlohmann::json::parse(line)); };
    hooks.art_ok = [&](const std::string&, const std::string& path) { checked_art.push_back(path); return path.find("evil") == std::string::npos; };
    Broker broker(bservice, hooks, "/data/test-art", "0.2.0");
    using nlohmann::json;
    const auto say = [&](int client, json message) {
        if (!message.contains("v")) message["v"] = 1;
        broker.data(client, message.dump() + "\n");
    };
    const auto last = [&](int client) { return heard[client].empty() ? json() : heard[client].back(); };
    const auto codes = [&](int client) { // the error codes a client was sent, then forgotten
        std::vector<std::string> out;
        for (const auto& m : heard[client]) if (m.contains("error")) out.push_back(m["error"]["code"]);
        heard[client].clear();
        return out;
    };
    const auto online = [&](const std::string& id) {
        for (const auto& t : parse_tiles(read_file(bdir + "/" + kTilesFile))) if (t.id == id) return true;
        return false;
    };
    const auto tile = [&](const std::string& id, const char* card) { return json{{"id", id}, {"title", "Tile " + id}, {"card", card}}; };

    // A good hello: welcomed, and the tile lands before System and Settings.
    const int a = broker.open();
    say(a, {{"hello", {{"provider", "example"}, {"tiles", {tile("hello1", "list")}}}}});
    assert(heard[a].size() == 1 && heard[a][0].contains("welcome") && heard[a][0]["welcome"]["versions"][0] == 1);
    assert(heard[a][0]["welcome"]["art_dir"] == "/data/test-art/example");
    assert(online("HELLO1") && bservice.owns("example", "HELLO1"));
    assert((arrange(bservice.settings(), bservice.tiles()) == std::vector<std::string>{"HELLO1", kSystemTile, kSettingsTile}));
    heard[a].clear();

    // Bad ids, reserved ids and plugin paths are refused one by one; the rest of the hello stands.
    const int b = broker.open();
    say(b, {{"hello", {{"provider", "other"}, {"tiles", {tile("ab", "list"), tile("KBKFAKE", "list"), tile("KBKSET", "list"),
                                                         tile("PLUGIN", "plugin"), tile("WEIRD!", "list"), tile("OTHER1", "media")}}}}});
    assert((codes(b) == std::vector<std::string>{"bad_tile_id", "bad_tile_id", "reserved_id", "bad_plugin", "bad_tile_id"}));
    assert(online("OTHER1") && !online("PLUGIN") && !online("KBKSET") && bservice.owns("other", "OTHER1"));
    assert(bservice.settings().slot_of(kSettingsTile) < 0 || !bservice.settings().arranged); // Settings is still Kabuki's

    // A stolen id, and content or show for another provider's tile.
    say(b, {{"hello", {{"provider", "thief"}, {"tiles", {}}}}}); // hello twice
    assert(codes(b) == std::vector<std::string>{"bad_message"});
    const int c = broker.open();
    say(c, {{"hello", {{"provider", "thief"}, {"tiles", {tile("HELLO1", "list"), tile("THIEF1", "list")}}}}});
    assert(codes(c) == std::vector<std::string>{"tile_taken"});
    assert(bservice.owns("example", "HELLO1") && bservice.owns("thief", "THIEF1"));
    say(c, {{"content", {{"tile", "HELLO1"}, {"data", {{"top", {{"heading", "mine now"}}}}}}}});
    say(c, {{"show", {{"tile", "HELLO1"}}}});
    assert((codes(c) == std::vector<std::string>{"not_yours", "not_yours"}));
    assert(parse_content(read_file(bdir + "/" + content_file("HELLO1"))).l.top.heading.empty());

    // Messages before hello, bad JSON, a wrong version, a provider name already connected.
    const int d = broker.open();
    say(d, {{"content", {{"tile", "HELLO1"}, {"data", {}}}}});
    broker.data(d, "this is not json\n");
    assert((codes(d) == std::vector<std::string>{"no_hello", "bad_json"}));
    say(d, {{"hello", {{"provider", "example"}, {"tiles", {tile("DUPES1", "list")}}}}});
    assert(codes(d) == std::vector<std::string>{"bad_provider"} && !broker.alive(d) && !online("DUPES1"));
    assert(bservice.owns("example", "HELLO1")); // the first keeps its tiles
    broker.closed(d);
    const int e = broker.open();
    say(e, {{"v", 2}, {"hello", {{"provider", "future"}, {"tiles", {tile("FUTURE", "list")}}}}});
    assert(last(e)["error"]["code"] == "unsupported_version" && last(e)["error"]["versions"][0] == 1 && !broker.alive(e));
    broker.closed(e);

    // librespot's adapter and a provider of the same name can't both be there.
    {
        LibrespotAdapter lsp(bservice, [](const std::string&) {});
        lsp.connected();
        const int f = broker.open();
        say(f, {{"hello", {{"provider", "librespot"}, {"tiles", {tile("LSPNOW", "media")}}}}});
        assert(codes(f) == std::vector<std::string>{"bad_provider"} && !broker.alive(f));
        broker.closed(f);
        assert(bservice.owns("librespot", "LSPNOW"));
        lsp.disconnected();
    }

    // Content: a list, then actions coming back to the provider.
    bnow += 200;
    say(a, {{"content", {{"tile", "hello1"}, {"data", {{"session", "generation-2"}, {"top", {{"title", "Counter"}, {"covers", false},
        {"rows", {{{"key", "n"}, {"title", "Count: 0"}}, {{"key", "reset"}, {"title", "Reset"}}}}}}}}}}});
    assert(codes(a).empty());
    auto shown = parse_content(read_file(bdir + "/" + content_file("HELLO1")));
    assert(shown.list && shown.l.session=="generation-2" && shown.l.top.title == "Counter" && !shown.l.top.covers && shown.l.top.rows.size() == 2 &&
           shown.l.top.rows[1].key == "reset" && !shown.l.top.loading);
    bservice.command(kTileAction, 0, "HELLO1\tselect\tn");
    assert(last(a)["action"]["tile"] == "HELLO1" && last(a)["action"]["action"] == "select" && last(a)["action"]["key"] == "n" &&
           !last(a)["action"].contains("value"));
    bservice.command(kTileShown, 0, "HELLO1");
    assert(last(a)["shown"]["tile"] == "HELLO1");
    bservice.command(kTileAction, 0, "THIEF1\tselect\tx"); // only the owner hears it
    assert(last(a)["shown"]["tile"] == "HELLO1");
    // A row without a key is skipped; too many rows are cut.
    json many = json::array();
    for (int i = 0; i < 510; ++i) many.push_back({{"key", "r" + std::to_string(i)}, {"title", "Row"}});
    bnow += 200;
    say(a, {{"content", {{"tile", "HELLO1"}, {"data", {{"top", {{"rows", many}}}}}}}});
    assert(codes(a) == std::vector<std::string>{"too_long"});
    assert(parse_content(read_file(bdir + "/" + content_file("HELLO1"))).l.top.rows.size() == 500);
    // Long text is cut at a character boundary.
    bnow += 200;
    say(a, {{"content", {{"tile", "HELLO1"}, {"data", {{"top", {{"heading", std::string(255, 'x') + "\xC3\xA9\xC3\xA9"}}}}}}}});
    assert(codes(a) == std::vector<std::string>{"too_long"});
    assert(parse_content(read_file(bdir + "/" + content_file("HELLO1"))).l.top.heading.size() == 255);
    // A media payload for a list tile is the wrong card.
    say(a, {{"content", {{"tile", "HELLO1"}, {"data", {{"position_ms", 5}}}}}});
    assert(codes(a) == std::vector<std::string>{"bad_card"});

    // Media: the position merges, keeps its timeline, and updates are held to 10 a second.
    bnow += 200;
    say(b, {{"content", {{"tile", "OTHER1"}, {"data", {{"title", "Song"}, {"subtitle", "Artist"}, {"duration_ms", 1000},
        {"position_ms", 100}, {"moving", true}, {"playing", true}, {"volume", 40}, {"repeat", "one"}}}}}});
    assert(codes(b).empty());
    auto media = parse_content(read_file(bdir + "/" + content_file("OTHER1")));
    assert(media.media && media.m.title == "Song" && media.m.volume == 40 && media.m.repeat == 2 && media.m.points.size() == 1 &&
           media.m.points[0].at == bnow && media.m.points[0].position == 100 && media.m.points[0].moving);
    bnow += 30; // too soon after the last write: held
    say(b, {{"content", {{"tile", "OTHER1"}, {"data", {{"position_ms", 5000}}}}}});
    assert(parse_content(read_file(bdir + "/" + content_file("OTHER1"))).m.points[0].position == 100);
    broker.tick();
    assert(parse_content(read_file(bdir + "/" + content_file("OTHER1"))).m.points[0].position == 100);
    bnow += 100; broker.tick(); // due now
    media = parse_content(read_file(bdir + "/" + content_file("OTHER1")));
    assert(media.m.title == "Song" && media.m.points[0].position == 5000 && media.m.points[0].moving); // title and moving kept
    // A change still to come, and a seek that isn't heard yet.
    bnow += 200;
    say(b, {{"content", {{"tile", "OTHER1"}, {"data", {{"position_ms", 9000}, {"moving", false}, {"pending", true},
        {"changes", {{{"in_ms", 900}, {"position_ms", 9000}, {"moving", true}}}}}}}}});
    media = parse_content(read_file(bdir + "/" + content_file("OTHER1")));
    assert(media.m.points.size() == 2 && media.m.points[1].at == bnow + 900 && media.m.pending_until == bnow + 900);
    bservice.command(kTileAction, 61000, "OTHER1\tseek\t");
    assert(last(b)["action"]["action"] == "seek" && last(b)["action"]["value"] == 61000 && !last(b)["action"].contains("key"));
    say(b, {{"show", {{"tile", "OTHER1"}}}});
    assert(parse_show(read_file(bdir + "/" + kShowFile)).tile == "OTHER1");

    // Pictures only inside the provider's own folders.
    const int g = broker.open();
    say(g, {{"hello", {{"provider", "artist"}, {"art_dirs", {"/data/art/mine", "/etc/elsewhere", "/data/../x"}},
                       {"tiles", {tile("ARTIST", "media")}}}}});
    assert((codes(g) == std::vector<std::string>{"bad_art", "bad_art"}));
    heard[g].clear();
    const std::string good = "/data/art/mine/cover.PNG";
    for (const std::string& path : {std::string("/data/art/mine/../other.png"), std::string("/data/art/other/c.png"), std::string("/data/art/mine/c.gif"),
                                    std::string("/data/art/mine/evil.png"), std::string("art/mine/c.png")}) {
        bnow += 200;
        say(g, {{"content", {{"tile", "ARTIST"}, {"data", {{"art", path}}}}}});
        assert(codes(g) == std::vector<std::string>{"bad_art"});
        assert(parse_content(read_file(bdir + "/" + content_file("ARTIST"))).m.art.empty());
    }
    bnow += 200;
    say(g, {{"content", {{"tile", "ARTIST"}, {"data", {{"art", good}}}}}});
    assert(codes(g).empty() && parse_content(read_file(bdir + "/" + content_file("ARTIST"))).m.art == good);
    assert(std::count(checked_art.begin(), checked_art.end(), good) == 1);
    broker.closed(g);

    // A provider going away takes its tiles off the rail; a new one lands before Kabuki's own.
    broker.closed(c);
    assert(!online("THIEF1"));
    broker.closed(b);
    assert(!online("OTHER1") && !std::filesystem::exists(bdir + "/" + content_file("OTHER1")));
    assert(online("HELLO1"));
    bservice.command(kSaveSlots, 0, "HELLO1,KBKSYS,KBKSET,,,,");
    const int h = broker.open();
    say(h, {{"hello", {{"provider", "later"}, {"tiles", {tile("LATER1", "list")}}}}});
    assert(bservice.settings().slots[0] == "HELLO1" && bservice.settings().slots[1] == "LATER1" &&
           bservice.settings().slots[2] == kSystemTile && bservice.settings().slots[3] == kSettingsTile);
    broker.closed(h);
    assert(bservice.settings().slot_of("LATER1") == 1); // its place is kept for when it's back

    // Oversize: a line without a newline, and a line with one.
    const int big = broker.open();
    say(big, {{"hello", {{"provider", "big"}, {"tiles", {tile("BIGONE", "list")}}}}});
    assert(online("BIGONE"));
    broker.data(big, std::string(70000, 'x'));
    assert(!broker.alive(big) && !online("BIGONE") && !bservice.has_provider("big"));
    broker.closed(big);
    const int big2 = broker.open();
    say(big2, {{"hello", {{"provider", "big"}, {"tiles", {tile("BIGONE", "list")}}}}});
    broker.data(big2, std::string(65600, 'y') + "\n");
    assert(!broker.alive(big2) && !online("BIGONE"));
    broker.closed(big2);

    // A client that keeps sending refused messages is dropped, and so is one that floods.
    const int noisy = broker.open();
    say(noisy, {{"hello", {{"provider", "noisy"}, {"tiles", {tile("NOISY1", "list")}}}}});
    for (int i = 0; i < 25 && broker.alive(noisy); ++i) say(noisy, {{"mystery", {}}});
    assert(!broker.alive(noisy) && !online("NOISY1"));
    broker.closed(noisy);
    bnow += 20000;
    const int flood = broker.open();
    say(flood, {{"hello", {{"provider", "flood"}, {"tiles", {tile("FLOOD1", "list")}}}}});
    for (int i = 0; i < 250 && broker.alive(flood); ++i)
        say(flood, {{"content", {{"tile", "FLOOD1"}, {"data", {{"top", {{"loading", true}}}}}}}});
    assert(!broker.alive(flood) && !online("FLOOD1"));
    broker.closed(flood);

    // At most 8 clients at once.
    std::vector<int> crowd;
    for (int i = 0; i < 20; ++i) { const int id = broker.open(); if (id >= 0) crowd.push_back(id); }
    assert(broker.clients() == kBrokerClients);
    assert(broker.open() < 0);
    for (const int id : crowd) broker.closed(id);
    assert(broker.open() >= 0);

    // The service still has Settings and System, whatever happened.
    assert(!bservice.owns("example", kSettingsTile) && !online(kSettingsTile));
    std::filesystem::remove_all(bdir);
    }
    std::cout << "Kabuki tile checks passed\n";
}
