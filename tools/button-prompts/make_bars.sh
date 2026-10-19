#!/bin/zsh
# make_bars.sh <smooth_dir> <glyph_dir> <out_dir>: the four menu bars with
# their painted-in buttons replaced, per controller family (default mapping).
SM=$1; G=$2; O=$3; BP=${BARPATCH:-barpatch}
for f in xbox playstation switch; do mkdir -p $O/$f
 $BP $SM/tex1_216x36_994c24e6f5ffa5b3_351b4707fc250a31_9.png $O/$f/tex1_216x36_994c24e6f5ffa5b3_351b4707fc250a31_9.png 13 7 34 28 $G/$f/east.png
 $BP $SM/tex1_216x58_4fc5a36dd42f2f7d_14.png $O/$f/tex1_216x58_4fc5a36dd42f2f7d_14.png 13 5 34 26 $G/$f/south.png 13 31 38 53 $G/$f/east.png
 $BP $SM/tex1_216x58_fa29a7f65a1aebb7_a32e580624f93fc5_9.png $O/$f/tex1_216x58_fa29a7f65a1aebb7_a32e580624f93fc5_9.png 13 3 34 26 $G/$f/start.png 13 32 34 53 $G/$f/east.png
 $BP $SM/tex1_216x62_cbb6608a3ac153ca_93eaf50c52184af7_9.png $O/$f/tex1_216x62_cbb6608a3ac153ca_93eaf50c52184af7_9.png 13 6 34 27 $G/$f/south.png 13 34 34 55 $G/$f/east.png
done
# The "Practice (Z) Back" banner: its checker background is rebuilt under the
# Z button (checker_fill.py) before the glyph is drawn; see art/*/bars.
