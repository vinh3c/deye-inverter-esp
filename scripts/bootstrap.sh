#!/usr/bin/env bash
# Compatibility entry point; the shared command is uv run fw bootstrap.
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
exec uv run fw bootstrap "$@"
