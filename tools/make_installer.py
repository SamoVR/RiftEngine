#!/usr/bin/env python3
"""Packs a RIFT release: RIFT-Setup-<version>.exe and RIFT-<version>-portable.zip.

Build the Release configuration first (RiftEditor, RiftPlayer and RiftSetup), then:

    python tools/make_installer.py            -> dist/
    python tools/make_installer.py --out D:/x

The setup is build/bin/Release/RiftSetup.exe with the files appended to it
(the format is described in src/setup/Installer.h).
"""
import argparse
import os
import re
import struct
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version():
    text = open(os.path.join(ROOT, "src", "core", "Version.h"), encoding="utf-8").read()
    return re.search(r'#define RIFT_VERSION_STRING "([^"]+)"', text).group(1)


def collect(config):
    """(path in the install folder, file on disk) for everything RIFT needs."""
    binaries = os.path.join(ROOT, "build", "bin", config)
    files = []

    def add(dest, src):
        if not os.path.isfile(src):
            sys.exit("missing: " + src + " (build the " + config + " configuration first)")
        files.append((dest.replace("\\", "/"), src))

    def add_tree(dest_root, src_root, skip=()):
        for folder, dirs, names in os.walk(src_root):
            dirs[:] = sorted(d for d in dirs if d not in ("Builds", ".git", ".rift"))
            for name in sorted(names):
                if any(name.endswith(s) for s in skip):
                    continue
                full = os.path.join(folder, name)
                rel = os.path.relpath(full, src_root)
                add(os.path.join(dest_root, rel), full)

    add("RiftEditor.exe", os.path.join(binaries, "RiftEditor.exe"))
    add("RiftPlayer.exe", os.path.join(binaries, "RiftPlayer.exe"))
    discord = os.path.join(binaries, "discord_game_sdk.dll")
    if os.path.isfile(discord):
        add("discord_game_sdk.dll", discord)
    add_tree("data", os.path.join(ROOT, "data"))
    add_tree("examples", os.path.join(ROOT, "examples"), skip=(".autosave.json", ".tmp"))
    add_tree("docs", os.path.join(ROOT, "docs"))
    add("README.md", os.path.join(ROOT, "README.md"))
    add("licenses/THIRD_PARTY.md", os.path.join(ROOT, "third_party", "README.md"))
    return files


def pack(stub_path, files, out_path):
    stub = open(stub_path, "rb").read()
    total = 0
    with open(out_path, "wb") as out:
        out.write(stub)
        offset = len(stub)
        for dest, src in files:
            data = open(src, "rb").read()
            packed = zlib.compress(data, 9)
            path = dest.encode("utf-8")
            out.write(b"FILE" + struct.pack("<I", len(path)) + path)
            out.write(struct.pack("<QQI", len(data), len(packed), zlib.crc32(data) & 0xFFFFFFFF))
            out.write(packed)
            total += len(data)
        out.write(struct.pack("<8sQIIQ", b"RIFTPAK1", offset, len(files), 0, total))
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = parser.parse_args()

    v = version()
    files = collect(args.config)
    os.makedirs(args.out, exist_ok=True)
    stub = os.path.join(ROOT, "build", "bin", args.config, "RiftSetup.exe")
    if not os.path.isfile(stub):
        sys.exit("missing: " + stub + " (build RiftSetup first)")

    setup = os.path.join(args.out, "RIFT-Setup-%s.exe" % v)
    total = pack(stub, files, setup)
    print("%s  (%d files, %.1f MB installed, %.1f MB download)" % (setup, len(files), total / 1048576.0, os.path.getsize(setup) / 1048576.0))

    portable = os.path.join(args.out, "RIFT-%s-portable.zip" % v)
    with zipfile.ZipFile(portable, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dest, src in files:
            z.write(src, "RIFT/" + dest)
    print("%s  (%.1f MB)" % (portable, os.path.getsize(portable) / 1048576.0))


if __name__ == "__main__":
    main()
