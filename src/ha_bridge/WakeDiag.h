#pragma once

#include <cbor.h>
#include <cstddef>
#include <cstdint>
#include "ha_bridge/WakeTelemetry.h"

// The HELLO's optional "wk" key (firmware 2.4.104). The WakeDiag struct, the size bound and the frame
// room are in WakeTelemetry.h (no CBOR there, so HomeAssistantService.h can hold a WakeDiag).
namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      inline uint32_t WakeCap16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : v; }

      // Appends "wk" and its array to an open map (one more entry than the map was created with).
      inline void EncodeWakeDiag(CborEncoder* map, const WakeDiag& d) {
        CborEncoder arr;
        cbor_encode_text_stringz(map, "wk");
        cbor_encoder_create_array(map, &arr, 5);
        cbor_encode_uint(&arr, WakeCap16(d.counters.wakes));
        cbor_encode_uint(&arr, WakeCap16(d.counters.notifyWakes));
        cbor_encode_uint(&arr, WakeCap16(d.counters.notifyStored));
        cbor_encode_uint(&arr, WakeCap16(d.counters.screenOnSec));
        cbor_encode_uint(&arr, WakeCap16(d.settings));
        cbor_encoder_close_container(map, &arr);
      }

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
