# Licensing and notices

**PartyBoard Beta is free software under the [GNU General Public License, version 2 or (at your option)
any later version](LICENSE)** (GPL-2.0-or-later). This covers the repository as a whole and the
Mac and Windows downloads.

PartyBoard Beta builds on other projects. Their own licences are all compatible with the GPL and stay
with their files:

| Part | Where | Licence |
| --- | --- | --- |
| [PartyBoard](https://github.com/mariopartyrd/partyboard) (upstream PC port) and the [zeranemesis/Marioparty4](https://github.com/zeranemesis/Marioparty4) fork it was imported from | `PartyBoard-build/` | upstream PartyBoard is dedicated to the public domain under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| Aurora | `PartyBoard-build/extern/aurora` | MIT (`extern/aurora/LICENSE`) |
| MusyX | `PartyBoard-build/extern/musyx` | MIT (`extern/musyx/LICENSE`) |
| libco | `PartyBoard-build/extern/libco` | ISC (`extern/libco/LICENSE`) |
| QR Code generator (Project Nayuki) | `PartyBoard-build/extern/qrcodegen` | MIT (in the source files) |
| Fonts and images in `res/` | `PartyBoard-build/res` | included as upstream PartyBoard ships them |

The PC audio backend was checked against [Dolphin](https://github.com/dolphin-emu/dolphin)'s AX code
(GPL-2.0-or-later); see `PartyBoard-build/DOLPHIN_AX_NOTICE.md`.

Libraries that CMake downloads when you build (SDL3, Dawn, RmlUi, nod, fmt, FreeType, libpng, zstd,
xxHash, mbedTLS, rcheevos and others) keep their own licences.

**Game content is not licensed here.** Mario Party 4 and its characters, art, music and data belong to
Nintendo and Hudson Soft; the few game-derived images and the optional upscaled textures are shared privately,
for people who own the game. You need your own copy of the game.
