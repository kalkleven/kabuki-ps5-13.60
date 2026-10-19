// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_host.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
struct Backend {
  template <class F> bool plugin_scope(F f) { return f(); }
  int removed = 0;
  void plugin_remove(auto &w) {
    nodes.at(w.id).visible = false;
    ++removed;
  }
  struct Widget {
    int id = -1;
  };
  struct Node {
    int parent = -1;
    bool visible = true;
    float alpha = 1;
    std::string text;
  };
  std::vector<Node> nodes{Node{}};
  Widget root{0};
  Widget &screen() { return root; }
  bool add(Widget &w, Widget &parent) {
    w.id = int(nodes.size());
    nodes.push_back({parent.id, true, 1, {}});
    return true;
  }
  bool panel(Widget &w, Widget &p, overlay::native::Box, bool) {
    return add(w, p);
  }
  bool skin(Widget &w, Widget &p, overlay::native::Box, int, float) {
    return add(w, p);
  }
  bool image(Widget &w, Widget &p, overlay::native::Box, const std::string &) {
    return add(w, p);
  }
  bool label(Widget &w, Widget &p, overlay::native::Box, float, int,
             overlay::native::Rgb, int, int, const std::string &s) {
    add(w, p);
    words(w, s);
    return true;
  }
  void geometry(Widget &, overlay::native::Box) {}
  void show(Widget &w, bool value) { nodes.at(w.id).visible = value; }
  void alpha(Widget &w, float value) { nodes.at(w.id).alpha = value; }
  void words(Widget &w, const std::string &value) {
    nodes.at(w.id).text = value;
  }
  void tint(Widget &, overlay::native::Rgb, float) {}
  void picture(Widget &, const std::string &) {}
  void scale(Widget &, float) {}
  float text_width(Widget &, const std::string &s) {
    return float(s.size()) * 12;
  }
  std::string icon(const std::string &s) { return s; }
  std::string fallback_art() { return {}; }
  bool focus(const overlay::native::Focus &, int) { return false; }
  bool visible(const std::string &text) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].text != text)
        continue;
      int at = int(i);
      while (at >= 0 && nodes[at].visible && nodes[at].alpha > 0.001f)
        at = nodes[at].parent;
      if (at < 0)
        return true;
    }
    return false;
  }
};

static int destroyed = 0, frames = 0, mode = 0;
static int create(const kabuki_api *api, kabuki_widget root, void **state) {
  auto label = api->label(api->context, root, {0, 0, 100, 40}, 22, 0,
                          {1, 1, 1, 1}, 0, 0, "Plugin child");
  *state = nullptr;
  return label ? 0 : -1;
}
static int frame(const kabuki_api *, void *, uint64_t, float,
                 kabuki_input input) {
  ++frames;
  if (mode == 1)
    return -1;
  if (mode == 2)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (input.pressed)
    assert(input.pressed == 0x4000);
  return 0;
}
static void destroy(const kabuki_api *, void *) { ++destroyed; }
int main() {
  Backend backend;
  const kabuki_plugin plugin = {KABUKI_PLUGIN_ABI, sizeof(kabuki_plugin),
                                create, frame, destroy};
  overlay::NativeView view;
  view.tab = overlay::kCustom;
  view.tile = "TESTCC";
  view.plugin_live = {"TESTCC"};
  view.plugin_address = reinterpret_cast<std::uintptr_t>(&plugin);
  view.focused = true;
  {
    kabuki::PluginHost host(backend);
    host.frame(backend.root, view, 1, 0.016f);
    assert(frames == 1);
    mode = 1;
    host.frame(backend.root, view, 2, 0.016f);
    const int stopped = frames;
    host.frame(backend.root, view, 3, 0.016f);
    assert(frames == stopped && backend.visible("Plugin frame failed"));
    view.plugin_live.clear();
    view.tab = overlay::kMedia;
    host.frame(backend.root, view, 4, 0.016f);
    assert(destroyed == 1 && backend.removed >= 3);
  }
  mode = 2;
  view.tab = overlay::kCustom;
  view.plugin_live = {"TESTCC"};
  {
    kabuki::PluginHost host(backend);
    for (int n = 0; n < 9; ++n)
      host.frame(backend.root, view, n + 5, 0.016f);
    assert(backend.visible("Plugin exceeded its frame budget"));
  }
  assert(destroyed == 2);
  std::cout
      << "Plugin callback failure, frame budget and provider cleanup passed\n";
}
