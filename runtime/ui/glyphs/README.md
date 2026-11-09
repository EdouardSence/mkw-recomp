# runtime/ui/glyphs

Button icons matching the controller: in the game's icon sheet and in the menu.

| File | What it is |
|---|---|
| `glyphs.c`, `glyphs.h` | Button icons matching the controller: the game's icon sheet (PicFont, 40 cells) is redrawn on upload with Xbox, PlayStation or keyboard and mouse buttons, following the bindings; `MKW_BUTTON_ICONS` (auto from the last device). `MKW_GLYPH_DUMP=<dir>` dumps the sheet |
| `glyph_draw.c`, `glyph_draw.h` | Drawing those icons: shapes by signed distance and text with stb_truetype (from SavageWindRecomp, MIT), composed in the game's style (white body, grey symbol, translucent border) |
| `pad_icons.cpp`, `pad_icons.h` | The same icons with ImGui primitives for the "Controller" column of Controls (F1 menu and start screen) |
