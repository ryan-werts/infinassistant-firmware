#pragma once

#include <cbor.h>

#include <cstddef>
#include <cstdint>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * The watch reports whether the link is encrypted (2.4.101): "enc" is a CBOR boolean in the
       * HELLO and in every PONG. The phone sends notification content only over an encrypted link.
       * Header-only, so the host tests pin the bytes the watch sends (CBOR false is F4, true F5).
       */
      inline CborError EncodeEncPair(CborEncoder* map, bool encrypted) {
        const CborError err = cbor_encode_text_stringz(map, "enc");
        if (err != CborNoError) return err;
        return cbor_encode_boolean(map, encrypted);
      }

      /**
       * The PONG payload, {"rid": <the PING's msgId>, "enc": <bool>}: 13 bytes at most. Returns
       * the length, or 0 when it does not fit.
       */
      inline std::size_t EncodePongPayload(uint16_t rid, bool encrypted, uint8_t* out, std::size_t outSize) {
        CborEncoder encoder, mapEncoder;
        cbor_encoder_init(&encoder, out, outSize, 0);
        if (cbor_encoder_create_map(&encoder, &mapEncoder, 2) != CborNoError) return 0;
        if (cbor_encode_text_stringz(&mapEncoder, "rid") != CborNoError) return 0;
        if (cbor_encode_uint(&mapEncoder, rid) != CborNoError) return 0;
        if (EncodeEncPair(&mapEncoder, encrypted) != CborNoError) return 0;
        if (cbor_encoder_close_container(&encoder, &mapEncoder) != CborNoError) return 0;
        return cbor_encoder_get_buffer_size(&encoder, out);
      }

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
