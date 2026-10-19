#!/usr/bin/env bash
# Public community SDK only; no console resources or private SDK upload.
set -euo pipefail
destination=${1:?Usage: setup-sdk.sh DESTINATION}
sdk_version=v0.44
sdk_sha256=4d06eab777bb135705b5a6816688c9132efb246093fe606ac488ab6cf65ca5b0
mkdir -p "$destination"
archive="$destination/ps5-payload-sdk.zip"
curl --fail --location --retry 3 --output "$archive" \
    "https://github.com/ps5-payload-dev/sdk/releases/download/$sdk_version/ps5-payload-sdk.zip"
printf '%s  %s\n' "$sdk_sha256" "$archive" | sha256sum --check --status
unzip -oq "$archive" -d "$destination"
test -x "$destination/ps5-payload-sdk/bin/prospero-clang++"
printf 'Installed PS5 Payload SDK %s\n' "$sdk_version"
