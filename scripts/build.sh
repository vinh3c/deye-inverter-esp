#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
exec uv run --python 3.11 "$project_root/scripts/project.py" build "$@"
