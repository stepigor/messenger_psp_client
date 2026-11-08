# Build dependencies

`make package` builds these pinned source archives locally. The builders verify
their SHA-256 digests and prepare their PSP build trees under `build/`.

| Source archive | Builder |
| --- | --- |
| [giflib 5.2.2](https://sourceforge.net/projects/giflib/files/giflib-5.x/giflib-5.2.2.tar.gz/) | `tools/build_gif.py` |
| [libwebp 1.6.0](https://github.com/webmproject/libwebp/releases/tag/v1.6.0) | `tools/build_webp.py` |
| [Opus 1.6.1](https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz) | `tools/build_opus.py` |
| [Mbed TLS 2.28.10](https://github.com/Mbed-TLS/mbedtls/releases/tag/v2.28.10) and [libSRTP 2.6.0](https://github.com/cisco/libsrtp/tree/v2.6.0) | `tools/build_rtc.py` |

## PSPDEV libraries

The release uses [PSPDEV v20260701](https://github.com/pspdev/pspdev/releases/tag/v20260701),
[PSPSDK `7ddea004`](https://github.com/pspdev/pspsdk/tree/7ddea00433f8dbef20a6f5d66291cd4b39417689),
and [Newlib `9e0a0736`](https://github.com/pspdev/newlib/tree/9e0a073634ad73e8e088f2e071c55a9fe5d39709).
The following package recipes at `psp-packages` commit `7fdc074` identify the
upstream source archives, their digests and the PSP patches used for the linked
libraries.

| Package | Version | Source recipe |
| --- | --- | --- |
| curl | 7.64.1-13 | [curl](https://github.com/pspdev/psp-packages/tree/7fdc074/curl) |
| mbedtls | 2.28.10-2 | [mbedtls](https://github.com/pspdev/psp-packages/tree/7fdc074/mbedtls) |
| cjson | 1.7.16-6 | [cjson](https://github.com/pspdev/psp-packages/tree/7fdc074/cjson) |
| freetype2 | 2.11.0-10 | [freetype2](https://github.com/pspdev/psp-packages/tree/7fdc074/freetype2) |
| jpeg | 3.1.3-2 | [jpeg](https://github.com/pspdev/psp-packages/tree/7fdc074/jpeg) |
| libvorbis | 1.3.7-8 | [libvorbis](https://github.com/pspdev/psp-packages/tree/7fdc074/libvorbis) |
| libogg | 1.3.6-2 | [libogg](https://github.com/pspdev/psp-packages/tree/7fdc074/libogg) |
| libpng | 1.6.53-2 | [libpng](https://github.com/pspdev/psp-packages/tree/7fdc074/libpng) |
| zlib | 1.3.1-8 | [zlib](https://github.com/pspdev/psp-packages/tree/7fdc074/zlib) |
| bzip2 | 1.0.8-7 | [bzip2](https://github.com/pspdev/psp-packages/tree/7fdc074/bzip2) |

## Relinking pthread-embedded

`pthread-embedded-97fe4ce.tar.gz` contains the LGPL library source from
[PSPDEV's `97fe4ce` revision](https://github.com/pspdev/pthread-embedded/tree/97fe4ce006b420894f2bcaeb530d1f1f53111fc2).
Its SHA-256 is `09d99c99388d3f5afc5120dbdc5e4d519f070a50cebf7f0eada9033de4099708`.
With the SDK environment from the main README, rebuild and relink the library
from the repository root. You can modify `build/pthread-src` before compiling.

```sh
make clean
mkdir -p build/pthread-src
tar -xzf vendor/pthread-embedded-97fe4ce.tar.gz -C build/pthread-src --strip-components=1
cmake -S build/pthread-src -B build/pthread \
  -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-G0
cmake --build build/pthread
make package PTHREAD_LIB="$PWD/build/pthread/libpthread.a"
```
