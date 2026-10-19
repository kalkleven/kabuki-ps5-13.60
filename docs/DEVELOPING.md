# Developing Kabuki

## Setting up

You need:
- Linux or WSL (Ubuntu 22.04 works) with the PS5 Payload SDK. Set `PS5_PAYLOAD_SDK`, or put
  `export PS5_PAYLOAD_SDK=...` in `.local/build-env.sh`, which `build.sh` reads and git ignores.
- Python 3.
- `g++` (or set `CXX`) for the host checks.
- clang for the PC preview (on Windows, LLVM's `clang++`).

Then:

```bash
python3 tools/fetch-deps.py
```

On Windows, run the shell scripts through WSL with `wsl -d Ubuntu-22.04 -- bash build.sh`.
From Git Bash, set `MSYS_NO_PATHCONV=1` first, or Git Bash rewrites `/mnt/...` paths.

## The preview

Most interface work happens here, and console builds come once a change looks right.

```bash
python3 tools/fetch_console_assets.py <console-ip>   # once: the console's fonts and UI resources
python3 preview/native_preview.py                    # then open http://127.0.0.1:8765
```

- **Controls:** Space opens and closes the panel. Arrow keys move. Enter is Cross, Esc is
  Circle, S is Square, and Q and E are L1 and R1. A controller works too.
- **Address:** use `127.0.0.1`, not `localhost`, which can add a delay of hundreds of
  milliseconds. Keep one tab open, because several tabs drive the same panel.
- **Live data:** `--console <ip>` mirrors a console's player state.
- **Providers:** the preview listens on `127.0.0.1:38590` like the console. Run
  `python3 tools/mock_provider.py` to add a player and a library tile, or point a payload built
  against `sdk/c/kabuki.h` at it. Set `KABUKI_PREVIEW_OUT` to keep a second preview's build and
  files apart from the first.
- **Snapshots:** in the page's console, `snapshot()` saves a 4K PNG of the current frame to
  `build/native-preview/snapshot.png`.
- **Reloading:** the preview rebuilds itself when the sources change. Restart the server to
  pick that up.

The console's fonts and resource files are Sony's. They stay in `assets/system/`, which git
ignores. Never commit them, and never put them in a release.

## Building for the console

```bash
bash tests/run.sh
bash build.sh --dev
python3 tools/console.py <console-ip> install
```

- **Development builds:** a `--dev` build can take over from a Kabuki that's already running.
  It asks the old one to stop, stops it if it doesn't, retires its worker, and loads the new one
  beside it, so you can try a change without rebooting. Each replaced worker stays in SceShellUI
  until the next reboot. Never use `--dev` for a release.
- **Size limit:** payloads much over 2 MiB have failed to start with no message, so `build.sh`
  refuses to finish over that size. Keep large buffers on the heap, not in static data.
- **Where it goes:** the payload goes on USB, never the console's internal storage. USB mount
  numbers change between boots, so `console.py install` finds the drive by what's on it.

## Logs and stopping

```bash
python3 tools/console.py <console-ip> log
python3 tools/console.py <console-ip> stop        # then, before the next run:
python3 tools/console.py <console-ip> clear-stop
```

Check a log's build id before trusting "nothing changed": an older Kabuki may still be the one
running.

## Before committing

- `bash tests/run.sh` passes, `bash build.sh` builds, and the change has been tried on a console.
- Nothing personal goes in: no console addresses, account names, paths from your own machine,
  time zones, or screenshots that show notifications. `git diff --cached` before every commit.
- Nothing of Sony's goes in: fonts, `.rco` files and firmware modules stay local (see
  `.gitignore`).
