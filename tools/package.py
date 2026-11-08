#!/usr/bin/env python3
"""Build a ready-to-copy folder. Preserve config on repeat packaging."""
import hashlib
import json
import re
import tempfile
import shutil
from pathlib import Path

root = Path(__file__).resolve().parent.parent
settings = (root / "include/stoat.h").read_text()
slug = re.search(r'ST_APP_SLUG "([^"]+)"', settings).group(1)
target = root / "dist" / slug
target.mkdir(parents=True, exist_ok=True)
for source in [root / "platform/psp/EBOOT.PBP", root / "assets/cacert.pem", root / "assets/NotoSans-Regular.ttf", root / "assets/emoji.pack"]:
    shutil.copy2(source, target / source.name)
runtime_names = {"session.json", "session.json.tmp", "entropy.bin", "entropy.bin.tmp", "netdiag.txt", "voicediag.txt", "calldiag.txt",
                 "theme-settings.json", "theme-settings.json.tmp", "recent-reactions.json", "recent-reactions.json.tmp",
                 "cache-settings.json", "cache-settings.json.tmp", "cache"}
legacy = [p for p in target.iterdir() if p.name in runtime_names or re.fullmatch(r"voicediag_[0-9]+\.txt",p.name)]
if legacy:
    previous = root / "build/previous"
    previous.mkdir(parents=True, exist_ok=True)
    archive = Path(tempfile.mkdtemp(prefix="GAME-runtime-data-", dir=previous))
    for old in legacy:
        shutil.move(str(old), str(archive / old.name))
    print(f"Previous GAME runtime data preserved: {archive}")
if not (target / "config.ini").exists():
    shutil.copy2(root / "config.ini", target / "config.ini")
shutil.copy2(root / "LICENSE", target / "LICENSE")
shutil.copytree(root / "licenses", target / "licenses", dirs_exist_ok=True)
manifest = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in target.iterdir() if p.is_file() and p.name not in {"entropy.bin", "entropy.bin.tmp", "session.json", "session.json.tmp", "netdiag.txt", "manifest.json", "cache-settings.json", "cache-settings.json.tmp", "theme-settings.json", "theme-settings.json.tmp", "recent-reactions.json", "recent-reactions.json.tmp"}}
(target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"PSP install folder: {target}")
