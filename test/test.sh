#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

make -s kernel.elf out/mkfat32 out/heap-test
./out/heap-test
python3 test/run_command_tests.py "$ROOT" test/commands/*.test

python3 test/run_script_tests.py
