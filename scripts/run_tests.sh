#!/usr/bin/env bash
# Host-native tests for the platform-independent core (ring, resampler, rate
# controller, sample formats, meter, JSON). No audio hardware is needed.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-tests
g++ -std=c++20 -O2 -Wall -Wextra -Isrc tests/dsp_test.cpp src/audio/SampleFormat.cpp \
    src/config/Json.cpp \
    -o build-tests/dsp_test
./build-tests/dsp_test
