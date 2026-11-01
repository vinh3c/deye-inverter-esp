#!/usr/bin/env bash
set -eo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
sdk_root="${ESP_SDK_ROOT:-$HOME/esp}"
export PATH="$sdk_root/python311/bin:$sdk_root/gn:$PATH"
if [[ ! -f "$sdk_root/esp-idf/export.sh" || ! -f "$sdk_root/esp-matter/export.sh" ]]; then
    echo 'SDK missing: run bash scripts/bootstrap.sh first.' >&2
    exit 1
fi
source "$sdk_root/esp-idf/export.sh"
export ESP_MATTER_PATH="$sdk_root/esp-matter"
source "$sdk_root/esp-matter/export.sh"
cd "$project_root"
idf.py "$@" build
