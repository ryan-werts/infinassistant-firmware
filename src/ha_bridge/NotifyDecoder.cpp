#include "ha_bridge/NotifyDecoder.h"

#include <cbor.h>

#include "ha_bridge/LayoutDecoder.h"  // SkipUnknownEntry, DecodeStringInto

// NOTIFY and NOTIFY_REMOVE decode. Inline and allocation-free: both run on the NimBLE host task,
// where a heap allocation per message fragmented the heap until a screen change failed (see
// BridgeController::HandleNotify). The decode used to be inline in HandleNotify; it is here so the
// host tests and the fuzz targets run the code the watch runs. The NOTIFY_DISMISS encode is here
// for the same reason: the host test pins its bytes.

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      namespace {
        bool KeyIs(CborValue* it, const char* key) {
          bool match = false;
          cbor_value_text_string_equals(it, key, &match);
          return match;
        }

        // The value at `it` as a nid, and the iterator past it. A value that is not an unsigned
        // integer in 1 to 65535 reads as 0, which is none; the value is skipped either way. False
        // only when the iterator cannot be advanced.
        bool ReadNid(CborValue* it, uint16_t& nid) {
          nid = 0;
          uint64_t v = 0;
          if (cbor_value_is_unsigned_integer(it) && cbor_value_get_uint64(it, &v) == CborNoError && v >= 1 && v <= 0xFFFF) {
            nid = static_cast<uint16_t>(v);
          }
          return cbor_value_advance(it) == CborNoError;
        }
      }  // namespace

      bool DecodeNotify(const uint8_t* payload, std::size_t len, NotifyFields& out) {
        CborParser parser;
        CborValue root, mapIter;

        if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return false;
        if (!cbor_value_is_map(&root)) return false;
        if (cbor_value_enter_container(&root, &mapIter) != CborNoError) return false;

        while (!cbor_value_at_end(&mapIter)) {
          if (!cbor_value_is_text_string(&mapIter)) {
            if (!SkipUnknownEntry(&mapIter)) return false;
            continue;
          }

          // "severity" and "ts" are skipped with the other unknown keys: no screen shows them.
          const bool isTitle = KeyIs(&mapIter, "title");
          const bool isBody = KeyIs(&mapIter, "body");
          const bool isTag = KeyIs(&mapIter, "tag");
          const bool isNid = KeyIs(&mapIter, "nid");
          if (cbor_value_advance(&mapIter) != CborNoError) return false;  // past the key

          if (cbor_value_at_end(&mapIter)) break;

          if (isTitle) {
            DecodeStringInto(&mapIter, out.title, sizeof(out.title));
          } else if (isBody) {
            DecodeStringInto(&mapIter, out.body, sizeof(out.body));
          } else if (isTag) {
            if (cbor_value_is_null(&mapIter)) {
              out.tag[0] = '\0';
              if (cbor_value_advance(&mapIter) != CborNoError) return false;
            } else {
              DecodeStringInto(&mapIter, out.tag, sizeof(out.tag));
            }
          } else if (isNid) {
            if (!ReadNid(&mapIter, out.nid)) return false;
          } else {
            if (cbor_value_advance(&mapIter) != CborNoError) return false;  // skip the unknown key's value
          }
        }
        return true;
      }

      uint16_t DecodeNotifyRemove(const uint8_t* payload, std::size_t len) {
        CborParser parser;
        CborValue root, mapIter;

        if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return 0;
        if (!cbor_value_is_map(&root)) return 0;
        if (cbor_value_enter_container(&root, &mapIter) != CborNoError) return 0;

        uint16_t nid = 0;
        while (!cbor_value_at_end(&mapIter)) {
          if (!cbor_value_is_text_string(&mapIter)) {
            if (!SkipUnknownEntry(&mapIter)) return 0;
            continue;
          }

          const bool isNid = KeyIs(&mapIter, "nid");
          if (cbor_value_advance(&mapIter) != CborNoError) return 0;
          if (cbor_value_at_end(&mapIter)) break;

          if (isNid) {
            if (!ReadNid(&mapIter, nid)) return 0;
          } else {
            if (cbor_value_advance(&mapIter) != CborNoError) return 0;
          }
        }
        return nid;
      }

      std::size_t EncodeNotifyDismiss(uint16_t nid, uint8_t* out, std::size_t outSize) {
        CborEncoder encoder, mapEncoder;
        cbor_encoder_init(&encoder, out, outSize, 0);
        if (cbor_encoder_create_map(&encoder, &mapEncoder, 1) != CborNoError) return 0;
        if (cbor_encode_text_stringz(&mapEncoder, "nid") != CborNoError) return 0;
        if (cbor_encode_uint(&mapEncoder, nid) != CborNoError) return 0;
        if (cbor_encoder_close_container(&encoder, &mapEncoder) != CborNoError) return 0;
        return cbor_encoder_get_buffer_size(&encoder, out);
      }

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
