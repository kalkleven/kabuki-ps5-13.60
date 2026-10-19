#!/usr/bin/env bash
# The same release packaging runs locally and in GitHub Actions.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to the verified community SDK}"
python3 tools/fetch-deps.py
bash tests/run.sh
bash build.sh
mkdir -p dist
cp build/kabuki-ps5.elf dist/kabuki-ps5.elf
revision=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || printf unknown)}
printf 'Kabuki release build\nSource revision: %s\nSDK: ps5-payload-dev/sdk v0.44\nMode: release (no development takeover)\n' "$revision" > dist/BUILD-INFO.txt
cp docs/RELEASE-NOTES.md dist/RELEASE-NOTES.md
(cd dist && sha256sum kabuki-ps5.elf BUILD-INFO.txt RELEASE-NOTES.md > SHA256SUMS.txt)
