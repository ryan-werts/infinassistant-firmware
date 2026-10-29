# Notice of modifications

This firmware is a modified version of InfiniTime, licensed under GPL-3.0 (see [LICENSE](LICENSE)). As GPL-3.0 section 5(a) requires, this file lists what changed.

**Base:** InfiniTimeOrg/InfiniTime, commit `7dea64af99ce1bc7110c49d91775bffbf1ccecf3` ("Remove unneeded casts").

**LVGL base:** InfiniTimeOrg/lvgl, commit `7c96fb87e33733a7a67b33bf8057e598c6843e3a`, the commit InfiniTime `7dea64af` pins. Our LVGL repository, `ryan-werts/lvgl`, is one root commit whose tree is that commit plus our patches.

This repository and the LVGL repository are not GitHub forks and carry none of the upstream history. To diff against upstream, add `https://github.com/InfiniTimeOrg/InfiniTime.git` (or `.../lvgl.git`) as a remote, fetch it, and run `git diff <base commit> HEAD`. Git compares the trees, so no shared history is needed.

Each release section lists the changes in that release's tag, measured from the base. Dates are the tag dates. A new release adds a section above the previous one, with a date and every change made since the last release.

## 2.4.105 (2026-10-28)

Changes from the base:

- **Build and start-up wiring.** The `ha_bridge` component is added to the build, the app is registered, and `SystemTask` and `DisplayApp` start it. FreeRTOS and LVGL configuration changes (13 files).
- **Home Assistant bridge.** The BLE service, the bridge screens, the phone notification sync (`ha_bridge/PhoneNotifySync.cpp`) and the boot box (`ha_bridge/BootBox.cpp`) are added under `src/`. `NimbleController.{h,cpp}` and `Settings.h` are replaced as whole files. The HELLO carries a `wk` screen-time report: wakes, notification wakes, quietly stored notifications, screen-on seconds, and the display settings. The sensor report carries the battery voltage in millivolts (`bmv`) and whether a power source is connected (`pp`).
- **Brightness B.** A fourth brightness level, B, follows High in the Quick Settings brightness button (Low, Medium, High, B). On B, a side-button wake lights the screen at High and every other wake at Low (`BrightnessController.h`, `DisplayApp.{h,cpp}`, `QuickSettings.{h,cpp}`, `SystemTask.{h,cpp}`). Find Watch and a charging event end the notification early sleep described below and hold it off.
- **Voice trigger.** A double click on the display asks the bridge to take it as the voice trigger first (`DisplayApp.cpp`).
- **Notifications.** `NotificationManager` gets a critical section around its list, `Push` returns the id it assigned, and a dismiss hook is added for phone sync. Swiping the pop-up away dismisses it as the list does, and the next notification, if any, is shown (`Notifications.cpp`). With the screen off, a notification less than 60 s after the last notification wake is only stored in the list, with no wake and no buzz. A wake caused only by a notification ends about 5 s after the 7 s pop-up closes, unless the watch is touched.
- **Dashboard power display.** While the watch has power, a green plug replaces the battery icon on the dashboard face and leads the default face's status line.
- **Fonts and weather symbols.** Built-in Montserrat fonts are declared `const` so they sit in flash. Montserrat 14 and 18 are regenerated at 1 bpp with ASCII only, and Montserrat 14 and 18 gain U+00B0. The night weather glyphs are dropped from the 25 px font, and six glyphs are added to `jetbrains_mono_bold_20`. The weather symbols have no night variant. Several font users are changed to `const lv_font_t`.
- **LVGL fixes.** Font and label hooks are added to LVGL, and a fix stops the glyph cache from writing into a `const` font descriptor. These changes are carried in our LVGL repository, the `src/libs/lvgl` submodule, not in this repository's files.
- **Build tooling.** `cmake-nRF5x/arm-gcc-toolchain-fixed.cmake` is added, a toolchain file that does not hard-code a local path. `.gitmodules` points `src/libs/lvgl` at our LVGL repository, and adds an entry for `external/tinycbor`, whose content is unchanged.

The patch series in `infinassistant/patches/` holds the exact line changes to upstream files.
