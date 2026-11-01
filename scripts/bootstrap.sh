#!/usr/bin/env bash
# Compatibility entry point. Windows uses project.py directly.
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
exec uv run --python 3.11 "$project_root/scripts/project.py" bootstrap "$@"
