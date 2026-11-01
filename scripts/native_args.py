"""CMake compile_commands -> GN flags, including native Windows command lines.

Installed into the pinned CHIP checkout by project.py on Windows.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import sys


def split_command(command):
    if sys.platform != "win32":
        return shlex.split(command)
    # CMake uses Windows quoting rules, not POSIX shlex rules.
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    shell32.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    count = ctypes.c_int()
    argv = shell32.CommandLineToArgvW(command, ctypes.byref(count))
    if not argv:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return list(argv[:count.value])
    finally:
        kernel32.LocalFree(argv)


def path_key(value):
    return os.path.normcase(os.path.normpath(value))


def flags_for(commands, source, idf_path, filters):
    matches = [entry for entry in commands if path_key(entry["file"]) == path_key(source)]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one compile command for {source}, found {len(matches)}")
    entry = matches[0]
    tokens = entry.get("arguments") or split_command(entry["command"])
    result = []
    index = 1  # Compiler executable is not a flag.
    while index < len(tokens):
        flag = tokens[index]
        if flag in ("-c", "-o"):
            index += 2
            continue
        index += 1
        if flag in filters or (flag.startswith("-DIDF_VER=") and any("IDF_VER=" in f for f in filters)):
            continue
        if flag.startswith("-I"):
            include = flag[2:].replace("\\", "/")
            sdk = str(idf_path).replace("\\", "/")
            flag = ("-isystem" if include.startswith(sdk + "/") else "-I") + include
        result.append(flag)
    return result


def gn_list(values):
    return "[" + ", ".join(json.dumps(v, ensure_ascii=False).replace("$", "\\$") for v in values) + "]"


def main():
    parser = argparse.ArgumentParser()
    for name in ("build_dir", "idf_path", "c_file", "cpp_file", "input", "output"):
        parser.add_argument(name)
    parser.add_argument("--filter-out", default="")
    args = parser.parse_args()
    commands = json.loads((Path(args.build_dir) / "compile_commands.json").read_text(encoding="utf-8"))
    filters = args.filter_out.split(";")
    content = Path(args.input).read_text(encoding="utf-8")
    for name, source in (("target_cflags_c", args.c_file), ("target_cflags_cc", args.cpp_file)):
        content += f"\n{name} = {gn_list(flags_for(commands, source, args.idf_path, filters))}\n"
    Path(args.output).write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
