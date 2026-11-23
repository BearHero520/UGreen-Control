#!/bin/bash
# Compatibility entry point. Every external request uses the authenticated frontend.
set -euo pipefail
exec python3 "$(dirname "${BASH_SOURCE[0]}")/portable_runtime.py" web "$@"
