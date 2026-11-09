// Button icons for the device in use (ui/glyphs/glyphs.c): MKW's button glyph sheet (PicFont), redrawn as it is uploaded.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The texture's level 0, as uploaded: format (NVN), w x h texels, its linear rows `row` bytes apart in lin. When it is the
// button glyph sheet its texels are replaced by the icons for the device in use; returns 1 when it is the sheet (whether
// or not it changed it), so the caller can upload it again when glyphs_generation changes.
int glyphs_apply(uint64_t addr, int format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row);

// The icons in effect: MKW_BUTTON_ICONS, or with "auto" the device used last (input_last_device) and the controller's
// labels. Any thread.
enum { GLYPHS_NINTENDO, GLYPHS_XBOX, GLYPHS_PLAYSTATION, GLYPHS_KEYBOARD };
int glyphs_kind(void);

// Changes whenever the icons to draw change (device, controller, bindings, setting); polled by the upload path.
uint32_t glyphs_generation(void);

#ifdef __cplusplus
}
#endif
