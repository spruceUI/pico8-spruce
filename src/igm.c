/*
 * The look, layout and lookup order are RetroArch's spruce_igm.c and
 * spruce_igm_theme.c, so one igm.json themes both menus. RetroArch draws with
 * its GPU font driver; here the same nunwen.ttf is rasterised with
 * stb_truetype into a software canvas.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "igm.h"

/* stb_truetype would take these from libm, whose symbols are newer than the oldest devices' glibc */
static double igm_floor(double x)
{
  double i = (double)(long long)x;

  return x < i ? i - 1 : i;
}

static double igm_ceil(double x)
{
  double i = (double)(long long)x;

  return x > i ? i + 1 : i;
}

static double igm_sqrt(double x)
{
  double root, next;
  int i;

  if (x <= 0) {
    return 0;
  }
  root = x > 1 ? x : 1;
  for (i = 0; i < 64; i++) {
    next = (root + x / root) / 2;
    if (next >= root) {
      break;
    }
    root = next;
  }
  return root;
}

static double igm_fabs(double x)
{
  return x < 0 ? -x : x;
}

static double igm_fmod(double x, double y)
{
  return y == 0 ? 0 : x - (double)(long long)(x / y) * y;
}

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_ifloor(x) ((int)igm_floor(x))
#define STBTT_iceil(x) ((int)igm_ceil(x))
#define STBTT_sqrt(x) igm_sqrt(x)
#define STBTT_fabs(x) igm_fabs(x)
#define STBTT_fmod(x, y) igm_fmod(x, y)
#define STBTT_pow(x, y) __builtin_pow(x, y)
#define STBTT_cos(x) __builtin_cos(x)
#define STBTT_acos(x) __builtin_acos(x)
#define STBTT_assert(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define LOG(...) gfx_log(__VA_ARGS__)

#define FONT_PRIMARY "/mnt/SDCARD/Themes/SPRUCE/nunwen.ttf"
#define FONT_FALLBACK "/mnt/SDCARD/App/PixelReader/resources/fonts/nunwen.ttf"
#define BASE_DIR "/mnt/SDCARD/RetroArch"

#define IGM_PCT(total, pct) ((int)((total) * (pct) / 100.0f))

/* ---- theme ---- */

static igm_theme theme;
static int theme_loaded;

static void set_defaults(igm_theme *t)
{
  memset(t, 0, sizeof *t);

  t->rgba[IGM_COL_DIM_BG] = 0x000000D9u;
  t->rgba[IGM_COL_PANEL_BG] = 0x00000000u;
  t->rgba[IGM_COL_SELECTION] = 0xC9A2272Eu;
  t->rgba[IGM_COL_ACCENT] = 0xC9A227D9u;
  t->rgba[IGM_COL_TITLE_LINE] = 0x665C5466u;
  t->rgba[IGM_COL_TEXT] = 0xBDAD91FFu;
  t->rgba[IGM_COL_TEXT_SELECTED] = 0xD5C4A1FFu;
  t->rgba[IGM_COL_TEXT_TITLE] = 0x689D6AFFu;
  t->rgba[IGM_COL_TEXT_SHADOW] = 0x000000C0u;
  t->rgba[IGM_COL_BATTERY] = 0xBDAD91FFu;

  t->margin_pct = 2.0f;
  t->panel_w_pct = 38.0f;
  t->item_h_pct = 8.0f;
  t->title_h_pct = 8.0f;
  t->panel_x_auto = 1;
  t->panel_y_auto = 1;
  t->separator_inset_pct = 10.0f;
  t->arrow_inset_pct = 10.0f;
  t->accent_w_pct = 100.0f / 80.0f;
  t->separator_h_px = 1;
  t->shadow_offset_px = 2;
  t->text_baseline = 0.68f;
  t->title_baseline = 0.68f;
  t->scale = 1.0f;
  t->accent_bar = 1;
  t->shadow = 1;
  t->show_battery = 1;
  t->show_preview = 1;

  t->font_size_pct = 100.0f / 20.0f;
  t->font_size_small_pct = 100.0f / 28.0f;
  t->font_battery_size_pct = 100.0f / 28.0f;
  snprintf(t->font_path, sizeof t->font_path, "%s", FONT_PRIMARY);

  snprintf(t->title, sizeof t->title, "spruceOS Menu");
  snprintf(t->labels[IGM_RESUME], sizeof t->labels[0], "Resume");
  snprintf(t->labels[IGM_SAVE], sizeof t->labels[0], "Save");
  snprintf(t->labels[IGM_LOAD], sizeof t->labels[0], "Load");
  snprintf(t->labels[IGM_RESET], sizeof t->labels[0], "Reset");
  snprintf(t->labels[IGM_PICO8], sizeof t->labels[0], "PICO-8 Menu");
  snprintf(t->labels[IGM_EXIT], sizeof t->labels[0], "Exit Game");
  snprintf(t->auto_label, sizeof t->auto_label, "Auto");
  snprintf(t->slot_label, sizeof t->slot_label, "Slot");
  snprintf(t->arrow_left, sizeof t->arrow_left, "<");
  snprintf(t->arrow_right, sizeof t->arrow_right, ">");
}

static int parse_color(const char *s, uint32_t *out)
{
  size_t n, i;
  unsigned long v;

  if (*s == '#') {
    s++;
  } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
  }
  n = strlen(s);
  if (n != 6 && n != 8) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  v = strtoul(s, NULL, 16);
  *out = n == 6 ? (uint32_t)((v << 8) | 0xFF) : (uint32_t)v;
  return 1;
}

enum section { SEC_ROOT, SEC_COLORS, SEC_LAYOUT, SEC_FONT, SEC_TEXT, SEC_LABELS, SEC_UNKNOWN };

static const struct {
  const char *key;
  int id;
} color_keys[] = {
    {"dim_bg", IGM_COL_DIM_BG},         {"panel_bg", IGM_COL_PANEL_BG},
    {"selection", IGM_COL_SELECTION},   {"accent", IGM_COL_ACCENT},
    {"title_line", IGM_COL_TITLE_LINE}, {"text", IGM_COL_TEXT},
    {"text_selected", IGM_COL_TEXT_SELECTED}, {"text_title", IGM_COL_TEXT_TITLE},
    {"text_shadow", IGM_COL_TEXT_SHADOW},     {"battery", IGM_COL_BATTERY},
};

/* RetroArch's fourth row is "retroarch"; here it opens PICO-8's pause menu */
static const char *label_keys[IGM_ITEM_COUNT] = {"resume", "save", "load", "reset", "pico8", "exit"};

struct json {
  const char *s, *end, *path;
  igm_theme *t;
  int failed;
};

static void apply_string(struct json *j, int section, const char *key, const char *value)
{
  igm_theme *t = j->t;
  size_t i;

  switch (section) {
  case SEC_COLORS:
    for (i = 0; i < sizeof color_keys / sizeof *color_keys; i++) {
      if (strcmp(key, color_keys[i].key) == 0 && !parse_color(value, &t->rgba[color_keys[i].id])) {
        LOG("%s: bad colour \"%s\" for colors.%s, keeping the default\n", j->path, value, key);
      }
    }
    break;
  case SEC_FONT:
    if (strcmp(key, "path") == 0) {
      const char *slash = strrchr(j->path, '/');

      if (value[0] == '/' || slash == NULL) {
        snprintf(t->font_path, sizeof t->font_path, "%s", value);
      } else {
        snprintf(t->font_path, sizeof t->font_path, "%.*s/%s", (int)(slash - j->path), j->path,
                 value);
      }
    }
    break;
  case SEC_TEXT:
    if (strcmp(key, "title") == 0) {
      snprintf(t->title, sizeof t->title, "%s", value);
    } else if (strcmp(key, "auto") == 0) {
      snprintf(t->auto_label, sizeof t->auto_label, "%s", value);
    } else if (strcmp(key, "slot") == 0) {
      snprintf(t->slot_label, sizeof t->slot_label, "%s", value);
    } else if (strcmp(key, "arrow_left") == 0) {
      snprintf(t->arrow_left, sizeof t->arrow_left, "%s", value);
    } else if (strcmp(key, "arrow_right") == 0) {
      snprintf(t->arrow_right, sizeof t->arrow_right, "%s", value);
    }
    break;
  case SEC_LABELS:
    for (i = 0; i < IGM_ITEM_COUNT; i++) {
      if (strcmp(key, label_keys[i]) == 0) {
        snprintf(t->labels[i], sizeof t->labels[i], "%s", value);
      }
    }
    break;
  }
}

static void apply_number(struct json *j, int section, const char *key, float v)
{
  igm_theme *t = j->t;

  if (section == SEC_LAYOUT) {
    if (strcmp(key, "margin_pct") == 0) {
      t->margin_pct = v;
    } else if (strcmp(key, "panel_w_pct") == 0) {
      t->panel_w_pct = v;
    } else if (strcmp(key, "item_h_pct") == 0) {
      t->item_h_pct = v;
    } else if (strcmp(key, "title_h_pct") == 0) {
      t->title_h_pct = v;
    } else if (strcmp(key, "panel_x_pct") == 0) {
      t->panel_x_pct = v;
      t->panel_x_auto = 0;
    } else if (strcmp(key, "panel_y_pct") == 0) {
      t->panel_y_pct = v;
      t->panel_y_auto = 0;
    } else if (strcmp(key, "separator_inset_pct") == 0) {
      t->separator_inset_pct = v;
    } else if (strcmp(key, "arrow_inset_pct") == 0) {
      t->arrow_inset_pct = v;
    } else if (strcmp(key, "accent_w_pct") == 0) {
      t->accent_w_pct = v;
    } else if (strcmp(key, "separator_h_px") == 0) {
      t->separator_h_px = (int)v;
    } else if (strcmp(key, "shadow_offset_px") == 0) {
      t->shadow_offset_px = (int)v;
    } else if (strcmp(key, "text_baseline") == 0) {
      t->text_baseline = v;
    } else if (strcmp(key, "title_baseline") == 0) {
      t->title_baseline = v;
    } else if (strcmp(key, "scale") == 0) {
      t->scale = v;
    }
  } else if (section == SEC_FONT) {
    if (strcmp(key, "size_pct") == 0) {
      t->font_size_pct = v;
    } else if (strcmp(key, "size_small_pct") == 0) {
      t->font_size_small_pct = v;
    } else if (strcmp(key, "battery_size_pct") == 0) {
      t->font_battery_size_pct = v;
    }
  }
}

static void apply_bool(struct json *j, int section, const char *key, int v)
{
  igm_theme *t = j->t;

  if (section != SEC_LAYOUT) {
    return;
  }
  if (strcmp(key, "accent_bar") == 0) {
    t->accent_bar = v;
  } else if (strcmp(key, "shadow") == 0) {
    t->shadow = v;
  } else if (strcmp(key, "battery") == 0) {
    t->show_battery = v;
  } else if (strcmp(key, "preview") == 0) {
    t->show_preview = v;
  }
}

static int fail(struct json *j)
{
  j->failed = 1;
  return 0;
}

/* whitespace and both kinds of comment, as RetroArch's rjson allows */
static void skip_space(struct json *j)
{
  while (j->s < j->end) {
    if (isspace((unsigned char)*j->s)) {
      j->s++;
    } else if (j->s + 1 < j->end && j->s[0] == '/' && j->s[1] == '/') {
      while (j->s < j->end && *j->s != '\n') {
        j->s++;
      }
    } else if (j->s + 1 < j->end && j->s[0] == '/' && j->s[1] == '*') {
      j->s += 2;
      while (j->s + 1 < j->end && !(j->s[0] == '*' && j->s[1] == '/')) {
        j->s++;
      }
      if (j->s + 1 >= j->end) {
        fail(j);
        return;
      }
      j->s += 2;
    } else {
      return;
    }
  }
}

static void put_utf8(char *out, size_t cap, size_t *n, unsigned c)
{
  char bytes[4];
  size_t len, i;

  if (c < 0x80) {
    bytes[0] = (char)c;
    len = 1;
  } else if (c < 0x800) {
    bytes[0] = (char)(0xC0 | (c >> 6));
    bytes[1] = (char)(0x80 | (c & 0x3F));
    len = 2;
  } else {
    bytes[0] = (char)(0xE0 | (c >> 12));
    bytes[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    bytes[2] = (char)(0x80 | (c & 0x3F));
    len = 3;
  }
  for (i = 0; i < len && *n + 1 < cap; i++) {
    out[(*n)++] = bytes[i];
  }
}

static int parse_string(struct json *j, char *out, size_t cap)
{
  size_t n = 0;

  if (j->s >= j->end || *j->s != '"') {
    return fail(j);
  }
  j->s++;
  while (j->s < j->end && *j->s != '"') {
    char c = *j->s++;

    if (c == '\\') {
      if (j->s >= j->end) {
        return fail(j);
      }
      c = *j->s++;
      switch (c) {
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'u': {
        char hex[5];

        if (j->end - j->s < 4) {
          return fail(j);
        }
        memcpy(hex, j->s, 4);
        hex[4] = '\0';
        j->s += 4;
        put_utf8(out, cap, &n, (unsigned)strtoul(hex, NULL, 16));
        continue;
      }
      case '"':
      case '\\':
      case '/':
        break;
      default:
        return fail(j);
      }
    }
    if (n + 1 < cap) {
      out[n++] = c;
    }
  }
  if (j->s >= j->end) {
    return fail(j);
  }
  j->s++;
  out[n] = '\0';
  return 1;
}

static int parse_value(struct json *j, int depth, int section, const char *key);

static int section_for(const char *key)
{
  if (strcmp(key, "colors") == 0) {
    return SEC_COLORS;
  }
  if (strcmp(key, "layout") == 0) {
    return SEC_LAYOUT;
  }
  if (strcmp(key, "font") == 0) {
    return SEC_FONT;
  }
  if (strcmp(key, "text") == 0) {
    return SEC_TEXT;
  }
  if (strcmp(key, "labels") == 0) {
    return SEC_LABELS;
  }
  return SEC_UNKNOWN;
}

/* trailing commas are refused, as rjson refuses them */
static int parse_container(struct json *j, int depth, int section, int object)
{
  char close = object ? '}' : ']';
  char key[64] = "";

  j->s++;
  skip_space(j);
  if (j->s < j->end && *j->s == close) {
    j->s++;
    return 1;
  }
  for (;;) {
    skip_space(j);
    if (object) {
      if (!parse_string(j, key, sizeof key)) {
        return 0;
      }
      skip_space(j);
      if (j->s >= j->end || *j->s != ':') {
        return fail(j);
      }
      j->s++;
    }
    if (!parse_value(j, depth, object ? section : SEC_UNKNOWN, key)) {
      return 0;
    }
    skip_space(j);
    if (j->s < j->end && *j->s == ',') {
      j->s++;
      continue;
    }
    if (j->s < j->end && *j->s == close) {
      j->s++;
      return 1;
    }
    return fail(j);
  }
}

static int parse_value(struct json *j, int depth, int section, const char *key)
{
  /* scalars count only one level inside a section, as in RetroArch */
  int applies = depth == 2 && section != SEC_UNKNOWN;

  skip_space(j);
  if (j->s >= j->end || j->failed) {
    return fail(j);
  }
  if (*j->s == '{') {
    return parse_container(j, depth + 1, depth == 1 ? section_for(key) : SEC_UNKNOWN, 1);
  }
  if (*j->s == '[') {
    return parse_container(j, depth + 1, SEC_UNKNOWN, 0);
  }
  if (*j->s == '"') {
    char value[1024];

    if (!parse_string(j, value, sizeof value)) {
      return 0;
    }
    if (applies) {
      apply_string(j, section, key, value);
    }
    return 1;
  }
  if (j->end - j->s >= 4 && strncmp(j->s, "true", 4) == 0) {
    j->s += 4;
    if (applies) {
      apply_bool(j, section, key, 1);
    }
    return 1;
  }
  if (j->end - j->s >= 5 && strncmp(j->s, "false", 5) == 0) {
    j->s += 5;
    if (applies) {
      apply_bool(j, section, key, 0);
    }
    return 1;
  }
  if (j->end - j->s >= 4 && strncmp(j->s, "null", 4) == 0) {
    j->s += 4;
    return 1;
  }
  if (*j->s == '-' || isdigit((unsigned char)*j->s)) {
    char *after;
    double v = strtod(j->s, &after);

    if (after == j->s || after > j->end) {
      return fail(j);
    }
    j->s = after;
    if (applies) {
      apply_number(j, section, key, (float)v);
    }
    return 1;
  }
  return fail(j);
}

/* a file that is there stops the search, even when it does not parse */
static int load_file(igm_theme *dst, const char *path)
{
  static igm_theme scratch;
  struct json j;
  FILE *file = fopen(path, "rb");
  char *text;
  long size;

  if (file == NULL) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0 ||
      (text = malloc((size_t)size + 1)) == NULL) {
    fclose(file);
    LOG("%s could not be read, using the built-in look\n", path);
    return 1;
  }
  if (fread(text, 1, (size_t)size, file) != (size_t)size) {
    size = 0;
  }
  fclose(file);
  text[size] = '\0';

  set_defaults(&scratch);
  memset(&j, 0, sizeof j);
  j.s = text;
  j.end = text + size;
  j.path = path;
  j.t = &scratch;
  if (j.end - j.s >= 3 && memcmp(j.s, "\xEF\xBB\xBF", 3) == 0) {
    j.s += 3;
  }
  skip_space(&j);
  if (j.s < j.end && *j.s == '{' && parse_container(&j, 1, SEC_ROOT, 1)) {
    skip_space(&j);
    if (j.s == j.end && !j.failed) {
      *dst = scratch;
      LOG("menu look from %s\n", path);
      free(text);
      return 1;
    }
  }
  LOG("%s is not valid JSON near byte %ld, using the built-in look\n", path, (long)(j.s - text));
  free(text);
  return 1;
}

const igm_theme *igm_theme_get(void)
{
  const char *dir = getenv("SPRUCE_THEME_DIR");
  char path[1024];

  if (theme_loaded) {
    return &theme;
  }
  theme_loaded = 1;
  set_defaults(&theme);
  if (dir != NULL && *dir != '\0') {
    snprintf(path, sizeof path, "%s/igm.json", dir);
    if (load_file(&theme, path)) {
      return &theme;
    }
  }
  if (load_file(&theme, BASE_DIR "/igm.json") || load_file(&theme, BASE_DIR "/igm/spruce.json")) {
    return &theme;
  }
  LOG("no igm.json found, using the built-in look\n");
  return &theme;
}

/* ---- font ---- */

static struct {
  int tried, ok;
  unsigned char *data;
  stbtt_fontinfo info;
} font;

static int load_font_file(const char *path)
{
  FILE *file = fopen(path, "rb");
  long size;
  int offset;

  if (file == NULL) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0 ||
      (font.data = malloc((size_t)size)) == NULL) {
    fclose(file);
    return 0;
  }
  if (fread(font.data, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    free(font.data);
    font.data = NULL;
    return 0;
  }
  fclose(file);
  offset = stbtt_GetFontOffsetForIndex(font.data, 0);
  if (offset < 0 || !stbtt_InitFont(&font.info, font.data, offset)) {
    free(font.data);
    font.data = NULL;
    return 0;
  }
  return 1;
}

static int open_font(const igm_theme *t)
{
  if (font.tried) {
    return font.ok;
  }
  font.tried = 1;
  font.ok = load_font_file(t->font_path);
  if (!font.ok) {
    LOG("font %s not found, using the stock one\n", t->font_path);
    font.ok = load_font_file(FONT_PRIMARY) || load_font_file(FONT_FALLBACK);
  }
  if (!font.ok) {
    LOG("no font could be opened, the menu has no text\n");
  }
  return font.ok;
}

static unsigned next_codepoint(const char **text)
{
  const unsigned char *s = (const unsigned char *)*text;
  unsigned c = *s++;
  int extra = 0;

  if (c >= 0xF0) {
    c &= 0x07;
    extra = 3;
  } else if (c >= 0xE0) {
    c &= 0x0F;
    extra = 2;
  } else if (c >= 0xC0) {
    c &= 0x1F;
    extra = 1;
  }
  while (extra-- > 0 && (*s & 0xC0) == 0x80) {
    c = (c << 6) | (*s++ & 0x3F);
  }
  *text = (const char *)s;
  return c;
}

static float text_width(float px, const char *text)
{
  float scale = stbtt_ScaleForMappingEmToPixels(&font.info, px);
  float width = 0;
  int advance, bearing;

  while (*text) {
    stbtt_GetCodepointHMetrics(&font.info, (int)next_codepoint(&text), &advance, &bearing);
    width += advance * scale;
  }
  return width;
}

/* ---- canvas ---- */

/* colour is 0xRRGGBBAA, the form igm.json and RetroArch use, its alpha scaled by cover (0-255) */
static void blend(uint32_t *dst, uint32_t colour, unsigned cover)
{
  unsigned a = ((colour & 0xFF) * cover + 127) / 255;
  unsigned r = colour >> 24, g = (colour >> 16) & 0xFF, b = (colour >> 8) & 0xFF;
  uint32_t d = *dst;
  unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;

  if (a == 0) {
    return;
  }
  dr = (dr * (255 - a) + r * a + 127) / 255;
  dg = (dg * (255 - a) + g * a + 127) / 255;
  db = (db * (255 - a) + b * a + 127) / 255;
  *dst = 0xFF000000u | (dr << 16) | (dg << 8) | db;
}

static void fill(igm_canvas *c, int x, int y, int w, int h, uint32_t colour)
{
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
  int px, py;

  if ((colour & 0xFF) == 0) {
    return;
  }
  for (py = y0; py < y1; py++) {
    for (px = x0; px < x1; px++) {
      blend(&c->pixels[py * c->w + px], colour, 255);
    }
  }
}

enum align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* y is the baseline, as gfx_display_draw_text takes it */
static void draw_glyphs(igm_canvas *c, float px, const char *text, float x, float y, uint32_t colour)
{
  float scale = stbtt_ScaleForMappingEmToPixels(&font.info, px);
  int baseline = (int)igm_floor(y + 0.5);

  while (*text) {
    int glyph = stbtt_FindGlyphIndex(&font.info, (int)next_codepoint(&text));
    int advance, bearing, x0, y0, x1, y1, gx, gy, w, h;
    float shift = x - (float)igm_floor(x);
    unsigned char *bitmap;

    stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, &bearing);
    stbtt_GetGlyphBitmapBoxSubpixel(&font.info, glyph, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
    w = x1 - x0;
    h = y1 - y0;
    if (w > 0 && h > 0 && (bitmap = malloc((size_t)w * h)) != NULL) {
      int left = (int)igm_floor(x) + x0, top = baseline + y0;

      stbtt_MakeGlyphBitmapSubpixel(&font.info, bitmap, w, h, w, scale, scale, shift, 0, glyph);
      for (gy = 0; gy < h; gy++) {
        if (top + gy < 0 || top + gy >= c->h) {
          continue;
        }
        for (gx = 0; gx < w; gx++) {
          if (left + gx >= 0 && left + gx < c->w && bitmap[gy * w + gx]) {
            blend(&c->pixels[(top + gy) * c->w + left + gx], colour, bitmap[gy * w + gx]);
          }
        }
      }
      free(bitmap);
    }
    x += advance * scale;
  }
}

static void draw_text(igm_canvas *c, const igm_theme *t, float px, const char *text, float x, float y,
                      uint32_t colour, enum align align)
{
  if (!font.ok || text[0] == '\0') {
    return;
  }
  if (align == ALIGN_CENTER) {
    x -= text_width(px, text) / 2;
  } else if (align == ALIGN_RIGHT) {
    x -= text_width(px, text);
  }
  if (t->shadow) {
    draw_glyphs(c, px, text, x + t->shadow_offset_px, y + t->shadow_offset_px,
                t->rgba[IGM_COL_TEXT_SHADOW]);
  }
  draw_glyphs(c, px, text, x, y, colour);
}

/* bilinear, as RetroArch's mipmapped texture is drawn smoothed */
static void draw_preview(igm_canvas *c, const igm_image *img, int x, int y, int w, int h)
{
  int px, py;

  for (py = 0; py < h; py++) {
    float sy = ((py + 0.5f) * img->h / h) - 0.5f;
    int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < img->h ? y0 + 1 : y0;
    unsigned fy = (unsigned)((sy < 0 ? 0 : sy - y0) * 256);

    if (y + py < 0 || y + py >= c->h) {
      continue;
    }
    for (px = 0; px < w; px++) {
      float sx = ((px + 0.5f) * img->w / w) - 0.5f;
      int x0 = sx < 0 ? 0 : (int)sx, x1 = x0 + 1 < img->w ? x0 + 1 : x0;
      unsigned fx = (unsigned)((sx < 0 ? 0 : sx - x0) * 256);
      uint32_t a = img->pixels[y0 * img->w + x0], b = img->pixels[y0 * img->w + x1];
      uint32_t d = img->pixels[y1 * img->w + x0], e = img->pixels[y1 * img->w + x1];
      uint32_t out = 0xFF000000u;
      int shift;

      if (x + px < 0 || x + px >= c->w) {
        continue;
      }
      for (shift = 0; shift < 24; shift += 8) {
        unsigned top = (((a >> shift) & 0xFF) * (256 - fx) + ((b >> shift) & 0xFF) * fx) >> 8;
        unsigned bottom = (((d >> shift) & 0xFF) * (256 - fx) + ((e >> shift) & 0xFF) * fx) >> 8;

        out |= ((top * (256 - fy) + bottom * fy) >> 8) << shift;
      }
      c->pixels[(y + py) * c->w + x + px] = out;
    }
  }
}

void igm_draw(igm_canvas *c, const uint32_t *frame, int selected, int slot, int battery,
              const igm_image *preview)
{
  const igm_theme *t = igm_theme_get();
  int vw = c->w, vh = c->h, i;
  double s = t->scale;
  int margin = (int)(IGM_PCT(vw, t->margin_pct) * s);
  int panel_w = (int)(IGM_PCT(vw, t->panel_w_pct) * s);
  int item_h = (int)(IGM_PCT(vh, t->item_h_pct) * s);
  int title_h = (int)(IGM_PCT(vh, t->title_h_pct) * s);
  int panel_h = item_h * IGM_ITEM_COUNT + title_h;
  int panel_x = t->panel_x_auto ? margin : IGM_PCT(vw, t->panel_x_pct);
  int panel_y = t->panel_y_auto ? (vh - panel_h) / 2 : IGM_PCT(vh, t->panel_y_pct);
  int text_cx = panel_x + panel_w / 2;
  int sep_inset = IGM_PCT(panel_w, t->separator_inset_pct);
  int sep_w = panel_w - IGM_PCT(panel_w, t->separator_inset_pct * 2.0f);
  int arrow_in = IGM_PCT(panel_w, t->arrow_inset_pct);
  int accent_w = IGM_PCT(panel_w, t->accent_w_pct);
  float font_px = vh * t->font_size_pct / 100.0f * t->scale;
  float small_px = vh * t->font_size_small_pct / 100.0f * t->scale;

  if (accent_w < 2) {
    accent_w = 2;
  }
  open_font(t);

  memcpy(c->pixels, frame, (size_t)vw * vh * sizeof *c->pixels);
  fill(c, 0, 0, vw, vh, t->rgba[IGM_COL_DIM_BG]);
  fill(c, panel_x, panel_y, panel_w, panel_h, t->rgba[IGM_COL_PANEL_BG]);

  if (t->show_battery && battery >= 0) {
    char text[8];

    snprintf(text, sizeof text, "%d%%", battery);
    draw_text(c, t, small_px, text, (float)(vw - margin),
              (float)(margin + vh * t->font_battery_size_pct / 100.0f), t->rgba[IGM_COL_BATTERY],
              ALIGN_RIGHT);
  }

  draw_text(c, t, small_px, t->title, (float)text_cx, (float)(panel_y + title_h * t->title_baseline),
            t->rgba[IGM_COL_TEXT_TITLE], ALIGN_CENTER);
  if (t->separator_h_px > 0) {
    fill(c, panel_x + sep_inset, panel_y + title_h - t->separator_h_px, sep_w, t->separator_h_px,
         t->rgba[IGM_COL_TITLE_LINE]);
  }

  for (i = 0; i < IGM_ITEM_COUNT; i++) {
    int iy = panel_y + title_h + i * item_h;
    uint32_t colour = i == selected ? t->rgba[IGM_COL_TEXT_SELECTED] : t->rgba[IGM_COL_TEXT];
    float baseline = (float)(iy + item_h * t->text_baseline);
    char label[128];

    if (i == selected) {
      fill(c, panel_x, iy, panel_w, item_h, t->rgba[IGM_COL_SELECTION]);
      if (t->accent_bar) {
        fill(c, panel_x, iy, accent_w, item_h, t->rgba[IGM_COL_ACCENT]);
      }
    }

    if (i == IGM_SAVE || i == IGM_LOAD) {
      if (slot < 0) {
        snprintf(label, sizeof label, "%s %s", t->labels[i], t->auto_label);
      } else {
        snprintf(label, sizeof label, "%s %s %d", t->labels[i], t->slot_label, slot);
      }
      draw_text(c, t, font_px, t->arrow_left, (float)(panel_x + arrow_in), baseline, colour,
                ALIGN_CENTER);
      draw_text(c, t, font_px, t->arrow_right, (float)(panel_x + panel_w - arrow_in), baseline,
                colour, ALIGN_CENTER);
    } else {
      snprintf(label, sizeof label, "%s", t->labels[i]);
    }
    draw_text(c, t, font_px, label, (float)text_cx, baseline, colour, ALIGN_CENTER);
  }

  if (t->show_preview && preview != NULL && preview->pixels != NULL && preview->w > 0 &&
      preview->h > 0) {
    int area_x = panel_x + panel_w + margin;
    int area_w = vw - area_x - margin;
    int area_h = panel_h;

    if (area_w > 0) {
      float sw = (float)area_w / preview->w, sh = (float)area_h / preview->h;
      float scale = sw < sh ? sw : sh;
      int w = (int)(preview->w * scale), h = (int)(preview->h * scale);

      draw_preview(c, preview, area_x + (area_w - w) / 2, panel_y + (panel_h - h) / 2, w, h);
    }
  }
}
