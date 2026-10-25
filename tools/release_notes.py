#!/usr/bin/env python3
"""Prints the CHANGELOG.md section of one version (for the GitHub release).

    python tools/release_notes.py v1.1.0
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    version = sys.argv[1].lstrip("vV")
    lines = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read().splitlines()
    out, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = line[3:].strip().split()[0] == version
            continue
        if inside:
            out.append(line)
    text = "\n".join(out).strip()
    if not text:
        text = "RIFT " + version
    download = ("\n\n---\n**Install:** download `RIFT-Setup-%s.exe` below and run it (no administrator rights needed). "
                "Already have RIFT? It offers the update by itself. `RIFT-%s-portable.zip` runs without installing." % (version, version))
    sys.stdout.write(text + download + "\n")


if __name__ == "__main__":
    main()
