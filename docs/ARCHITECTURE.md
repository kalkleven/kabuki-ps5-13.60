# How Kabuki works

Kabuki is two programs. The **daemon** is the payload you load (`kabuki-ps5.elf`). The
**worker** is embedded in it and runs inside the console's system interface process,
SceShellUI. The worker draws, and the daemon does everything else.

```
 ┌──────────────────────────── SceShellUI ────────────────────────────┐
 │  worker (src/shellui, src/ui)                                      │
 │   PUI widgets · UI3 glass focus · sounds · the panel's model       │
 │   reads small files the daemon writes; sends commands back         │
 └───────────────────────────▲──────────────┬─────────────────────────┘
          shared state ring  │              │ commands
 ┌───────────────────────────┴──────────────▼─────────────────────────┐
 │  daemon (src/daemon) = kabuki-ps5.elf                              │
 │   loads the worker (src/loader) · pad, shortcut and input capture  │
 │   system readings · settings · links to providers (librespot-ps5)  │
 └────────────────────────────────────────────────────────────────────┘
```

**Why the split:** anything that crashes inside SceShellUI takes the home screen down with it.
Sockets, JSON from other programs and anything else that can fail stay in the daemon. The
worker only reads small, validated files and a shared-memory ring, and draws.

## Source layout

| Folder | What it is |
|---|---|
| `src/common/protocol.hpp` | The shared-memory `State` both sides use: input, commands, readiness, release reasons |
| `src/daemon/` | `controller.cpp`, the payload's `main`. It finds the live SceShellUI, loads the worker, holds the pad while the panel is open (`capture-parent.inc`), reads CPU, memory, frame rate and temperatures (`system-stats.inc`), keeps settings, and links to librespot-ps5 |
| `src/shellui/` | The worker. `worker.cpp` is its entry; `native-worker.inc` builds and drives the PUI widgets through Mono; `input-worker.inc` and `toggle-worker.inc` handle the pad and the open shortcut |
| `src/ui/` | The interface itself: `native_model.hpp` (navigation, settings, sounds; no drawing) and `native_layout.hpp` (every widget's place, size and animation, written against a small backend interface) |
| `src/loader/` | Loading the worker into SceShellUI: ptrace, an ELF loader and a method-lookup helper, adapted from GPL projects (`SOURCES.md`) |
| `preview/` | The PC preview: a backend for `native_layout.hpp` that records widgets instead of making them, and a small web server and page that draw them |
| `tests/` | Host checks for the model and the library browser |
| `tools/` | Building (`embed*.py`), installing and logs (`console.py`), dependencies, and fetching the console's fonts and icons for the preview |

## One layout, two backends

`native_layout.hpp` is a template over a backend `B`, with calls such as `label`, `image`,
`skin`, `geometry`, `alpha` and `words`:
- **On the console:** the backend in `native-worker.inc` makes real PUI widgets.
- **On a PC:** the backend in `preview/native_preview.cpp` records them as JSON for the page to
  draw.

The preview shares the layout and navigation code with the console. Rendering and system
readings still need console validation.

## Data on the console

Everything lives in `/data/custom-overlays` (to be renamed `/data/kabuki`):

| File | Written by | What it holds |
|---|---|---|
| `probe.log`, `probe.lock` | daemon | the log; one Kabuki at a time |
| `stop.txt` | you (`tools/console.py stop`) | asks a running Kabuki to stop |
| `settings.json` | daemon, from the worker's commands | shortcut, hold time, sounds, the tile arrangement and every tile seen |
| `settings.rec`, `tiles.rec` | daemon | the same settings, and the registered tiles, for the worker |
| `tile-<ID>.rec` | daemon | a tile's template data (`media` or `list`) |
| `show.rec` | daemon | a provider asking to show one of its tiles |
| `system.rec` | daemon | the System tile's readings |
| `chip.png` | daemon | the System tile's icon |

The worker reads only `.rec` files: simple tab-separated records (`src/common/records.hpp`). All
JSON is parsed in the daemon.

## Providers

Any program on the console can add tiles to the panel through the daemon's broker, a small
versioned protocol on `127.0.0.1:38590` (see `protocol/PROTOCOL.md`). A tile either sends data to
one of Kabuki's card templates (`media`, `list`), or ships a native plugin. Either way
Kabuki draws the card, and everything a provider sends is checked in the daemon.

| Part | Where |
|---|---|
| The protocol | `protocol/PROTOCOL.md` |
| The broker: parsing, checks, limits, one `Service` provider per client | `src/daemon/broker.hpp` |
| Its sockets, ticked with the daemon's loop | `PlayerLink` in `src/daemon/controller.cpp` |
| A C client | `sdk/c/kabuki.h` |
| A small payload using it | `examples/hello-tile/` |
| A provider for the preview | `tools/mock_provider.py` |

`src/daemon/librespot_adapter.hpp` supports older receivers through their control channel on
`127.0.0.1:38574`. A receiver registering as the native `librespot` provider takes over those
tiles and disables the legacy adapter, so the two transports cannot duplicate the music UI.
