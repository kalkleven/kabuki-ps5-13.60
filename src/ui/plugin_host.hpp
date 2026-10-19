// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../../sdk/plugin/kabuki_plugin.h"
#include "native_layout.hpp"
#include <chrono>
#include <functional>
#include <memory>

namespace kabuki {
// The same host drives the recorder and PUI. Backend::plugin_scope routes
// managed calls through native_try, retaining a failure in this scope, never
// closing Kabuki.
template <class B> class PluginHost {
  using W = typename B::Widget;
  B &b;
  struct Instance {
    std::string id, data, error, icon;
    std::uintptr_t address = 0;
    const kabuki_plugin *plugin = nullptr;
    void *state = nullptr;
    W root{}, message{};
    std::vector<W> widgets;
    int slow = 0;
    bool created = false;
    std::uint64_t input_seq = 0;
    overlay::native::Focus focus{};
    kabuki_api api{};
    PluginHost *host = nullptr;
  };
  std::map<std::string, std::unique_ptr<Instance>> instances;
  static Instance &self(void *c) { return *static_cast<Instance *>(c); }
  static overlay::native::Box box(kabuki_box r) { return {r.x, r.y, r.w, r.h}; }
  static bool valid(kabuki_box r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) &&
           std::isfinite(r.h) && std::abs(r.x) <= 4096 &&
           std::abs(r.y) <= 4096 && r.w >= 0 && r.w <= 2048 && r.h >= 0 &&
           r.h <= 2048;
  }
  static W *widget(Instance &i, kabuki_widget id) {
    return id < i.widgets.size() ? &i.widgets[id] : nullptr;
  }
  template <class F> static int change(void *c, kabuki_widget id, F fn) {
    auto &i = self(c);
    auto *w = widget(i, id);
    if (!w)
      return -1;
    return i.host->b.plugin_scope([&] {
      fn(i.host->b, *w);
      return true;
    })
               ? 0
               : -1;
  }
  template <class F>
  static kabuki_widget make(void *c, kabuki_widget parent, kabuki_box r, F fn) {
    auto &i = self(c);
    if (!valid(r) || i.widgets.size() >= KABUKI_PLUGIN_WIDGETS ||
        !widget(i, parent))
      return 0;
    // Reserve once: callbacks may keep a parent's reference while appending.
    i.widgets.emplace_back();
    auto &w = i.widgets.back();
    if (!i.host->b.plugin_scope(
            [&] { return fn(i.host->b, w, *widget(i, parent)); })) {
      i.error = "A widget could not be created";
      return 0;
    }
    return kabuki_widget(i.widgets.size() - 1);
  }
  void api(Instance &i) {
    i.api = {};
    i.api.abi = KABUKI_PLUGIN_ABI;
    i.api.size = sizeof(kabuki_api);
    i.api.context = &i;
    i.api.panel = [](void *c, kabuki_widget p, kabuki_box r, int clip) {
      return make(c, p, r, [&](B &b, W &w, W &parent) {
        return b.panel(w, parent, box(r), clip != 0);
      });
    };
    i.api.label = [](void *c, kabuki_widget p, kabuki_box r, float size,
                     int weight, kabuki_colour ink, int align, int trim,
                     const char *text) {
      if (!text || size < 8 || size > 100 || weight < 0 || weight > 6 ||
          align < 0 || align > 2)
        return kabuki_widget(0);
      return make(c, p, r, [&](B &b, W &w, W &parent) {
        return b.label(w, parent, box(r), size, weight, {ink.r, ink.g, ink.b},
                       align, trim, std::string(text).substr(0, 1024));
      });
    };
    i.api.image = [](void *c, kabuki_widget p, kabuki_box r, const char *uri) {
      return make(c, p, r, [&](B &b, W &w, W &parent) {
        return b.image(w, parent, box(r), uri ? uri : "");
      });
    };
    i.api.skin = [](void *c, kabuki_widget p, kabuki_box r, int kind,
                    float radius) {
      if (kind < 0 || kind > 7 || radius < 0 || radius > 1024)
        return kabuki_widget(0);
      return make(c, p, r, [&](B &b, W &w, W &parent) {
        return b.skin(w, parent, box(r), kind, radius);
      });
    };
    i.api.geometry = [](void *c, kabuki_widget w, kabuki_box r) {
      return valid(r)
                 ? change(c, w,
                          [&](B &b, W &target) { b.geometry(target, box(r)); })
                 : -1;
    };
    i.api.alpha = [](void *c, kabuki_widget w, float a) {
      return std::isfinite(a)
                 ? change(c, w,
                          [&](B &b, W &target) {
                            b.alpha(target, std::clamp(a, 0.f, 1.f));
                          })
                 : -1;
    };
    i.api.tint = [](void *c, kabuki_widget w, kabuki_colour a) {
      return change(
          c, w, [&](B &b, W &target) { b.tint(target, {a.r, a.g, a.b}, a.a); });
    };
    i.api.words = [](void *c, kabuki_widget w, const char *text) {
      return change(c, w, [&](B &b, W &target) {
        b.words(target, std::string(text ? text : "").substr(0, 1024));
      });
    };
    i.api.picture = [](void *c, kabuki_widget w, const char *uri) {
      return change(
          c, w, [&](B &b, W &target) { b.picture(target, uri ? uri : ""); });
    };
    i.api.show = [](void *c, kabuki_widget w, int on) {
      return change(c, w, [&](B &b, W &target) { b.show(target, on != 0); });
    };
    i.api.text_width = [](void *c, kabuki_widget w, const char *text) {
      float width = 0;
      change(c, w, [&](B &b, W &target) {
        width = b.text_width(target, text ? text : "");
      });
      return width;
    };
    i.api.icon = [](void *c, const char *name) {
      auto &i = self(c);
      i.icon = i.host->b.icon(name ? name : "");
      return i.icon.c_str();
    };
    i.api.focus = [](void *c, kabuki_widget w, kabuki_box r, float radius) {
      auto &i = self(c);
      if (!widget(i, w) || !valid(r))
        return -1;
      i.focus = {1000 + int(w),
                 overlay::native::kCardX + r.x,
                 overlay::native::kCardY + overlay::native::kListTop + r.y,
                 r.w,
                 r.h,
                 radius};
      return 0;
    };
    i.api.data = [](void *c) { return self(c).data.c_str(); };
    i.api.action = [](void *c, const char *id, int value) {
      auto &i = self(c);
      if (!id || std::strlen(id) > 64)
        return -1;
      if (i.host->action) {
        i.host->action(i.id, id, value);
      }
      return 0;
    };
  }
  void destroy(Instance &i) {
    if (i.created && i.plugin)
      b.plugin_scope([&] {
        i.plugin->destroy(&i.api, i.state);
        return true;
      });
    b.plugin_scope([&] {
      for (std::size_t n = i.widgets.size(); n > 1; --n)
        b.plugin_remove(i.widgets[n - 1]);
      b.plugin_remove(i.root);
      b.plugin_remove(i.message);
      return true;
    });
    i.created = false;
  }

public:
  std::function<void(const std::string &, const std::string &, int)> action;
  std::function<void(const std::string &, std::uint64_t)> timing;
  explicit PluginHost(B &backend) : b(backend) {}
  ~PluginHost() {
    for (auto &[id, i] : instances)
      destroy(*i);
  }
  overlay::native::Focus frame(W &card, const overlay::NativeView &v,
                               std::uint64_t now, float dt) {
    for (auto it = instances.begin(); it != instances.end();) {
      if (std::find(v.plugin_live.begin(), v.plugin_live.end(), it->first) ==
          v.plugin_live.end()) {
        destroy(*it->second);
        it = instances.erase(it);
      } else {
        b.plugin_scope([&] {
          b.show(it->second->root, false);
          b.show(it->second->message, false);
          return true;
        });
        ++it;
      }
    }
    if (v.tab != overlay::kCustom)
      return {};
    auto &item = instances[v.tile];
    if (item && item->address != v.plugin_address) {
      destroy(*item);
      item.reset();
    }
    if (!item) {
      item = std::make_unique<Instance>();
      auto &i = *item;
      i.host = this;
      i.id = v.tile;
      i.address = v.plugin_address;
      i.plugin = reinterpret_cast<const kabuki_plugin *>(i.address);
      i.widgets.reserve(KABUKI_PLUGIN_WIDGETS);
      api(i);
      b.plugin_scope([&] {
        return b.panel(i.root, card, {0, overlay::native::kListTop, 520, 682},
                       true) &&
               b.label(i.message, card, {24, 180, 472, 100}, 22, 0, {1, 1, 1},
                       0, 3, "");
      });
      i.widgets.push_back(i.root);
      if (!v.plugin_error.empty())
        i.error = v.plugin_error;
      else if (!i.plugin || i.plugin->abi != KABUKI_PLUGIN_ABI ||
               i.plugin->size < sizeof(kabuki_plugin) || !i.plugin->create ||
               !i.plugin->frame || !i.plugin->destroy)
        i.error = "Plugin could not be loaded";
      else {
        i.data = v.plugin_data;
        i.created = true;
        if (!b.plugin_scope(
                [&] { return i.plugin->create(&i.api, 0, &i.state) == 0; }))
          i.error = "Plugin setup failed";
      }
    }
    auto &i = *item;
    i.data = v.plugin_data;
    i.focus = {};
    if (i.error.empty()) {
      b.plugin_scope([&] {
        b.show(i.root, true);
        return true;
      });
      const auto start = std::chrono::steady_clock::now();
      const auto pressed =
          i.input_seq == v.plugin_input_seq ? 0 : v.plugin_pressed;
      i.input_seq = v.plugin_input_seq;
      const bool ok = b.plugin_scope([&] {
        return i.plugin->frame(&i.api, i.state, now, dt,
                               {v.focused ? v.plugin_held : 0,
                                v.focused ? pressed : 0}) == 0;
      });
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
      if (timing)
        timing(i.id, std::uint64_t(us));
      i.slow = us > 4000 ? i.slow + 1 : std::max(0, i.slow - 1);
      if (!ok)
        i.error = "Plugin frame failed";
      if (i.slow >= 8)
        i.error = "Plugin exceeded its frame budget";
    }
    if (!i.error.empty())
      b.plugin_scope([&] {
        b.show(i.root, false);
        b.show(i.message, true);
        b.words(i.message, i.error);
        return true;
      });
    return i.error.empty() && v.focused ? i.focus : overlay::native::Focus{};
  }
};
} // namespace kabuki
