#!/usr/bin/env python3
"""Compare the latest Pulsync git tag with the Arduino Library Manager index.

Prints one JSON object:
  latest_tag   semver from the newest v* tag (no leading v)
  indexed      newest Pulsync version in the index, or null
  caught_up    true when the index lists that tag
"""

import gzip
import json
import os
import re
import subprocess
import sys
import urllib.request

INDEX_URL = "https://downloads.arduino.cc/libraries/library_index.json.gz"
LIBRARY_NAME = "Pulsync"


def version_key(version):
    parts = []
    for piece in version.split("."):
        match = re.match(r"(\d+)", piece)
        parts.append(int(match.group(1)) if match else 0)
    return tuple(parts)


def latest_tag():
    if os.environ.get("PULSYNC_LATEST_TAG"):
        tag = os.environ["PULSYNC_LATEST_TAG"]
    else:
        out = subprocess.check_output(
            ["git", "tag", "-l", "v*", "--sort=-v:refname"],
            text=True,
        )
        tags = [line.strip() for line in out.splitlines() if line.strip()]
        if not tags:
            raise SystemExit("no v* tags found")
        tag = tags[0]
    return tag[1:] if tag.startswith("v") else tag


def indexed_versions():
    path = os.environ.get("PULSYNC_INDEX_GZ")
    if path:
        with gzip.open(path, "rb") as handle:
            payload = handle.read()
    else:
        with urllib.request.urlopen(INDEX_URL, timeout=120) as response:
            payload = response.read()
    if payload[:2] == b"\x1f\x8b":
        payload = gzip.decompress(payload)
    data = json.loads(payload)
    versions = []
    for lib in data.get("libraries", []):
        if lib.get("name") != LIBRARY_NAME:
            continue
        version = lib.get("version")
        if version:
            versions.append(version)
    return versions


def main():
    tag = latest_tag()
    versions = indexed_versions()
    indexed = max(versions, key=version_key) if versions else None
    result = {
        "latest_tag": tag,
        "indexed": indexed,
        "caught_up": indexed == tag,
    }
    json.dump(result, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
