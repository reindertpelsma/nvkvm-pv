# SPDX-License-Identifier: GPL-2.0 OR Apache-2.0
#
# nvkvm-display-broker
#
# Backends are compiled in only when their libraries are present, and the build
# PRINTS which ones it kept.  A missing backend must be visible at build time,
# not discovered as a mysterious runtime failure.
#
#   make                 build with whatever is available
#   make check           build, then run selftest.sh
#   sudo make install    -> /usr/local/bin
#
# Note what is NOT here: no libdrm, no EGL, no GL, no libgbm.  The broker turns
# a dma-buf into a wl_buffer or an X pixmap with protocol requests; the import
# happens in the compositor or the X server.  That is the same property the
# VMM gains, one process in.
#
# Debian/Ubuntu build deps:
#   Wayland backend : libwayland-dev wayland-protocols
#   X11 backend     : libxcb1-dev libxcb-dri3-dev libxcb-present-dev
#   X11 rel. motion : libxcb-xinput-dev      (optional; see the report below)

PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
BIN         = nvkvm-display-broker
TESTCLIENT  = nvkvm-broker-testclient
RELAY       = nvkvm-broker-relay
DMASRC      = nvkvm-broker-dmabuf-src

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -Wformat=2 -Wshadow -Wvla \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
           -D_GNU_SOURCE -I. -I../common
LDFLAGS += -Wl,-z,relro,-z,now

OBJS = nvkvm_broker.o nb_common.o nb_session_x11.o nb_session_wl.o \
       nb_session_test.o

# ── X11 backend: xcb + DRI3 + Present ─────────────────────────────────────
# DRI3 is what lets a dma-buf become an X pixmap without a GL context, and
# Present is what puts that pixmap on screen and reports back when it is done
# with it.  Without both there is no GL-free X11 path, so the backend is left
# out rather than silently degraded.
X11_PKGS = xcb xcb-dri3 xcb-present
ifeq ($(shell $(PKG_CONFIG) --exists $(X11_PKGS) && echo yes),yes)
HAVE_X11 = 1
CFLAGS  += -DNB_HAVE_X11 $(shell $(PKG_CONFIG) --cflags $(X11_PKGS))
LIBS    += $(shell $(PKG_CONFIG) --libs $(X11_PKGS))
# Optional: XI2 raw motion, the only source of true relative deltas under a
# pointer grab (core motion is clamped at the confine boundary).
ifeq ($(shell $(PKG_CONFIG) --exists xcb-xinput && echo yes),yes)
HAVE_XI  = 1
CFLAGS  += -DNB_HAVE_XCB_XINPUT $(shell $(PKG_CONFIG) --cflags xcb-xinput)
LIBS    += $(shell $(PKG_CONFIG) --libs xcb-xinput)
endif
endif

# ── Wayland backend ───────────────────────────────────────────────────────
WL_PROTO_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)
WL_SCANNER   := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null)
ifeq ($(WL_SCANNER),)
WL_SCANNER   := $(shell command -v wayland-scanner 2>/dev/null)
endif

WL_XML = \
  $(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml \
  $(WL_PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
  $(WL_PROTO_DIR)/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml \
  $(WL_PROTO_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
  $(WL_PROTO_DIR)/unstable/relative-pointer/relative-pointer-unstable-v1.xml

WL_HDRS = $(notdir $(WL_XML:.xml=-client-protocol.h))
WL_SRCS = $(notdir $(WL_XML:.xml=-protocol.c))
WL_OBJS = $(WL_SRCS:.c=.o)

ifneq ($(WL_PROTO_DIR),)
ifneq ($(WL_SCANNER),)
ifeq ($(shell $(PKG_CONFIG) --exists wayland-client && echo yes),yes)
ifeq ($(shell test -f $(WL_PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml && echo yes),yes)
HAVE_WL  = 1
CFLAGS  += -DNB_HAVE_WAYLAND $(shell $(PKG_CONFIG) --cflags wayland-client)
LIBS    += $(shell $(PKG_CONFIG) --libs wayland-client)
OBJS    += $(WL_OBJS)
endif
endif
endif
endif

ifeq ($(HAVE_X11)$(HAVE_WL),)
$(warning No display backend will be compiled in: only --backend test will work.)
$(warning   Wayland: apt install libwayland-dev wayland-protocols)
$(warning   X11:     apt install libxcb1-dev libxcb-dri3-dev libxcb-present-dev)
endif

# ── test tooling: a real dma-buf producer ─────────────────────────────────
# dmabuf_source is the ONLY thing here that links libgbm, and it is not part
# of the broker.  It exists because testclient sends a memfd, which every real
# backend correctly rejects — so it can prove the validator but can never put
# a pixel on screen.  Built only when gbm is present.
ifeq ($(shell $(PKG_CONFIG) --exists gbm && echo yes),yes)
HAVE_GBM = 1
GBM_CFLAGS = $(shell $(PKG_CONFIG) --cflags gbm)
GBM_LIBS   = $(shell $(PKG_CONFIG) --libs gbm)
endif

.SECONDARY:
.PHONY: all install clean report check
all: report $(BIN) $(TESTCLIENT) $(RELAY) $(if $(HAVE_GBM),$(DMASRC),)

report:
	@echo "nvkvm-display-broker backends:"
	@echo "  dma-buf test source : $(if $(HAVE_GBM),yes (libgbm),NO - apt install libgbm-dev)"
	@echo "  wayland : $(if $(HAVE_WL),yes,NO - install libwayland-dev wayland-protocols)"
	@echo "  x11     : $(if $(HAVE_X11),yes (dri3+present),NO - install libxcb1-dev libxcb-dri3-dev libxcb-present-dev)$(if $(HAVE_X11),$(if $(HAVE_XI), + xi2 raw motion, - NO relative motion under grab: install libxcb-xinput-dev),)"
	@echo "  test    : yes (always)"

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

# Generated Wayland protocol glue.  Every .o depends on the headers because
# they are generated, and make has no way to know that otherwise.
$(OBJS): $(if $(HAVE_WL),$(WL_HDRS),)

%-client-protocol.h:
	$(WL_SCANNER) client-header $(filter %/$*.xml,$(WL_XML)) $@

%-protocol.c:
	$(WL_SCANNER) private-code $(filter %/$*.xml,$(WL_XML)) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# A minimal client, for proving the broker works before involving QEMU.  It is
# also the selftest's attack harness: its --bad-* options are the ways a
# hostile VMM can lie to the privileged side.
$(TESTCLIENT): testclient.c ../common/nvkvm_broker_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# The sandboxed side, deliberately linked against libc and nothing else — run
# ldd on it inside the container; that link line IS the security claim.
$(RELAY): dmabuf_relay.c ../common/nvkvm_broker_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(DMASRC): dmabuf_source.c ../common/nvkvm_broker_proto.h
	$(CC) $(CFLAGS) $(GBM_CFLAGS) $(if $(HAVE_X11),-DSRC_HAVE_X11,) \
	      -o $@ $< $(LDFLAGS) $(GBM_LIBS) $(if $(HAVE_X11),$(shell $(PKG_CONFIG) --libs xcb xcb-dri3),)

check: all
	bash selftest.sh

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(TESTCLIENT) $(RELAY) $(DMASRC) *.o \
	      *-client-protocol.h *-protocol.c
