# ======================================================================
# MusicPlayer — lecteur audio MP3/MP4 pour Windows
# Compilation croisée Linux -> Windows via MinGW-w64.
#
# Dépendances (à laisser dans vendor/) :
#   - vendor/miniaudio.h           (miniaudio 0.11.x, mono-fichier)
#   - vendor/ffmpeg/               (FFmpeg n8.1 win64-gpl-shared, BtbN)
#
# Usage :
#   make            -> bin/MusicPlayer.exe + DLLs FFmpeg
#   make test       -> compile + selftest sous Wine
#   make zip        -> archive portable prête pour Windows 11
#   make clean
# ======================================================================

VERSION := 2026.08.001

CROSS    := x86_64-w64-mingw32-
CC       := $(CROSS)gcc
WINDRES  := $(CROSS)windres

CFLAGS   := -O2 -Wall -Wextra -std=c11 \
            -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0601 \
            -DMP_VERSION=\"$(VERSION)\" \
            -Ivendor -Ivendor/ffmpeg/include
LDFLAGS  := -Lvendor/ffmpeg/lib \
            -lavformat -lavcodec -lavutil -lswresample \
            -lole32 -luuid -lwinmm -ldsound \
            -luser32 -lgdi32 -lshell32 -lcomdlg32 -lcomctl32 \
            -static-libgcc

SRC := src/main.c src/player.c src/plugin_loader.c
OBJ := $(SRC:.c=.o)
BIN := bin/MusicPlayer.exe

# DLLs FFmpeg nécessaires au runtime (à livrer à côté de l'exe)
FFMPEG_DLLS := avcodec-62.dll avformat-62.dll avutil-60.dll swresample-6.dll

all: dirs $(BIN)

dirs:
	mkdir -p bin plugins test dist

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	cp $(addprefix vendor/ffmpeg/bin/,$(FFMPEG_DLLS)) bin/

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ----------------------------------------------------------------------
# Fichiers de test (générés avec ffmpeg Linux)
# ----------------------------------------------------------------------
test-samples: dirs
	@test -f test/test.mp3 || ffmpeg -y -v error -f lavfi -i "sine=frequency=440:duration=6" -c:a libmp3lame -q:a 4 test/test.mp3
	@test -f test/test.mp4 || ffmpeg -y -v error -f lavfi -i "sine=frequency=660:duration=6" -c:a aac -b:a 128k test/test.mp4
	@echo "Fichiers de test prêts :"; ls -la test/

# ----------------------------------------------------------------------
# Tests sous Wine (lecture, vitesse, pause, stop, fin de fichier)
# ----------------------------------------------------------------------
test: all test-samples
	cd bin && wine64 ./MusicPlayer.exe --selftest ../test/test.mp3 ../test/test.mp4; \
	echo "exit code = $$?"; \
	echo "--- selftest.log ---"; cat selftest.log

# ----------------------------------------------------------------------
# Archive portable pour Windows 11
# ----------------------------------------------------------------------
zip: all dirs
	cd bin && zip -q ../dist/MusicPlayer-$(VERSION)-win64.zip MusicPlayer.exe $(FFMPEG_DLLS) && \
	cd .. && echo "Archive : dist/MusicPlayer-$(VERSION)-win64.zip"

clean:
	rm -f $(OBJ) bin/*.exe bin/*.dll bin/*.log dist/*.zip

.PHONY: all dirs test-samples test zip clean
