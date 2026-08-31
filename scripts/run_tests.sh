#!/usr/bin/env bash
# Host-native tests for the platform-independent core (ring, resampler, rate
# controller, meter, JSON). Runs anywhere; no Windows or audio hardware needed.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-tests
g++ -std=c++20 -O2 -Wall -Wextra -Isrc tests/dsp_test.cpp src/config/Json.cpp \
    -o build-tests/dsp_test
./build-tests/dsp_test
