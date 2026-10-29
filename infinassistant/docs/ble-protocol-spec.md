---
owner: Global
review_cadence_days: 90
last_reviewed: 2026-10-28
status: active
---

# BLE Protocol Specification (v1)

Full specification for the CBOR-encoded binary protocol used between the Android companion app and PineTime watch over BLE GATT. For a summary, see the [Architecture Overview](architecture.md).

## Transport & GATT

- **Watch (InfiniTime)** is a BLE peripheral / GATT server.
- **Phone (Android)** is a BLE central / GATT client.

Service UUID (SSSS=0006):
`00060000-78fc-48fe-8e23-433b3a1942d0`

Characteristics:

| Characteristic | UUID suffix | Direction | Properties |
|----------------|-------------|-----------|------------|
| TX Notify | `00060001-...` | watch → phone | Notify |
| RX Write | `00060002-...` | phone → watch | Write / Write Without Response |
| Meta | `00060003-...` | bidirectional | Read/Write (version/capabilities; optional) |

## Framing

All payloads are chunked to survive MTU limits.

Header (9 bytes):
`[version:1][type:1][flags:1][msgId:2][totalLen:2][chunkIndex:1][chunkCount:1]`

- `version` = 1
- `msgId` is u16; used for reassembly and ACK correlation.
- Reassembly timeout: **5 seconds**.
- Maximum payload size: **10,240 bytes** (enforced by FrameCodec on both sides).

## Serialization

Payloads are CBOR encoded.
- Android: `kotlinx.serialization.cbor`
- Watch: TinyCBOR (with max nesting depth of 10)

## Safety Limits

| Limit | Value | Enforcement |
|-------|-------|-------------|
| Payload cap | 10 KB | FrameCodec rejects reassembled payloads >10 KB |
| CBOR nesting depth | 10 | Firmware scans raw bytes before parsing; rejects depth >10 |
| Message queue | 16 messages (512 bytes each) | Firmware drops oldest on overflow |
| Flow control | 5 pending BLE notifications | Prevents mbuf pool exhaustion |

## Reserved Action IDs

| ID | Purpose |
|----|---------|
| `0xFFFE` | Voice Assist (triggered when the side button is double-clicked, only if the BLE link is up AND the phone's last CONFIG_PUSH carried `voiceAssist: true`) |
| `0xFFFD` | Tile entity toggle (sent when user taps a tile) |
| `0x0000`–`0xFFFC` | User-defined actions |

## Message Types

| Code | Name | Direction | Description |
|------|------|-----------|-------------|
| `0x01` | HELLO | Bidirectional | Connection handshake with capabilities and diagnostics |
| `0x02` | CONFIG_PUSH | Phone → Watch | Push action/entity configuration |
| `0x03` | CONFIG_ACK | Watch → Phone | Acknowledge config was applied |
| `0x04` | STATE_UPDATE | Phone → Watch | Push entity state changes |
| `0x05` | STATE_REFRESH_REQUEST | Watch → Phone | Request state refresh |
| `0x06` | ACTION_INVOKE | Watch → Phone | User triggered an action |
| `0x07` | ACTION_RESULT | Phone → Watch | Result of action execution |
| `0x08` | NOTIFY | Phone → Watch | Push notification to watch |
| `0x09` | NOTIFY_ACK | Watch → Phone | Acknowledge notification displayed |
| `0x0A` | PING | Bidirectional | Keepalive request |
| `0x0B` | PONG | Bidirectional | Keepalive response |
| `0x0C` | SENSOR_REPORT | Watch → Phone | Periodic sensor data |
| `0x0D` | HAPTIC_FEEDBACK | Phone → Watch | Trigger vibration pattern |
| `0x0E` | WATCHFACE_LAYOUT | Phone → Watch | Configure watch face display |
| `0x0F` | TIME_SYNC | Phone → Watch | Synchronize clock |
| `0x10` | LOGBOOK_REQUEST | Watch → Phone | Request logbook entries (screen or app-level) |
| `0x11` | LOGBOOK_UPDATE | Phone → Watch | Send logbook entries |
| `0x12` | WATCHFACE_ACK | Watch → Phone | Acknowledge watch face layout was applied |
| `0x13` | CALENDAR_UPDATE | Phone → Watch | Send calendar events for the requested day |
| `0x14` | CALENDAR_NAV | Watch → Phone | Request calendar events for a given day offset |
| `0x15` | HOME_CONTROL_UPDATE | Phone → Watch | Push home control device states |
| `0x16` | TIME_SYNC_ACK | Watch → Phone | Acknowledge time sync was applied |
| `0x17` | HELLO_ACK | Phone → Watch | ACK of HELLO with peer protocol version |
| `0x18` | VERSION_MISMATCH | Watch → Phone | Incompatible protocol major version |
| `0x19` | CONFIG_PUSH_ACK | Watch → Phone | ACK after applying CONFIG_PUSH (with CRC-32) |
| `0x1A` | CRASH_DUMP_REQUEST | Bidirectional | Phone asks for the crash report; the watch replies with the same type (see [0x1A](#0x1a-crash_dump_request--0x1b-crash_dump_ack)) |
| `0x1B` | CRASH_DUMP_ACK | Phone → Watch | Phone has the crash context; the watch stops offering it |
| `0x1C` | FIND_PHONE | Watch → Phone | (Unused; no firmware sends it; app ignores it) |
| `0x1D` | FIND_WATCH | Phone → Watch | Phone asks the watch to buzz (see [0x1D](#0x1d-find_watch)) |
| `0x1E` | NOTIFY_DISMISS | Watch → Phone | The user dismissed a phone notification on the watch (firmware 2.4.101+, see [0x1E](#0x1e-notify_dismiss)). Firmware 2.4.91–2.4.94 sent it with the retired REQ-071 meaning. |
| `0x1F` | NOTIFY_SNOOZE | Watch → Phone | (Unused; not sent from firmware 2.4.95+; app ignores it) |
| `0x20` | DASHBOARD_STATE | Phone → Watch | The dashboard's live state (see [0x20](#0x20-dashboard_state)) |
| `0x21` | NOTIFY_REMOVE | Phone → Watch | The phone removed a forwarded notification; the watch drops it silently (firmware 2.4.101+, see [0x21](#0x21-notify_remove)) |

Source of truth:
- Android: `core-domain/.../Protocol.kt` in the bridge repository (`ryan-werts/infinassistant-bridge`)
- Firmware: `src/ha_bridge/ProtocolMessages.h` in the firmware repository (`ryan-werts/infinassistant-firmware`)

---

## 0x01: HELLO

**Direction:** Bidirectional (phone ↔ watch)

On BLE connection, both sides exchange HELLO messages.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `device` | string | yes | `"watch"` or `"phone"` |
| `proto` | int | yes | Protocol version (currently 1) |
| `capabilities` | map | yes | See Capabilities below |
| `features` | int | no | Bitmask of supported features (v1.131.0+, default 0) |
| `fwVersion` | string | watch only | Firmware version (e.g., `"1.131.0"`) |
| `resetReason` | string | watch only | Last reboot reason (`"Watchdog"`, `"Soft reset"`, etc.) |
| `freeHeap` | uint32 | watch only | Current free heap bytes |
| `minFreeHeap` | uint32 | watch only | Minimum free heap since boot |
| `mallocFails` | int | watch only | FreeRTOS malloc failure count |
| `stackOverflows` | int | watch only | FreeRTOS stack overflow count |
| `mbufFailures` | int | watch only | NimBLE mbuf allocation failure count |
| `uptimeTicks` | uint32 | watch only | FreeRTOS tick count |
| `helloDiagRev` | int | watch only | Revision of the HELLO diagnostic fields. A diagnostic field is meaningful only when `helloDiagRev` is at least the revision that introduced it. |
| `layoutRefused` | uint32 | watch only, `helloDiagRev` >= 3 | Count of layout pushes the watch refused since boot (first-chunk refusals, hand-off refusals and write refusals, summed). The app shows it as "Layout pushes refused" on the Debug screen and as `lastLayoutRefused` in the debug bundle; a watch below `helloDiagRev` 3 reports nothing, and the app shows a dash, never 0. |
| `layoutWriteFailed` | uint32 | watch only, `helloDiagRev` >= 4 | Count of layout pushes the watch accepted (its WATCHFACE_ACK said `ok`) and then did not write to its store, so the previous layout stands: a failed flash write, or a write dropped because the watch did not wake within 2 s. The app shows it as "Layout writes failed" on the Debug screen and as `lastLayoutWriteFailed` in the debug bundle; a watch below `helloDiagRev` 4 reports nothing, and the app shows a dash, never 0. |
| `crashContext` | map | watch only | See Crash Context below |
| `wk` | array of 5 uint | watch only, `helloDiagRev` >= 5, firmware 2.4.104+, **optional** | Where the screen time goes, since boot. See [`wk`: screen-wake telemetry](#wk-screen-wake-telemetry-firmware-24104) below. Present only when the whole HELLO fits the frame; when it is absent the app shows a dash, never 0, and an app that does not know the key ignores it. |
| `enc` | bool | watch only, firmware 2.4.101+ | Whether the link is encrypted when the HELLO is sent (the connection's `sec_state.encrypted`). The same key is in every PONG. The app forwards phone notifications only over an encrypted link (see [Phone notifications](#phone-notifications-nid-firmware-24101-app-21121)). |

**Which HELLO the watch sends.** The table above is the rich map. It is about 300 bytes, and the watch has no chunked transmit path: a single frame carries at most 244 payload bytes at the 256-byte MTU. So on real hardware the rich map never goes out, and the watch answers with a **compact map** instead. The compact map is the HELLO the phone gets.

| Firmware | `helloDiagRev` | Compact map fields |
|----------|----------------|--------------------|
| 2.4.104 and later | 5 | The same 15 fields, plus `wk` (about 15 bytes, at most 19) when the result still fits the frame: at the 256-byte MTU the room is 244 payload bytes against about 224 typical, so it normally goes, and a HELLO that is too full sends the 15 fields without it |
| 2.4.103 | 4 | The same 15 fields. `features` is 5 bytes (a CBOR `0x1A` and four bytes) instead of 3, because bit 16 takes the word past 16 bits: about 224 bytes typical, against the 244-byte frame limit |
| 2.4.101, 2.4.102 | 4 | The 2.4.64 fields plus `enc`, a CBOR boolean (5 bytes more; about 222 bytes typical) |
| 2.4.64 to 2.4.100 | 4 | `device`, `features`, `fwVersion`, `bridgeProtocolRev`, `helloDiagRev`, `freeHeap`, `minFreeHeap`, `mallocFails`, `stackOverflows`, `uptimeTicks`, `lvCreateFailures`, `safeFaceTrips`, `layoutRefused`, `layoutWriteFailed` (216 bytes typical, 240 at most) |
| 2.4.62, 2.4.63 | 3 | `device`, `fwVersion`, `bridgeProtocolRev`, `helloDiagRev`, `freeHeap`, `minFreeHeap`, `mallocFails`, `lvCreateFailures`, `safeFaceTrips`, `negotiatedMtu`, `oversizeNotifyDrops`, `helloEncodeTruncated` |

### `wk`: screen-wake telemetry (firmware 2.4.104)

`wk` is a CBOR array of five unsigned integers, in this order. The first four count since boot and are capped at 65535 (3 bytes on the wire); the fifth is a bit field.

| Index | Name | Unit | Meaning |
|-------|------|------|---------|
| 0 | `wakes` | count | Times the screen came on, from any cause. The boot itself is not one. A wake that arrives while the screen is still on its way off counts once. |
| 1 | `notifyWakes` | count | Of those, the wakes a notification caused: a notification (watch notification status On) that came while the screen was off and was let through. |
| 2 | `notifyStored` | count | Notifications only stored in the list, with no wake, no buzz and no pop-up (see [NOTIFY](#0x08-notify): the burst rule below). |
| 3 | `screenOnSec` | seconds | Total time the screen has been on, the boot's first session included. Whole seconds. |
| 4 | `settings` | bit field | The settings that drive battery life, packed (below). |

`settings`, least significant bit first:

| Bits | Value |
|------|-------|
| 0-4 | Wake-up modes: bit 0 single tap, 1 double tap, 2 raise wrist, 3 shake, 4 lower wrist |
| 5 | Always On display setting (1 on) |
| 6-7 | Notification status: 0 On, 1 Off, 2 Sleep |
| 8-10 | Brightness: 0 Off, 1 AlwaysOn, 2 Low, 3 Medium, 4 High, 5 B (firmware 2.4.105; the fourth Quick Settings level, after High) |
| 11-15 | Screen timeout in whole seconds, 31 at most (the watch offers 5, 7, 10, 15, 20 and 30) |

Example: `A1 62 77 6B 85 19 01 2C 18 3C 14 19 04 D2 19 78 14` is a map holding `wk` = [300, 60, 20, 1234, 0x7814]: 300 wakes, 60 of them by a notification, 20 notifications stored without a wake, 1234 s of screen time, and the settings 0x7814 (raise and lower wrist wake the screen, Always On off, notifications On, brightness 0 = Off, timeout 15 s). Medium brightness would read 0x7B14 (bits 8-10 = 3), and B would read 0x7D14 (bits 8-10 = 5).

**Brightness B (firmware 2.4.105).** A side-button wake lights the screen at High. Every other wake (notification, alarm, charging, Find Watch, chime, tap or raise) lights it at Low. The dim before sleep and the restore after a touch keep the current wake's level, and a button press while the screen is on changes nothing. Quick Settings' brightness button cycles Low, Medium, High, B, Low; on B it shows the brightness icon with a small B. A firmware older than 2.4.105 reads a stored 5 as High.

**Why it can be absent.** The HELLO has no chunked transmit path and little room: the 15 fields are about 224 bytes typical and 241 at their worst, against the 244 the frame carries. The watch encodes the 16-key map first and sends it only when it fits `min(256, MTU - 3) - 9` payload bytes; otherwise it sends the 15 fields, as every earlier firmware did. A reader must treat a missing `wk` as "not reported" (an `helloDiagRev` of 5 does not promise the key).

**Notification wake rules (firmware 2.4.104).** They are why `notifyStored` exists, and they change what the watch does with a NOTIFY when the notification status is On:

- The first notification of a burst wakes the screen, buzzes and shows the pop-up, as before.
- While the screen is on, the next ones show as before.
- Once the screen is off, a notification that comes less than 60 s after the last notification wake is only stored in the watch's list: no wake, no buzz, no pop-up. The 60 s run from the wake, not from the last notification, so a steady trickle still wakes the screen once a minute.
- A screen that only a notification woke goes off 5 s after the pop-up closes (the pop-up shows 7 s), unless the user touches the watch or presses the button, in which case the normal screen timeout applies.
- The Find Watch and layout-write wakes are unchanged.

Consequences for the phone:

- **No feature word before 2.4.64.** The phone reads an absent `features` as 0, which it treats as "unknown": it keeps every enrichment on and does not wait for TIME_SYNC_ACK.
- **`wk` on firmware before 2.4.104.** It is absent, and `helloDiagRev` is below 5. A reader that skips unknown keys needs no change.
- **Firmware before 2.4.103 is unsupported by the renamed app.** Its HELLO has no bit 16 (feature bit 16, CONFIG_KEY_V2), or no feature word at all. The app sends such a watch no CONFIG_PUSH and shows "Update the watch", which leads to the firmware update (DFU does not use CONFIG_PUSH). There is no dual-key support: an older watch is updated, not accommodated.
- **Short keys.** For the layout key spelling, the app treats a HELLO with no feature word at `helloDiagRev` >= 3 as short-key capable (`ProtocolHandler.peerAcceptsShortKeys`), because the dual-key decoders arrived in 2.4.62 together with revision 3.
- **`layoutRefused` on 2.4.62 and 2.4.63.** These report `helloDiagRev` 3 but never deliver the field, so the app shows a dash.
- **Rich-only fields never arrive in a HELLO.** `resetReason`, `mbufFailures`, `notifyFailures` and `crashContext` are in the rich map only. From 2.4.65 the phone gets `resetReason` and the crash context another way, through [CRASH_DUMP_REQUEST](#0x1a-crash_dump_request--0x1b-crash_dump_ack); `mbufFailures` and `notifyFailures` still never arrive.
- **Firmware 2.4.99 layout decoder changes.** The watch no longer decodes layout keys: `theme`, `statusBar`, the header's `timeAlign` and `presenceIcons`, and v1 layout-level `tiles`. These are skipped as unknown keys. The app puts them at their default values when encoding for 2.4.99+, and enum values stay reserved.

**Capabilities map:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cbor` | bool | `true` | CBOR serialization supported |
| `maxActions` | int | `20` | Maximum action count |
| `supportsDoubleTap` | bool | `true` | Double-tap gesture supported |
| `supportsNotifyDedupe` | bool | `true` | Notification dedup by tag supported |

**Features bitmask** (v1.131.0+):

| Bit | Value | Name | Description |
|-----|-------|------|-------------|
| 0 | `0x01` | WATCHFACE_LAYOUT | Watch face layout support |
| 1 | `0x02` | LOGBOOK | Logbook screen support (deprecated — use standalone HaLogbook app) |
| 2 | `0x04` | HAPTIC_FEEDBACK | Haptic feedback support |
| 3 | `0x08` | TIME_SYNC | Time synchronization support |
| 4 | `0x10` | VOICE_ASSIST | Voice assistant support |
| 5 | `0x20` | WATCHFACE_ACK | Watch face ACK (0x12) support |
| 6 | `0x40` | CALENDAR | HaCalendar standalone app: CALENDAR_NAV (0x14) / CALENDAR_UPDATE (0x13) |
| 7 | `0x80` | HOME_CONTROL | HaHomeControl standalone app: HOME_CONTROL_UPDATE (0x15) |
| 8 | `0x100` | PROTOCOL_NEGOTIATION | HELLO_ACK / VERSION_MISMATCH |
| 9 | `0x200` | FIND_DEVICE | FIND_PHONE / FIND_WATCH |
| 10 | `0x400` | NOTIFY_DISMISS_SNOOZE | Watch can send NOTIFY_DISMISS / NOTIFY_SNOOZE. Firmware 2.4.91–2.4.94 set it; 2.4.95 does not, since the Notifications app that sent them is gone. Retired: it is not reused, because firmware before 2.4.95 still sets it for the old meaning. Phone notification sync uses bit 15. |
| 11 | `0x800` | SHORT_KEYS | The `WATCHFACE_LAYOUT` decoder accepts the short key spelling (see [Short keys](#short-keys-feature-bit-11)). Firmware 2.4.62 sets it; the app sends short keys only to a peer that advertises it. |
| 12 | `0x1000` | CRASH_REPORT | The watch answers CRASH_DUMP_REQUEST with its reset reason and crash context, and takes CRASH_DUMP_ACK (see [0x1A](#0x1a-crash_dump_request--0x1b-crash_dump_ack)). Firmware 2.4.65 sets it; the app sends the request only to a peer that advertises it. |
| 13 | `0x2000` | DASHBOARD_STATE | The watch takes DASHBOARD_STATE and draws the dashboard from it (see [0x20](#0x20-dashboard_state)). Firmware 2.4.96 sets it; the app (2.1.107 and later) sends the message only to a peer that advertises it. |
| 14 | `0x4000` | SIX_SCREENS | The watch takes up to 6 layout screens (the dashboard plus 5 tile screens). Firmware 2.4.100 sets it, so the features word is `0x7BFF`. The app sends more than 4 screens only to a peer that advertises it; otherwise it trims to 4 and logs `WATCHFACE_LAYOUT screens trimmed`. |
| 15 | `0x8000` | PHONE_NOTIFY_SYNC | The watch takes NOTIFY `nid` and NOTIFY_REMOVE, and sends NOTIFY_DISMISS (see [0x08](#0x08-notify), [0x1E](#0x1e-notify_dismiss), [0x21](#0x21-notify_remove)). Firmware 2.4.101 sets it, so the features word is `0xFBFF`. The app (2.1.121) sends NOTIFY_REMOVE only after a HELLO with this bit, and drops a NOTIFY_DISMISS from a watch without it. Firmware: `kFeaturePhoneNotifySync`. |
| 16 | `0x10000` | CONFIG_KEY_V2 | The watch's CONFIG_PUSH decoder reads the key `infinassistant` (see [0x02](#0x02-config_push)). Firmware 2.4.103 sets it, so the features word is `0x1FBFF`. The word is a 32-bit CBOR unsigned int: 3 bytes up to `0xFFFF`, 5 bytes (`1A 00 01 FB FF`) from this bit on. The app sends CONFIG_PUSH only to a peer that advertises it, and shows "Update the watch" to one that does not. **Firmware before 2.4.103 is unsupported by the renamed app.** Firmware: `kFeatureConfigKeyV2`. |

Backward compatible: old firmware sends `features=0` (absent), phone treats as "all features unknown." Phone can check `peerSupports(flag)` before sending feature-specific messages.

**Crash Context map** (optional, present when the previous boot left one; see [0x1A](#0x1a-crash_dump_request--0x1b-crash_dump_ack) for when that is):

| Field | Type | Description |
|-------|------|-------------|
| `breadcrumb` | uint8 | Last processing phase |
| `lastMsgType` | uint8 | Last protocol message type being processed |
| `lastMsgId` | uint16 | Last protocol message ID |
| `freeHeap` | uint32 | Free heap at time of crash |
| `uptimeTicks` | uint32 | Uptime at time of crash |

---

## 0x02: CONFIG_PUSH

**Direction:** Phone → Watch

Pushes configuration to the watch.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `configVersion` | int | yes | Config version number (monotonically increasing) |
| `infinassistant` | map | no | HA integration config. The key was spelled differently before firmware 2.4.103, which reads only this one (no dual-key support): the app sends CONFIG_PUSH only to a watch with feature bit 16. |
| `voiceAssist` | bool | no | Enable voice assist on the watch (top-level CBOR bool; absent = false; cleared on disconnect) |

The watch reads only `configVersion`, `infinassistant`, and `voiceAssist`. The app no longer sends `actions`, `entities`, `notificationPrefs`, or `stateUpdatePolicy` (removed in app 2.1.116). Firmware 2.4.94+ ignores any such fields if present.


---

## 0x03: CONFIG_ACK

**Direction:** Watch → Phone

Acknowledges that config was applied (or rejected).

| Field | Type | Description |
|-------|------|-------------|
| `ok` | bool | `true` if config applied successfully |
| `appliedVersion` | int | The `configVersion` that was applied |

---

## 0x04: STATE_UPDATE

**Direction:** Phone → Watch

Pushes entity state changes to the watch. From app 2.1.116, only tile entity states are sent (settings entities were removed).

| Field | Type | Description |
|-------|------|-------------|
| `items` | array | List of StateItem objects |

**StateItem:**

| Field | Type | Description |
|-------|------|-------------|
| `entityId` | string | HA entity ID (tile entity from the current layout) |
| `state` | string | Current state value (e.g., `"on"`, `"22.5"`) |
| `unit` | string? | Unit suffix (e.g., `"°F"`, `"lux"`) |
| `ts` | long | Unix timestamp of state change |

**What the watch keeps.** The states are for the face's tiles, the only screen that reads them:

- **Capacity.** From firmware 2.4.100 the watch keeps up to 24 states, enough for every tile of any layout (6 screens of 4 tiles). Firmware 2.4.94 to 2.4.99 kept 16 (4 screens of 4 tiles). Each is keyed by a hash of the entity ID, and only the state is stored, not `unit` or `ts`.
- **When full.** A state for an entity the watch does not have is dropped once 16 are kept.
- **After a new layout.** The first STATE_UPDATE after an accepted WATCHFACE_LAYOUT empties the table first. The app follows the layout with its tiles' states, so they replace the previous layout's.
- **Order.** The app sends the tile entities in the order they appear in the layout, and keeps that order when it fetches the states from HA.

---

## 0x05: STATE_REFRESH_REQUEST

**Direction:** Watch → Phone

**Reserved.** No firmware sends this message; the app ignores it if received. Firmware 2.4.99 ignores it if received from the app.

---

## 0x06: ACTION_INVOKE

**Direction:** Watch → Phone

User triggered an action on the watch (tile tap, voice assist, or home control).

| Field | Type | Description |
|-------|------|-------------|
| `actionId` | int | Action ID: `0xFFFE`=voice assist (double-click side button), `0xFFFD`=tile entity tap, `0xFFFC`=home control (deprecated) |
| `gesture` | string | For voice assist (`0xFFFE`): `"tap"` (starts a session) or `"double"` (cancels if a session is active). For tiles: `"tap"`, `"long"`, or `"double"`. |
| `entityId` | string? | For tile taps (`0xFFFD`): the HA entity being toggled |

**Voice behavior:** The watch sends ACTION_INVOKE with actionId `0xFFFE` and gesture `"tap"` to start a voice session, entering provisional session state (10 s until the first ACTION_RESULT for `0xFFFE`, then 60 s from each result). A gesture `"double"` sent while the watch thinks a session is active cancels it. Otherwise the double-click opens InfiniTime's Notifications. The watch session ends by a final ACTION_RESULT.

---

## 0x07: ACTION_RESULT

**Direction:** Phone → Watch

Result of executing an action. Sent in response to ACTION_INVOKE. From app 2.1.116, sent for tile-tap invocations (actionId `0xFFFD`) and voice assist (actionId `0xFFFE`); no "queued" interim status for tile taps.

| Field | Type | Description |
|-------|------|-------------|
| `actionId` | int | The action that was executed (e.g., `0xFFFD` for tile tap, `0xFFFE` for voice assist) |
| `ok` | bool | `true` if action succeeded |
| `message` | string? | Status message (error text for failures, or null for success) |

**Voice assist results:** For actionId `0xFFFE`, non-final messages carry `ok=true` and `message` one of: `"listening"` (each listening turn), `"processing"`, `"speaking"`. Final results: `"done"` (ok=true), `"cancelled"` (ok=true), or a short reason with ok=false. Results for `0xFFFE` bypass the watch's stale check and never touch tile glyphs. The phone no longer sends "queued" interim results for tile taps.

---

## 0x08: NOTIFY

**Direction:** Phone → Watch

Push a notification to the watch display.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `title` | string | — | Notification title |
| `body` | string | — | Notification body text |
| `severity` | enum | `"INFO"` | `"INFO"`, `"WARN"`, or `"CRITICAL"` |
| `tag` | string? | null | Deduplication tag (replaces existing notification with same tag) |
| `ts` | long | — | Unix timestamp |
| `nid` | uint? | absent | A phone notification's id, 1 to 65535 (firmware 2.4.101+, app 2.1.121+). Home Assistant notifications have none. |

**How the watch shows it (firmware 2.4.95).** As one of InfiniTime's own notifications, the way an Alert Notification Service alert from a phone is shown:
- **Arrival:** the popup and a vibration. Under the watch's do-not-disturb setting it arrives silently.
- **Afterwards:** it stays in the swipe-down list, which holds InfiniTime's last 5.
- **Message:** `title`, then `body`, cut to InfiniTime's 100 characters. A notification with no title shows "Home Assistant".
- **Ignored:** `severity`, `ts` and `tag` are not used on the watch. The tag is still echoed in NOTIFY_ACK.

Until 2.4.95 a NOTIFY went silently into a list of 3 that only the HA Notifications app showed. That app is removed.

### Phone notifications: `nid` (firmware 2.4.101, app 2.1.121)

- **What it is.** The app gives each forwarded phone notification (one `StatusBarNotification.key`) a short id, 1 to 65535, wrapping (`PhoneNotificationRegistry`, at most 64 keys). It sends that id as `nid` and, as text, as `tag`.
- **Decode.** A value that is not an unsigned integer in 1 to 65535 reads as 0, which is none (`NotifyDecoder.cpp`). A NOTIFY with no `nid` behaves as in 2.4.100.
- **Older firmware.** Firmware before 2.4.101 skips the unknown key, so the app may always send it.
- **The replace rule.** A NOTIFY whose `nid` is already on the watch replaces that entry. The old entry is removed with no NOTIFY_DISMISS back, and the new one is pushed with the popup and the vibration (`PhoneNotifySync::Push`).
- **The watch's table.** One pair `{list id, nid}` per list entry, 5 at most (20 bytes). A pair whose entry left the list (the list's oldest pushed out by a sixth, including by the Alert Notification Service) is dropped after every push.
- **The link gate (app).** The app forwards phone notifications only when the watch is bonded, this connection's HELLO has arrived, and, if that HELLO or a later PONG reports `enc`, `enc` is true. Firmware without `enc` (2.4.100) is gated on the bond alone once its HELLO arrives.
  - **Watch not bonded:** a notification is skipped (reason `link-not-encrypted`), not queued.
  - **Bonded, link not open yet** (no HELLO yet, or `enc` false): forwards are held, the newest 5, and sent once the link opens.
  - **`enc` false in a HELLO, or the phone's bond just completed:** the app sends an early PING, so the PONG's `enc` arrives within seconds.
  - Home Assistant notifications are not gated.
- **Resends.** After a reconnect the app resends pending phone forwards only while they are still on the phone, the newest 5 at most.

---

## 0x09: NOTIFY_ACK

**Direction:** Watch → Phone

Acknowledges that a notification was displayed on the watch.

| Field | Type | Description |
|-------|------|-------------|
| `tag` | string? | The tag of the acknowledged notification |
| `msgId` | int | The message ID of the NOTIFY being acknowledged |
| `displayed` | bool | `true` if notification was shown to user |

---

## 0x0A: PING / 0x0B: PONG

**Direction:** Bidirectional

Keepalive messages. The watch's PONG carries `{"rid": <the PING's msgId>, "enc": <bool>}`, 13 bytes at most (`ha_bridge/LinkReport.h`); the phone matches `rid` against the PING it sent.

| Field | Type | Description |
|-------|------|-------------|
| `rid` | uint | The msgId of the PING being answered |
| `enc` | bool | Firmware 2.4.101+: whether the link is encrypted at this moment (CBOR `F4` false, `F5` true). The app reads it to update the link state; older apps ignore the unknown key. |

If the payload does not fit its buffer, the watch sends an empty PONG.

---

## 0x0C: SENSOR_REPORT

**Direction:** Watch → Phone

Periodic sensor data from the watch (default interval: 60 seconds, set by CONFIG_PUSH `infinassistant.sensorReportIntervalSeconds`). The watch sends it only when CONFIG_PUSH `infinassistant.enabled` is true, and only while the HA face is on screen and the display is awake, because the watch checks the interval from the face's 1 s tick. The interval is measured in wall-clock time, so the first check after the screen wakes sends a report if one is due. Firmware before 2.4.65 never sent it: its millisecond clock read 0 at the 1024 Hz tick rate, so the interval never elapsed.

Payload uses compact single-letter keys:

| Key | Type | Description |
|-----|------|-------------|
| `s` | uint32 | Step count |
| `hr` | uint8 | Heart rate BPM |
| `hs` | uint8 | HR sensor state (0=Stopped, 1=NotEnough, 2=NoTouch, 3=Running) |
| `b` | uint8 | Battery percentage (0-100) |
| `bc` | bool | Battery charging |
| `mx` | int16 | Accelerometer X |
| `my` | int16 | Accelerometer Y |
| `mz` | int16 | Accelerometer Z |
| `ts` | uint64 | Unix timestamp |
| `fh` | uint32 | Current free heap bytes (diagnostic) |
| `mh` | uint32 | Minimum free heap since boot (diagnostic) |
| `sw` | uint32 | Stack high-water mark in words (diagnostic) |
| `lvfs` | uint32 | LVGL heap free bytes (`lv_mem_monitor` `free_size`; REQ-056) |
| `lvfb` | uint32 | The LVGL heap's biggest free block, bytes (REQ-056) |
| `lvfp` | uint8 | LVGL heap fragmentation, % (`(free - biggest) * 100 / free`; REQ-056) |
| `lvcf` | uint16 | LVGL object creations that failed since boot (REQ-056) |
| `dsOk` | uint16 | DASHBOARD_STATE messages decoded and published since boot (wraps; firmware 2.4.97) |
| `dsBad` | uint16 | DASHBOARD_STATE messages the decoder refused since boot (wraps; 2.4.97) |
| `fpMis` | uint16 | Live states drawn against a layout of another fingerprint, counted once per state (wraps; 2.4.97) |
| `dsAge` | uint32 | Seconds since the last DASHBOARD_STATE was published, 0 when none since boot (2.4.97) |
| `bmv` | uint | Battery voltage in millivolts (firmware 2.4.105). Absent until the watch has measured the battery once. |
| `pp` | bool | `true` while a power source is connected (firmware 2.4.105). Absent otherwise. Unlike `bc`, which is false once the battery is full, it stays on for as long as the watch is on the charger. |

The keys go out in this order: 22 entries, 163 bytes at most with `bmv` and `pp` (`ha_bridge/SensorReport.h`, which the host tests encode too; the largest report was 152 bytes before them). The 256-byte buffer has room for it. Firmware 2.4.96 and older send the first 16, firmware 2.4.97 to 2.4.104 send the first 20. The app reads the keys it knows and skips the rest.

---

## 0x0D: HAPTIC_FEEDBACK

**Direction:** Phone → Watch

Triggers haptic feedback on the watch.

| Field | Type | Description |
|-------|------|-------------|
| `pattern` | string | One of: `"short"`, `"long"`, `"double"`, `"recording_start"` (two short pulses at each listening turn), `"recording_stop"` (short at voice session end, double at a failure) |

**Voice haptics:** From firmware 2.4.99, haptics play from any screen and with the display off (timer-driven). A voice final result's own buzz and the phone's matching HAPTIC_FEEDBACK within 1.5 s play once (merged).

---

## 0x0E: WATCHFACE_LAYOUT

**Direction:** Phone → Watch

Configures the watch face display. Complex nested structure:

| Field | Type | Description |
|-------|------|-------------|
| `version` | int | Layout version |
| `theme` | map | Theme colors |
| `header` | map | Header config (time format/align, presence icons) |
| `screens` | array | Array of screen definitions (max 6: the dashboard plus 5 tile screens; more than 4 needs feature bit 14, `0x4000`, otherwise the app trims to 4) |
| `statusBar` | map | Status bar configuration |
| `logbookApp` | map? | Config for standalone HaLogbook app (optional) |
| `homeControl` | map? | Config for standalone HaHomeControl app (optional) |
| `defaultActiveColor` | string | Hex color for active entity tiles (e.g., `"#FFD700"`) |
| `defaultInactiveColor` | string | Hex color for inactive entity tiles (e.g., `"#800080"`) |

Each screen has a `screenType`: 0 (dashboard), 1 (tiles).
Tile screens include `tileLayout`: 4 = 2×2 grid (the only supported layout). From app 2.1.68 the push omits it, because it equals its default and nothing on the watch reads it.

**How the watch draws a tile screen (firmware 2.4.94).** It uses the Home Control app's style. The 2×2 grid fills the screen above the "n/N" indicator, with no clock or status row. Each tile:

- **Top line(s).** The icon or the name, white. A long name wraps, up to 3 lines.
- **State line.** The entity state below the name. A state too long for one line is cut with a "." ("unavailable" shows as "unavail."), and `_` shows as a space.
- **Off.** A dark tile with a dim frame and a grey state.
- **On.** A tile with `colorEnabled` takes `defaultActiveColor` (gold by default) for its frame and state, on a dark tint of it.
- **Own colour.** A tile without `colorEnabled` keeps its own `color` as its frame.
- **`defaultInactiveColor`** is no longer drawn. Until 2.4.94 it filled every off tile (purple by default).

Taps:

- A `confirm` tile shows "confirm?" on the first tap.
- An action result replaces the state for 2 s: an hourglass while pending, a green check, or a red "failed".

Icons (`displayMode` 1) draw only where the tile font has a glyph: home, music, bell, heart, bluetooth, door, battery, steps, tv, fan, power, car, truck and person. Any other icon (light, garage, fire, ...) shows the name; until 2.4.94 those drew an empty line.

### Short keys (feature bit 11)

When the watch advertises feature bit 11 (`0x800`, `kFeatureShortKeys` in `ProtocolMessages.h`), the app sends the layout with one-letter map keys, which shortens the payload the watch has to hold while it reassembles it. The spelling is negotiated per session, from the live HELLO, and the bit is the only switch:

- **Bit 11 set:** the app sends short keys, and every map and array is **definite-length**. Short keys and definite-length containers travel together, and only when the bit is set.
- **Bit 11 clear** (firmware 2.4.61 and older): the app sends the long keys exactly as before.
- **Dual-key acceptance:** the 2.4.62 decoder accepts BOTH spellings and keeps accepting the long keys for at least one release, so an older app (2.1.67 and earlier) keeps working against the new firmware. Both spellings of a key count as one key: for a scalar the last occurrence wins, and for an array the first occurrence is decoded and later ones are skipped.
- **`kProtocolMajor` is unchanged** (1). Bit 11 is a feature bit, not a protocol version, so no VERSION_MISMATCH is involved.

The table is generated from one wire-key definition, which emits the firmware, Kotlin and TypeScript copies. A short key is unique within its map level, never equals a long key of the same level, and is scoped to that level, so the same letter means different things at different levels. Keys the firmware never decodes stay long.

| Map level | Long key -> short key | Kept long |
|-----------|-----------------------|-----------|
| Layout (top-level map) | `version` -> `v`, `theme` -> `m`, `header` -> `h`, `screens` -> `s`, `tiles` -> `t`, `statusBar` -> `b`, `defaultActiveColor` -> `a`, `defaultInactiveColor` -> `i` | `logbookApp`, `homeControlConfig`, `calendarEntityIds` |
| `theme` | `background` -> `b`, `accent` -> `a` | - |
| `header` | `timeFormat` -> `f`, `timeAlign` -> `a`, `presenceIcons` -> `p` | - |
| `header.presenceIcons[]` | `type` -> `t`, `color` -> `c`, `entity` -> `e` | - |
| `statusBar` | `battery` -> `b`, `steps` -> `s`, `heartRate` -> `h`, `notifications` -> `n`, `bluetooth` -> `t` | - |
| `screens[]` | `screenType` -> `t`, `tileLayout` -> `l`, `tiles` -> `i`, `dashboard` -> `d` | `logbook` |
| `screens[].tiles[]` and layout-level `tiles[]` | `label` -> `l`, `entityId` -> `e`, `color` -> `c`, `colorEnabled` -> `n`, `icon` -> `i`, `displayMode` -> `m`, `confirm` -> `f` | - |
| `screens[].dashboard` | `statusGroups` -> `g`, `sensorGroups` -> `s`, `weather` -> `w`, `events` -> `e`, `notifications` -> `n`, `lea` -> `a`, `cvl` -> `c`, `cvv` -> `v`, `im0l` -> `i`, `im0v` -> `j`, `im1l` -> `k`, `im1v` -> `m` | - |
| `dashboard.statusGroups[]` | `type` -> `t`, `glyphs` -> `g` | - |
| `statusGroups[].glyphs[]` | `symbol` -> `s`, `entityId` -> `e`, `active` -> `a` | - |
| `dashboard.sensorGroups[]` | `type` -> `t`, `sensors` -> `s` | - |
| `sensorGroups[].sensors[]` | `entityId` -> `e`, `displayType` -> `d`, `value` -> `v` | - |
| `dashboard.weather` | `entityId` -> `e`, `condition` -> `c`, `temp` -> `t`, `hi` -> `h`, `lo` -> `l` | - |
| `dashboard.events[]` | `time` -> `t`, `title` -> `i`, `ese` -> `e` | - |
| `dashboard.notifications[]` | `source` -> `s`, `text` -> `t` | - |
| `screens[].logbook` | (none) | `entityIds`, `maxEntries`, `hoursBack` |

### Fields the app no longer sends (rebalance R1)

To keep the payload inside the watch heap budget, app 2.1.68 puts every field the firmware never renders at its declared default before encoding, and the encoder omits a field that equals its default. The phone keeps the full layout for its own preview and persistence; only the copy sent to the watch is stripped. Omitted on the wire: `theme.background`, `theme.accent`, `header.timeAlign`, `header.presenceIcons`, the five `statusBar` flags, `screens[].tileLayout`, `dashboard.notifications`, `statusGroups[].type`, `sensorGroups[].type`, `weather.entityId`, `weather.temp`, `glyph.entityId`, `sensor.entityId` and `sensor.displayType`. The firmware treats an absent key as its default, so a long-key push from an older app that still carries them is accepted unchanged.

### Refusals and the WATCHFACE_ACK `pending` reason

The watch refuses a layout transfer at its first chunk, before allocating anything, while another transfer is in flight, while a stored layout is being written, or while the dashboard is materializing a screen. It answers with a `WATCHFACE_ACK` whose `ok` is `false` and whose `reason` is exactly `"pending"`. Once a transfer ends, a 1100 ms cooldown keeps the next one out until the display has had a full 1000 ms refresh.

- **App retry:** on `reason == "pending"` (and only that reason), on the first attempt of a command, the app waits **34 s** (the completion bound of a layout write) and runs the whole attempt again once: a fresh HELLO, one encode in the then-current spelling, preflight, send. A second `pending` is reported as a failure. `validation_error`, `decode_error`, `oom`, an ACK timeout and a preflight refusal are not retried.
- **Stalled transfer:** if a transfer stops arriving, the watch releases it about 5.1 s after the first chunk (the 5 s reassembly timeout plus a 100 ms slack) without waiting for another frame, and a disconnect clears it at once.
- **Heap refusal:** a transfer refused because free heap cannot hold it sends no ACK; the app sees an ACK timeout. The app normally refuses such a push itself first, from the last HELLO `freeHeap`.
- **Counting:** every refusal increments the HELLO `layoutRefused` field (`helloDiagRev` 3).
- **Deferred swipes:** while a transfer or write holds the watch, a swipe or wake reset on the dashboard is deferred and then served. It is never dropped. The bound is about 5.1 s of receive, plus the write (at most 34 s, 17 s in steady state), plus one 1000 ms refresh.

### Troubleshooting: layout not updating while the dashboard is up

Two counters tell a client-side refusal from a watch-side one:

1. Find the app log line `WATCHFACE_LAYOUT push` (Debug screen protocol log, the debug bundle, or the diagnostic export). One line is written per attempt: `attempt=<n> keys=<short|long> bytes=<n> preflight=<pass|PreflightFailed> ...`.
2. Read "Layout pushes refused" (HELLO `layoutRefused`) from a HELLO taken after the push settled, and compare it with the count before the push.

| App log line | `layoutRefused` | Meaning |
|--------------|-----------------|---------|
| `preflight=PreflightFailed` (with `freeHeap=` and `field=`) | unchanged | The app refused before sending; nothing reached the watch. The watch did not have the heap for this payload. Switch the watch off the HA face and push again, or shrink the layout. |
| `preflight=pass`, then `attempt=2` about 34 s later | +1 per refused attempt | The watch answered `pending`; the app retried once. If both fail, wait out the completion bound (34 s) and push again. |
| `preflight=pass`, one attempt, ACK timeout | unchanged | The transfer never completed (link loss, or the watch-side heap guard, which sends no ACK). Check the connection and push again. |
| `preflight=pass`, ACK `ok` | unchanged | The layout was accepted. The dashboard redraws on its second 1000 ms refresh after the commit; a swipe or wake that lands inside a push is deferred until the push ends. |

A watch below `helloDiagRev` 3 does not report `layoutRefused`; the app shows a dash and only the app log line is available.

---

## 0x0F: TIME_SYNC

**Direction:** Phone → Watch

Synchronizes the watch clock. Decoder: `BridgeController::HandleTimeSync`.

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | uint64 | Unix epoch seconds |
| `timezoneOffset` | int | Timezone offset from UTC in **seconds**, DST included |
| `isDst` | bool | Whether daylight saving time is active |

The watch sets its clock to local wall time (`timestamp + timezoneOffset`) and, from firmware 2.4.92, its zone offset (`DateTime::SetTimeZone`, in quarters of an hour, all of it as the zone with DST 0), so `UTCDateTime()` is UTC.
- **Why it matters:** the meeting countdowns, the next-event header and its buzz, and the stale banner all compare `UTCDateTime()` with epochs from the phone.
- **Before 2.4.92:** the zone was never set, so on a phone at UTC-6 they ran 6 h off, and every sync reported a 21600 s delta.
- **When it applies:** when the drift exceeds 5 s or the zone changed (DST, travel). Otherwise the sync is refused as too small.

---

## 0x10: LOGBOOK_REQUEST

**Direction:** Watch → Phone

A request for logbook entries. **No firmware sends it** (up to 2.4.89): the type is declared in `ProtocolMessages.h`, but nothing in the watch writes it, so the phone pushes the logbook app's entries unasked (see LOGBOOK_UPDATE). The phone still answers a request if one arrives.

| Key | Type | Description |
|-----|------|-------------|
| `screenIndex` | int | Screen index, or `255` (`0xFF`) for the HaLogbook app. |

- **`screenIndex = 0xFF`:** the phone uses the layout's `logbookApp` config (`entityIds`, `maxEntries`, `hoursBack`). With no `entityIds` it uses the face's motion sensors (see LOGBOOK_UPDATE).
- **`screenIndex = 0–3`:** the phone uses that screen's logbook config (the legacy logbook screen type).

---

## 0x11: LOGBOOK_UPDATE

**Direction:** Phone → Watch

Logbook entries for the HaLogbook app, which shows them as `time state subject`. The phone sends the app's entries:
- on every connection;
- after a layout push;
- 2 s after a state change of one of the app's entities (one push per burst).

The app's entities are the layout's `logbookApp.entityIds`. When that list is empty, they are the face's motion sensors: the glyphs of its dashboards' `motion` status groups (app 2.1.99 and later; `LogbookOrchestrator.appLogbook`). The entries are the newest events of the last `hoursBack` hours.

The events are HA's logbook (`logbook/get_events`) plus the state changes the phone saw live: from app 2.1.110, the changes of its state cache (`HaStateCache`, fed by the `subscribe_entities` subscription). HA's recorder writes a change to its database at its next commit, every 5 s by default, so a fetch 2 s after the change does not list it yet. Since app 2.1.100 the phone adds the changes it saw that the logbook does not list (same entity, state and second). Before that, the push a change caused usually missed that change. Attribute-only changes are not entries. When the logbook cannot be read, nothing is sent and the watch keeps what it has.

It also sends them in reply to LOGBOOK_REQUEST. The watch keeps the entries in RAM and does not acknowledge them. Decoder: `BridgeController::HandleLogbookUpdate`. Encoder: `LogbookUpdatePayload` and `LogbookOrchestrator`.

| Key | Type | Description |
|-----|------|-------------|
| `screenIndex` | int | `0xFF` for the app. Sent, but the watch skips it. |
| `entries` | array | At most 8 (`kMaxLogbookEntries`; 4 before firmware 2.4.91), most recent first; the watch drops any beyond that. The phone sends as many of the newest as fit one frame (244 payload bytes), typically 5 to 7: a longer message needs the watch's reassembly buffer, which it refuses while its heap is low. |

**Entry:**

| Key | Type | Description |
|-----|------|-------------|
| `t` | text, ≤ 7 bytes | Local time `"HH:mm"`. Required. |
| `s` | text, ≤ 7 bytes | Short state: `ON`, `OFF`, `OPN`, `CLSD`, `LCKD`, `ULKD`, `HOME`, `AWAY`, `N/A`, `TRGD`, `CLR`, `PLAY`, or the state in upper case, cut to 7 bytes. The phone builds it from the event's `state`, or else from its message (`"turned on"`, `"changed to Home"`, `"was unlocked"`). Required. |
| `n` | text, ≤ 31 bytes | Subject: the entity's friendly name. |

An entry without `t` or `s` is dropped. The watch's buffers are `char[8]`, `char[8]` and `char[32]`, and the string rule of CALENDAR_UPDATE applies. Until app 2.1.97 the phone sent `{"t", "x"}`, so every entry was dropped.

---

## 0x12: WATCHFACE_ACK

**Direction:** Watch → Phone

Sent by the watch after it has successfully applied a `WATCHFACE_LAYOUT` (0x0E) message.

| Field | Type | Description |
|-------|------|-------------|
| `ok` | bool | `true` if the layout was applied successfully |
| `version` | int | The layout `version` that was applied |
| `reason` | string | Present when `ok` is `false`: `validation_error`, `decode_error`, `oom`, or `pending` (see [Refusals](#refusals-and-the-watchface_ack-pending-reason)). With `ok` `true`, `reason` may be `screens_trimmed` (firmware 2.4.100): the layout had more screens than the watch's cap, so the watch kept the first ones and applied them. |

---

## 0x13: CALENDAR_UPDATE

**Direction:** Phone → Watch

Calendar entries for one day, shown by the HaCalendar app. The phone sends them in reply to `CALENDAR_NAV` (0x14), and unasked with today's entries on every connection and after a layout push. The watch keeps them in RAM and does not acknowledge them. Decoder: `BridgeController::HandleCalendarUpdate`. Encoder: `CalendarUpdatePayload` and `CalendarOrchestrator`.

| Key | Type | Description |
|-----|------|-------------|
| `day` | int | The day the entries are for: `0` today, `1` tomorrow, `-1` yesterday. A reply echoes the requested day. The watch uses it as the header ("Today", "Tomorrow", "Yesterday", "+2d"). The phone leaves it out at `0`, and the watch reads a missing `day` as `0`. |
| `entries` | array | At most 5 entries (`kMaxCalendarEntries`; 3 before firmware 2.4.91); the watch drops any beyond that. All-day events come first, then the rest by start time. The phone sends as many as fit one frame (244 payload bytes). |

**Entry:**

| Key | Type | Description |
|-----|------|-------------|
| `t` | text, ≤ 5 bytes | Local start time `"HH:mm"`, or `"All"` for an all-day event. |
| `n` | text, ≤ 47 bytes | Event title. |

Both keys are required: an entry without one of them is dropped. The watch's buffers are `char[6]` and `char[48]`. A string that does not fit is dropped, not truncated, and the field shows blank, so the phone cuts each string to the byte budget in UTF-8, on a character boundary (`WireText.fit`). Unknown keys are skipped.

---

## 0x14: CALENDAR_NAV

**Direction:** Watch → Phone

Sent by the HaCalendar app when it opens (today, from firmware 2.4.89) and on each swipe (left for the next day, right for the previous day). Encoder: `BridgeController::SendCalendarNav`.

| Key | Type | Description |
|-----|------|-------------|
| `day` | int | Day offset from today: `0` today, `1` tomorrow, `-1` yesterday, and so on (int8 on the watch). |

The phone fetches that local day's events from every configured calendar (`events.calendars` in the layout) and replies with `CALENDAR_UPDATE` (0x13). Past days are fetched too. For today, only events still ahead or under way are fetched. Until the reply arrives, the watch shows "Loading..." under the new day.

---

## 0x15: HOME_CONTROL_UPDATE

**Direction:** Phone → Watch

The devices of the HaHomeControl app, with their current states. The phone sends it on every connection, after a `WATCHFACE_LAYOUT` push, about 500 ms after a home-control command, and (app 2.1.100 and later) 1 s after one of its devices changes state in HA. The watch keeps the list in RAM and does not acknowledge it. Decoder: `BridgeController::HandleHomeControlUpdate`. Encoder: `HomeControlUpdatePayload` and `HomeControlOrchestrator`.

| Key | Type | Description |
|-----|------|-------------|
| `devices` | array | At most 4 devices (`kMaxHomeControlDevices`; 2 before firmware 2.4.91); the watch drops any beyond that, and the phone sends as many as fit one frame. They come from the layout's `homeControl.devices`. When that list is empty (app 2.1.100 and later, `HomeControlOrchestrator.appDevices`), they are the face's tiles on lights, switches, fans and `input_boolean`s, in face order, named as the tiles are. Covers, locks and alarms are left out of that default. |

**Device entry:**

| Key | Type | Description |
|-----|------|-------------|
| `l` | text, ≤ 19 bytes | Display name (the device's `name`, or its entity ID's object part). Required. |
| `e` | text, ≤ 63 bytes | HA entity ID. Required. The phone skips a device whose ID does not fit, because a cut ID would toggle nothing. |
| `s` | text, ≤ 15 bytes | Current state, for example `"on"`, `"open"` or `"unavailable"`. |
| `a` | bool | `true` when the state is an active one (`on`, `open`, `opening`, `playing`, `home`, `true`, `unlocked`). |
| `i`, `c` | text | Icon and colour. Not sent. Firmware before 2.4.91 read them into fields no screen showed; 2.4.91 skips them as unknown keys. |

The same string rule applies as for calendar entries.

**Home Control commands** (watch → phone): a tile tap sends `ACTION_INVOKE` (0x06) with `actionId=0xFFFC`, gesture `"tap"` and `entityId` set to the tile's entity. Before firmware 2.4.89 the entity ID was left out, so the phone ignored these taps. The phone calls the matching HA service (`toggle`; `lock`/`unlock` for locks; climate entities are ignored) and then sends a fresh `HOME_CONTROL_UPDATE`.

---

## 0x16: TIME_SYNC_ACK

**Direction:** Watch → Phone

Sent by the watch in reply to every valid `TIME_SYNC` (0x0F). From app 2.1.116, status 1 (clock already in sync) counts as success, so the app sends at most one TIME_SYNC per connection.

| Field | Type | Description |
|-------|------|-------------|
| `s` | uint | `0` applied; `1` already in sync (drift within 5 s and same zone) |
| `d` | int | The drift that was corrected, phone UTC minus watch UTC, in seconds; `0` when not applied |
| `t` | uint | The watch's UTC epoch after the sync |

---

## 0x17: HELLO_ACK

**Direction:** Phone → Watch

Sent by the phone in response to the watch's HELLO message. Carries the phone's supported protocol version so the watch can detect incompatibilities.

Available from firmware v1.131.0+ (requires `kFeatureProtocolNegotiation = 0x100` in peer features).

| Field | Type | Description |
|-------|------|-------------|
| `peerProto` | int | Phone's protocol major version (currently `1`) |
| `features` | int | Phone's supported feature bitmask (same bit definitions as HELLO `features`) |

If the watch receives a HELLO_ACK with `peerProto` different from its own `kProtocolMajor`, it sends `VERSION_MISMATCH` (0x18) and halts the handshake.

---

## 0x18: VERSION_MISMATCH

**Direction:** Watch → Phone

Sent by the watch when the phone's `HELLO_ACK.peerProto` is incompatible with `kProtocolMajor`. **Firmware 2.4.99 no longer sends this message.**

| Field | Type | Description |
|-------|------|-------------|
| `ourMajor` | int | Watch firmware's protocol major version |
| `peerMajor` | int | Phone's protocol major version (from HELLO_ACK) |

The phone should display an "update required" message when it receives this. The watch remains connected but will not process further messages until the user resolves the version mismatch.

---

## 0x19: CONFIG_PUSH_ACK

**Direction:** Watch → Phone

Sent by the watch after processing a `CONFIG_PUSH` (0x02) message. **Firmware 2.4.99 no longer sends this message.** Includes a CRC-32 of the raw CBOR payload so the phone can verify the config was received intact.

| Field | Type | Description |
|-------|------|-------------|
| `status` | int | Status code (see below) |
| `crc32` | uint32 | CRC-32 (ISO 3309 / zlib polynomial 0xEDB88320) of the raw CONFIG_PUSH CBOR bytes |
| `configVersion` | int | Echoed from the `configVersion` field of the applied CONFIG_PUSH |

**Status codes:**

| Value | Name | Description |
|-------|------|-------------|
| `0` | `OK` | Config applied successfully |
| `1` | `CRC_MISMATCH` | Computed CRC-32 does not match the raw payload |
| `2` | `OOM` | Out of memory — could not allocate storage for the new config |
| `3` | `INVALID` | Malformed CBOR or missing required fields |

The phone computes `CRC-32` over the raw CONFIG_PUSH payload bytes (before framing) and compares against `CONFIG_PUSH_ACK.crc32`. A mismatch triggers an automatic CONFIG_PUSH retry.

---

## 0x1A: CRASH_DUMP_REQUEST / 0x1B: CRASH_DUMP_ACK

**Direction:** CRASH_DUMP_REQUEST phone → watch (empty map), and the watch's reply with the same type byte, watch → phone. CRASH_DUMP_ACK phone → watch (empty map).

**Availability:** firmware 2.4.65 and later, feature bit 12 (`0x1000`, CRASH_REPORT). The app (2.1.74 and later) sends the request once per connection, right after the HELLO, and only to a watch that advertises the bit.

This is how the phone learns why the watch last reset. The reset reason and the crash context are also fields of the rich HELLO, but the rich HELLO never fits one frame (see [Which HELLO the watch sends](#0x01-hello)).

The watch keeps one crash context: the one its previous boot left in `.noinit` RAM. Any reset that keeps RAM leaves one (watchdog, soft reset, reset pin, and a crash); a power-on reset does not, and neither does a firmware update that moves `.noinit`. So a context does not by itself mean the watch crashed: read it with the reset reason. The breadcrumb says what the watch was doing, `IDLE` (0) for nothing in particular.

**Reply with a context** (7 fields):

| Field | Type | Description |
|-------|------|-------------|
| `resetReason` | string | The watch's reason for its last reset, from the nRF RESETREAS register (`"Watchdog"`, `"Soft reset"`, `"Reset pin"`, ...) |
| `slotIndex` | uint | Always 0 |
| `breadcrumb` | uint8 | Last processing phase: 0 IDLE, 1 BLE_RX, 2 FRAME_DECODE, 3 CBOR_DECODE, 4 CONFIG_APPLY, 5 SCREEN_TRANSITION, 6 WATCHFACE_APPLY, 7 SENSOR_REPORT, 8 BLE_TX, 9 CBOR_ENCODE |
| `lastMsgType` | uint8 | Last protocol message type being processed |
| `lastMsgId` | uint16 | Its message id |
| `freeHeap` | uint32 | Free heap at the last breadcrumb |
| `uptimeTicks` | uint32 | Uptime at the last breadcrumb, in ticks (1024 per second) |

**Reply without one:** `{"resetReason": "...", "empty": true}`.

**The crash record (2.4.90+).** When the previous boot ended badly, either form also carries `box`, the boot black box's record of that end. "Badly" means a HardFault, an SDK error, a lockup, or the watchdog firing with the button up (holding the button is how a user restarts the watch, so that end is not reported). This is the record the watch shows on its screen for a minute after such a boot. It is what says the watch crashed: a context alone does not.

| Key | Type | Description |
|-----|------|-------------|
| `k` | uint | 1 HardFault, 2 watchdog, 3 SDK error; 0 a lockup or watchdog reset that left no record |
| `c` | uint | Where it hit: 1 display task, 2 SystemTask, 3 BLE host, 4 BLE link layer, 5 timer daemon, 6 heart rate, 7 other task, 8 interrupt, 9 before the scheduler, 10 idle |
| `s` | uint | Furthest SystemTask start-up stage (12 = running) |
| `u` | uint32 | Uptime at the end, ticks (1024 per second) |
| `pc`, `lr` | uint32 | Stacked PC and LR (an SDK error: its pc argument, and the fault id) |
| `cf` | uint32 | CFSR (an SDK error: the error code) |
| `ad` | uint32 | BFAR or MMFAR when valid |
| `h` | uint32 | Lowest free heap of that boot, bytes |
| `cl` | array of 2 uint32 | The innermost return addresses above the fault frame |

The reply is about 183 B at most, in one frame.

When the reply carries a context or a record, the phone answers CRASH_DUMP_ACK and the watch stops offering both, in this reply and in the rich HELLO: later requests read `empty` until the watch resets again. Without the ACK (timeout, lost frame), the watch offers the same context on the next connection. HELLO_ACK `crashContextAck` clears the context too.

The app records the reset reason in its status (`lastResetReason`, Debug screen "Reset reason"). Where the report goes depends on how the boot ended:
- **A crash** goes to `lastCrashContext` (Debug screen "Crash context"), as one line such as `FAULT in display at 2.5s stage=12 pc=… lr=… cfsr=… addr=… minHeap=… calls=…`. A crash is the record from 2.4.90, or, before it, a context with reset reason `Watchdog` or `CPU Lock-up`.
- **Any other reset** (an update, a reboot from settings) puts the context in `lastRestartActivity` ("Last activity before restart"); it is not a crash. Until app 2.1.98 every context was shown as the crash context.

Built with `HA_CRASH_CONTEXT_EXTENDED` and `HA_CRASH_RING` (both off), the firmware serves a 4-slot ring over the same two messages, in the same format without `resetReason` and with the extended crash-context fields (ARM fault registers, task name and more) added.

---

## 0x1D: FIND_WATCH

**Direction:** Phone → Watch

| Field | Type | Description |
|-------|------|-------------|
| `duration` | uint | Alert length in seconds, capped at 60 by the watch (default 30) |

The watch turns its screen on (as a notification does, without holding it on), buzzes twice, then buzzes about once a second. It stops when the duration ends or when the screen times out, whichever comes first. The buzzes are driven by the HA face's 1 s tick, so they play only while the HA face is on display.

Firmware before 2.4.65 buzzed once at most: its millisecond clock read 0, so the alert never repeated or ended, and it did not turn the screen on. App versions before 2.1.74 never sent FIND_WATCH. The app sends it from the Debug screen ("Find watch") and on the HA event `infinassistant_command` with `command: find_watch`.

---

## 0x1E: NOTIFY_DISMISS

**Direction:** Watch → Phone. **Availability:** firmware 2.4.101 and later (feature bit 15, `0x8000`), app 2.1.121 and later.

| Field | Type | Description |
|-------|------|-------------|
| `nid` | uint | The `nid` of the phone notification the user dismissed |

- **When it is sent.** The user dismissed an entry that came with a `nid`, in the swipe-down list or in the preview. The list's dismiss hook finds the pair and the bridge sends the message from the display task (`SendFromUi`).
- **When it is not sent:**
  - Home Assistant notifications (no `nid`).
  - The watch's own removes: a replace by `nid` and a NOTIFY_REMOVE. The pair is dropped before the entry is removed, so the hook finds no `nid`.
  - The list pushing out its oldest when a sixth arrives.
  - Swiping away a stale copy still on screen after the entry was removed: the id is gone, so nothing happens.
- **No retry.** When the link is down the message is lost and the phone notification stays, as before this message existed.
- **The app.** It drops the message unless the watch's HELLO had bit 15. Otherwise it cancels the phone notification for that `nid` if it is still active and clearable; a non-clearable notification stays on the phone. The app sends nothing back.
- **History.** Firmware 2.4.91–2.4.94 sent 0x1E with the retired REQ-071 meaning (feature bit 10). That handler is gone from the app.

## 0x20: DASHBOARD_STATE

**Direction:** Phone → Watch. **Availability:** firmware 2.4.96 and later (feature bit 13, `0x2000`), app 2.1.107 and later.

The dashboard's live state: which doors, garage doors, motion sensors, lights and media players are open or on, the sensor readings, the weather and the next two events. The layout record keeps the glyph letters, the group order, and a fallback for the weather and the events.

Until 2.4.96 these values came only inside a `WATCHFACE_LAYOUT` push. The layout is pushed only from the card or the app, so the dashboard showed that push's states until the next one.

The watch keeps the last message in RAM, not in flash, and does not acknowledge it.
- **Decoding:** whole or not at all. A malformed or cut-short message leaves the previous state in place.
- **Out-of-range input:** unknown keys are skipped, lists longer than the caps below are cut, and numbers out of range are clamped.
- **Code:** decoder `ha_bridge/DashboardState.cpp`; encoder `DashboardStatePayload`, `DashboardStateBuilder` and `DashboardStateOrchestrator`.

**When the phone sends it:**
- **Full read** (HA's states, today's forecast and the upcoming events read again):
  - on every connection;
  - after a `WATCHFACE_LAYOUT` push the watch took;
  - after HA reconnects (app 2.1.108 and later: at most once a minute, so an HA that drops often cannot multiply the reads);
  - every 15 minutes;
  - soon after the first upcoming event leaves the watch (its start plus 10 minutes), so the next one takes its place.

  The periodic reads are checked on every message from the watch, which sends a sensor report every minute, as well as on a timer. A phone timer does not run while the phone sleeps, so on its own it could come late. A read that fails (HA does not answer) is tried again a minute later.
- **After a change:** 1.5 s after a dashboard entity changes state in HA, and at most one message per 3 s, so a burst of changes is one message. A state equal to the last one sent (apart from `t`) is not sent again.
- **Freezer alarm:** when a freezer has been warm for 20 minutes (see the sensor values below).

Nothing is sent while HA is disconnected, or to a watch without feature bit 13. The message always fits one frame: the phone keeps it to 220 bytes or less, by shortening the event titles and then dropping events.

| Key | Type | Description |
|-----|------|-------------|
| `f` | uint32 | Status fingerprint of the layout the bits were computed for (below). The watch draws the status rows only when its own layout gives the same value. |
| `t` | uint32 | When the state was read, in UTC epoch seconds. Once this is more than 30 minutes in the past, the watch greys the status rows, the temperatures and the presence count: the phone or HA has been out of reach for a while. |
| `g` | array of maps, ≤ 5 | Status groups, in layout order. `t` is the group type: 1 doors, 2 garage, 3 motion, 4 lights, 5 media, 0 other. `b` has a bit per glyph that is on or open (bit i is glyph i of the group, as the watch stored it). |
| `s` | array of maps, ≤ 3 | Sensor groups. `t` is the type: 1 thermostats, 2 freezers, 3 beacons. `v` holds up to 7 values, each an int or null (no reading). `a` holds alert bits, bit i for sensor i in alarm; it is omitted when 0. |
| `w` | map | Weather: `c` a weather code (below), `t` the temperature, and `h` and `l` today's high and low (omitted when the forecast is unknown). `w` itself is omitted when the weather entity has no condition or no temperature. |
| `e` | array of maps, ≤ 2 | The next events: `s` the start (UTC epoch seconds) and `n` the title, at most 24 bytes of UTF-8. The watch refuses a longer title and shows the event as "event". An event that began up to 10 minutes ago is still sent; the watch shows it as "now". |

**Status bits.** A glyph is on when its entity's HA state is one of: `on`, `open`, `opening`, `closing`, `home`, `playing`, `heat`, `cool`, `heat_cool`, `auto`, `unlocked`, `above_horizon`, `fan_only`, `dry`.

The bits count only the glyphs the watch kept: it drops a glyph whose symbol is not exactly one ASCII character, and it keeps at most 10 per group.

**Status fingerprint.** FNV-1a, 32-bit (offset `0x811C9DC5`, prime 16777619). For each status group in order, the hash takes the group's glyph count (one byte), then its glyph symbols (one byte each), counting only the glyphs the watch kept.
- **Golden vector:** groups `FSMDD`, `??`, `MDK`, `PKW` and `LHM?` give `0x19826696` (`status_fingerprint` in the [golden vectors](#golden-vectors)).
- **Pinned by:** the firmware host test, the app's `DashboardStateTest` and the Home Assistant `test_protocol_compat.py`, all reading the golden vectors.

**Sensor values.**
- **Thermostats and freezers:** a climate entity's `current_temperature`, otherwise the state as a number, rounded.
- **Beacons:** 1 when the state is `home` (or `on`, or a number above 0); 0 for any other known state, other zones included; null when the entity is `unavailable` or `unknown`.
- **Freezer alarm:** a freezer alarms once it has read above 15 °F for 20 minutes (above −9 °C when its `unit_of_measurement` is Celsius). The phone decides this, and it remembers when a freezer went warm across watch connections.

**Weather codes.** 0 none, 1 sunny (also `clear`), 2 clear-night, 3 partlycloudy, 4 cloudy, 5 rainy, 6 pouring, 7 snowy, 8 snowy-rainy, 9 hail, 10 lightning, 11 lightning-rainy, 12 windy, 13 windy-variant, 14 fog, 15 exceptional.

**Which dashboard.** The first dashboard among the screens the watch holds (the first 4). A second dashboard screen shows the same weather, temperatures and events, and no status rows, since its glyphs give another fingerprint.

Its events come from the dashboard's own calendar references. When it lists none, they come from every calendar of the face, read all at once (app 2.1.110 and later; before, from the first two, the layout parser's old default). `maxEvents` caps the events shown, not the calendars they come from.

From app 2.1.107 the stored layout keeps the dashboard's calendar references and every configured sensor. A layout stored by an older app has neither, until the next push from the card: it reads every calendar of the face, and it is missing the sensors that had no reading at that push (device-tracker beacons, for example).

**How the watch draws it (firmware 2.4.98, dashboard v8).** The rules:
- **Top line:** the date, the screen indicator, and the weather (icon, temperature, high/low).
- **Time:** across the whole width.
- **Events:** the next two, start and title.
  - **Starts:** an event later today, or started, shows a countdown ("now", "38m", "2.5h"); one on
    another day shows the weekday and time ("Wed 9:00").
  - **Titles** are cut with "..." at a fixed width.
- **Status:** a strip of up to ten coloured dots, one per glyph that is on, each with its letter.
  - **Order:** they pack left to right in type order: doors, garage, lights, media, motion.
  - **The first place answers "are the doors shut?":** a dim green check when every door and garage
    door is shut, otherwise the first door or garage dot. With ten dots due the check gives way.
  - **A calm house** shows only the check.
- **Freezer:** a red bar across the screen, with the warmest temperature (`!17°`) when a freezer is warm (`!-12°` is the same form). Otherwise it shows `1 freezer offline` or `N freezers offline` when freezer sensors have no reading. The bar is red in both cases. Firmware 2.4.99 and earlier showed `!--` for offline sensors.
- **Bottom:** the thermostats and the number at home (the count is hidden when no beacon has a
  reading), then the battery, steps and heart rate (`--` with no reading).
- **Without a state** (an app before 2.1.107, or before the first message): no dots, no check, no
  temperatures. The weather and the next event come from the layout record, greyed.
- **On the dashboard the "HA offline" banner does not show:** the stale grey says the same, and the
  banner would cover the bottom row. The other screens keep it.

**Legend.** The face has no labels: colour, and the place in the strip, say what a letter is. The
letters are the layout's own glyph symbols.

| Where | Colour | Means |
|-------|--------|-------|
| Strip, first place | green check | Every door and garage door is shut |
| Strip | orange | A door is open |
| Strip | red | A garage door is open (or opening, or closing) |
| Strip | yellow | A light is on |
| Strip | violet | A media player is playing (or on) |
| Strip | cyan | Motion |
| Red bar | `!17°` | A freezer has been above 15 °F for 20 minutes; the number is the warmest one |
| Red bar | `N freezers offline` (`1 freezer offline` for one) | Freezer sensors have no reading and none is warm (firmware 2.4.100; earlier firmware showed `!--`) |
| Dots, check, thermostats | grey | The state is over 30 minutes old: the phone or HA is out of reach |
| First event's start | cyan | When the next event starts ("12m", "1.3h", "now", "Wed 9:00") |

Before 2.4.98 (2.4.96 and 2.4.97, dashboard v7) the status letters sat in two rows, each type in a
fixed place, the presence and the freezer warning at the bottom right, and the next event in the top
row.

---

## 0x21: NOTIFY_REMOVE

**Direction:** Phone → Watch. **Availability:** firmware 2.4.101 and later (feature bit 15, `0x8000`), app 2.1.121 and later.

| Field | Type | Description |
|-------|------|-------------|
| `nid` | uint | The `nid` of the phone notification the phone removed (read, opened, cleared, or cancelled by its app) |

- **The watch.** It removes the entry if it is still there: no popup, no vibration, no message back, no ACK. A missing or out-of-range `nid`, one already dismissed on the watch, and one pushed out of the list all end the same way (`BridgeController::HandleNotifyRemove`). When the list is then empty, the watch face's new-notification mark is cleared too.
- **On screen.** An entry open on the Notifications screen stays shown until the user moves on; nothing dangles (`PhoneNotifySync.h`).
- **The app.** It sends the message only after this connection's HELLO had bit 15; after a HELLO without it the queued removes are dropped. While disconnected (or before the HELLO) it queues them, the newest 16 at most, and sends them on reconnect. It sends none for a notification the watch dismissed, since it is already gone there. A removal also drops that notification's un-ACKed pending forward.

---

## ACK Rules

- Each `NOTIFY` must be ACKed with `NOTIFY_ACK` when displayed.
- `NOTIFY_REMOVE` and `NOTIFY_DISMISS` are not ACKed.
- Each `ACTION_INVOKE` must be answered with `ACTION_RESULT` within 5 seconds (voice assist and tile taps only; home control is deprecated).
  - From app 2.1.116, the app sends only final results (success or error), not interim "queued" status.

## Dedupe by Tag

If `NOTIFY.tag` is present and dedupe is enabled (`capabilities.supportsNotifyDedupe`), the watch replaces an existing notification with the same tag.

From firmware 2.4.95 the watch does not read `tag` (it only echoes it in NOTIFY_ACK), so it does not dedupe by tag. From 2.4.101 a phone notification is replaced by its `nid` instead (see [0x08](#0x08-notify)).

## Typical Message Flow

```
Phone                          Watch
  |                              |
  |  -------- HELLO (0x01) ---> |  (phone identifies itself)
  |  <------- HELLO (0x01) ---  |  (watch sends diagnostics + features)
  |  -------- HELLO_ACK (0x17)> |  (phone confirms compatible version)
  |                              |  (watch sends VERSION_MISMATCH if incompatible)
  |  -------- CRASH_DUMP_REQ -> |  (feature bit 12 only)
  |  <------- CRASH_DUMP_REQ -- |  (reset reason + context or empty, + crash record "box" from 2.4.90)
  |  -------- CRASH_DUMP_ACK -> |  (only when a context or a record was delivered)
  |  -------- CONFIG_PUSH ----> |  (config version, HA integration config)
  |  <------- CONFIG_PUSH_ACK-  |  (echo CRC-32 of applied config, status)
  |  -------- STATE_UPDATE ---> |  (initial entity states)
  |  -------- TIME_SYNC ------> |  (sync clock)
  |  <------- TIME_SYNC_ACK --  |  (clock applied ok)
  |                              |
  |  ... steady state ...        |
  |                              |
  |  <------- SENSOR_REPORT --  |  (every 60s: steps, HR, battery)
  |  -------- STATE_UPDATE ---> |  (on a watched entity's state change)
  |  <------- ACTION_INVOKE --  |  (user tapped button/tile)
  |  -------- ACTION_RESULT --> |  (success/failure)
  |  <------- PING -----------  |  (keepalive)
  |  -------- PONG -----------> |
  |                              |
  |  -------- CALENDAR_UPDATE -> |  (on connect: today's entries)
  |  -------- HOME_CONTROL_UPD> |  (on connect: device states)
  |  -------- LOGBOOK_UPDATE -> |  (on connect: the logbook app's entries)
  |  -------- DASHBOARD_STATE > |  (on connect, feature bit 13: the dashboard's live state)
  |                              |
  |  -------- WATCHFACE_LAYOUT> |  (when user pushes from HA card)
  |  <------- WATCHFACE_ACK ---  |  (layout applied ok)
  |  -------- CALENDAR_UPDATE -> |  (today's entries again)
  |  -------- HOME_CONTROL_UPD> |  (device states again)
  |                              |
  |  ... a logbook entity changes |
  |  -------- LOGBOOK_UPDATE -> |  (2 s later: the app's newest entries)
  |                              |
  |  ... a dashboard entity changes
  |  -------- DASHBOARD_STATE > |  (1.5 s later, at most one per 3 s; and every 15 min)
  |                              |
  |  ... HaCalendar app opened . |
  |  <------- CALENDAR_NAV ----  |  (day=0, requesting today; 2.4.89+)
  |  -------- CALENDAR_UPDATE -> |  (entries, day omitted = 0)
  |  <------- CALENDAR_NAV ----  |  (day=1, user swiped left)
  |  -------- CALENDAR_UPDATE -> |  (day=1 + entries)
  |                              |
  |  ... HaHomeControl app ...   |
  |  <------- ACTION_INVOKE ---  |  (actionId=0xFFFC, entityId=light.x; 2.4.89+)
  |  -------- HOME_CONTROL_UPD> |  (updated states ~500 ms after the toggle)
```

## Privacy: the persisted layout record

Firmware 2.4.62 stores the last accepted layout in the watch flash file system as `/ha_layout.rec`, so that screens which are not on display do not have to be held in RAM. The record holds event titles and times, tile labels, entity ids, and weather and sensor values. It **persists in flash** across power cycles.

InfiniTime's file-transfer BLE service lets a connected BLE peer **read** that file as well as delete it (`src/components/ble/FSService.cpp`; the service is path-based and has no per-file access control). Whether to restrict that service is a decision for the owner of the device; this repository does not change it.

What will **erase** the record:

- another layout push, which replaces it;
- a BLE file-service delete of `/ha_layout.rec` by any connected peer;
- a file-system reformat, which InfiniTime performs when the mount fails.

What does **not** erase it:

- unpairing the phone (bond data lives in `/bond.dat`, and no code path removes the layout record);
- a power cycle;
- an app DFU (the DFU image is written to the OTA region, outside the file-system region; not verified in `DfuService`, confidence 80%).

Firmware 2.4.61 has no reader for the file and ignores it. A later roll-forward validates the header, type hash and CRCs before using it, and a corrupt or unrecognised record is discarded as "no layout".

**Record format 2 (firmware 2.4.100).** The header is 84 bytes: flags bit 0 is the dashboard, bits 1-6 are the tile screens, and there are 7 section slots. Format 1 (72-byte header, firmware 2.4.99 and earlier) is still read, so a layout survives the update. Going back to 2.4.99 shows the default face until the layout is pushed again.

## Forward Compatibility

- HELLO includes `capabilities` map and `features` bitmask.
- Unknown CBOR keys are ignored on Android.
- `WATCHFACE_LAYOUT` short keys are gated by feature bit 11; the long spelling stays accepted for at least one release.
- Features bitmask allows capability-gating: phone checks `peerSupports(flag)` before sending feature-specific messages. Old firmware (features=0) is treated as "capabilities unknown" and receives all messages.

## Golden vectors

`infinassistant/vectors/protocol-vectors.json` is the single source for the wire bytes and numbers that more than one side pins: the HELLO feature word (`0x1FBFF`, and the `0xFBFF` of firmware before 2.4.103), the full watch HELLO of 2.4.103, the CONFIG_PUSH with the `infinassistant` key, the status fingerprint, NOTIFY_REMOVE / NOTIFY_DISMISS / NOTIFY with a `nid`, the PONG `enc` pair, and a CRC-32. A `hex` field is a CBOR payload as its bytes on the wire (space-separated); a `value` is a plain number.

It is read by the firmware host tests (through `golden_vectors.h`), the app's tests (`GoldenVectors` in `core-data`) and the Home Assistant `tests/test_protocol_compat.py`. A change to a vector fails each side that no longer agrees; a vector removed from the file fails the test that asks for it. The directory is outside `src/`, so the firmware sync never copies it into the watch tree.
