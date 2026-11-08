#!/usr/bin/env python3
"""Build the pinned WebP decoder and animation demuxer locally."""
import hashlib, os, subprocess, sys, tarfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
archive = root / 'vendor/libwebp-1.6.0.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
source = root / 'build/webp-source/libwebp-1.6.0'
if not source.exists():
    source.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            assert not member.issym() and not member.islnk()
            assert str((source.parent / member.name).resolve()).startswith(str(source.parent.resolve()) + '/')
        tar.extractall(source.parent)
# Upstream enables PIC for object libraries even in static builds. Allegrex
# EABI cannot generate it; patch only the extracted build copy, not the archive.
cmake_file = source / 'CMakeLists.txt'
cmake_text = cmake_file.read_text().replace('POSITION_INDEPENDENT_CODE ON', 'POSITION_INDEPENDENT_CODE OFF')
cmake_text = cmake_text.replace('target_link_libraries(webpdemux webp)', 'target_link_libraries(webpdemux webpdecoder)')
cmake_file.write_text(cmake_text)
pspdev = Path(sys.argv[1])
build = root / 'build/webp-psp'
env = {**os.environ, 'PATH': f'{pspdev}/bin:{os.environ["PATH"]}'}
subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_SYSTEM_NAME=Generic',
                f'-DCMAKE_C_COMPILER={pspdev}/bin/psp-gcc', '-DCMAKE_C_FLAGS=-G0',
                '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY', '-DCMAKE_BUILD_TYPE=Release',
                '-DWEBP_ENABLE_SIMD=OFF', '-DWEBP_USE_THREAD=OFF', '-DWEBP_BUILD_CWEBP=OFF',
                '-DWEBP_BUILD_DWEBP=OFF', '-DWEBP_BUILD_GIF2WEBP=OFF', '-DWEBP_BUILD_IMG2WEBP=OFF',
                '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_WEBPINFO=OFF', '-DWEBP_BUILD_EXTRAS=OFF',
                '-DWEBP_BUILD_WEBPMUX=OFF'], env=env, check=True)
subprocess.run(['cmake', '--build', str(build), '--target', 'webpdemux', '-j', '4'], env=env, check=True)

os.utime(build / 'libwebpdecoder.a', None)
