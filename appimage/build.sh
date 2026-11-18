#!/bin/sh
# Builds oot2-x86_64.AppImage from ../OUT/oot2 (run `oot2 prepare` first). Run from this folder.
set -e
rm -rf AppDir
mkdir -p AppDir/usr/bin
cp ../OUT/oot2 AppDir/usr/bin/oot2
llvm-strip AppDir/usr/bin/oot2 2>/dev/null || strip AppDir/usr/bin/oot2
cp ../build/xlate/publish/Oot2Xlate/release_linux-x64/oot2-xlate AppDir/usr/bin/oot2-xlate
cp AppRun oot2.desktop AppDir/
cp oot2.png AppDir/oot2.png
cp ../LICENSE ../THIRD_PARTY_NOTICES.md AppDir/ 2>/dev/null || true
ARCH=x86_64 ./appimagetool --appimage-extract-and-run AppDir oot2-x86_64.AppImage
