# pico8-spruce

A library preloaded into native PICO-8 on spruceOS that adds the spruce in-game menu (the same look and `igm.json` as RetroArch's) and save states.

- Menu: tap Menu. spruce's menu button watchdog sends `SIGUSR2`, and the menu opens over the next frame.
- Save states: `src/state.c` from [josegonzalez/minui-pico-8-pak](https://github.com/josegonzalez/minui-pico-8-pak) (MIT, see `src/LICENSE.state`), changed only to unpack a state a chunk at a time on load, which the Miyoo Mini needs. States go to `PICO_PAK_STATES_DIR` and are named the way RetroArch names its own. The auto slot is saved on Exit Game and on SIGTERM, and loaded the next time the cart starts.

## Environment

| variable | |
|---|---|
| `PICO_PAK_STATES_DIR` | where states go; unset turns states off (Splore) |
| `PICO8_IGM_CART` | the cart being run, for state names |
| `PICO8_IGM_STATE_KEY` | the screen setup a state is checked against before it loads |
| `EVENT_PATH_READ_INPUTS_SPRUCE`, `B_UP` ... `B_B` | the pad, as the platform `.cfg` exports them |
| `PICO8_IGM_INPUT`, `PICO8_IGM_B_*` | override the two above |
| `PICO8_IGM_HIDE_SCANCODE` | an SDL scancode kept from pico-8, for a Menu button its SDL sends as a key |
| `SPRUCE_THEME_DIR` | a theme's own `igm.json` |
| `DISPLAY_ROTATION` | which way a portrait panel turns |

## Building

Builds run in CI only: `build.yml` builds `pico8-igm-arm64.so` and `pico8-igm-armhf.so` and publishes them to the `beta` release.
