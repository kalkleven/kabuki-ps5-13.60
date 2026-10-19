#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/custom-card"
mkdir -p "$out"
if [[ ${1:-} == --host ]]; then
    "${CC:-cc}" -std=c11 -O2 -fPIC -fno-stack-protector -shared -nostdlib -Wl,-Bsymbolic \
        -I"$repo/sdk/plugin" "$repo/examples/custom-card/card.c" -o "$out/kabuki-custom-card.so"
    exit
fi
if [[ -z ${PS5_PAYLOAD_SDK:-} && -f "$repo/.local/build-env.sh" ]];then source "$repo/.local/build-env.sh";fi
sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
"$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC -fno-stack-protector -nostdlib -shared \
    -Wl,-Bsymbolic -Wl,--hash-style=sysv -Wl,-e,kabuki_plugin_entry -I"$repo/sdk/plugin" \
    "$repo/examples/custom-card/card.c" -o "$out/kabuki-custom-card.elf"
"$sdk/bin/prospero-clang" -O2 -I"$repo/sdk/c" "$repo/examples/custom-card/provider.c" -o "$out/kabuki-custom-provider.elf"
