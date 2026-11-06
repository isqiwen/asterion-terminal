#!/bin/bash
# Runs inside the container started by scripts/services/build_linux_services.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build python3 python3-venv ca-certificates git curl unzip pkg-config
python3 -m venv /opt/build-tools
export PATH=/opt/build-tools/bin:$PATH
pip install conan==2.32.0 uv==0.12.9
rm -rf /work/source && mkdir -p /work/source
cd /work/source
tar xf /input/service-source.tar
conan profile detect --force
python3 scripts/prepare_ctp.py
# Build against this glibc so cpptrace enables signal-safe _dl_find_object.
conan install . -s build_type=Release -s compiler.cppstd=20 -o with_tests=False -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing '--build=cpptrace/*'
cmake --preset conan-release
# The recording and trading hosts' own releases, one self-extracting script each.
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build/Release --target asterion_history_setup asterion_trading_setup
mkdir -p /output
version=$(cat VERSION)
cp "build/Release/Asterion-History-${version}-linux-x86_64.sh" "build/Release/Asterion-Trading-${version}-linux-x86_64.sh" /output/
