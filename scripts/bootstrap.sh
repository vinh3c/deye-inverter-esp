#!/usr/bin/env bash
# Run in Ubuntu/WSL. SDKs live on the Linux filesystem for build performance.
set -eo pipefail
sdk_root="${ESP_SDK_ROOT:-$HOME/esp}"
matter_revision=abdb759d7aae8a445e3edc5769a10f5cb23feffc
gn_revision=05eed8f6252e2dd6b555e0b65192ef03e2c4a276

if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null || ! command -v g++ >/dev/null; then
    sudo apt-get update
    sudo apt-get install -y git wget curl flex bison gperf python3 python3-pip python3-venv \
        cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
        build-essential pkg-config unzip
fi
mkdir -p "$sdk_root"
# Use Python 3.11 even on Ubuntu 20.04; recent Pigweed scripts require Python >= 3.9.
if [[ ! -x "$sdk_root/python311/bin/python3" ]]; then
    python3 -m venv "$sdk_root/bootstrap-venv"
    "$sdk_root/bootstrap-venv/bin/pip" install 'uv==0.8.17'
    "$sdk_root/bootstrap-venv/bin/uv" python install 3.11.13
    python_path="$("$sdk_root/bootstrap-venv/bin/uv" python find 3.11.13)"
    ln -s "$(dirname "$(dirname "$python_path")")" "$sdk_root/python311"
fi
export PATH="$sdk_root/python311/bin:$PATH"
if [[ ! -d "$sdk_root/esp-idf/.git" ]]; then
    git clone --branch v5.4.1 --depth 1 --recursive --shallow-submodules --jobs 8 \
        https://github.com/espressif/esp-idf.git "$sdk_root/esp-idf"
fi
if [[ "$(git -C "$sdk_root/esp-idf" describe --tags --exact-match)" != v5.4.1 ]]; then
    echo 'Expected ESP-IDF v5.4.1; use a separate ESP_SDK_ROOT.' >&2
    exit 1
fi
git -C "$sdk_root/esp-idf" submodule update --init --recursive --depth 1 --jobs 8
"$sdk_root/esp-idf/install.sh" esp32c3
source "$sdk_root/esp-idf/export.sh"
if [[ ! -d "$sdk_root/esp-matter/.git" ]]; then
    git init "$sdk_root/esp-matter"
    git -C "$sdk_root/esp-matter" remote add origin https://github.com/espressif/esp-matter.git
    git -C "$sdk_root/esp-matter" fetch --depth 1 origin "$matter_revision"
    git -C "$sdk_root/esp-matter" checkout --detach FETCH_HEAD
fi
if [[ "$(git -C "$sdk_root/esp-matter" rev-parse HEAD)" != "$matter_revision" ]]; then
    echo 'Unexpected ESP-Matter version; use a separate ESP_SDK_ROOT.' >&2
    exit 1
fi
git -C "$sdk_root/esp-matter" submodule update --init --depth 1
cd "$sdk_root/esp-matter/connectedhomeip/connectedhomeip"
python3 scripts/checkout_submodules.py --platform esp32 --shallow
cd "$sdk_root/esp-matter"
# Firmware only needs GN, not the full ARM/Clang/QEMU/ZAP host development suite.
case "$(uname -m)" in
    x86_64) gn_platform=linux-amd64 ;;
    aarch64) gn_platform=linux-arm64 ;;
    *) echo 'Unsupported host architecture for GN' >&2; exit 1 ;;
esac
mkdir -p "$sdk_root/gn"
if [[ ! -x "$sdk_root/gn/gn" ]]; then
    curl --retry 3 -fL "https://chrome-infra-packages.appspot.com/dl/gn/gn/${gn_platform}/+/git_revision:${gn_revision}" \
        -o "$sdk_root/gn.zip"
    unzip -o "$sdk_root/gn.zip" -d "$sdk_root/gn"
    chmod +x "$sdk_root/gn/gn"
fi
./install.sh --no-bootstrap --no-host-tool
