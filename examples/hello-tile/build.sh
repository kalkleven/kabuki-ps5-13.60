#!/usr/bin/env bash
# Builds build/hello-tile.elf with the PS5 Payload SDK (PS5_PAYLOAD_SDK, default /opt/ps5-payload-sdk).
# Send it to the console the way you send any payload; Kabuki must be running for the tile to appear.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
if [[ -z ${PS5_PAYLOAD_SDK:-} && -f "$root/.local/build-env.sh" ]]; then
    source "$root/.local/build-env.sh"
fi
sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
mkdir -p "$here/build"
"$sdk/bin/prospero-clang" -O2 -Wall -Wextra -Werror -I"$root/sdk/c" "$here/hello.c" -o "$here/build/hello-tile.elf"
"$sdk/bin/prospero-strip" --strip-debug "$here/build/hello-tile.elf"
echo "built $here/build/hello-tile.elf ($(stat -c %s "$here/build/hello-tile.elf") bytes)"
