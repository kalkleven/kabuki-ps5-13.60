// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_layout.hpp"
#include <cassert>
#include <iostream>

// Record the real layout, including visibility inherited from parents.
struct Backend {
    struct Widget { int id = -1; };
    struct Node { int parent = -1; bool visible = true; float alpha = 1; std::string text; };
    std::vector<Node> nodes{Node{}};
    Widget root{0};
    Widget& screen() { return root; }
    bool add(Widget& w, Widget& parent) {
        w.id = int(nodes.size()); nodes.push_back({parent.id, true, 1, {}}); return true;
    }
    bool panel(Widget& w, Widget& p, overlay::native::Box, bool) { return add(w,p); }
    bool skin(Widget& w, Widget& p, overlay::native::Box, int, float) { return add(w,p); }
    bool image(Widget& w, Widget& p, overlay::native::Box, const std::string&) { return add(w,p); }
    bool label(Widget& w, Widget& p, overlay::native::Box, float, int, overlay::native::Rgb, int, int, const std::string& s) {
        add(w,p); words(w,s); return true;
    }
    void geometry(Widget&, overlay::native::Box) {}
    void show(Widget& w, bool value) { nodes.at(w.id).visible = value; }
    void alpha(Widget& w, float value) { nodes.at(w.id).alpha = value; }
    void words(Widget& w, const std::string& value) { nodes.at(w.id).text = value; }
    void tint(Widget&, overlay::native::Rgb, float) {}
    void picture(Widget&, const std::string&) {}
    void scale(Widget&, float) {}
    float text_width(Widget&, const std::string& s) { return float(s.size()) * 12; }
    std::string icon(const std::string& s) { return s; }
    std::string fallback_art() { return {}; }
    bool focus(const overlay::native::Focus&, int) { return false; }
    bool visible(const std::string& text) const {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].text != text) continue;
            int at = int(i);
            while (at >= 0 && nodes[at].visible && nodes[at].alpha > 0.001f) at = nodes[at].parent;
            if (at < 0) return true;
        }
        return false;
    }
};

int main() {
    Backend backend;
    overlay::native::Layout<Backend> layout(backend);
    assert(layout.create());
    overlay::NativeView view;
    view.opening = 1;
    std::uint64_t now = 1000000;
    const auto settle = [&] {
        for (int i = 0; i < 50; ++i) { layout.frame(view, true, now, view.opening); now += 20000; }
    };
    const auto system_labels = [&](bool visible) {
        for (const char* label : {"Performance", "Console", "CPU", "Memory", "Frame rate", "Temperature",
                                  "Firmware", "Uptime", "IP address", "librespot-ps5"})
            assert(backend.visible(label) == visible);
    };
    view.tile = "LSPNOW"; view.tab = overlay::kMedia; view.header = "Now Playing";
    settle();
    system_labels(false);
    view.tile = "KBKSYS"; view.tab = overlay::kStats; view.header = "System";
    view.system.cpu = 25; view.system.firmware = "13.20";
    settle();
    system_labels(true);
    assert(backend.visible("25%"));
    assert(backend.visible("13.20"));
    assert(!backend.visible("Shuffle"));
    view.system.cpu = -1;
    settle();
    system_labels(true);
    assert(!backend.visible("25%"));
    view.tile = "KBKSET"; view.tab = overlay::kSettings; view.header = "Settings";
    settle();
    system_labels(false);
    assert(backend.visible("Open shortcut"));
    assert(backend.visible("Hold to open"));
    assert(backend.visible("Librespot"));
    view.page = 2; view.setting = 1; view.system.librespot = "Connected";
    view.settings.librespot_tiles = true;
    settle();
    assert(backend.visible("Connection"));
    assert(backend.visible("Connected"));
    assert(backend.visible("Show music tiles"));
    assert(backend.visible("Stop Librespot"));
    assert(backend.visible("Stop"));
    assert(backend.visible("Cross stops librespot-ps5.elf; configure the receiver when launching it."));
    assert(!backend.visible("Open shortcut"));
    view.page = 0;
    settle();
    assert(backend.visible("Librespot"));
    view.tile = "LSPNOW"; view.tab = overlay::kMedia; view.header = "Now Playing";
    settle();
    system_labels(false);
    assert(!backend.visible("Open shortcut"));
    // Also cover opening directly on System without a provider's media card.
    Backend standalone;
    overlay::native::Layout<Backend> direct(standalone);
    assert(direct.create());
    view.tile = "KBKSYS"; view.tab = overlay::kStats; view.header = "System";
    for (int i = 0; i < 50; ++i) { direct.frame(view, true, now, view.opening); now += 20000; }
    assert(standalone.visible("CPU"));
    assert(standalone.visible("Firmware"));
    std::cout << "Native layout System/media/Settings transitions passed\n";
}
