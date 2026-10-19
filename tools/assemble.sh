#!/bin/zsh
# assemble.sh <template.app> <out.app> <build PartyBoard.app/Contents> <name> <version> <ButtonPrompts dir> [Textures dir]
# Builds a signed PartyBoard app: the template supplies the launcher wrapper,
# phone controller and Info.plist; the build supplies the game binary,
# libraries and res; ButtonPrompts (and, for the HD beta, Textures) are copied
# in. Example: assemble.sh "PartyBoard V1.7.app" /tmp/out/"PartyBoard V1.8.app" \
#   PartyBoard-build/build/install/PartyBoard.app/Contents "PartyBoard V1.8" 1.8 bp
set -e
TPL=$1; OUT=$2; B=$3; NAME=$4; VER=$5; BP=$6; TEX=${7:-}
rm -rf "$OUT"; ditto "$TPL" "$OUT"; A="$OUT/Contents"
cp "$B/MacOS/PartyBoard" "$A/MacOS/PartyBoard-bin"
rsync -a --delete "$B/Frameworks/" "$A/Frameworks/"; rsync -a --delete "$B/Resources/res/" "$A/Resources/res/"
cp "$B/Resources/initial_pipeline_cache.db" "$A/Resources/"
rm -rf "$A/Resources/ButtonPrompts"; cp -R "$BP" "$A/Resources/ButtonPrompts"
PC="${0:h}/../PartyBoard Phone Controller"   # launcher wrapper and phone bridge sources
cp "$PC/PartyBoard-launcher.sh" "$A/MacOS/PartyBoard"; chmod 755 "$A/MacOS/PartyBoard"
cp "$PC/bridge.py" "$PC/controller.html" "$A/Resources/PhoneController/"
if [ -n "$TEX" ]; then rm -rf "$A/Resources/Textures"; cp -R "$TEX" "$A/Resources/Textures"; fi
strip -S "$A/MacOS/PartyBoard-bin" "$A"/Frameworks/*.dylib 2>&1 | grep -v warning || true
/usr/libexec/PlistBuddy -c "Set :CFBundleName $NAME" -c "Set :CFBundleDisplayName $NAME" -c "Set :PartyBoardCustomVersion $VER" "$A/Info.plist"
hits=$(grep -rl "$HOME/" "$A" || true); [ -z "$hits" ] || { echo "home path strings found in:"; echo "$hits"; exit 1; }
xattr -cr "$OUT"; codesign --force --deep -s - "$OUT" 2>&1 | grep -v replacing || true
codesign --verify --deep --strict "$OUT" && echo "$NAME signed"
