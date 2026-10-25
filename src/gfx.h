/* state.c is vendored unchanged and only takes its logging from here. */

#ifndef GFX_H
#define GFX_H

void gfx_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
