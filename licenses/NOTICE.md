# Third-party notices

The [dependency sources and build recipes](../vendor/README.md) cover the PSP
libraries linked into Messenger. The table identifies their included notices.

| Component | Notices in this directory |
| --- | --- |
| PSPSDK, libcglue, pthreadglue | `pspsdk/LICENSE` |
| KUBridge import stub from psp-cfw-sdk | `psp-cfw-sdk/gpl-3.0.txt` |
| Newlib | `newlib/COPYING.NEWLIB` |
| pthread-embedded | `pthread-embedded/COPYING.LIB`, `pthread-embedded/COPYING.vita`, `pthread-embedded/README.md` |
| libcurl | `curl-COPYING.txt` |
| Mbed TLS, including the private DTLS-SRTP build | `mbedtls/LICENSE`, `mbedtls-rtc/LICENSE` |
| libSRTP | `libsrtp/LICENSE` |
| cJSON | `cjson/LICENSE` |
| FreeType | `freetype2/FTL.TXT`, `freetype2/LICENSE.TXT` |
| libwebp | `libwebp/COPYING`, `libwebp/PATENTS` |
| giflib | `giflib/COPYING` |
| libjpeg-turbo | `jpeg/LICENSE.md`, `jpeg/README.ijg`, `jpeg/NOTICE.txt` |
| libvorbis and libogg | `libvorbis/COPYING`, `libogg/COPYING` |
| Opus | `opus/COPYING` |
| libpng | `libpng/LICENSE` |
| zlib | `zlib/README` |
| bzip2 | `bzip2/COPYING` |
| Noto Sans Regular | `NotoSans-OFL.txt` |
| Mozilla CA bundle | `Mozilla-MPL-2.0.txt` and the notices in `cacert.pem` |
| Twemoji 17.0.2 | `TWEMOJI-NOTICE.txt`, `TWEMOJI-GRAPHICS.txt`, `TWEMOJI-CODE.txt` |
| Material Color Utilities 0.3.0 | `Material-Color-Utilities-Apache-2.0.txt` |

This software uses the FreeType font engine under the FreeType License (FTL).
This software is based in part on the work of the Independent JPEG Group.

The declaration in `platform/psp/libcglue_fd.h` follows PSPSDK
`src/libcglue/fdman.h`, copyright (c) 2006 Rafael Cabezas, under the included BSD
license. Messenger links an app-local implementation of `select` with
`sceNetInetPoll`.

Twemoji artwork by Twitter, Inc. and other contributors is licensed under CC BY
4.0. Messenger resizes the original PNGs to 32×32 and encodes them as lossless
WebP. See [attribution and source](TWEMOJI-NOTICE.txt).

Noto Sans Regular comes from the [Noto Fonts project](https://github.com/notofonts/noto-fonts).
The unmodified Mozilla CA bundle comes from the [curl CA extract](https://curl.se/docs/caextract.html).
