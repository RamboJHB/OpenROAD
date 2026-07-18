#!/usr/bin/env bash
# Compatibility entry point. The portable E2E source and runner now travel
# inside fillerRepair/test with the code being ported.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/../src/fillerRepair/test/run_e2e_tests.sh" "$@"
