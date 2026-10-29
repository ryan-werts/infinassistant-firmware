#pragma once

#include <cstddef>
#include <cstdint>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * What a NOTIFY (0x08) carries that the watch uses. The buffers are the watch's own: the title
       * and the body are cut to what InfiniTime's notification holds, and a string that does not fit
       * is left empty (DecodeStringInto), as the inline decoder in HandleNotify did until 2.4.101.
       */
      struct NotifyFields {
        static constexpr std::size_t kTitleSize = 48;
        static constexpr std::size_t kBodySize = 96;
        static constexpr std::size_t kTagSize = 32;

        char title[kTitleSize] {};
        char body[kBodySize] {};
        char tag[kTagSize] {};
        // The phone's id for a phone notification (2.4.101), 1 to 65535; 0 for none: an HA
        // notification, an older phone, or a value outside the range.
        uint16_t nid {0};
      };

      /**
       * Decode a NOTIFY payload inline, with no heap use. Returns false when the payload is not a
       * CBOR map or an entry cannot be skipped: nothing is shown then. Unknown keys are skipped.
       */
      bool DecodeNotify(const uint8_t* payload, std::size_t len, NotifyFields& out);

      /**
       * Decode a NOTIFY_REMOVE (0x21) payload, {"nid": n}. Returns the nid (1 to 65535), or 0 when
       * the payload has none or it is out of range: there is nothing to remove then.
       */
      uint16_t DecodeNotifyRemove(const uint8_t* payload, std::size_t len);

      /**
       * Encode a NOTIFY_DISMISS (0x1E) payload, {"nid": n}, into `out`. Returns the length, or 0
       * when it does not fit (16 bytes are always enough).
       */
      std::size_t EncodeNotifyDismiss(uint16_t nid, uint8_t* out, std::size_t outSize);

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
