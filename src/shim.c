/*
 * Preloaded into pico-8: the spruce in-game menu and save states.
 *
 * spruce's menu button watchdog sends SIGUSR2 on a tap of Menu, as it does to
 * PPSSPP and PCSX. The next SDL_RenderPresent reads back the finished frame and
 * runs the menu over it on pico-8's main thread, with its audio paused, until
 * it closes. The menu reads the pad straight from evdev, from the node and
 * B_* codes the platform .cfg exports, since every device hands pico-8 its
 * buttons a different way.
 *
 * Save states are state.c, from josegonzalez/minui-pico-8-pak. They are named
 * as RetroArch names its own, in PICO_PAK_STATES_DIR, with a .png beside each.
 * The auto slot is saved on Exit Game and on SIGTERM, and put back when the
 * cart next starts.
 *
 * Every SDL call goes through dlsym, so this builds without SDL headers.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfx.h"
#include "igm.h"
#include "state.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#define EXPORT __attribute__((visibility("default")))
#define LOG(...) gfx_log(__VA_ARGS__)

typedef struct {
  int x, y, w, h;
} SDL_Rect;

#define SDL_PIXELFORMAT_ARGB8888 0x16362004
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_AUDIO_PLAYING 1
#define SDL_QUIT 0x100
#define SDL_KEYDOWN 0x300
#define SDL_KEYUP 0x301
#define SDL_SCANCODE_R 21
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_LCTRL 224
#define SDLK_r 'r'
#define SDLK_RETURN '\r'
#define SDLK_LCTRL 0x400000e0
#define KMOD_LCTRL 0x0040

#define KEY_STEP_MS 50
#define TERMINATE_WAIT_S 8
#define RESUME_WAIT_FRAMES 600
#define RESUME_SAFE_FRAMES 300
#define THUMB_MAX_W 640
#define SLOT_AUTO (-1)
#define SLOT_MAX 999

void gfx_log(const char *format, ...)
{
  va_list args;

  fprintf(stderr, "pico8-igm: ");
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

static struct {
  int resolved;
  void (*RenderPresent)(void *);
  uint32_t (*GetTicks)(void);
  int (*GetModState)(void);
  int (*PollEvent)(void *);
  int (*PushEvent)(void *);
  int (*GetRendererOutputSize)(void *, int *, int *);
  int (*RenderReadPixels)(void *, const SDL_Rect *, uint32_t, void *, int);
  void *(*CreateTexture)(void *, uint32_t, int, int, int);
  void (*DestroyTexture)(void *);
  int (*UpdateTexture)(void *, const SDL_Rect *, const void *, int);
  int (*SetRenderDrawColor)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
  int (*RenderClear)(void *);
  int (*RenderCopy)(void *, void *, const SDL_Rect *, const SDL_Rect *);
  void (*RenderGetLogicalSize)(void *, int *, int *);
  int (*RenderSetLogicalSize)(void *, int, int);
  void (*RenderGetViewport)(void *, SDL_Rect *);
  int (*RenderSetViewport)(void *, const SDL_Rect *);
  void (*RenderGetScale)(void *, float *, float *);
  int (*RenderSetScale)(void *, float, float);
  int (*RenderIsClipEnabled)(void *);
  void (*RenderGetClipRect)(void *, SDL_Rect *);
  int (*RenderSetClipRect)(void *, const SDL_Rect *);
  int (*OpenAudio)(void *, void *);
  uint32_t (*OpenAudioDevice)(const char *, int, const void *, void *, int);
  void (*PauseAudioDevice)(uint32_t, int);
  int (*GetAudioDeviceStatus)(uint32_t);
  void (*LockAudioDevice)(uint32_t);
  void (*UnlockAudioDevice)(uint32_t);
  void (*Delay)(uint32_t);
  const char *(*GetError)(void);
  int (*PeepEvents)(void *, int, int, uint32_t, uint32_t);
  int (*WaitEvent)(void *);
  int (*WaitEventTimeout)(void *, int);
  const uint8_t *(*GetKeyboardState)(int *);
  void (*PumpEvents)(void);
} sdl;

static int is_pico8;
/* a key pico-8 must not see, the Mini's Menu, which its SDL sends as Escape */
static int hidden_scancode = -1;
static void release_hidden_key(void);
/* PICO8_IGM_DEBUG: what pico-8 reads of the keyboard, to Saves/spruce/pico8-igm-debug.log */
static FILE *debug_log;

static void debug(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void debug(const char *format, ...)
{
  va_list args;

  if (debug_log == NULL) {
    return;
  }
  fprintf(debug_log, "%u ", sdl.GetTicks != NULL ? sdl.GetTicks() : 0);
  va_start(args, format);
  vfprintf(debug_log, format, args);
  va_end(args);
  fflush(debug_log);
}

static void debug_key_event(const char *from, const void *event, int hidden)
{
  uint32_t type = *(const uint32_t *)event;
  int32_t scancode, sym;

  if (debug_log == NULL || (type != SDL_KEYDOWN && type != SDL_KEYUP)) {
    return;
  }
  memcpy(&scancode, (const unsigned char *)event + 16, sizeof scancode);
  memcpy(&sym, (const unsigned char *)event + 20, sizeof sym);
  debug("%s %s scancode %d sym %d%s\n", from, type == SDL_KEYDOWN ? "down" : "up", scancode, sym,
        hidden ? " (hidden)" : "");
}

/* the keyboard state pico-8 could poll, logged as it changes */
static void debug_keyboard_state(void)
{
  static uint8_t last[512];
  const uint8_t *now;
  int count = 0, i;

  if (debug_log == NULL || sdl.GetKeyboardState == NULL) {
    return;
  }
  now = sdl.GetKeyboardState(&count);
  for (i = 0; now != NULL && i < count && i < 512; i++) {
    if (now[i] != last[i]) {
      debug("keyboard state scancode %d %s\n", i, now[i] ? "down" : "up");
      last[i] = now[i];
    }
  }
}
static volatile sig_atomic_t menu_requests, terminating;
static sig_atomic_t menu_seen;
static int quitting;

static const char *states_dir, *cart;
static int slot;
static int32_t tick_offset;
static uint32_t audio_device;

/* the auto slot waiting for the cart to start, and the frames since it was put back */
static int resume_pending = -1, resume_frames = -1;
static char resume_marker[4096];

/* ---- SDL ---- */

#define RESOLVE(name) (*(void **)&sdl.name = dlsym(RTLD_NEXT, "SDL_" #name))

static int resolve_sdl(void)
{
  if (sdl.resolved) {
    return sdl.resolved > 0;
  }
  RESOLVE(RenderPresent);
  RESOLVE(GetTicks);
  RESOLVE(GetModState);
  RESOLVE(PollEvent);
  RESOLVE(PushEvent);
  RESOLVE(GetRendererOutputSize);
  RESOLVE(RenderReadPixels);
  RESOLVE(CreateTexture);
  RESOLVE(DestroyTexture);
  RESOLVE(UpdateTexture);
  RESOLVE(SetRenderDrawColor);
  RESOLVE(RenderClear);
  RESOLVE(RenderCopy);
  RESOLVE(RenderGetLogicalSize);
  RESOLVE(RenderSetLogicalSize);
  RESOLVE(RenderGetViewport);
  RESOLVE(RenderSetViewport);
  RESOLVE(RenderGetScale);
  RESOLVE(RenderSetScale);
  RESOLVE(RenderIsClipEnabled);
  RESOLVE(RenderGetClipRect);
  RESOLVE(RenderSetClipRect);
  RESOLVE(OpenAudio);
  RESOLVE(OpenAudioDevice);
  RESOLVE(PauseAudioDevice);
  RESOLVE(GetAudioDeviceStatus);
  RESOLVE(LockAudioDevice);
  RESOLVE(UnlockAudioDevice);
  RESOLVE(Delay);
  RESOLVE(GetError);
  RESOLVE(PeepEvents);
  RESOLVE(WaitEvent);
  RESOLVE(WaitEventTimeout);
  RESOLVE(GetKeyboardState);
  RESOLVE(PumpEvents);

  sdl.resolved = sdl.RenderPresent && sdl.GetTicks && sdl.PollEvent && sdl.PushEvent &&
                         sdl.GetRendererOutputSize && sdl.RenderReadPixels && sdl.CreateTexture &&
                         sdl.DestroyTexture && sdl.UpdateTexture && sdl.RenderClear &&
                         sdl.RenderCopy && sdl.Delay
                     ? 1
                     : -1;
  if (sdl.resolved < 0) {
    LOG("this SDL2 lacks a function the menu needs, the menu is off\n");
  }
  return sdl.resolved > 0;
}

static uint32_t real_ticks(void)
{
  return sdl.GetTicks != NULL ? sdl.GetTicks() : 0;
}

struct letterbox {
  int logical_w, logical_h, clip_enabled;
  float scale_x, scale_y;
  SDL_Rect viewport, clip;
};

/* pico-8 may letterbox, which would squeeze the menu into the game's square */
static void clear_letterbox(void *renderer, struct letterbox *saved)
{
  memset(saved, 0, sizeof *saved);
  saved->scale_x = saved->scale_y = 1.0f;
  if (sdl.RenderGetLogicalSize && sdl.RenderSetLogicalSize) {
    sdl.RenderGetLogicalSize(renderer, &saved->logical_w, &saved->logical_h);
    sdl.RenderSetLogicalSize(renderer, 0, 0);
  }
  if (sdl.RenderGetScale && sdl.RenderSetScale) {
    sdl.RenderGetScale(renderer, &saved->scale_x, &saved->scale_y);
    sdl.RenderSetScale(renderer, 1.0f, 1.0f);
  }
  if (sdl.RenderGetViewport && sdl.RenderSetViewport) {
    sdl.RenderGetViewport(renderer, &saved->viewport);
    sdl.RenderSetViewport(renderer, NULL);
  }
  if (sdl.RenderIsClipEnabled && sdl.RenderGetClipRect && sdl.RenderSetClipRect) {
    saved->clip_enabled = sdl.RenderIsClipEnabled(renderer);
    sdl.RenderGetClipRect(renderer, &saved->clip);
    sdl.RenderSetClipRect(renderer, NULL);
  }
}

static void restore_letterbox(void *renderer, const struct letterbox *saved)
{
  if (sdl.RenderSetLogicalSize && saved->logical_w > 0 && saved->logical_h > 0) {
    sdl.RenderSetLogicalSize(renderer, saved->logical_w, saved->logical_h);
  }
  if (sdl.RenderSetScale) {
    sdl.RenderSetScale(renderer, saved->scale_x, saved->scale_y);
  }
  if (sdl.RenderSetViewport) {
    sdl.RenderSetViewport(renderer, &saved->viewport);
  }
  if (sdl.RenderSetClipRect) {
    sdl.RenderSetClipRect(renderer, saved->clip_enabled ? &saved->clip : NULL);
  }
}

static void clear_to_black(void *renderer)
{
  struct letterbox saved;

  clear_letterbox(renderer, &saved);
  if (sdl.SetRenderDrawColor) {
    sdl.SetRenderDrawColor(renderer, 0, 0, 0, 255);
  }
  sdl.RenderClear(renderer);
  restore_letterbox(renderer, &saved);
}

static void request_quit(void)
{
  /* SDL_Event is a 56 byte union and SDL_QUIT needs only its type */
  uint32_t event[14] = {SDL_QUIT};

  quitting = 1;
  sdl.PushEvent(event);
}

/* ---- keys pressed for pico-8 ---- */

struct key_step {
  uint32_t type;
  int32_t scancode, sym;
  uint16_t mod;
};

static struct key_step key_steps[4];
static int key_step_count, key_step_next, ctrl_held;
static uint32_t key_step_at;

static void push_key(const struct key_step *step)
{
  /* SDL_KeyboardEvent: the state at byte 12, then the scancode, symbol and modifiers */
  uint32_t event[14] = {step->type};
  unsigned char *bytes = (unsigned char *)event;

  bytes[12] = step->type == SDL_KEYDOWN ? 1 : 0;
  memcpy(bytes + 16, &step->scancode, sizeof step->scancode);
  memcpy(bytes + 20, &step->sym, sizeof step->sym);
  memcpy(bytes + 24, &step->mod, sizeof step->mod);
  sdl.PushEvent(event);
}

/* one step a frame, KEY_STEP_MS apart, so pico-8 reads each on its own */
static void press_keys(const struct key_step *steps, int count)
{
  memcpy(key_steps, steps, (size_t)count * sizeof *steps);
  key_step_count = count;
  key_step_next = 0;
  key_step_at = real_ticks() - KEY_STEP_MS;
}

static void continue_keys(void)
{
  const struct key_step *step;

  if (key_step_next >= key_step_count || real_ticks() - key_step_at < KEY_STEP_MS) {
    return;
  }
  step = &key_steps[key_step_next++];
  key_step_at = real_ticks();
  if (step->scancode == SDL_SCANCODE_LCTRL) {
    ctrl_held = step->type == SDL_KEYDOWN;
  }
  push_key(step);
}

static void reset_cart(void)
{
  static const struct key_step steps[] = {
      {SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL, KMOD_LCTRL},
      {SDL_KEYDOWN, SDL_SCANCODE_R, SDLK_r, KMOD_LCTRL},
      {SDL_KEYUP, SDL_SCANCODE_R, SDLK_r, KMOD_LCTRL},
      {SDL_KEYUP, SDL_SCANCODE_LCTRL, SDLK_LCTRL, 0},
  };

  LOG("pressing Ctrl+R to reset the cart\n");
  press_keys(steps, 4);
}

static void open_pico8_menu(void)
{
  static const struct key_step steps[] = {
      {SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN, 0},
      {SDL_KEYUP, SDL_SCANCODE_RETURN, SDLK_RETURN, 0},
  };

  LOG("pressing Enter for pico-8's pause menu\n");
  press_keys(steps, 2);
}

/* ---- the pad ---- */

enum { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_COUNT };

static struct {
  int type, code, sign, set;
} pad_keys[PAD_COUNT];
static const char *input_path;

/* "1 305" for a key, "3 17 -1" for a hat's direction, as spruce's .cfg files write them */
static void load_pad(void)
{
  static const char *names[PAD_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B"};
  char name[32];
  int i;

  input_path = getenv("PICO8_IGM_INPUT");
  if (input_path == NULL || *input_path == '\0') {
    input_path = getenv("EVENT_PATH_READ_INPUTS_SPRUCE");
  }
  for (i = 0; i < PAD_COUNT; i++) {
    const char *value;
    int type, code, sign = 1, got;

    snprintf(name, sizeof name, "PICO8_IGM_B_%s", names[i]);
    value = getenv(name);
    if (value == NULL || *value == '\0') {
      snprintf(name, sizeof name, "B_%s", names[i]);
      value = getenv(name);
    }
    if (value == NULL || (got = sscanf(value, "%d %d %d", &type, &code, &sign)) < 2) {
      LOG("no button for %s, the menu cannot use it\n", names[i]);
      continue;
    }
    pad_keys[i].type = type;
    pad_keys[i].code = code;
    pad_keys[i].sign = got == 3 && type == EV_ABS && sign < 0 ? -1 : 1;
    pad_keys[i].set = 1;
  }
}

static unsigned read_pad(int fd, unsigned held)
{
  struct input_event events[32];
  ssize_t got;
  int i, k;

  while ((got = read(fd, events, sizeof events)) > 0) {
    for (i = 0; i < got / (ssize_t)sizeof *events; i++) {
      for (k = 0; k < PAD_COUNT; k++) {
        int down;

        if (!pad_keys[k].set || events[i].type != pad_keys[k].type ||
            events[i].code != pad_keys[k].code) {
          continue;
        }
        if (pad_keys[k].type == EV_ABS) {
          down = events[i].value != 0 && (events[i].value < 0) == (pad_keys[k].sign < 0);
        } else {
          down = events[i].value != 0;
        }
        held = down ? held | (1u << k) : held & ~(1u << k);
      }
    }
  }
  return held;
}

/* ---- the screen, turned on a panel mounted portrait ---- */

static int rotation;

static void land_to_out(int lx, int ly, int lw, int lh, int *ox, int *oy)
{
  if (rotation == 90) {
    *ox = lh - 1 - ly;
    *oy = lx;
  } else if (rotation == 270) {
    *ox = ly;
    *oy = lw - 1 - lx;
  } else {
    *ox = lx;
    *oy = ly;
  }
}

static void out_to_land(const uint32_t *out, int ow, uint32_t *land, int lw, int lh)
{
  int lx, ly, ox, oy;

  for (ly = 0; ly < lh; ly++) {
    for (lx = 0; lx < lw; lx++) {
      land_to_out(lx, ly, lw, lh, &ox, &oy);
      land[ly * lw + lx] = out[oy * ow + ox];
    }
  }
}

static void land_to_out_pixels(const uint32_t *land, int lw, int lh, uint32_t *out, int ow)
{
  int lx, ly, ox, oy;

  for (ly = 0; ly < lh; ly++) {
    for (lx = 0; lx < lw; lx++) {
      land_to_out(lx, ly, lw, lh, &ox, &oy);
      out[oy * ow + ox] = land[ly * lw + lx];
    }
  }
}

/* ---- slots ---- */

static int states_ready(void)
{
  return state_available() && states_dir != NULL && *states_dir != '\0' && cart != NULL &&
         *cart != '\0';
}

/* as RetroArch names them: .state for slot 0, .state1 and up, .state.auto */
static int slot_path(int which, char *path, size_t size)
{
  int n;

  if (which == SLOT_AUTO) {
    n = snprintf(path, size, "%s/%s.state.auto", states_dir, cart);
  } else if (which == 0) {
    n = snprintf(path, size, "%s/%s.state", states_dir, cart);
  } else {
    n = snprintf(path, size, "%s/%s.state%d", states_dir, cart, which);
  }
  return n > 0 && (size_t)n < size;
}

static void make_dirs(const char *path)
{
  char copy[4096];
  char *at;

  snprintf(copy, sizeof copy, "%s", path);
  for (at = copy + 1; *at; at++) {
    if (*at == '/') {
      *at = '\0';
      mkdir(copy, 0777);
      *at = '/';
    }
  }
  mkdir(copy, 0777);
}

static int read_battery(void)
{
  DIR *dir = opendir("/sys/class/power_supply");
  struct dirent *entry;
  int value = -1;

  if (dir == NULL) {
    return -1;
  }
  while (value < 0 && (entry = readdir(dir)) != NULL) {
    char path[512];
    FILE *file;

    if (entry->d_name[0] == '.') {
      continue;
    }
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", entry->d_name);
    if ((file = fopen(path, "r")) != NULL) {
      if (fscanf(file, "%d", &value) != 1) {
        value = -1;
      }
      fclose(file);
    }
  }
  closedir(dir);
  return value < 0 ? -1 : value > 100 ? 100 : value;
}

/* a picture no wider than THUMB_MAX_W, averaged down by a whole factor */
static void write_thumbnail(const char *state, const uint32_t *frame, int w, int h)
{
  char path[4096 + 8];
  int f = (w + THUMB_MAX_W - 1) / THUMB_MAX_W, tw, th, x, y, dx, dy;
  unsigned char *rgb;

  if (f < 1) {
    f = 1;
  }
  tw = w / f;
  th = h / f;
  rgb = malloc((size_t)tw * th * 3);
  if (rgb == NULL) {
    return;
  }
  for (y = 0; y < th; y++) {
    for (x = 0; x < tw; x++) {
      unsigned r = 0, g = 0, b = 0;

      for (dy = 0; dy < f; dy++) {
        for (dx = 0; dx < f; dx++) {
          uint32_t p = frame[(y * f + dy) * w + x * f + dx];

          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
        }
      }
      rgb[(y * tw + x) * 3] = (unsigned char)(r / (f * f));
      rgb[(y * tw + x) * 3 + 1] = (unsigned char)(g / (f * f));
      rgb[(y * tw + x) * 3 + 2] = (unsigned char)(b / (f * f));
    }
  }
  snprintf(path, sizeof path, "%s.png", state);
  if (!stbi_write_png(path, tw, th, 3, rgb, tw * 3)) {
    LOG("cannot write %s\n", path);
  }
  free(rgb);
}

struct state_job {
  struct state_snapshot *snapshot;
  char path[4096];
  uint32_t *frame;
  int w, h;
};

static pthread_t state_thread;
static int state_running;

static void *write_state(void *data)
{
  struct state_job *job = data;

  if (!state_write(job->snapshot, job->path)) {
    LOG("cannot write %s\n", job->path);
  } else {
    write_thumbnail(job->path, job->frame, job->w, job->h);
  }
  state_free(job->snapshot);
  free(job->frame);
  free(job);
  return NULL;
}

/* a state still being written is finished before another is saved or loaded, or pico-8 exits */
static void wait_for_state(void)
{
  if (state_running) {
    pthread_join(state_thread, NULL);
    state_running = 0;
  }
}

/* the audio thread is held off pico-8's memory while a state is taken or put back */
static void hold_audio(int hold)
{
  if (audio_device == 0 || sdl.LockAudioDevice == NULL || sdl.UnlockAudioDevice == NULL) {
    return;
  }
  if (hold) {
    sdl.LockAudioDevice(audio_device);
  } else {
    sdl.UnlockAudioDevice(audio_device);
  }
}

/*
 * pico-8 waits only for its memory to be copied, and the card is written on a
 * thread of its own, unless pico-8 is about to exit.
 */
static int save_slot(int which, const uint32_t *frame, int w, int h, int background)
{
  struct state_snapshot *snapshot;
  struct state_job *job;
  char path[4096];
  int saved;

  wait_for_state();
  if (!states_ready() || !state_cart_started()) {
    return 0;
  }
  if (state_busy()) {
    LOG("pico-8 is downloading, not saving\n");
    return 0;
  }
  if (!slot_path(which, path, sizeof path)) {
    return 0;
  }
  make_dirs(states_dir);

  hold_audio(1);
  snapshot = state_capture(cart, real_ticks() + (uint32_t)tick_offset);
  hold_audio(0);
  if (snapshot == NULL) {
    LOG("cannot copy pico-8's memory, not saving %s\n", path);
    return 0;
  }

  job = background ? calloc(1, sizeof *job) : NULL;
  if (job != NULL && (job->frame = malloc((size_t)w * h * sizeof *frame)) != NULL) {
    job->snapshot = snapshot;
    snprintf(job->path, sizeof job->path, "%s", path);
    memcpy(job->frame, frame, (size_t)w * h * sizeof *frame);
    job->w = w;
    job->h = h;
    if (pthread_create(&state_thread, NULL, write_state, job) == 0) {
      state_running = 1;
    } else {
      write_state(job);
    }
    LOG("saved %s\n", path);
    return 1;
  }
  free(job);

  saved = state_write(snapshot, path);
  state_free(snapshot);
  if (saved) {
    write_thumbnail(path, frame, w, h);
    LOG("saved %s\n", path);
  } else {
    LOG("cannot write %s\n", path);
  }
  return saved;
}

/* puts a slot back, and pico-8's clock carries on from when it was saved */
static int load_slot(int which)
{
  char path[4096];
  uint32_t ticks;
  int loaded;

  wait_for_state();
  if (!states_ready() || !state_cart_started() || state_busy() ||
      !slot_path(which, path, sizeof path) || access(path, F_OK) != 0) {
    return 0;
  }
  hold_audio(1);
  loaded = state_load(path, cart, &ticks);
  if (loaded) {
    tick_offset = (int32_t)(ticks - real_ticks());
  }
  hold_audio(0);
  return loaded;
}

static struct {
  int which;
  uint32_t *pixels;
  int w, h;
} preview = {SLOT_AUTO - 1, NULL, 0, 0};

static void forget_preview(void)
{
  free(preview.pixels);
  preview.pixels = NULL;
  preview.which = SLOT_AUTO - 1;
}

static void load_preview(int which)
{
  char state[4096], path[4096 + 8];
  unsigned char *rgba;
  int w, h, n, i;

  if (preview.which == which) {
    return;
  }
  forget_preview();
  preview.which = which;
  if (!states_ready() || !slot_path(which, state, sizeof state)) {
    return;
  }
  snprintf(path, sizeof path, "%s.png", state);
  if (access(path, F_OK) != 0 || (rgba = stbi_load(path, &w, &h, &n, 4)) == NULL) {
    return;
  }
  preview.pixels = malloc((size_t)w * h * sizeof *preview.pixels);
  if (preview.pixels != NULL) {
    for (i = 0; i < w * h; i++) {
      preview.pixels[i] = 0xFF000000u | (uint32_t)rgba[i * 4] << 16 |
                          (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
    }
    preview.w = w;
    preview.h = h;
  }
  stbi_image_free(rgba);
}

static void preview_from_frame(int which, const uint32_t *frame, int w, int h)
{
  forget_preview();
  preview.which = which;
  preview.pixels = malloc((size_t)w * h * sizeof *frame);
  if (preview.pixels != NULL) {
    memcpy(preview.pixels, frame, (size_t)w * h * sizeof *frame);
    preview.w = w;
    preview.h = h;
  }
}

/* ---- the menu ---- */

/*
 * The finished frame, turned upright, in a buffer of its own, and *out a
 * buffer the size of the screen. A panel mounted portrait is the A30's, whose
 * DISPLAY_ROTATION says which way it turns.
 */
static uint32_t *read_frame(void *renderer, uint32_t **out, int *ow, int *oh, int *lw, int *lh)
{
  uint32_t *frame;
  int i;

  *out = NULL;
  if (sdl.GetRendererOutputSize(renderer, ow, oh) != 0 || *ow <= 0 || *oh <= 0) {
    LOG("cannot read the screen size\n");
    return NULL;
  }
  rotation = 0;
  if (*oh > *ow) {
    const char *r = getenv("DISPLAY_ROTATION");

    rotation = r != NULL && atoi(r) == 90 ? 90 : 270;
  }
  *lw = rotation ? *oh : *ow;
  *lh = rotation ? *ow : *oh;
  *out = malloc((size_t)*ow * *oh * sizeof **out);
  frame = malloc((size_t)*lw * *lh * sizeof *frame);
  if (*out == NULL || frame == NULL) {
    free(*out);
    free(frame);
    *out = NULL;
    return NULL;
  }
  if (sdl.RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, *out,
                           *ow * (int)sizeof **out) != 0) {
    LOG("cannot read the game's frame: %s\n", sdl.GetError ? sdl.GetError() : "");
    memset(*out, 0, (size_t)*ow * *oh * sizeof **out);
  }
  /* the back buffer's alpha means nothing */
  for (i = 0; i < *ow * *oh; i++) {
    (*out)[i] |= 0xFF000000u;
  }
  out_to_land(*out, *ow, frame, *lw, *lh);
  return frame;
}

static int pause_audio(void)
{
  if (audio_device == 0 || sdl.PauseAudioDevice == NULL ||
      (sdl.GetAudioDeviceStatus != NULL &&
       sdl.GetAudioDeviceStatus(audio_device) != SDL_AUDIO_PLAYING)) {
    return 0;
  }
  sdl.PauseAudioDevice(audio_device, 1);
  return 1;
}

static void exit_game(const uint32_t *frame, int w, int h)
{
  wait_for_state();
  if (save_slot(SLOT_AUTO, frame, w, h, 0)) {
    LOG("saved the auto slot for the next launch\n");
  }
  request_quit();
}

#define PRESSED(now, was, k) (((now) & (1u << (k))) && !((was) & (1u << (k))))

static void run_menu(void *renderer)
{
  struct letterbox saved;
  igm_canvas canvas;
  igm_image image;
  uint32_t *out = NULL, *frame = NULL, *drawn = NULL;
  uint32_t event[14], opened_at;
  void *texture;
  int ow, oh, lw, lh, fd, paused, battery, selected = IGM_RESUME, action = -1, deferred = -1;
  int dirty = 1;
  unsigned held = 0, was = 0;
  sig_atomic_t requests = menu_requests;

  clear_letterbox(renderer, &saved);
  frame = read_frame(renderer, &out, &ow, &oh, &lw, &lh);
  drawn = frame != NULL ? malloc((size_t)lw * lh * sizeof *drawn) : NULL;
  texture = drawn != NULL ? sdl.CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, ow, oh)
                          : NULL;
  if (texture == NULL) {
    LOG("cannot make the menu's buffers\n");
    restore_letterbox(renderer, &saved);
    free(out);
    free(frame);
    free(drawn);
    return;
  }

  opened_at = real_ticks();
  paused = pause_audio();
  battery = read_battery();
  fd = input_path != NULL ? open(input_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1;
  if (fd < 0) {
    LOG("cannot open %s, the menu can only be closed with Menu\n", input_path ? input_path : "");
  }
  canvas.pixels = drawn;
  canvas.w = lw;
  canvas.h = lh;
  LOG("menu opened\n");

  while (action < 0) {
    while (sdl.PollEvent(event)) {
      if (event[0] == SDL_QUIT) {
        action = IGM_EXIT;
      }
    }
    if (terminating) {
      action = IGM_EXIT;
    }
    if (menu_requests != requests) {
      action = IGM_RESUME;
    }
    if (action >= 0) {
      break;
    }

    held = fd >= 0 ? read_pad(fd, held) : 0;

    /* an action waits for A and B to be let go, so the press never reaches the cart */
    if (deferred >= 0) {
      if (!(held & ((1u << PAD_A) | (1u << PAD_B)))) {
        action = deferred;
      }
      was = held;
      sdl.Delay(16);
      continue;
    }

    if (PRESSED(held, was, PAD_UP)) {
      selected = (selected + IGM_ITEM_COUNT - 1) % IGM_ITEM_COUNT;
      dirty = 1;
    }
    if (PRESSED(held, was, PAD_DOWN)) {
      selected = (selected + 1) % IGM_ITEM_COUNT;
      dirty = 1;
    }
    if (selected == IGM_SAVE || selected == IGM_LOAD) {
      if (PRESSED(held, was, PAD_LEFT) && slot > SLOT_AUTO) {
        slot--;
        dirty = 1;
      }
      if (PRESSED(held, was, PAD_RIGHT) && slot < SLOT_MAX) {
        slot++;
        dirty = 1;
      }
    }
    if (PRESSED(held, was, PAD_B)) {
      deferred = IGM_RESUME;
    } else if (PRESSED(held, was, PAD_A)) {
      if (selected == IGM_SAVE) {
        /* Save stays in the menu, as RetroArch's does */
        if (save_slot(slot, frame, lw, lh, 1)) {
          preview_from_frame(slot, frame, lw, lh);
        }
        dirty = 1;
      } else {
        deferred = selected;
      }
    }
    was = held;

    if (dirty) {
      load_preview(slot);
      image.pixels = preview.pixels;
      image.w = preview.w;
      image.h = preview.h;
      igm_draw(&canvas, frame, selected, slot, battery, &image);
      if (rotation) {
        land_to_out_pixels(drawn, lw, lh, out, ow);
      }
      sdl.UpdateTexture(texture, NULL, rotation ? out : drawn, ow * (int)sizeof *out);
      sdl.RenderClear(renderer);
      sdl.RenderCopy(renderer, texture, NULL, NULL);
      sdl.RenderPresent(renderer);
      dirty = 0;
    }
    sdl.Delay(16);
  }

  /* what the game showed goes back, for the frame pico-8 is presenting */
  if (rotation) {
    land_to_out_pixels(frame, lw, lh, out, ow);
  }
  sdl.UpdateTexture(texture, NULL, rotation ? out : frame, ow * (int)sizeof *out);
  sdl.RenderClear(renderer);
  sdl.RenderCopy(renderer, texture, NULL, NULL);
  sdl.DestroyTexture(texture);
  restore_letterbox(renderer, &saved);
  if (fd >= 0) {
    close(fd);
  }
  while (sdl.PollEvent(event)) {
    if (event[0] == SDL_QUIT) {
      action = IGM_EXIT;
    }
  }
  /* pico-8's clock does not see the time the menu was open */
  tick_offset -= (int32_t)(real_ticks() - opened_at);
  menu_seen = menu_requests;
  LOG("menu closed\n");

  if (action == IGM_EXIT) {
    exit_game(frame, lw, lh);
  }
  /* the menu's memory goes first: a load stages a copy of pico-8's, and the Mini has little to spare */
  forget_preview();
  igm_release();
  free(out);
  free(frame);
  free(drawn);

  switch (action) {
  case IGM_LOAD:
    if (load_slot(slot)) {
      LOG("loaded slot %d\n", slot);
    }
    break;
  case IGM_RESET:
    reset_cart();
    break;
  case IGM_PICO8:
    open_pico8_menu();
    break;
  }
  if (paused && !quitting) {
    sdl.PauseAudioDevice(audio_device, 0);
  }
}

/* ---- startup and shutdown ---- */

static void on_menu(int signal)
{
  (void)signal;
  menu_requests++;
}

/* SDL would turn SIGTERM into a quit before the auto slot is saved, so it is taken here */
static void on_terminate(int signal)
{
  (void)signal;
  terminating = 1;
  alarm(TERMINATE_WAIT_S);
}

__attribute__((constructor)) static void shim_init(void)
{
  struct sigaction action;
  char exe[1024];
  const char *name;
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);

  if (n <= 0) {
    return;
  }
  exe[n] = '\0';
  name = strrchr(exe, '/');
  name = name != NULL ? name + 1 : exe;
  if (strncmp(name, "pico8", 5) != 0) {
    return;
  }
  is_pico8 = 1;

  memset(&action, 0, sizeof action);
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  action.sa_handler = on_menu;
  sigaction(SIGUSR2, &action, NULL);
  action.sa_handler = on_terminate;
  sigaction(SIGTERM, &action, NULL);

  if (getenv("PICO8_IGM_HIDE_SCANCODE") != NULL) {
    hidden_scancode = atoi(getenv("PICO8_IGM_HIDE_SCANCODE"));
  }
  if (getenv("PICO8_IGM_DEBUG") != NULL) {
    debug_log = fopen("/mnt/SDCARD/Saves/spruce/pico8-igm-debug.log", "a");
    debug("started, hiding scancode %d\n", hidden_scancode);
  }
  states_dir = getenv("PICO_PAK_STATES_DIR");
  cart = getenv("PICO8_IGM_CART");
  if (cart != NULL && strrchr(cart, '/') != NULL) {
    cart = strrchr(cart, '/') + 1;
  }
  load_pad();
}

/* the auto slot goes back once the cart has started, as pico-8 sets the cart up after its first frame */
static int resume(void)
{
  static int waited, started;
  char path[4096];

  if (resume_pending < 0) {
    if (resume_pending == -1) {
      resume_pending = -2;
      if (states_ready() && slot_path(SLOT_AUTO, path, sizeof path) && access(path, F_OK) == 0) {
        snprintf(resume_marker, sizeof resume_marker, "%s.loading", path);
        if (access(resume_marker, F_OK) == 0) {
          LOG("the last launch did not survive the auto slot, starting afresh\n");
          unlink(resume_marker);
        } else {
          resume_pending = 1;
        }
      }
    }
    if (resume_frames >= 0 && ++resume_frames > RESUME_SAFE_FRAMES) {
      unlink(resume_marker);
      resume_frames = -1;
    }
    if (resume_pending < 0) {
      return 0;
    }
  }
  if ((!state_cart_started() || state_busy()) && ++waited < RESUME_WAIT_FRAMES) {
    return 1;
  }
  if (state_cart_started() && !state_busy() && started++ == 0) {
    return 1;
  }
  resume_pending = -2;
  if (!state_cart_started() || state_busy()) {
    LOG("the cart did not start in time, not loading the auto slot\n");
    return 1;
  }
  close(open(resume_marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0666));
  sync();
  if (load_slot(SLOT_AUTO)) {
    LOG("loaded the auto slot\n");
    resume_frames = 0;
  } else {
    unlink(resume_marker);
  }
  return 1;
}

/* ---- the wrappers ---- */

EXPORT void SDL_RenderPresent(void *renderer)
{
  if (!resolve_sdl()) {
    if (sdl.RenderPresent != NULL) {
      sdl.RenderPresent(renderer);
    }
    return;
  }
  if (!is_pico8) {
    sdl.RenderPresent(renderer);
    return;
  }
  if (quitting) {
    clear_to_black(renderer);
    sdl.RenderPresent(renderer);
    return;
  }

  state_frame();
  if (resume()) {
    clear_to_black(renderer);
    sdl.RenderPresent(renderer);
    return;
  }

  if (terminating) {
    struct letterbox saved;
    uint32_t *out, *frame;
    int ow, oh, lw, lh;

    clear_letterbox(renderer, &saved);
    frame = read_frame(renderer, &out, &ow, &oh, &lw, &lh);
    restore_letterbox(renderer, &saved);
    if (frame != NULL) {
      exit_game(frame, lw, lh);
    } else {
      request_quit();
    }
    free(out);
    free(frame);
    clear_to_black(renderer);
    sdl.RenderPresent(renderer);
    return;
  }

  continue_keys();
  release_hidden_key();
  debug_keyboard_state();
  if (menu_requests != menu_seen) {
    debug("menu requested\n");
    menu_seen = menu_requests;
    run_menu(renderer);
    if (quitting) {
      clear_to_black(renderer);
    }
  }
  sdl.RenderPresent(renderer);
}

/*
 * pico-8 also polls the table SDL_GetKeyboardState hands it, so the key is
 * let go there each time SDL reads the keyboard. SDL then drops its release.
 */
static void release_hidden_key(void)
{
  uint8_t *state;
  int count = 0;

  if (hidden_scancode < 0 || sdl.GetKeyboardState == NULL) {
    return;
  }
  state = (uint8_t *)sdl.GetKeyboardState(&count);
  if (state != NULL && hidden_scancode < count) {
    state[hidden_scancode] = 0;
  }
}

static int is_hidden_key(const void *event)
{
  uint32_t type = *(const uint32_t *)event;
  int32_t scancode;

  if (type != SDL_KEYDOWN && type != SDL_KEYUP) {
    return 0;
  }
  memcpy(&scancode, (const unsigned char *)event + 16, sizeof scancode);
  return scancode == hidden_scancode;
}

EXPORT int SDL_PollEvent(void *event)
{
  int got;

  resolve_sdl();
  if (sdl.PollEvent == NULL) {
    return 0;
  }
  /* a NULL event only asks whether one is waiting, so nothing can be dropped */
  if (!is_pico8 || event == NULL || (hidden_scancode < 0 && debug_log == NULL)) {
    return sdl.PollEvent(event);
  }
  for (;;) {
    got = sdl.PollEvent(event);
    release_hidden_key();
    if (!got) {
      return 0;
    }
    debug_key_event("PollEvent", event, is_hidden_key(event));
    if (!is_hidden_key(event)) {
      return got;
    }
  }
}

EXPORT int SDL_PeepEvents(void *events, int count, int action, uint32_t min_type, uint32_t max_type)
{
  static int seen;

  resolve_sdl();
  if (!seen++) {
    debug("pico-8 calls SDL_PeepEvents (action %d)\n", action);
  }
  return sdl.PeepEvents != NULL ? sdl.PeepEvents(events, count, action, min_type, max_type) : -1;
}

EXPORT int SDL_WaitEvent(void *event)
{
  int got;

  resolve_sdl();
  got = sdl.WaitEvent != NULL ? sdl.WaitEvent(event) : 0;
  release_hidden_key();
  if (got && event != NULL) {
    debug_key_event("WaitEvent", event, 0);
  }
  return got;
}

EXPORT int SDL_WaitEventTimeout(void *event, int timeout)
{
  int got;

  resolve_sdl();
  got = sdl.WaitEventTimeout != NULL ? sdl.WaitEventTimeout(event, timeout) : 0;
  release_hidden_key();
  if (got && event != NULL) {
    debug_key_event("WaitEventTimeout", event, 0);
  }
  return got;
}

EXPORT void SDL_PumpEvents(void)
{
  resolve_sdl();
  if (sdl.PumpEvents != NULL) {
    sdl.PumpEvents();
  }
  release_hidden_key();
}

EXPORT const uint8_t *SDL_GetKeyboardState(int *count)
{
  static int seen;

  resolve_sdl();
  if (!seen++) {
    debug("pico-8 calls SDL_GetKeyboardState\n");
  }
  return sdl.GetKeyboardState != NULL ? sdl.GetKeyboardState(count) : NULL;
}

/* pico-8's clock, without the time the menu was open, and carried on from a loaded state */
EXPORT uint32_t SDL_GetTicks(void)
{
  resolve_sdl();
  return sdl.GetTicks != NULL ? sdl.GetTicks() + (uint32_t)tick_offset : 0;
}

/* pico-8 reads Ctrl from here, for the Ctrl+R that resets the cart */
EXPORT int SDL_GetModState(void)
{
  resolve_sdl();
  return (sdl.GetModState != NULL ? sdl.GetModState() : 0) | (ctrl_held ? KMOD_LCTRL : 0);
}

EXPORT uint32_t SDL_OpenAudioDevice(const char *device, int iscapture, const void *desired,
                                    void *obtained, int allowed_changes)
{
  uint32_t opened;

  resolve_sdl();
  if (sdl.OpenAudioDevice == NULL) {
    return 0;
  }
  opened = sdl.OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
  if (opened != 0 && !iscapture) {
    audio_device = opened;
  }
  return opened;
}

/* the legacy call always opens device 1 */
EXPORT int SDL_OpenAudio(void *desired, void *obtained)
{
  int status;

  resolve_sdl();
  if (sdl.OpenAudio == NULL) {
    return -1;
  }
  status = sdl.OpenAudio(desired, obtained);
  if (status == 0) {
    audio_device = 1;
  }
  return status;
}
