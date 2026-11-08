#!/usr/bin/env python3
"""Build pinned giflib locally; never modify the installed SDK or Homebrew."""
import hashlib, os, subprocess, sys, tarfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
archive = root / 'vendor/giflib-5.2.2.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == 'be7ffbd057cadebe2aa144542fd90c6838c6a083b5e8a9048b8ee3b66b29d5fb'
source = root / 'build/gif-source/giflib-5.2.2'
if not source.exists():
    source.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            assert not member.issym() and not member.islnk()
            assert str((source.parent / member.name).resolve()).startswith(str(source.parent.resolve()) + '/')
        tar.extractall(source.parent)
pspdev = Path(sys.argv[2]) if sys.argv[1] == 'psp' else None
build = root / ('build/gif-psp' if pspdev else 'build/gif-host')
build.mkdir(parents=True, exist_ok=True)
cc = str(pspdev / 'bin/psp-gcc') if pspdev else os.environ.get('HOST_CC', 'cc')
ar = str(pspdev / 'bin/psp-ar') if pspdev else 'ar'
files = ['dgif_lib', 'gifalloc', 'gif_err', 'openbsd-reallocarray']
if not pspdev: files += ['egif_lib', 'gif_hash']  # synthetic fixture encoder, host only
flags = ['-std=gnu99', '-O2', '-I' + str(source)]
flags += ['-G0'] if pspdev else ['-g', '-fsanitize=address,undefined']
for name in files:
    subprocess.run([cc, *flags, '-c', str(source / (name + '.c')), '-o', str(build / (name + '.o'))], check=True)
subprocess.run([ar, 'rcs', str(build / 'libgif.a'), *[str(build / (name + '.o')) for name in files]], check=True)
