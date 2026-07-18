#!/usr/bin/env bash
# Compatibility entry point for the repository-local fake-UDM runner.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/local/run_fake_udm_e2e.sh" "$@"
