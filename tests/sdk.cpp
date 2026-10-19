// SPDX-License-Identifier: GPL-3.0-or-later
// The C SDK (sdk/c/kabuki.h) against the real broker over a socket pair: hello, content, and an
// action coming back, with the broker on its own thread as the daemon's loop would run it.
#include "broker.hpp"
#include "kabuki.h"
#include <sys/socket.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using namespace kabuki;

int main() {
    const auto dir = (std::filesystem::temp_directory_path() / "kabuki-sdk-tests").string();
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    const auto clock = [] {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    Service service(dir, clock);
    service.load_settings();
    std::string out; // what the broker says to the one client
    Broker::Hooks hooks;
    hooks.send = [&](int, const std::string& line) { out += line + "\n"; };
    Broker broker(service, hooks, "/data/test-art", "0.1.0");
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const int id = broker.open();

    // The daemon's side: read, hand to the broker, write back; jobs run here so the service has one thread.
    std::atomic<bool> stop{false};
    std::mutex lock;
    std::vector<std::function<void()>> jobs;
    std::thread daemon([&] {
        while (!stop) {
            struct pollfd p = {pair[1], POLLIN, 0};
            if (poll(&p, 1, 10) > 0) {
                char bytes[4096];
                const ssize_t n = recv(pair[1], bytes, sizeof bytes, 0);
                if (n > 0) broker.data(id, bytes, std::size_t(n));
            }
            broker.tick();
            while (!out.empty()) {
                const ssize_t n = send(pair[1], out.data(), out.size(), 0);
                if (n <= 0) break;
                out.erase(0, std::size_t(n));
            }
            std::vector<std::function<void()>> now;
            { std::lock_guard<std::mutex> g(lock); now.swap(jobs); }
            for (auto& job : now) job();
        }
    });
    const auto run = [&](std::function<void()> job) {
        std::promise<void> done;
        { std::lock_guard<std::mutex> g(lock); jobs.push_back([&, job] { job(); done.set_value(); }); }
        done.get_future().wait();
    };

    kabuki_client k;
    kabuki_attach(&k, pair[0]);
    const ::kabuki_tile tiles[] = {{"sdktst", "SDK \"test\"", "icon:iconid_music", "media", 100}, {"SDKLST", "List", NULL, "list", 0}};
    assert(kabuki_hello(&k, "sdk-test", tiles, 2, NULL, 2000) == 0);
    run([&] { assert(service.owns("sdk-test", "SDKTST") && service.owns("sdk-test", "SDKLST")); });

    // Content; the broker holds updates to 10 a second, so give it time to write.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(kabuki_content(&k, "SDKTST", "{\"title\":\"Caf\u00e9 \\\"Song\\\"\",\"duration_ms\":1000,\"position_ms\":250,\"moving\":true,\"volume\":30}") == 0);
    Content got;
    for (int i = 0; i < 100 && got.m.title.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        got = parse_content(read_file(dir + "/" + content_file("SDKTST")));
    }
    assert(got.media && got.m.title == "Caf\xC3\xA9 \"Song\"" && got.m.volume == 30 && got.m.points.size() == 1 && got.m.points[0].position == 250);

    // Actions: a seek carries its value; a key with quotes and a backslash survives both ways.
    kabuki_event ev;
    run([&] { service.command(kTileAction, 61000, "SDKTST\tseek\t"); });
    assert(kabuki_poll(&k, &ev, 2000) == 1 && ev.kind == KABUKI_ACTION && !strcmp(ev.tile, "SDKTST") && !strcmp(ev.action, "seek") &&
           ev.has_value && ev.value == 61000 && ev.key[0] == 0);
    run([&] { service.command(kTileAction, 0, "SDKLST\tselect\ta\"b\\c d"); });
    assert(kabuki_poll(&k, &ev, 2000) == 1 && ev.kind == KABUKI_ACTION && !strcmp(ev.action, "select") && !strcmp(ev.key, "a\"b\\c d") && !ev.has_value);
    run([&] { service.command(kTileShown, 0, "SDKLST"); });
    assert(kabuki_poll(&k, &ev, 2000) == 1 && ev.kind == KABUKI_SHOWN && !strcmp(ev.tile, "SDKLST"));

    // A refusal arrives as an error event, and show works.
    assert(kabuki_content(&k, "NOPE00", "{}") == 0);
    assert(kabuki_poll(&k, &ev, 2000) == 1 && ev.kind == KABUKI_ERROR && !strcmp(ev.code, "not_yours"));
    assert(kabuki_show(&k, "SDKTST") == 0 && kabuki_status(&k, "Counting") == 0);
    for (int i = 0; i < 100 && parse_show(read_file(dir + "/" + kShowFile)).tile.empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(parse_show(read_file(dir + "/" + kShowFile)).tile == "SDKTST");

    // Nothing more to hear; then the daemon closes and the SDK notices.
    assert(kabuki_poll(&k, &ev, 50) == 0);
    stop = true;
    daemon.join();
    close(pair[1]);
    assert(kabuki_poll(&k, &ev, 1000) < 0);
    kabuki_close(&k);
    std::filesystem::remove_all(dir);
    std::cout << "Kabuki SDK checks passed\n";
}
