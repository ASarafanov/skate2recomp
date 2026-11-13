#!/usr/bin/env python3
"""Builds dist/Skate2-PC-Setup-v<version>.exe.

The setup is setup/out/Launch Skate 2.exe with the program files appended:

  [launcher exe][file data ...][index][trailer]

  index:   one line per file, "<sha256> <size> <offset> <name>\\n"
           (offset from the start of the setup exe, name '/' separated)
  trailer: b"S2SETUP1", u64 launcher size, u64 index offset, u64 index size
           (little-endian, 32 bytes, the last bytes of the file)

The program files are an allowlist: skate2.exe and its runtime DLLs taken
unchanged from the upstream player package (checked by SHA-256), the
Microsoft Visual C++ runtime DLLs from the local Visual Studio redist folder
(Proton has none), and the license files. Game files are never packed.

usage: python tools/package_setup.py [--upstream-zip PATH] [--redist DIR]
"""
import argparse
import glob
import hashlib
import io
import os
import struct
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = "0.2.1"

UPSTREAM_URL = ("https://github.com/christianortel/skate2-recompile-pc/releases/download/"
                "v0.1.1/Skate2-PC-v0.1.1-Windows-x64.zip")
UPSTREAM_SHA256 = "55a1062c6d0e6ed74a1bf291617a60a1b74cdd6c6bdad9366652516ca17bd270"
# Files taken from the upstream package, unchanged.
UPSTREAM_FILES = ["skate2.exe", "rexruntime.dll", "rexgpu-xenos.dll"]
UPSTREAM_LICENSES = ["LICENSE", "THIRD_PARTY_NOTICES.md"]

VC_RUNTIME = ["msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll",
              "vcruntime140_1.dll"]

# DLLs every supported system (Windows 10/11, Wine/Proton) provides.
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "shell32.dll", "advapi32.dll", "gdi32.dll", "hid.dll",
    "imm32.dll", "setupapi.dll", "version.dll", "winmm.dll", "ws2_32.dll", "bcrypt.dll",
    "dxgi.dll", "d3d12.dll", "dbghelp.dll", "ole32.dll", "comctl32.dll",
}
FORBIDDEN_EXTENSIONS = {".xex", ".iso", ".pdb", ".toml", ".bin", ".log", ".ini"}
MAX_FILE = 256 << 20


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pe_imports(data):
    """Names of the DLLs a PE32+ image imports (normal import table)."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("not a 64-bit image")
    import_rva = struct.unpack_from("<I", data, optional + 112 + 8)[0]
    sections = []
    for i in range(sections_count):
        s = optional + optional_size + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError("bad RVA")

    names = []
    if not import_rva:
        return names
    pos = offset(import_rva)
    while True:
        entry = struct.unpack_from("<IIIII", data, pos)
        if not any(entry):
            break
        name_at = offset(entry[3])
        names.append(data[name_at:data.index(b"\0", name_at)].decode("ascii").lower())
        pos += 20
    return names


def find_redist(explicit):
    if explicit:
        return explicit
    candidates = []
    if os.environ.get("VCToolsRedistDir"):
        candidates.append(os.path.join(os.environ["VCToolsRedistDir"], "x64",
                                       "Microsoft.VC143.CRT"))
    candidates += sorted(glob.glob(
        r"C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\14.*\x64"
        r"\Microsoft.VC143.CRT"), reverse=True)
    for c in candidates:
        if all(os.path.isfile(os.path.join(c, n)) for n in VC_RUNTIME):
            return c
    sys.exit("Visual C++ redist folder not found; pass --redist.")


def load_upstream(path):
    if not os.path.isfile(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"Downloading {UPSTREAM_URL}")
        urllib.request.urlretrieve(UPSTREAM_URL, path + ".part")
        os.replace(path + ".part", path)
    data = open(path, "rb").read()
    if sha256(data) != UPSTREAM_SHA256:
        sys.exit(f"{path}: SHA-256 differs from the upstream v0.1.1 release.")
    return zipfile.ZipFile(io.BytesIO(data))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream-zip",
                        default=os.path.join(ROOT, "downloads",
                                             "Skate2-PC-v0.1.1-Windows-x64.zip"))
    parser.add_argument("--launcher", default=os.path.join(ROOT, "setup", "out",
                                                           "Launch Skate 2.exe"))
    parser.add_argument("--redist")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = parser.parse_args()

    upstream = load_upstream(args.upstream_zip)
    redist = find_redist(args.redist)
    launcher = open(args.launcher, "rb").read()

    files = []  # (name, data)
    for name in UPSTREAM_FILES + UPSTREAM_LICENSES:
        files.append((name, upstream.read(name)))
    for info in upstream.infolist():
        if info.filename.startswith("licenses/") and not info.is_dir():
            files.append((info.filename, upstream.read(info.filename)))
    for name in VC_RUNTIME:
        files.append((name, open(os.path.join(redist, name), "rb").read()))
    for name, source in [("README.txt", "setup/README-player.txt"),
                         ("licenses/Microsoft-Visual-C++-Runtime-NOTICE.txt",
                          "setup/vc-runtime-notice.txt")]:
        files.append((name, open(os.path.join(ROOT, source), "rb").read()))

    # Allowlist checks.
    shipped = {n.lower() for n, _ in files}
    for name, data in files:
        low = name.lower()
        ext = os.path.splitext(low)[1]
        if ext in FORBIDDEN_EXTENSIONS or low.startswith(("game/", "userdata/", "cache/")):
            sys.exit(f"refusing to pack {name}")
        if len(data) > MAX_FILE:
            sys.exit(f"refusing to pack {name}: too large")
    for name, data in [("Launch Skate 2.exe", launcher)] + [
            (n, d) for n, d in files if n.lower().endswith((".exe", ".dll"))]:
        for dll in pe_imports(data):
            ok = (dll in SYSTEM_DLLS or dll in shipped or dll.startswith("api-ms-win-crt-"))
            if name == "Launch Skate 2.exe" and (dll.startswith(("msvcp", "vcruntime",
                                                                 "api-ms-win-crt-"))):
                ok = False  # the launcher must run without any runtime
            if not ok:
                sys.exit(f"{name} imports {dll}, which is neither shipped nor a system DLL")

    if launcher[-32:-24] == b"S2SETUP1":
        sys.exit("--launcher already has a payload")

    os.makedirs(args.out, exist_ok=True)
    setup_name = f"Skate2-PC-Setup-v{VERSION}.exe"
    setup_path = os.path.join(args.out, setup_name)
    offset = len(launcher)
    index_lines = []
    with open(setup_path + ".part", "wb") as out:
        out.write(launcher)
        for name, data in files:
            out.write(data)
            index_lines.append(f"{sha256(data)} {len(data)} {offset} {name}\n")
            offset += len(data)
        index = "".join(index_lines).encode("ascii")
        out.write(index)
        out.write(b"S2SETUP1" + struct.pack("<QQQ", len(launcher), offset, len(index)))
    os.replace(setup_path + ".part", setup_path)

    setup = open(setup_path, "rb").read()
    zip_name = f"Skate2-PC-Setup-v{VERSION}.zip"
    zip_path = os.path.join(args.out, zip_name)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(setup_name, setup)
    sums = (f"{sha256(setup)} *{setup_name}\n"
            f"{sha256(open(zip_path, 'rb').read())} *{zip_name}\n")
    open(os.path.join(args.out, "SHA256SUMS.txt"), "w", newline="\n").write(sums)
    print(f"{len(files)} program files, redist from {redist}")
    print(sums, end="")
    print(f"setup {len(setup) >> 20} MB, zip {os.path.getsize(zip_path) >> 20} MB")


if __name__ == "__main__":
    main()
