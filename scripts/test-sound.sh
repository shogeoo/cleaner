#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  tests/sound_director_test.cpp -o build/sound_director_test
./build/sound_director_test
