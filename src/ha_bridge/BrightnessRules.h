#pragma once

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * The rules of the B brightness setting (firmware 2.4.105), apart from the screens that use
       * them so the host tests (firmware/tests/test_brightness_rules.cpp) run the code the watch runs.
       *
       * `Levels` is BrightnessController::Levels on the watch: Off, AlwaysOn, Low, Medium, High and
       * Button, in that order (the stored value, and the code the HELLO's settings word carries).
       * Button is a setting only. The backlight is only ever given the result of ResolveBrightness.
       */

      // The level the backlight takes for the stored `setting`. B is High after a side-button wake and
      // Low after any other; Low, Medium and High are themselves; any other stored value (Off and
      // AlwaysOn, or one a later firmware wrote) reads as High, as every firmware before 2.4.105 read it.
      template <typename Levels>
      Levels ResolveBrightness(Levels setting, bool buttonWake) {
        switch (setting) {
          case Levels::Button:
            return buttonWake ? Levels::High : Levels::Low;
          case Levels::Low:
          case Levels::Medium:
          case Levels::High:
            return setting;
          default:
            return Levels::High;
        }
      }

      // What Quick Settings' brightness button stores next: Low, Medium, High, B, Low. A stored value
      // this firmware does not offer reads as High, so it continues to B.
      template <typename Levels>
      Levels NextBrightnessSetting(Levels setting) {
        switch (setting) {
          case Levels::Low:
            return Levels::Medium;
          case Levels::Medium:
            return Levels::High;
          case Levels::Button:
            return Levels::Low;
          default:
            return Levels::Button;
        }
      }

    }
  }
}
