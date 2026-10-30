# runtime/ — the port's own code (written by hand)

The game itself is generated C in `build/gen*` (never edited); the generic PSP runtime and
firmware are in `third_party/psprecomp`. This directory is everything in between. Full map of
the layers: `docs/26-capas.md`; boundaries checked by `tools/check_layers.sh`.

| Where | What |
|---|---|
| `common/pes6_addrs.h` `pes6_types.h` `pes6_game.h` | **the game, by name**: addresses (the only place they are spelled), record layouts, accessors |
| `common/pes6_tuning.h` `tuning.c` | gameplay parameters (`docs/25` §2) |
| `common/pes6_hooks.h` `rules.c` | game functions replaced by C: match rules (`docs/25` §3) |
| `common/commentary.c` | the match commentary |
| `common/ge.c` `ge_gl.c` `mpeg.c` `h264.c` `atrac.c` `at3.c` `audio.c` `net.c` `osk.c` | firmware that needs host libraries: GE, video, music, audio out, ad hoc network, keyboard |
| `common/launcher.c` `settings.c` `version.c` | the app: pre-game menu, settings, versions and mods |
| `desktop/` | macOS/Linux host: SDL2 window, input (sceCtrl), overlay registry, debug tools |
| `android/` | Android host: NativeActivity, GLES, AAudio, touch, Wi-Fi Direct |
