#!/usr/bin/env python3
"""Génère la ressource VERSIONINFO Windows (onglet Détails) depuis VERSION.

Usage: python3 tools/vergen.py VERSION out.rc client|core
Écrit un fichier .rc autonome (aucune macro à passer au windres)."""

import re
import sys

META = {
    "client": {
        "desc": "MusicPlayer - MP3/MP4 player (client)",
        "orig": "MusicPlayer.exe",
        "name": "MusicPlayer",
    },
    "core": {
        "desc": "MusicPlayer Core - headless engine (client/server)",
        "orig": "musicplayer-core.exe",
        "name": "MusicPlayer Core",
    },
}


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: vergen.py VERSION out.rc client|core")
    version = open(sys.argv[1], encoding="utf-8").read().strip()
    meta = META.get(sys.argv[3])
    if not meta:
        sys.exit("unknown target: " + sys.argv[3])
    m = re.match(r"(\d+)\.(\d+)\.(\d+)(?:-c(\d+))?$", version)
    if not m:
        sys.exit("bad version: " + version)
    num = "%d,%d,%d,%d" % (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4) or "0"))

    rc = """/* genéré par tools/vergen.py — ne pas éditer */
#include <winver.h>

VS_VERSION_INFO VERSIONINFO
 FILEVERSION %(num)s
 PRODUCTVERSION %(num)s
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "LostInTheBugs"
            VALUE "FileDescription", "%(desc)s"
            VALUE "FileVersion", "%(version)s"
            VALUE "InternalName", "%(orig)s"
            VALUE "LegalCopyright", "Copyright (C) 2026 LostInTheBugs - MIT License"
            VALUE "OriginalFilename", "%(orig)s"
            VALUE "ProductName", "%(name)s"
            VALUE "ProductVersion", "%(version)s"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
""" % {"num": num, "version": version, "desc": meta["desc"],
       "orig": meta["orig"], "name": meta["name"]}
    open(sys.argv[2], "w", encoding="utf-8", newline="\n").write(rc)
    print("version.rc -> %s (FileVersion %s)" % (sys.argv[2], version))


if __name__ == "__main__":
    main()
