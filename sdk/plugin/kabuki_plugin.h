/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KABUKI_PLUGIN_H
#define KABUKI_PLUGIN_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KABUKI_PLUGIN_ABI 1u
#define KABUKI_PLUGIN_WIDGETS 128u
typedef uint32_t kabuki_widget;
typedef struct {
  float x, y, w, h;
} kabuki_box;
typedef struct {
  float r, g, b, a;
} kabuki_colour;
typedef struct {
  uint32_t held, pressed;
} kabuki_input;
/* Append-only. Check size before using extensions. All coordinates are 1080p
 * units, relative to the clipped content panel (520 x 682). Widget 0 is that
 * panel. Strings are borrowed only for the duration of the call. No threads or
 * imports. */
typedef struct kabuki_api {
  uint32_t abi, size;
  void *context;
  kabuki_widget (*panel)(void *, kabuki_widget, kabuki_box, int clip);
  kabuki_widget (*label)(void *, kabuki_widget, kabuki_box, float size,
                         int weight, kabuki_colour, int align, int trim,
                         const char *);
  kabuki_widget (*image)(void *, kabuki_widget, kabuki_box, const char *);
  kabuki_widget (*skin)(void *, kabuki_widget, kabuki_box, int kind,
                        float radius);
  int (*geometry)(void *, kabuki_widget, kabuki_box);
  int (*alpha)(void *, kabuki_widget, float);
  int (*tint)(void *, kabuki_widget, kabuki_colour);
  int (*words)(void *, kabuki_widget, const char *);
  int (*picture)(void *, kabuki_widget, const char *);
  int (*show)(void *, kabuki_widget, int);
  float (*text_width)(void *, kabuki_widget, const char *);
  const char *(*icon)(void *, const char *);
  int (*focus)(void *, kabuki_widget, kabuki_box, float radius);
  const char *(*data)(void *); /* provider's latest JSON object */
  int (*action)(void *, const char *id, int value);
} kabuki_api;
struct kabuki_plugin {
  uint32_t abi, size;
  int (*create)(const kabuki_api *, kabuki_widget card, void **instance);
  int (*frame)(const kabuki_api *, void *instance, uint64_t now_us, float dt,
               kabuki_input);
  void (*destroy)(const kabuki_api *, void *instance);
};
/* Export this data symbol. Console plugins also export a trivial executable
 * kabuki_plugin_entry; the loader maps it but never executes the entry. */
extern const struct kabuki_plugin kabuki_plugin_v1;
#ifdef __cplusplus
}
#endif
#endif
