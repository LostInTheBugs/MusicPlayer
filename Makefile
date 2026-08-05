# ======================================================================
# MusicPlayer — lecteur audio MP3/MP4 pour Windows
# Compilation croisée Linux -> Windows via MinGW-w64.
#
# Dépendances (à laisser dans vendor/) :
#   - vendor/miniaudio.h           (miniaudio 0.11.x, mono-fichier)
#   - vendor/ffmpeg/               (FFmpeg n8.1 win64-lgpl-shared, BtbN)
#
# Usage :
#   make            -> bin/MusicPlayer.exe + DLLs FFmpeg
#   make test       -> compile + selftest sous Wine
#   make zip        -> archive portable prête pour Windows 11
#   make clean
# ======================================================================

VERSION := 2026.08.042-c1

CROSS    := x86_64-w64-mingw32-
CC       := $(CROSS)gcc
WINDRES  := $(CROSS)windres
CFLAGS   := -O2 -Wall -Wextra -std=c11 \
            -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0601 \
            -DMP_VERSION=\"$(VERSION)\" \
            -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
            -MMD -MP \
            -Ivendor -Ivendor/ffmpeg/include
LDFLAGS  := -Lvendor/ffmpeg/lib \
            -lavformat -lavcodec -lavutil -lswresample \
            -lole32 -luuid -lwinmm -ldsound \
            -luser32 -lgdi32 -lshell32 -lcomdlg32 -lcomctl32 -lwininet -lws2_32 -liphlpapi -lgdiplus -ladvapi32 \
            -static -mwindows

SRC := src/main.c src/player.c src/plugin_loader.c src/lang.c src/update.c src/config.c src/cd.c \
       src/client_core.c src/stream_player.c src/svc.c
OBJ := $(SRC:.c=.o)
RES := src/musicplayer_res.o
BIN := bin/MusicPlayer.exe

# ----------------------------------------------------------------------
# Core (musicplayer-core.exe) — moteur sans UI, API REST publique.
# Même moteur (player.c) compilé avec -DMP_CORE : pas de carte son,
# le flux part vers les clients via /stream.
# ----------------------------------------------------------------------
CORE_OBJ := build/core_main.o build/core_http.o build/core_playlist.o \
            build/core_player.o build/core_plugin_loader.o \
            build/core_config.o build/core_cd.o
CORE_BIN := bin/musicplayer-core.exe

# DLLs FFmpeg nécessaires au runtime (à livrer à côté de l'exe)
FFMPEG_DLLS := avcodec-62.dll avformat-62.dll avutil-60.dll swresample-6.dll

all: dirs $(BIN)

dirs:
	mkdir -p bin/plugins bin/lang plugins test dist

$(BIN): $(OBJ) $(RES)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(RES) $(LDFLAGS)
	cp $(addprefix vendor/ffmpeg/bin/,$(FFMPEG_DLLS)) bin/
	cp vendor/ffmpeg/LICENSE.txt bin/LICENSE-FFmpeg.txt
	cp lang/* bin/lang/

src/musicplayer_res.o: src/musicplayer.rc src/musicplayer.ico
	$(WINDRES) -i src/musicplayer.rc -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- core (objets séparés dans build/, -DMP_CORE pour player.c) ---
core: dirs $(CORE_BIN)

$(CORE_BIN): $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(LDFLAGS)

build/core_main.o: src/core/core_main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/core_http.o: src/core/core_http.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/core_playlist.o: src/core/core_playlist.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/core_player.o: src/player.c
	@mkdir -p build
	$(CC) $(CFLAGS) -DMP_CORE -c -o $@ $<
build/core_plugin_loader.o: src/plugin_loader.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/core_config.o: src/config.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<
build/core_cd.o: src/cd.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# dépendances d'en-têtes générées par -MMD -MP : modifier player.h
# (ou tout autre .h) déclenche la recompilation des .c concernés
-include $(OBJ:.o=.d)
-include $(CORE_OBJ:.o=.d)

# ----------------------------------------------------------------------
# Dépendances vendor (miniaudio + FFmpeg LGPL) — utilisé par la CI
# ----------------------------------------------------------------------
setup: dirs
	@test -f vendor/miniaudio.h || (echo "==> miniaudio 0.11.25"; \
	 curl -L -o vendor/miniaudio.h https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h)
	@test -d vendor/ffmpeg/bin || (echo "==> FFmpeg n8.1 win64-lgpl-shared (BtbN)"; \
	 mkdir -p vendor/ffmpeg; \
	 curl -L -o /tmp/ffmpeg.zip https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip && \
	 unzip -q /tmp/ffmpeg.zip -d /tmp/ff && \
	 cp -r /tmp/ff/ffmpeg-*/bin /tmp/ff/ffmpeg-*/lib /tmp/ff/ffmpeg-*/include vendor/ffmpeg/ && \
	 cp /tmp/ff/ffmpeg-*/LICENSE.txt vendor/ffmpeg/ && \
	 rm -rf /tmp/ff /tmp/ffmpeg.zip)

# ----------------------------------------------------------------------
# Fichiers de test (générés avec ffmpeg Linux)
# ----------------------------------------------------------------------
test-samples: dirs
	@test -f test/test.mp3 || ffmpeg -y -v error -f lavfi -i "sine=frequency=440:duration=6" -c:a libmp3lame -q:a 4 test/test.mp3
	@test -f test/test.mp4 || ffmpeg -y -v error -f lavfi -i "sine=frequency=660:duration=6" -c:a aac -b:a 128k test/test.mp4
	@echo "Fichiers de test prêts :"; ls -la test/

# ----------------------------------------------------------------------
# Plugins d'exemple (compilés dans bin/plugins/)
# ----------------------------------------------------------------------
plugins-examples: $(BIN)
	$(CC) -O2 -shared -o bin/plugins/gaindemo.dll examples/plugin_gaindemo.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/spectrum.dll examples/plugin_spectrum.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/vumeter.dll examples/plugin_vumeter.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/fireworks.dll examples/plugin_fireworks.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/3dspectrum.dll examples/plugin_3dspectrum.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/3diso.dll examples/plugin_3diso.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/fractal.dll examples/plugin_fractal.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/hypnotic.dll examples/plugin_hypnotic.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/webserver.dll examples/plugin_webserver.c -Isrc -Ivendor/ffmpeg/include -Lvendor/ffmpeg/lib -lavformat -lavcodec -lavutil -lswresample -lws2_32 -static-libgcc -static
	$(CC) -O2 -shared -o bin/plugins/restapi.dll examples/plugin_restapi.c -Isrc -lws2_32 -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/rtp.dll examples/plugin_rtp.c -Isrc -lws2_32 -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/upnp.dll examples/plugin_upnp.c -Isrc -lws2_32 -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/multiroom.dll examples/plugin_multiroom.c -Isrc -lws2_32 -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/soundquality.dll examples/plugin_soundquality.c -Isrc -static-libgcc -lm
	$(CC) -O2 -shared -o bin/plugins/equalizer.dll examples/plugin_equalizer.c -Isrc -static-libgcc -lgdi32 -lm
	$(CC) -O2 -shared -o bin/plugins/ts.dll examples/plugin_ts.c -Isrc -Ivendor -static-libgcc -lole32 -lwinmm
	cp -f examples/multiroom.txt bin/plugins/multiroom.txt
	$(CC) -O2 -shared -o bin/plugins/metadata.dll examples/plugin_metadata.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/plugins/lyrics.dll examples/plugin_lyrics.c -Isrc -static-libgcc -luser32 -lgdi32
	$(CC) -O2 -shared -o bin/plugins/cover.dll examples/plugin_cover.c -Isrc -static-libgcc -luser32 -lgdi32 -lgdiplus -Wno-incompatible-pointer-types
	@mkdir -p bin/core_plugins
	cp -f bin/plugins/webserver.dll bin/plugins/metadata.dll bin/plugins/cover.dll \
	      bin/plugins/upnp.dll bin/plugins/rtp.dll bin/plugins/multiroom.dll bin/core_plugins/
	cp -f examples/multiroom.txt bin/core_plugins/multiroom.txt
	mkdir -p bin/skins
	$(CC) -O2 -shared -o bin/skins/skin_retro60.dll examples/plugin_skin_retro60.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_retro70.dll examples/plugin_skin_retro70.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_retro80.dll examples/plugin_skin_retro80.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_retro90.dll examples/plugin_skin_retro90.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_2000s.dll examples/plugin_skin_2000s.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_radio.dll examples/plugin_skin_radio.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_winamp.dll examples/plugin_skin_winamp.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_clean.dll examples/plugin_skin_clean.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_kitsch.dll examples/plugin_skin_kitsch.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_cartoon.dll examples/plugin_skin_cartoon.c -Isrc -static-libgcc
	$(CC) -O2 -shared -o bin/skins/skin_bnw.dll examples/plugin_skin_bnw.c -Isrc -static-libgcc
	cp -f examples/radio_bg.png bin/skins/radio_bg.png
	cp -f examples/winamp_bg.png bin/skins/winamp_bg.png
	@echo "Plugins d'exemple compilés dans bin/plugins/ et skins dans bin/skins/"

# ----------------------------------------------------------------------
# Tests sous Wine (lecture, vitesse, pause, stop, fin de fichier)
# ----------------------------------------------------------------------
test: all plugins-examples test-samples
	cd bin && wine64 ./MusicPlayer.exe --selftest ../test/test.mp3 ../test/test.mp4; \
	echo "exit code = $$?"; \
	echo "--- selftest.log ---"; cat selftest.log

# ----------------------------------------------------------------------
# Archive portable pour Windows 11
# ----------------------------------------------------------------------
zip: all plugins-examples dirs core
	cd bin && zip -q ../dist/MusicPlayer-$(VERSION)-win64.zip MusicPlayer.exe musicplayer-core.exe $(FFMPEG_DLLS) LICENSE-FFmpeg.txt plugins/*.dll core_plugins/*.dll core_plugins/*.txt skins/*.dll skins/*.png lang/*.lang && \
	cd .. && echo "Archive : dist/MusicPlayer-$(VERSION)-win64.zip"

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) bin/*.exe bin/*.dll bin/*.log
	rm -rf build
	@echo "Note : dist/*.zip (livrables) n'est pas supprimé ; utilisez 'make distclean' si besoin."

.PHONY: all dirs setup plugins-examples test-samples test zip clean
