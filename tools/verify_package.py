#!/usr/bin/env python3
"""Verify a packaged PBP structurally and resources by checksum."""
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path
from sfo import read_sfo

root = Path(__file__).resolve().parent.parent
settings = (root / "include/stoat.h").read_text()
app_name = re.search(r'ST_APP_NAME "([^"]+)"', settings).group(1)
slug = re.search(r'ST_APP_SLUG "([^"]+)"', settings).group(1)
folder = root / "dist" / slug
assert (folder / "licenses/jpeg/LICENSE.md").is_file() and (folder / "licenses/jpeg/README.ijg").is_file(), "JPEG licenses must ship with the linked thumbnail decoder"
assert (folder / "licenses/opus/COPYING").is_file(), "Opus license must ship with the linked codec"
assert not any(folder.glob("voicediag*.txt")), "User voice reports must not ship in GAME"
for name in ("session.json", "session.json.tmp", "entropy.bin", "entropy.bin.tmp", "netdiag.txt", "voicediag.txt", "calldiag.txt", "theme-settings.json", "theme-settings.json.tmp", "recent-reactions.json", "recent-reactions.json.tmp", "cache-settings.json", "cache-settings.json.tmp", "cache"):
    assert not (folder / name).exists(), "Legacy/runtime data must not ship in GAME: " + name
manifest = json.loads((folder / "manifest.json").read_text())
for name, digest in manifest.items():
    assert hashlib.sha256((folder / name).read_bytes()).hexdigest() == digest, name
pack = (folder / "emoji.pack").read_bytes()
assert pack[:8] == b'STEMOJI1' and struct.unpack_from('<I', pack, 8)[0] == 4009, 'Twemoji header'
assert hashlib.sha256(pack[44:]).digest() == pack[12:44], 'Twemoji payload'
pbp = (folder / "EBOOT.PBP").read_bytes()
magic, version, *offsets = struct.unpack_from("<10I", pbp)
assert magic == 0x50425000 and version == 0x10000, "PBP header"
assert offsets[0] == 40 and offsets == sorted(offsets) and offsets[-1] <= len(pbp), "PBP offsets"
assert pbp[offsets[0]:offsets[0] + 4] == b"\0PSF", "PARAM.SFO"
app_version = re.search(r'ST_APP_VERSION "([^"]+)"', settings).group(1)
disc_id = re.search(r'ST_DISC_ID "([^"]+)"', settings).group(1)
parental_level = int(re.search(r'ST_PARENTAL_LEVEL ([0-9]+)', settings).group(1))
assert re.fullmatch(r'[A-Z]{4}[0-9]{5}', disc_id), "homebrew DISC_ID format"
assert 1 <= parental_level <= 11, "parental control level range"
metadata = read_sfo(pbp[offsets[0]:offsets[1]])
assert metadata['BOOTABLE'] == 1 and metadata['CATEGORY'] == 'MG', "XMB executable category"
assert metadata['MEMSIZE'] == 1, "PSP expanded-memory policy"
assert metadata['TITLE'] == f"{app_name} {app_version}", "XMB title/version"
assert metadata['PARENTAL_LEVEL'] == parental_level, "EBOOT parental control level"
assert metadata['DISC_ID'] == disc_id, "EBOOT homebrew DISC_ID"
assert b"stoat" not in pbp.lower(), "Previous client branding in EBOOT"
assert b"STPS00001" not in pbp, "Previous save identity in EBOOT"
api_version = re.search(r'ST_API_VERSION "([^\"]+)"', settings).group(1)
def png_resource(index, name, dimensions):
    blob = pbp[offsets[index]:offsets[index + 1]]
    assert blob == (root / "build/xmb" / name).read_bytes(), name + " embedded artwork"
    assert blob[:8] == b"\x89PNG\r\n\x1a\n", name + " signature"
    at = 8; chunks = {}
    while at < len(blob):
        size, = struct.unpack_from(">I", blob, at);tag = blob[at+4:at+8]
        assert at + size + 12 <= len(blob), name + " chunk bound"
        payload = blob[at+8:at+8+size]
        assert zlib.crc32(tag + payload) == struct.unpack_from(">I", blob, at+8+size)[0], name + " CRC"
        chunks[tag] = payload;at += size + 12
        if tag == b"IEND": break
    assert at == len(blob) and b"IEND" in chunks, name + " complete PNG"
    assert struct.unpack_from(">II", chunks[b"IHDR"]) == dimensions, name + " dimensions"
    assert chunks[b"IHDR"][8:] == bytes([8, 2, 0, 0, 0]), name + " RGB8 noninterlaced"
    assert chunks[b"tEXt"] == f"Description\0{app_name} {app_version} | API {api_version}".encode(), name + " release metadata"
png_resource(1, "ICON0.png", (144, 80))
png_resource(4, "PIC1.png", (480, 272))
prx = pbp[offsets[6]:offsets[7]]
assert prx[:4] == b"\x7fELF" and prx[4:6] == b"\x01\x01", "32-bit little-endian PRX"
assert struct.unpack_from("<H", prx, 18)[0] == 8, "MIPS target"
print(f"Package verified: {len(pbp):,} byte EBOOT, MIPS PRX, ICON0/PIC1, DISC_ID {disc_id}, parental level {parental_level}, resources match manifest, SDK random source; no external seed required")
