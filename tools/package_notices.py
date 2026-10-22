#!/usr/bin/env python3
"""Collect dependency licenses and source notices for firmware distribution."""
import json
import pathlib
import re
import subprocess
import sys
import zipfile

root = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2])
files = set()
for build in ("build-p", "build-e"):
    for entry in json.loads((root / build / "compile_commands.json").read_text()):
        files.add(pathlib.Path(entry["file"]).resolve())
    deps = subprocess.check_output(["ninja", "-C", str(root / build), "-t", "deps"], text=True)
    for line in deps.splitlines():
        if line.startswith("    /"):
            files.add(pathlib.Path(line.strip()).resolve())
notices = {}
for path in sorted(files):
    if not path.is_relative_to(root) or not path.is_file():
        continue
    relative = path.relative_to(root).as_posix()
    if not relative.startswith(("zephyr/", "modules/", "vendor/")):
        continue
    text = path.read_text(errors="replace")
    match = re.match(r"\s*(/\*.*?\*/)", text, re.DOTALL)
    if match:
        notices.setdefault(match[1], []).append(relative)
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    archive.write(root / "LICENSE", "LICENSE")
    archive.write(root / "vendor/SOURCES.txt", "vendor/SOURCES.txt")
    archive.writestr("source-notices.txt", "\n\n".join(
        "\n".join(paths) + "\n" + notice for notice, paths in notices.items()))
    for base in (root / "zephyr", root / "modules", root / "vendor"):
        for path in base.rglob("*"):
            if path.is_file() and (path.name.lower().startswith(("license", "copying", "notice"))
                                   or "LICENSES" in path.parts):
                archive.write(path, path.relative_to(root).as_posix())
print(output)
