#!/bin/bash
# Runs inside the container started by scripts/services/build_linux_services.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build python3 python3-venv ca-certificates git curl unzip pkg-config
python3 -m venv /opt/build-tools
export PATH=/opt/build-tools/bin:$PATH
pip install conan==2.32.0
rm -rf /work/source && mkdir -p /work/source
cd /work/source
tar xf /input/service-source.tar
conan profile detect --force
python3 scripts/prepare_ctp.py
conan install . -s build_type=Release -s compiler.cppstd=20 -o with_tests=False -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing
cmake --preset conan-release
CMAKE_BUILD_PARALLEL_LEVEL=4 python3 scripts/services/deployment_bundle.py build/Release
mkdir -p /output
cp build/Release/deployment/asterion-services-linux-x86_64.zip /output/
./build/Release/asterion-factor --version
./build/Release/asterion-data-service --version
./build/Release/asterion-task-service --version
