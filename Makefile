# atlas_wm - SIEOS's Atlas desktop as a Wayland compositor (README.md).
# Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
#
#   make                 build ./atlas_wm, ./atlas-files (the file manager) and ./atlas-net (the network, with nmcli)
#   make install         into $(PREFIX)/bin, and the session for GDM and SDDM
#   make uninstall       all of it again
#   make run             nested in the current Wayland or X11 session
#
# Debian 13: apt install libwlroots-0.18-dev libwayland-dev wayland-protocols libxkbcommon-dev
#            libpixman-1-dev libdrm-dev libegl-dev libgles-dev libinput-dev libxcb1-dev
#            libxcb-icccm4-dev libpam0g-dev pkg-config xwayland

PREFIX   ?= /usr/local
PKG      ?= pkg-config
PKGS     := wlroots-0.18 wayland-server xkbcommon pixman-1 libdrm egl glesv2 xcb xcb-icccm libinput
CC       ?= cc
CFLAGS   ?= -O2 -g
PKG_CFLAGS ?= $(shell $(PKG) --cflags $(PKGS))
PKG_LIBS   ?= $(shell $(PKG) --libs $(PKGS))
WL_PROTOCOLS ?= $(or $(shell $(PKG) --variable=pkgdatadir wayland-protocols 2>/dev/null),/usr/share/wayland-protocols)
WL_SCANNER   ?= wayland-scanner
override CFLAGS += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation -Wno-unused-parameter -Wno-missing-field-initializers -DWLR_USE_UNSTABLE -Ibuild $(PKG_CFLAGS)
LDLIBS   += $(PKG_LIBS) -lpam -lm

SRC := src/main.c src/output.c src/glass.c src/view.c src/xwayland.c src/layers.c src/lock.c src/toplevels.c \
       src/settings.c src/wm.c src/deskbar.c src/palette.c \
       src/chrome.c src/look.c src/draw.c src/icons.c src/logo.c \
       src/font.c src/font13.c src/font15.c src/font18.c src/sans.c src/fontsans.c
OBJ := $(SRC:src/%.c=build/%.o)
# the protocols' server headers wlroots' headers include
# (wlr-protocols' are in protocol/: wayland-protocols does not have them)
PROTO := build/xdg-shell-protocol.h build/xdg-decoration-unstable-v1-protocol.h \
         build/wlr-layer-shell-unstable-v1-protocol.h build/wlr-output-power-management-unstable-v1-protocol.h

all: atlas_wm atlas-files atlas-net

atlas_wm: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# atlas-files: a Wayland client, drawn with the compositor's display list
FILES_PKGS   := wayland-client xkbcommon
FILES_CFLAGS ?= $(shell $(PKG) --cflags $(FILES_PKGS))
FILES_LIBS   ?= $(shell $(PKG) --libs $(FILES_PKGS))
FPROTO := xdg-shell:stable/xdg-shell/xdg-shell.xml \
          xdg-decoration-unstable-v1:unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
          fractional-scale-v1:staging/fractional-scale/fractional-scale-v1.xml \
          viewporter:stable/viewporter/viewporter.xml
FPROTO_H := $(foreach p,$(FPROTO),build/files/$(firstword $(subst :, ,$(p)))-client-protocol.h)
FPROTO_O := $(foreach p,$(FPROTO),build/files/$(firstword $(subst :, ,$(p)))-protocol.o)
FILES_OBJ := build/files/files.o build/files/wl.o $(FPROTO_O) build/draw.o build/look.o build/icons.o build/logo.o \
             build/font.o build/font13.o build/font15.o build/font18.o build/sans.o build/fontsans.o
atlas-files: $(FILES_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(FILES_OBJ) $(FILES_LIBS)
build/files/%.o: files/%.c files/files.h src/look.h src/draw.h $(FPROTO_H) | build/files
	$(CC) $(CFLAGS) $(FILES_CFLAGS) -Ibuild/files -c -o $@ $<
build/files/%-protocol.o: build/files/%-protocol.c | build/files
	$(CC) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ $<
define fproto
build/files/$(1)-client-protocol.h: | build/files
	$$(WL_SCANNER) client-header $$(WL_PROTOCOLS)/$(2) $$@
build/files/$(1)-protocol.c: | build/files
	$$(WL_SCANNER) private-code $$(WL_PROTOCOLS)/$(2) $$@
endef
$(foreach p,$(FPROTO),$(eval $(call fproto,$(firstword $(subst :, ,$(p))),$(lastword $(subst :, ,$(p))))))
build/files:
	mkdir -p build/files

# atlas-net: the network (NetworkManager's nmcli), a window as atlas-files' (files/wl.c)
NET_OBJ := build/net/net.o $(filter-out build/files/files.o,$(FILES_OBJ))
atlas-net: $(NET_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(NET_OBJ) $(FILES_LIBS)
build/net/%.o: net/%.c files/files.h src/look.h src/draw.h $(FPROTO_H) | build/net
	$(CC) $(CFLAGS) $(FILES_CFLAGS) -Ibuild/files -c -o $@ $<
build/net:
	mkdir -p build/net

build/%.o: src/%.c src/atlas.h src/look.h src/draw.h src/fontsans.h $(PROTO) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

build/xdg-shell-protocol.h: | build
	$(WL_SCANNER) server-header $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
build/xdg-decoration-unstable-v1-protocol.h: | build
	$(WL_SCANNER) server-header $(WL_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
build/wlr-%-protocol.h: protocol/wlr-%.xml | build
	$(WL_SCANNER) server-header $< $@

# The session: GDM and SDDM both list /usr/share/wayland-sessions (whatever
# the PREFIX), and start what Exec says: the full path, as their PATH may
# not hold $(PREFIX)/bin; TryExec hides it if atlas_wm is gone.
SESSIONS := /usr/share/wayland-sessions
install: atlas_wm atlas-files atlas-net | build
	install -Dm755 atlas_wm $(DESTDIR)$(PREFIX)/bin/atlas_wm
	install -Dm755 atlas-files $(DESTDIR)$(PREFIX)/bin/atlas-files
	install -Dm755 atlas-net $(DESTDIR)$(PREFIX)/bin/atlas-net
	install -Dm644 atlas-files.desktop $(DESTDIR)/usr/share/applications/atlas-files.desktop
	install -Dm644 atlas-net.desktop $(DESTDIR)/usr/share/applications/atlas-net.desktop
	sed -e 's|^Exec=.*|Exec=$(PREFIX)/bin/atlas_wm|' -e 's|^TryExec=.*|TryExec=$(PREFIX)/bin/atlas_wm|' atlas.desktop > build/atlas.desktop
	install -Dm644 build/atlas.desktop $(DESTDIR)$(SESSIONS)/atlas.desktop
	install -Dm644 atlas.pam $(DESTDIR)/etc/pam.d/atlas

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/atlas_wm $(DESTDIR)$(PREFIX)/bin/atlas-files $(DESTDIR)$(PREFIX)/bin/atlas-net
	rm -f $(DESTDIR)/usr/share/applications/atlas-files.desktop $(DESTDIR)/usr/share/applications/atlas-net.desktop
	rm -f $(DESTDIR)$(SESSIONS)/atlas.desktop $(DESTDIR)/etc/pam.d/atlas

run: atlas_wm
	./atlas_wm

clean:
	rm -rf build atlas_wm atlas-files atlas-net

.PHONY: all install uninstall run clean
