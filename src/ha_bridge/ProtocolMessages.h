#pragma once

#include <cstdint>

namespace Pinetime {
namespace Applications {
namespace HaBridge {

// Wire constants shared with the companion app: the message types and the feature bits.
// There are no message structs here. BridgeController decodes and encodes every message in
// place, without the heap (the structs and their ProtocolMessages.cpp were never linked).

// Message types - must match Android Protocol.kt exactly
enum class MessageType : uint8_t {
  HELLO = 0x01,
  CONFIG_PUSH = 0x02,
  CONFIG_ACK = 0x03,
  STATE_UPDATE = 0x04,
  STATE_REFRESH_REQUEST = 0x05,  // ignored (the case was an empty handler until 2.4.99)
  ACTION_INVOKE = 0x06,
  ACTION_RESULT = 0x07,
  NOTIFY = 0x08,
  NOTIFY_ACK = 0x09,
  PING = 0x0A,
  PONG = 0x0B,
  SENSOR_REPORT = 0x0C,
  HAPTIC_FEEDBACK = 0x0D,
  WATCHFACE_LAYOUT = 0x0E,
  TIME_SYNC = 0x0F,
  LOGBOOK_REQUEST = 0x10,  // watch → phone: request logbook entries for a screen
  LOGBOOK_UPDATE  = 0x11,  // phone → watch: logbook entries payload
  WATCHFACE_ACK   = 0x12,  // watch → phone: ACK after applying watch face layout
  CALENDAR_UPDATE = 0x13,  // phone → watch: calendar events for a day
  CALENDAR_NAV        = 0x14,  // watch → phone: user navigated to different day (offset from today)
  HOME_CONTROL_UPDATE = 0x15,  // phone → watch: home control device list
  TIME_SYNC_ACK       = 0x16,  // watch → phone: ACK after applying time sync
  HELLO_ACK           = 0x17,  // phone → watch: ACK of HELLO (carries crashContextAck)
  VERSION_MISMATCH    = 0x18,  // watch → phone: incompatible protocol major version (not sent since 2.4.99)
  CONFIG_PUSH_ACK     = 0x19,  // watch → phone: ACK after applying CONFIG_PUSH, with CRC-32 (not sent since 2.4.99)
  // REQ-059 — crash-dump ring retrieval handshake.
  CRASH_DUMP_REQUEST  = 0x1A,  // phone → watch: request next pending crash dump entry
  CRASH_DUMP_ACK      = 0x1B,  // phone → watch: confirm delivery of crash dump entry
  // REQ-072 — find-device round-trip.  Closes the REQ-057+REQ-059+REQ-068+REQ-072
  // protocol-rev batch; BRIDGE_PROTOCOL_REV bumps to 2 in this commit.
  FIND_PHONE          = 0x1C,  // watch → phone: user requests phone-alert (not sent since 2.4.99)
  FIND_WATCH          = 0x1D,  // phone → watch: phone requests watch vibrate+flash 30s
  // REQ-071 — notification screen dismiss/snooze round-trip. Retired in 2.4.95; 0x1E is back in 2.4.101
  // with a new payload, and 0x1F stays unused.
  NOTIFY_DISMISS      = 0x1E,  // watch → phone: {"nid": n}, the user dismissed phone notification n (2.4.101, kFeaturePhoneNotifySync)
  NOTIFY_SNOOZE       = 0x1F,  // watch → phone: unused (the watch has no snooze; not sent since 2.4.95)
  DASHBOARD_STATE     = 0x20,  // phone → watch: the dashboard's live state (2.4.96, DashboardState.h)
  NOTIFY_REMOVE       = 0x21,  // phone → watch: {"nid": n}, the phone removed notification n; the watch drops it silently, no ACK (2.4.101)
};

// Reserved action ID for voice assistant trigger
constexpr uint16_t VOICE_ASSIST_ACTION_ID = 0xFFFE;

// Protocol feature flags (bitmask) — negotiated during HELLO handshake.
// Must match Android Protocol.Features constants.
constexpr uint32_t kFeatureWatchfaceLayout = (1 << 0);  // 0x01
constexpr uint32_t kFeatureLogbook         = (1 << 1);  // 0x02
constexpr uint32_t kFeatureHapticFeedback  = (1 << 2);  // 0x04
constexpr uint32_t kFeatureTimeSync        = (1 << 3);  // 0x08
constexpr uint32_t kFeatureVoiceAssist     = (1 << 4);  // 0x10 — the double-click sends ACTION_INVOKE 0xFFFE (2.4.99), and takes its ACTION_RESULT status
constexpr uint32_t kFeatureWatchfaceAck    = (1 << 5);  // 0x20
constexpr uint32_t kFeatureCalendar        = (1 << 6);  // 0x40
constexpr uint32_t kFeatureHomeControl        = (1 << 7);  // 0x80
constexpr uint32_t kFeatureProtocolNegotiation = (1 << 8);  // 0x100 — takes HELLO_ACK (the only key read is crashContextAck)
constexpr uint32_t kFeatureFindDevice          = (1 << 9);  // 0x200 — takes FIND_WATCH
// bit 10 (0x400) was kFeatureNotifyDismissSnooze: the watch sending NOTIFY_DISMISS / NOTIFY_SNOOZE. Not set since 2.4.95.
constexpr uint32_t kFeatureShortKeys          = (1 << 11); // 0x800 — REQ009-FR-C3: WATCHFACE_LAYOUT decoders accept short keys
constexpr uint32_t kFeatureCrashReport        = (1 << 12); // 0x1000 — answers CRASH_DUMP_REQUEST (reset reason + crash context), takes CRASH_DUMP_ACK
constexpr uint32_t kFeatureDashboardState     = (1 << 13); // 0x2000 — takes DASHBOARD_STATE: the dashboard renders live, not from the last layout push
constexpr uint32_t kFeatureSixScreens         = (1 << 14); // 0x4000 — a layout of up to 6 screens (2.4.100); without it the cap is 4
constexpr uint32_t kFeaturePhoneNotifySync    = (1 << 15); // 0x8000 — takes NOTIFY `nid` and NOTIFY_REMOVE, sends NOTIFY_DISMISS (2.4.101); bit 10 is not reused
constexpr uint32_t kFeatureConfigKeyV2        = (1u << 16); // 0x10000 — the CONFIG_PUSH decoder reads the key "infinassistant" (2.4.103); the app sends no CONFIG_PUSH to a watch without this bit
// The feature word is 32 bits wide: HELLO writes it as a CBOR unsigned int, 5 bytes once bit 16 is set (3 bytes up to 0xFFFF).
// The key of the HA integration map in CONFIG_PUSH, which the CONFIG decoder matches (the older spelling is not read).
constexpr const char* kConfigKeyInfinassistant = "infinassistant";

// Combined bitmask of all features supported by this firmware build.
constexpr uint32_t kSupportedFeatures =
    kFeatureWatchfaceLayout | kFeatureLogbook | kFeatureHapticFeedback | kFeatureTimeSync |
    kFeatureVoiceAssist | kFeatureWatchfaceAck | kFeatureCalendar | kFeatureHomeControl | kFeatureProtocolNegotiation |
    kFeatureFindDevice | kFeatureShortKeys | kFeatureCrashReport | kFeatureDashboardState | kFeatureSixScreens |
    kFeaturePhoneNotifySync | kFeatureConfigKeyV2;

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
