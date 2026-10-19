#!/bin/zsh
# precompose.sh [deps]: redraws composed/ -- the 3D button textures and strip
# cells the Windows build installs as ButtonPrompts/<family>/composed (see
# src/port/button_prompts_compose.cpp) -- with the Mac composer, then checks
# that the Windows composer gives the Mac's pixels for every 3D texture and for
# many strips. Run on a Mac after changing the art or a target in
# src/port/button_prompts_tables.h, and commit composed/.
# deps: a Mac build's _deps folder (for the SDL headers and libpng).
set -euo pipefail
here=${0:A:h}
src=${here:h:h}/PartyBoard-build
deps=${1:?usage: precompose.sh <a Mac build's _deps folder>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

clang -fobjc-arc -O2 -c "$src/src/port/button_prompts_compose.m" -o "$tmp/compose_mac.o"
clang++ -std=c++20 -O2 -DTARGET_PC "$here/precompose.cpp" "$tmp/compose_mac.o" -o "$tmp/precompose" \
    -I"$src/src/port" -I"$src/extern/aurora/include" -I"$deps/sdl-src/include" \
    -I"$deps/png-src" -I"$deps/png-build" "$deps/png-build/libpng16.a" -lz -framework AppKit

rm -rf "$here/composed"
"$tmp/precompose" draw "$here/art" "$here/composed"

# The installed layout: the art, with composed/<family> as <family>/composed.
cp -R "$here/art" "$tmp/ButtonPrompts"
for family in xbox playstation switch; do
    cp -R "$here/composed/$family" "$tmp/ButtonPrompts/$family/composed"
done
"$tmp/precompose" verify "$tmp/ButtonPrompts" 300
