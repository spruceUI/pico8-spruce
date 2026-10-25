/*
 * The spruce in-game menu, drawn the way RetroArch's spruce_igm.c draws it,
 * from the same igm.json, into a canvas of 0xAARRGGBB pixels.
 */

#ifndef IGM_H
#define IGM_H

#include <stdint.h>

enum igm_item { IGM_RESUME, IGM_SAVE, IGM_LOAD, IGM_RESET, IGM_PICO8, IGM_EXIT, IGM_ITEM_COUNT };

enum igm_color {
  IGM_COL_DIM_BG,
  IGM_COL_PANEL_BG,
  IGM_COL_SELECTION,
  IGM_COL_ACCENT,
  IGM_COL_TITLE_LINE,
  IGM_COL_TEXT,
  IGM_COL_TEXT_SELECTED,
  IGM_COL_TEXT_TITLE,
  IGM_COL_TEXT_SHADOW,
  IGM_COL_BATTERY,
  IGM_COL_COUNT
};

typedef struct {
  uint32_t rgba[IGM_COL_COUNT];
  float margin_pct, panel_w_pct, item_h_pct, title_h_pct;
  float panel_x_pct, panel_y_pct;
  float separator_inset_pct, arrow_inset_pct, accent_w_pct;
  float text_baseline, title_baseline, scale;
  int separator_h_px, shadow_offset_px;
  int panel_x_auto, panel_y_auto, accent_bar, shadow, show_battery, show_preview;
  float font_size_pct, font_size_small_pct, font_battery_size_pct;
  char font_path[1024];
  char title[64];
  char labels[IGM_ITEM_COUNT][48];
  char auto_label[24], slot_label[24], arrow_left[8], arrow_right[8];
} igm_theme;

typedef struct {
  uint32_t *pixels;
  int w, h;
} igm_canvas;

typedef struct {
  const uint32_t *pixels;
  int w, h;
} igm_image;

/* read once, on the first call */
const igm_theme *igm_theme_get(void);

/*
 * Draws the menu over frame, the game as it was when the menu opened, at the
 * canvas size. slot is -1 for the auto slot, battery -1 when unknown, and
 * preview the selected slot's picture, or pixels NULL for none.
 */
void igm_draw(igm_canvas *canvas, const uint32_t *frame, int selected, int slot, int battery,
              const igm_image *preview);

#endif
