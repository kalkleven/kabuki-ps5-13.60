# Native plugin cards

Use `media` and `list` template cards for players and lists. Kabuki handles their layout,
navigation and animation. Native plugins are for cards those templates cannot express.

**Plugins are not sandboxed. Their code runs inside SceShellUI. A pointer bug or native
crash can crash the home screen.** Managed widget exceptions and slow callbacks are
handled, but that does not isolate native memory access.

Include `sdk/plugin/kabuki_plugin.h` and export `kabuki_plugin_v1`, a data symbol containing
the ABI, struct size and `create`, `frame` and `destroy` callbacks. Console plugins are
freestanding x86-64 shared ELFs with no imports, constructors or TLS. Export a trivial
executable `kabuki_plugin_entry`. It is only the loader's mapping anchor; it is never called.
The parent validates file ranges, relocations, symbol size and ABI before mapping, without
creating another thread. Plugin code remains mapped until reboot. Up to 16 images can be
mapped by one daemon, so do not use a build-and-reload loop indefinitely.

Register through the provider protocol:

```json
{"v":1,"hello":{"provider":"my-card","tiles":[{"id":"MYCARD","title":"My card","card":"plugin","plugin":"/mnt/usb0/my-card.elf"}]}}
{"v":1,"data":{"tile":"MYCARD","data":{"count":4}}}
```

Discover the USB drive by its contents before constructing the path; the number above is
only an example. The tile's data is a bounded JSON object. `api.data` returns its latest
serialization; `api.action` routes a named action and integer value to the provider.

Widget 0 is the clipped content panel, 520 × 682 logical pixels. Its origin is below
Kabuki's header. Create at most 127 children using panel, label, skin and image calls.
Keep widget handles inside your instance, never native PUI pointers. API strings are
borrowed for the duration of the call. Geometry must be finite and bounded. Images use
the same local-file conventions as built-in cards.

Kabuki owns the rail, header, card fade, open shortcut and Circle. A focused plugin gets
the D-pad and Cross. Press edges are consumed once even when the renderer runs faster
than input polling. `api.focus` takes a box in content coordinates. The host removes all
widgets when the provider disappears. Return nonzero on failure. Eight sustained frames
above 4 ms disable the card and show an error. The console log records plugin frame times
every five seconds while one is displayed.

Run `bash examples/custom-card/build.sh --host` for a Linux preview library, or build
`card.c` as a Windows DLL exporting the same data symbol. Set
`KABUKI_PLUGIN_PREVIEW_ROOT` to the host-library directory before starting the native
preview. A registration for `.../kabuki-custom-card.elf` resolves to
`kabuki-custom-card.so` or `.dll` in that explicitly configured directory. Console ELFs
are never run by the PC preview.

`bash examples/custom-card/build.sh` produces separate plugin and provider payloads.
Stage them on USB and launch the provider manually. The example animates a circle and
counts Cross presses. Do not include demonstration providers in autoload or releases.
