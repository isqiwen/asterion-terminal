#!/bin/bash
# Rebuilds build/linux-bundles/asterion-services-linux-x86_64.zip from the
# current checkout in an Ubuntu 24.04 x86_64 container (Docker/OrbStack).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK="$ROOT/build/linux-service-work"
IMAGE=${ASTERION_LINUX_IMAGE:-ubuntu:24.04}
rm -rf "$WORK/input" "$WORK/output"
mkdir -p "$WORK/input" "$WORK/output" "$ROOT/build/linux-bundles"
cd "$ROOT"
# macOS tar metadata would become ._* files on Linux and change the fingerprint.
# GNU tar on a Linux host has no such metadata and no flag for it.
METADATA=(--no-xattrs)
if [ "$(uname -s)" = Darwin ]; then METADATA+=(--no-mac-metadata); fi
git ls-files -co --exclude-standard -z | grep -zv '^build/\|node_modules\|/dist/\|test-results' |
  while IFS= read -r -d '' file; do
    if [ -e "$file" ]; then printf '%s\0' "$file"; fi
  done |
  COPYFILE_DISABLE=1 xargs -0 tar "${METADATA[@]}" -cf "$WORK/input/service-source.tar"
docker run --rm --platform linux/amd64 \
  -v "$WORK/input:/input:ro" -v "$WORK/output:/output" \
  -v "$ROOT/scripts/services/linux-services-container.sh:/build.sh:ro" "$IMAGE" bash /build.sh
cp "$WORK/output/asterion-services-linux-x86_64.zip" "$ROOT/build/linux-bundles/"
echo "Linux services bundle updated: build/linux-bundles/asterion-services-linux-x86_64.zip"
