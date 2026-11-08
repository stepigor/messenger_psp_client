PSPDEV ?= $(shell psp-config --pspdev-path 2>/dev/null)
PSPSDK = $(shell psp-config --pspsdk-path)
TARGET = messenger
OBJS = thumbnail.o preferences.o photos.o call_catchup.o call_view.o call.o rtc.o rtc_tls.o rtc_protocol.o call_api.o voice_test.o voice_codec.o audio.o sound.o notices.o main.o ui.o controls.o paint.o entropy.o network.o wifi.o clock.o net_cancel.o dns.o socket_poll.o tls_work.o savedata.o storage.o markdown.o common.o session.o jobs.o transfer.o http.o websocket.o client.o roles.o tls_diag.o members.o media.o animation.o reactions.o emoji.o disk_cache.o panels.o theme.o message.o people.o social.o live.o
BUILD_PRX = 1
LDFLAGS = -Wl,--wrap=_getentropy,--wrap=select,--wrap=mbedtls_ssl_handshake,--wrap=mbedtls_net_recv,--wrap=mbedtls_net_send
PSP_FW_VERSION = 661
INCDIR = ../../include ../../build/gif-source/giflib-5.2.2 $(PSPDEV)/psp/include/freetype2 ../../build/webp-source/libwebp-1.6.0/src
CFLAGS = -std=c11 -O2 -G0 -Wall -Wextra -Werror -Wno-deprecated-declarations -MMD -MP
PTHREAD_LIB ?= -lpthread
LIBS = -Wl,--start-group -ljpeg -lcurl -lcjson ../../build/rtc-psp/libmbedtls.a ../../build/rtc-psp/libmbedx509.a ../../build/rtc-psp/libmbedcrypto.a ../../build/rtc-psp/srtp/libsrtp2.a ../../build/opus-psp/libopus.a -lvorbisfile -lvorbis -logg ../../build/webp-psp/libwebpdemux.a ../../build/webp-psp/libwebpdecoder.a ../../build/gif-psp/libgif.a -lfreetype -lpng -lbz2 -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthreadglue $(PTHREAD_LIB) -lcglue -lc -lm -Wl,--end-group -lpspaudio -lpspgu -lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver -lpsppower -lpspwlan
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = $(shell sed -n 's/^\#define ST_APP_NAME "\(.*\)"/\1/p' ../../include/stoat.h) $(shell sed -n 's/^\#define ST_APP_VERSION "\(.*\)"/\1/p' ../../include/stoat.h)
PSP_EBOOT_ICON = ../../build/xmb/ICON0.png
PSP_EBOOT_PIC1 = ../../build/xmb/PIC1.png
VPATH = ../../src
EXTRA_CLEAN = $(OBJS:.o=.d)
include $(PSPSDK)/lib/build.mak
-include $(OBJS:.o=.d)
$(TARGET).elf: ../../build/opus-psp/libopus.a

# mksfoex emits explicit options before its defaults; build.mak also prepends
# MEMSIZE. Set every field in lexical order AFTER that include so XMB can find
# BOOTABLE/DISC_ID/PARENTAL_LEVEL. Keep expanded-memory policy intact.
# TITLE is appended by mksfoex and sorts last. New fields must preserve order.
SFOFLAGS = -d BOOTABLE=1 -s CATEGORY=MG \
    -s DISC_ID=$(shell sed -n 's/^\#define ST_DISC_ID "\(.*\)"/\1/p' ../../include/stoat.h) \
    -s DISC_VERSION=1.00 -d MEMSIZE=$(EXPAND_MEMORY) \
    -d PARENTAL_LEVEL=$(shell sed -n 's/^\#define ST_PARENTAL_LEVEL \([0-9]*\)/\1/p' ../../include/stoat.h) \
    -s PSP_SYSTEM_VER=1.00 -d REGION=0x8000

# Rebuild XMB metadata when the release title/version changes.
PARAM.SFO: Makefile ../../include/stoat.h

EBOOT.PBP: $(PSP_EBOOT_ICON) $(PSP_EBOOT_PIC1)

# Private headers must precede the SDK: DTLS-SRTP changes SSL struct layout.
call.o: CFLAGS += -Wno-misleading-indentation
rtc.o: CFLAGS := -I../../build/rtc-source/libsrtp-2.6.0/include $(CFLAGS) -Wno-misleading-indentation
rtc_tls.o rtc_protocol.o: CFLAGS := -I../../build/rtc-psp -I../../build/rtc-source/mbedtls-2.28.10/include $(CFLAGS) -Wno-misleading-indentation
rtc.o rtc_tls.o rtc_protocol.o: Makefile ../../build/rtc-psp/ready
$(TARGET).elf: | ../../build/rtc-psp/ready
