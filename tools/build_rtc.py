#!/usr/bin/env python3
"""Build isolated DTLS/TLS and SRTP in build/. Never replace SDK libraries."""
import hashlib,os,re,subprocess,sys,tarfile
from pathlib import Path
root=Path(__file__).resolve().parent.parent
psp=Path(sys.argv[2]) if sys.argv[1]=='psp' else None
kind='psp' if psp else 'host'
env=dict(os.environ)
if psp:env['PATH']=str(psp/'bin')+':'+env['PATH']
def run(args):subprocess.run([str(a) for a in args],env=env,check=True)
def extract(name,sha):
    archive=root/'vendor'/name;assert hashlib.sha256(archive.read_bytes()).hexdigest()==sha
    dest=root/'build/rtc-source';dest.mkdir(parents=True,exist_ok=True)
    with tarfile.open(archive) as t:
        top=t.getmembers()[0].name.split('/')[0];source=dest/top
        if not source.exists():
            for m in t.getmembers():
                assert not m.issym() and not m.islnk()
                assert str((dest/m.name).resolve()).startswith(str(dest.resolve())+'/')
            t.extractall(dest)
    return source
common=['-DCMAKE_BUILD_TYPE=Release','-DBUILD_SHARED_LIBS=OFF','-DCMAKE_POSITION_INDEPENDENT_CODE=OFF']
psp_flags='-G0'
if psp:common += ['-DCMAKE_SYSTEM_NAME=Generic',f'-DCMAKE_C_COMPILER={psp}/bin/psp-gcc',f'-DCMAKE_C_FLAGS={psp_flags} -I{psp}/psp/sdk/include','-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG','-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',f'-DCMAKE_PROJECT_INCLUDE={root}/tools/relative_build_paths.cmake']
else:common += ['-DCMAKE_C_FLAGS=-g -fsanitize=address,undefined','-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG']
tls=extract('mbedtls-2.28.10.tar.gz','0f2e0525903a89ae1d39ce439d858be66933bda54c5b6102b72a29ed8fe7c088')
build=root/f'build/rtc-{kind}';build.mkdir(parents=True,exist_ok=True)
tls_build=build/'tls'
config=build/'rtc_config.h'
config.write_text('#include <mbedtls/config.h>\n#define MBEDTLS_SSL_DTLS_SRTP\n#define MBEDTLS_NO_PLATFORM_ENTROPY\n#undef MBEDTLS_NET_C\n#undef MBEDTLS_TIMING_C\n')
tls_opts=common+[f'-DCMAKE_C_FLAGS={psp_flags if psp else "-g -fsanitize=address,undefined"} -I{build} -I{tls}/include {"-I"+str(psp)+"/psp/sdk/include " if psp else ""}-include {config}','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF']
run(['cmake','-S',tls,'-B',tls_build,*tls_opts]);run(['cmake','--build',tls_build,'-j','4'])
# Give the entire private TLS stack a namespace. Adding use_srtp changes SSL
# structure layout; curl must continue to use the untouched SDK ABI/config.
libs=[tls_build/'library'/f'lib{x}.a' for x in ['mbedtls','mbedx509','mbedcrypto']]
nm=str(psp/'bin/psp-nm') if psp else 'nm'
symbols=set()
for lib in libs:
    output=subprocess.check_output([nm,'-g',str(lib)],text=True)
    for line in output.splitlines():
        m=re.search(r'\b[TRDBSWV]\s+_?((?:mbedtls|psa|everest|Hacl)_\w+)$',line)
        if m:symbols.add(m[1])
header=build/'rtc_tls_symbols.h'
header.write_text('\n'.join('#define '+s+' st_rtc_'+s for s in sorted(symbols))+'\n')
if psp:
    mapping=build/'symbols.map';mapping.write_text('\n'.join(s+' st_rtc_'+s for s in sorted(symbols))+'\n')
    for lib in libs:run([psp/'bin/psp-objcopy','--redefine-syms='+str(mapping),lib,build/lib.name])
else:
    # Darwin archives cannot be rewritten with GNU objcopy. Recompile with the
    # identical generated namespace header; all crypto stays inside this stack.
    run(['cmake','-S',tls,'-B',tls_build,*common,f'-DCMAKE_C_FLAGS=-g -fsanitize=address,undefined -I{build} -I{tls}/include {"-I"+str(psp)+"/psp/sdk/include " if psp else ""}-include {config} -include {header}','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF'])
    run(['cmake','--build',tls_build,'--clean-first','-j','4'])
    import shutil
    for lib in libs:shutil.copy2(lib,build/lib.name)
srtp=extract('libsrtp-2.6.0.tar.gz','bf641aa654861be10570bfc137d1441283822418e9757dc71ebb69a6cf84ea6b')
# Upstream mixes uint32_t* and unsigned int*. Newlib uses unsigned long for
# uint32_t on PSP (both are 32 bits). Normalize those explicit 32-bit counters
# in the extracted build copy instead of disabling incompatible-pointer errors.
for file in list(srtp.rglob('*.c'))+list(srtp.rglob('*.h')):
    original=file.read_text();normalized=original.replace('unsigned int','uint32_t')
    if normalized!=original:file.write_text(normalized)
template=srtp/'config_in_cmake.h'
text=template.read_text().replace('#define CPU_CISC 1','/* #undef CPU_CISC */').replace('/* #undef CPU_RISC */','#define CPU_RISC 1')
template.write_text(text)
run(['cmake','-S',srtp,'-B',build/'srtp',*common,'-DENABLE_OPENSSL=OFF','-DENABLE_MBEDTLS=OFF','-DENABLE_NSS=OFF','-DENABLE_WARNINGS_AS_ERRORS=OFF','-DLIBSRTP_TEST_APPS=OFF',*(['-DHAVE_NETINET_IN_H=1','-DHAVE_ARPA_INET_H=1'] if psp else [])])
run(['cmake','--build',build/'srtp','-j','4'])
(build/'ready').touch()
