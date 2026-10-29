# InfinAssistant firmware

Firmware for the PineTime smartwatch that pairs with the InfinAssistant Bridge Android app and the InfinAssistant Home Assistant integration. It shows watch faces and sends sensor readings to Home Assistant, and it shows notifications and runs commands sent from Home Assistant.

## Relation to InfiniTime

This firmware is based on [InfiniTime](https://github.com/InfiniTimeOrg/InfiniTime), the open-source firmware for the PineTime. It is based on InfiniTimeOrg/InfiniTime commit `7dea64af99ce1bc7110c49d91775bffbf1ccecf3`. This repository is not a GitHub fork and does not carry InfiniTime's history: it starts from one root commit, followed by one commit per release. InfiniTime does the watch's core work. This repository adds:

- an `ha_bridge` component with the BLE service for the bridge and the screens that use it;
- small changes to upstream files for build wiring, start-up and notifications;
- font and glyph-cache fixes for LVGL, carried in our own LVGL repository, which is the `src/libs/lvgl` submodule.

The LVGL repository, [ryan-werts/lvgl](https://github.com/ryan-werts/lvgl), is based on InfiniTimeOrg/lvgl commit `7c96fb87e33733a7a67b33bf8057e598c6843e3a`, the commit InfiniTime `7dea64af` pins. It is one root commit with our three patches applied. It is not a fork either.

To see what changed from InfiniTime, add it as a remote, fetch it and diff against the base commit:

    git remote add upstream https://github.com/InfiniTimeOrg/InfiniTime.git
    git fetch upstream
    git diff 7dea64af99ce1bc7110c49d91775bffbf1ccecf3 HEAD

Git compares the two trees, so this works without a shared history. The LVGL repository works the same way against `https://github.com/InfiniTimeOrg/lvgl.git` and `7c96fb87e33733a7a67b33bf8057e598c6843e3a`.

[NOTICE-MODIFICATIONS.md](NOTICE-MODIFICATIONS.md) lists every change from upstream, by release. The license is unchanged: GPL-3.0, see [LICENSE](LICENSE). Thanks to the InfiniTime developers for the base firmware.

## Build

The firmware is on the `infinassistant` branch, so clone that branch. To build a release, clone its tag instead, which gives that exact release. LVGL is a git submodule, so clone with submodules:

    git clone --recursive -b infinassistant https://github.com/ryan-werts/infinassistant-firmware.git
    cd infinassistant-firmware

For release 2.4.105 exactly, use `git clone --recursive -b 2.4.105 https://github.com/ryan-werts/infinassistant-firmware.git`.
    npm install lv_font_conv@1.5.3

You need the GNU Arm Embedded Toolchain 10.3-2021.10, the nRF5 SDK 15.3.0 (59ac345), CMake and make. Put the toolchain and `node_modules/.bin` on `PATH`, then build:

    mkdir build && cd build
    cmake -G "Unix Makefiles" \
          -DCMAKE_TOOLCHAIN_FILE=../cmake-nRF5x/arm-gcc-toolchain-fixed.cmake \
          -DCMAKE_BUILD_TYPE=Release \
          -DARM_NONE_EABI_TOOLCHAIN_PATH=/path/to/gcc-arm-none-eabi-10.3-2021.10 \
          -DNRF5_SDK_PATH=/path/to/nRF5_SDK_15.3.0_59ac345 \
          -DBUILD_DFU=1 -DBUILD_RESOURCES=0 -DTARGET_DEVICE=PINETIME ..
    make pinetime-mcuboot-app -j$(nproc)

The version comes from the `project(pinetime VERSION ...)` line in `CMakeLists.txt`, and the firmware reports it. Each release tag has a DFU zip attached, with its sha256 in the release notes. Build the zip with `adafruit-nrfutil` as the release workflow does.

## Flashing

Install a release by using the firmware update in the InfinAssistant Bridge app, which carries the matching zip. This README does not cover flashing with other DFU tools.

## Documentation

- [BLE protocol spec](infinassistant/docs/ble-protocol-spec.md): the messages between the watch and the app. The watch's decoder is the reference.
- [Architecture](infinassistant/docs/architecture.md): how the firmware is organised.
- [Golden test vectors](infinassistant/vectors/): the byte-level test data the app checks against.
- [Patch series](infinassistant/patches/): the upstream modifications as patches, in apply order.

## Compatibility

<!-- compat:begin -->

| Component | Version | Release |
|-----------|---------|---------|
| Firmware (InfiniTime fork) | 2.4.105 | [2.4.105](https://github.com/ryan-werts/infinassistant-firmware/releases/tag/2.4.105) |
| Android app (InfinAssistant Bridge) | 2.1.127 | [2.1.127](https://github.com/ryan-werts/infinassistant-bridge/releases/tag/2.1.127) |
| Home Assistant integration | 0.3.5 | [0.3.5](https://github.com/ryan-werts/infinassistant-ha/releases/tag/0.3.5) |

**Breaking pairs.** These pairs must be upgraded together:

- App 2.1.124 or later needs firmware 2.4.103 or later. The watch's BLE config key was renamed from `infinisstant` to `infinassistant`, and older firmware is not supported. The app detects older firmware and offers the update.
- App 2.1.124 or later needs integration 0.3.3 or later. The Home Assistant domain was renamed from `infinisstant` to `infinassistant`.

Other version mismatches degrade gracefully: the HELLO feature bits tell each side what the other supports. If the watch does not report feature bit 16, the app shows "Update the watch" and offers the firmware bundled with it.

<!-- compat:end -->

## Contributing

Pull requests are applied to the source repository first, and they reach this repository with the next export. Open an issue to discuss larger changes before you start.

## License

GPL-3.0. See [LICENSE](LICENSE).
