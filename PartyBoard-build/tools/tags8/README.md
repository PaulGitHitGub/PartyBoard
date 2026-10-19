# 5P-8P player tags (8-player party) — approved, wired in (feature/8p-seats)

Asked for on 2026-10-16: 5P-8P versions of the game's 1P-4P tags, from the game's own art, for when
seats 5-8 can be people (feature/8p-input). Not used by the game yet.

- Sources (decoded from the game's sprite data with `gxdec.py`: C8 bitmap + RGB5A3 palette, dumped
  from the loaded `ANIMDATA` of the Party Mode sprites with a temporary debug hook, not kept):
  the 200x64 character-select cursor-tag atlas (`DATADIR_MENT` 0x23-0x26: 1P-4P and COM, star and
  dome versions), the 32x72 small 1P-4P labels (0x3A, party strip and rules screen), the red
  portrait frame (0x28), and the white digit font the approved 5th-8th rank labels use
  (`tex1_16x176_f2475068fb22a0b6`, from a `PARTYBOARD_DUMP_TEXTURES=1` dump).
- `gen.py <in> <out>`: keeps the 1P art's star/dome base, outline and "P", redraws the numeral from
  the game's digit font (supersampled, 2-pixel outline like the originals) and moves the colours to
  5P orange, 6P pink, 7P silver, 8P brown (approved 2026-10-16, brown brightened; also the phone badges; no clash with red, blue, yellow, green, COM cyan or the
  purple base). Outputs `tags_5p8p_1x.png` (160x64: 5P-8P, star row then dome row),
  `labels_5p8p_1x.png` (32x72) and `frame_5p..8p_1x.png`.
- `compose.py`: preview mock-ups on full-size screenshots (character select, party strip, rules).
- `inc.py . ../../src/REL/mentDll/tags5to8_tex.inc`: embeds the approved tags, labels and frames in the game
  as RGBA8 textures laid out like the game's own 1P-4P bitmaps (tag atlas 200x64 with 5P-8P where 1P-4P are,
  labels 32x72, frames 96x192). The menu (`Ment8ArtAnim` in `src/REL/mentDll/main.c`) copies the game's
  1P-4P sprite data and swaps in these bitmaps. Rerun after changing the art.
