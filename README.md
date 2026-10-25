# pico8-spruce

A library preloaded into native PICO-8 on spruceOS that adds the spruce in-game menu (the same look and `igm.json` as RetroArch's) and save states.

- Menu: tap Menu. spruce's menu button watchdog sends `SIGUSR2`, and the menu opens over the next frame.
- Save states: `src/state.c` from [josegonzalez/minui-pico-8-pak](https://github.com/josegonzalez/minui-pico-8-pak) (MIT, see `src/LICENSE.state`), unchanged. States go to `PICO_PAK_STATES_DIR` and are named the way RetroArch names its own. The auto slot is saved on Exit Game and on SIGTERM, and loaded the next time the cart starts.

## Environment

| variable | |
|---|---|
| `PICO_PAK_STATES_DIR` | where states go; unset turns states off (Splore) |
| `PICO8_IGM_CART` | the cart being run, for state names |
| `EVENT_PATH_READ_INPUTS_SPRUCE`, `B_UP` ... `B_B` | the pad, as the platform `.cfg` exports them |
| `PICO8_IGM_INPUT`, `PICO8_IGM_B_*` | override the two above |
| `SPRUCE_THEME_DIR` | a theme's own `igm.json` |
| `DISPLAY_ROTATION` | which way a portrait panel turns |

## Building

Builds run in CI only: `build.yml` builds `pico8-igm-arm64.so` and `pico8-igm-armhf.so` and publishes them to the `beta` release.
