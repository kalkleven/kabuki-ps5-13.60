Kabuki 0.2.0 release candidate 2 is a native control panel and homebrew overlay hub for PS5.

This pre-release is for testing. It includes the current 13.60-focused changes and is not a confirmed firmware-compatible build.

- Configurable shortcut, sounds and sidebar arrangement.
- Built-in System and Settings cards.
- Provider protocol, media and list templates, C SDK and native plugin API.
- Music controls, queue and library cards supplied by a compatible librespot-ps5 receiver.
- Library navigation resets when the receiver changes account or session.

The original Kabuki project reports testing on firmware 13.20. This version targets
firmware 13.60, but has not been confirmed on-console. Treat it as experimental:
runtime method and hook checks cannot guarantee compatibility when ShellUI differs.
Please report your firmware version and relevant logs with any test results.

Copy `kabuki-ps5.elf` to a USB drive and load it manually. The default shortcut is
L3 + R3 held for half a second. When using librespot-ps5, launch Kabuki first.
Settings and logs are stored in `/data/custom-overlays`. No autoload is installed.

`SHA256SUMS.txt` verifies the payload and its accompanying build information.
The release payload excludes development takeover support. Source and build
instructions are available in the repository.
