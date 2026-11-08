.DEFAULT_GOAL := package

PSPDEV ?= $(shell psp-config --pspdev-path 2>/dev/null)
HOST_CC ?= cc
PKG_CONFIG ?= pkg-config
XMB_PACKAGES := freetype2 libpng libcjson

.PHONY: psp package xmb clean check-sdk

check-sdk:
	@test -x "$(PSPDEV)/bin/psp-gcc" || { echo "Set PSPDEV to your PSPDEV installation." >&2; exit 1; }

build/rtc-psp/ready: tools/build_rtc.py tools/relative_build_paths.cmake vendor/mbedtls-2.28.10.tar.gz vendor/libsrtp-2.6.0.tar.gz | check-sdk
	python3 tools/build_rtc.py psp "$(PSPDEV)"
build/opus-psp/libopus.a: tools/build_opus.py tools/relative_build_paths.cmake vendor/opus-1.6.1.tar.gz | check-sdk
	python3 tools/build_opus.py psp "$(PSPDEV)"
build/webp-psp/libwebpdecoder.a: tools/build_webp.py vendor/libwebp-1.6.0.tar.gz | check-sdk
	python3 tools/build_webp.py "$(PSPDEV)"
build/gif-psp/libgif.a: tools/build_gif.py vendor/giflib-5.2.2.tar.gz | check-sdk
	python3 tools/build_gif.py psp "$(PSPDEV)"

build/xmb-assets: tools/xmb_assets.c include/stoat.h
	mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude \
		$$($(PKG_CONFIG) --cflags $(XMB_PACKAGES)) $< \
		$$($(PKG_CONFIG) --libs freetype2 libpng) -o $@

xmb: build/xmb/ICON0.png build/xmb/PIC1.png
build/xmb/ICON0.png: build/xmb-assets assets/xmb/messenger.png assets/NotoSans-Regular.ttf
	mkdir -p build/xmb
	./build/xmb-assets build/xmb/ICON0.png build/xmb/PIC1.png
build/xmb/PIC1.png: build/xmb/ICON0.png
	@test -f $@ || ./build/xmb-assets build/xmb/ICON0.png build/xmb/PIC1.png

psp: check-sdk build/rtc-psp/ready build/opus-psp/libopus.a build/webp-psp/libwebpdecoder.a build/gif-psp/libgif.a xmb
	PATH="$(PSPDEV)/bin:$$PATH" $(MAKE) -C platform/psp PSPDEV="$(PSPDEV)"

package: psp
	python3 tools/package.py
	python3 tools/verify_package.py

clean: check-sdk
	PATH="$(PSPDEV)/bin:$$PATH" $(MAKE) -C platform/psp PSPDEV="$(PSPDEV)" clean
	rm -rf build
