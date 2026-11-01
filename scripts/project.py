#!/usr/bin/env python3
"""Native ESP-Matter development commands. Requires Python 3.11 and Git."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
IDF_VERSION = "v5.4.1"
MATTER_REVISION = "abdb759d7aae8a445e3edc5769a10f5cb23feffc"
GN_REVISION = "05eed8f6252e2dd6b555e0b65192ef03e2c4a276"
WINDOWS = sys.platform == "win32"
SDK = Path(os.environ.get("ESP_SDK_ROOT", ROOT / ".tools" / "sdk")).expanduser().resolve()
IDF = SDK / "esp-idf"
MATTER = SDK / "esp-matter"
CHIP = MATTER / "connectedhomeip" / "connectedhomeip"
TOOLS = SDK / "tools"
VENV = TOOLS / "python_env" / "idf5.4_py3.11_env"
PYTHON = VENV / ("Scripts/python.exe" if WINDOWS else "bin/python")
BUILD = ROOT / "build" / f"native-{sys.platform}-{platform.machine().lower()}"


def run(args, *, cwd=ROOT, env=None, capture=False):
    args = [str(arg) for arg in args]
    if env is not None and not os.path.isabs(args[0]):
        # Windows CreateProcess does not search the PATH passed through env.
        args[0] = shutil.which(args[0], path=env["PATH"]) or args[0]
    if not capture:
        print("+ " + subprocess.list2cmdline(args), flush=True)
    return subprocess.run(args, cwd=cwd, env=env, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def base_env():
    env = os.environ.copy()
    # Ignore an SDK previously exported by the user's shell.
    for key in ("IDF_PYTHON_ENV_PATH", "IDF_PATH", "ESP_MATTER_PATH", "VIRTUAL_ENV"):
        env.pop(key, None)
    env.update(IDF_PATH=str(IDF), IDF_TOOLS_PATH=str(TOOLS),
               ESP_MATTER_PATH=MATTER.as_posix(), PYTHONUTF8="1")
    # Per-process Git options also apply to recursively cloned submodules.
    index = int(env.get("GIT_CONFIG_COUNT", "0"))
    for key, value in (("core.longpaths", "true"), ("core.autocrlf", "false")):
        env[f"GIT_CONFIG_KEY_{index}"] = key
        env[f"GIT_CONFIG_VALUE_{index}"] = value
        index += 1
    env["GIT_CONFIG_COUNT"] = str(index)
    return env


def checkout(path, url, revision, env):
    if not (path / ".git").exists():
        path.mkdir(parents=True, exist_ok=True)
        run(["git", "init", path], env=env)
        run(["git", "-C", path, "remote", "add", "origin", url], env=env)
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                          env=env, text=True, capture_output=True)
    expected = subprocess.run(["git", "-C", str(path), "rev-parse", revision + "^{commit}"],
                              env=env, text=True, capture_output=True)
    if expected.returncode != 0:
        ref = f"refs/tags/{revision}:refs/tags/{revision}" if revision.startswith("v") else revision
        run(["git", "-C", path, "fetch", "--depth", "1", "origin", ref], env=env)
        expected = subprocess.run(["git", "-C", str(path), "rev-parse", revision + "^{commit}"],
                                  env=env, text=True, capture_output=True)
    if head.returncode == 0:
        if expected.returncode != 0 or head.stdout.strip() != expected.stdout.strip():
            raise RuntimeError(f"Unexpected SDK revision in {path}; use a separate ESP_SDK_ROOT.")
        return
    run(["git", "-C", path, "checkout", "--detach", revision], env=env)


def gn_platform():
    machine = platform.machine().lower()
    arch = {"amd64": "amd64", "x86_64": "amd64", "arm64": "arm64", "aarch64": "arm64"}.get(machine)
    host = {"win32": "windows", "darwin": "mac", "linux": "linux"}.get(sys.platform)
    if not host or not arch or (host == "windows" and arch != "amd64"):
        raise RuntimeError(f"Unsupported GN host: {sys.platform}/{machine}")
    return f"{host}-{arch}"


def install_gn():
    executable = SDK / "gn" / ("gn.exe" if WINDOWS else "gn")
    if executable.exists():
        return
    executable.parent.mkdir(parents=True, exist_ok=True)
    archive = SDK / "gn.zip"
    url = ("https://chrome-infra-packages.appspot.com/dl/gn/gn/"
           f"{gn_platform()}/+/git_revision:{GN_REVISION}")
    print(f"Downloading {url}", flush=True)
    urllib.request.urlretrieve(url, archive)
    with zipfile.ZipFile(archive) as package:
        # Only extract the executable, never archive-provided paths.
        executable.write_bytes(package.read(executable.name))
    executable.chmod(0o755)
    archive.unlink()


def windows_links(repository, env):
    """Replace Git's symlink placeholder files without requiring Developer Mode."""
    import _winapi
    entries = run(["git", "-C", repository, "ls-files", "--stage", "-z"], env=env, capture=True)
    pending = {}
    for entry in entries.split("\0"):
        if not entry.startswith("120000 "):
            continue
        relative = entry.split("\t", 1)[1]
        link = repository / relative
        if link.is_symlink() or link.is_dir():
            continue
        target_name = run(["git", "-C", repository, "show", f"HEAD:{relative}"], env=env, capture=True).strip()
        pending[link] = link.parent / target_name
    # config/esp32/build points through third_party/connectedhomeip, itself a link.
    # Resolve such dependencies before deciding that a target is unavailable.
    while pending:
        progressed = False
        for link, raw_target in list(pending.items()):
            target = raw_target.resolve()
            if not target.is_relative_to(SDK):
                raise RuntimeError(f"SDK symlink points outside SDK root: {link}")
            if not target.exists() or target in pending:
                continue
            if target.is_dir():
                link.unlink()
                _winapi.CreateJunction(str(target), str(link))
            else:
                shutil.copy2(target, link)
            del pending[link]
            progressed = True
        if not progressed:
            break  # Remaining links belong to optional, uninitialized platforms.


def windows_compatibility(env):
    windows_links(CHIP, env)
    # Some of Pigweed's scripts are symlinks too.
    windows_links(CHIP / "third_party/pigweed/repo", env)
    shutil.copy2(ROOT / "scripts/native_args.py", CHIP / "config/esp32/components/chip/create_args_gn.py")
    shutil.copy2(PYTHON, PYTHON.with_name("python3.exe"))


def prepare_environment():
    # Pigweed normally generates this during its full host-tools bootstrap.
    # This firmware uses IDF's cross compiler and Python/GN from PATH instead.
    environment = CHIP / "build_overrides/pigweed_environment.gni"
    if not environment.exists():
        environment.write_text("# Firmware-only environment; tools are supplied by project.py.\ndeclare_args() {\n}\n", encoding="utf-8")


def bootstrap():
    if sys.version_info[:2] != (3, 11):
        raise RuntimeError("Use Python 3.11: uv run --python 3.11 scripts/project.py bootstrap")
    for path in (ROOT, SDK):
        if any(c.isspace() for c in str(path)):
            raise RuntimeError(f"This SDK requires paths without whitespace: {path}")
    gn_platform()  # Fail before downloading SDKs on an unsupported host.
    sdk_tools = ["required", "cmake"]
    if gn_platform() == "linux-arm64":
        if not shutil.which("ninja"):
            raise RuntimeError("Install native Ninja first (for example: sudo apt-get install ninja-build).")
    else:
        sdk_tools.append("ninja")
    env = base_env()
    checkout(IDF, "https://github.com/espressif/esp-idf.git", IDF_VERSION, env)
    run(["git", "-C", IDF, "submodule", "update", "--init", "--recursive", "--depth", "1", "--jobs", "8"], env=env)
    run([sys.executable, IDF / "tools/idf_tools.py", "install", "--targets", "esp32c3", *sdk_tools], env=env)
    run([sys.executable, IDF / "tools/idf_tools.py", "install-python-env", "--features", "core"], env=env)
    checkout(MATTER, "https://github.com/espressif/esp-matter.git", MATTER_REVISION, env)
    run(["git", "-C", MATTER, "submodule", "update", "--init", "--depth", "1"], env=env)
    run([PYTHON, CHIP / "scripts/checkout_submodules.py", "--platform", "esp32", "--shallow", "--jobs", "8"], cwd=CHIP, env=env)
    # Firmware generation only: no chip-tool, manufacturing tools, or host compiler suite.
    run([PYTHON, "-m", "pip", "install", "lark==1.1.2", "stringcase==1.2.0", "jinja2==3.1.4", "urllib3<2"], env=env)
    install_gn()
    prepare_environment()
    if WINDOWS:
        windows_compatibility(env)
    print("Native SDK ready. Run: uv run --python 3.11 scripts/project.py build")


def build_env():
    if not PYTHON.exists() or not (SDK / "gn").exists():
        raise RuntimeError("SDK missing. Run: uv run --python 3.11 scripts/project.py bootstrap")
    env = base_env()
    exports = run([sys.executable, IDF / "tools/idf_tools.py", "export", "--format", "key-value"], env=env, capture=True)
    for line in exports.splitlines():
        key, sep, value = line.partition("=")
        if sep and (key.isidentifier()):
            env[key] = value.replace("%PATH%", env["PATH"]).replace("$PATH", env["PATH"])
    env["PATH"] = os.pathsep.join([str(SDK / "gn"), str(PYTHON.parent), env["PATH"]])
    env["VIRTUAL_ENV"] = str(VENV)
    env.setdefault("CCACHE_DIR", str(ROOT / ".cache/ccache"))
    return env


def idf(action, build_dir):
    build_dir.mkdir(parents=True, exist_ok=True)
    run([PYTHON, IDF / "tools/idf.py", "-B", build_dir,
         "-D", f"SDKCONFIG={(build_dir / 'sdkconfig').as_posix()}",
         "-D", f"Python3_EXECUTABLE={PYTHON.as_posix()}", action], env=build_env())


def serial_command(module, args):
    # Also works on a machine with downloaded CI artifacts and no SDK.
    run(["uv", "run", "--isolated", "--no-project", "--python", "3.11",
         "--with", "esptool==4.8.1", "--with", "pyserial==3.5", "python", "-m", module, *args])


def flash(build_dir, port, baud):
    manifest = build_dir / "flasher_args.json"
    if not manifest.is_file():
        raise RuntimeError(f"No firmware in {build_dir}. Build first or use --build-dir with extracted CI artifacts.")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    args = ["--chip", "esp32c3", "--port", port, "--baud", str(baud),
            "--before", "default_reset", "--after", "hard_reset", "write_flash",
            *data["write_flash_args"]]
    for offset, name in data["flash_files"].items():
        binary = (build_dir / name).resolve()
        if not binary.is_relative_to(build_dir.resolve()) or not binary.is_file():
            raise RuntimeError(f"Missing or invalid flash binary: {name}")
        args.extend([offset, str(binary)])
    serial_command("esptool", args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["bootstrap", "build", "menuconfig", "clean", "ports", "flash", "deploy", "monitor", "dev", "erase-flash"])
    parser.add_argument("--build-dir", type=Path, default=BUILD)
    parser.add_argument("--port", help="COM3, /dev/cu.usbmodem*, or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=460800, help="flash baud rate (monitor uses 115200)")
    args = parser.parse_args()
    if sys.version_info[:2] != (3, 11):
        parser.error("Use Python 3.11: uv run --python 3.11 scripts/project.py " + args.command)
    if args.command in ("flash", "deploy", "monitor", "dev", "erase-flash") and not args.port:
        parser.error("--port is required; use the ports command to list devices")
    build_dir = args.build_dir.expanduser().resolve()
    if args.command == "bootstrap":
        bootstrap()
    elif args.command == "ports":
        serial_command("serial.tools.list_ports", ["-v"])
    elif args.command == "erase-flash":
        serial_command("esptool", ["--chip", "esp32c3", "--port", args.port, "erase_flash"])
    else:
        if args.command in ("build", "deploy", "dev", "menuconfig", "clean"):
            idf({"menuconfig": "menuconfig", "clean": "clean"}.get(args.command, "build"), build_dir)
        if args.command in ("flash", "deploy", "dev"):
            flash(build_dir, args.port, args.baud)
        if args.command in ("monitor", "dev"):
            serial_command("serial.tools.miniterm", [args.port, "115200"])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
