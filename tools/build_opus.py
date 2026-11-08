#!/usr/bin/env python3
"""Build pinned fixed-point Opus inside build/, without changing SDK/Homebrew."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile

root=Path(__file__).resolve().parent.parent
archive=root/'vendor/opus-1.6.1.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1'
source=root/'build/opus-source/opus-1.6.1'
if not source.exists():
    source.parent.mkdir(parents=True,exist_ok=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            assert not member.issym() and not member.islnk()
            assert str((source.parent/member.name).resolve()).startswith(str(source.parent.resolve())+'/')
        tar.extractall(source.parent)
pspdev=Path(sys.argv[2]) if sys.argv[1]=='psp' else None
build=root/('build/opus-psp' if pspdev else 'build/opus-host')
options=['-DCMAKE_BUILD_TYPE=Release','-DOPUS_FIXED_POINT=ON','-DOPUS_ENABLE_FLOAT_API=OFF',
    '-DOPUS_DISABLE_INTRINSICS=ON','-DOPUS_DRED=OFF','-DOPUS_OSCE=OFF',
    '-DOPUS_BUILD_PROGRAMS=OFF','-DOPUS_BUILD_TESTING=OFF','-DBUILD_SHARED_LIBS=OFF']
env=dict(os.environ)
if pspdev:
    env['PATH']=f'{pspdev}/bin:'+env['PATH']
    options += ['-DCMAKE_SYSTEM_NAME=Generic',f'-DCMAKE_C_COMPILER={pspdev}/bin/psp-gcc',
        '-DCMAKE_C_FLAGS=-G0','-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG',
        f'-DCMAKE_PROJECT_INCLUDE={root}/tools/relative_build_paths.cmake',
        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY']
else:
    options += ['-DCMAKE_C_COMPILER='+os.environ.get('HOST_CC','cc'),
        '-DCMAKE_C_FLAGS=-g -fsanitize=address,undefined','-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG']
subprocess.run(['cmake','-S',str(source),'-B',str(build),*options],env=env,check=True)
subprocess.run(['cmake','--build',str(build),'--target','opus','-j','4'],env=env,check=True)
os.utime(build/'libopus.a',None)
