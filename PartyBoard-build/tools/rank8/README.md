# 5th-8th rank labels (8-player party)

Approved by the user on 2026-10-16. Built by `gen.py` from the game's own art: the "4th" frame of the
board rank picture (`tex1_40x160_0f6eb81f8b991bb4_14`, colours and "th") and the white digit font
(`tex1_16x176_f2475068fb22a0b6_73b6cd56ac5039ef_9`, numeral shapes). Inputs come from a
`PARTYBOARD_DUMP_TEXTURES=1` dump and the HD beta's upscaled "4th".

- `rank5to8_1x.png`: the 40x160 strip, embedded in `src/game/board/rank5to8_tex.inc` (GX RGBA8 tiles).
- `tex1_40x160_3806220894f70c44_6.png`: the HD (4x) strip. Copy it into the HD app's
  `Contents/Resources/Textures/upscaled/`. The name is the embedded texture's hash; regenerate the
  `.inc` and the name changes (dump again to find it).
