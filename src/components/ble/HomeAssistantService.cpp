#include "components/ble/HomeAssistantService.h"
#include <task.h>
#include <timers.h>
#include <lvgl/lvgl.h>   // lv_disp_get_inactive_time: the display's time since the last touch or press
#include <host/ble_gatt.h>
#include <nrf_log.h>
#include <cstring>
#include "drivers/InternalFlash.h"
#include "systemtask/SystemTask.h"
#include "components/ble/NotificationManager.h"
#include "components/settings/Settings.h"

using namespace Pinetime::Controllers;

// Static member definitions — these live in BSS (zero-initialized at startup),
// NOT on the main() stack where SystemTask/NimbleController/HomeAssistantService reside.
// Moving ~4400 bytes from main stack to BSS prevents stack overflow.
HomeAssistantService::Entity HomeAssistantService::entities_[HomeAssistantService::kMaxEntities];
std::array<HomeAssistantService::LogbookEntry, HomeAssistantService::kMaxLogbookEntries> HomeAssistantService::logbookEntries_{};
std::array<HomeAssistantService::CalendarAppEntry, HomeAssistantService::kMaxCalendarEntries> HomeAssistantService::calendarEntries_{};
Pinetime::Applications::HaBridge::DashLive HomeAssistantService::dashLive_{};
std::array<HomeAssistantService::HomeControlDevice, HomeAssistantService::kMaxHomeControlDevices> HomeAssistantService::homeControlDevices_{};

namespace {
  int HomeAssistantCallback(uint16_t conn_handle, uint16_t /*attr_handle*/,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
    auto* svc = static_cast<HomeAssistantService*>(arg);
    // Capture the connection handle from every write.
    //
    // This is the ONLY place the handle is available: nothing in NimbleController
    // passes it to this service. SendNotification() returns early when connHandle
    // is BLE_HS_CONN_HANDLE_NONE, so without this the watch accepts every write
    // and can never answer one — bonded and connected, with PING, HELLO and every
    // ACK silently unanswered.
    //
    // Losing this line is what killed the bridge in 2.4.57. SetConnectionHandle
    // was then referenced by nothing, so --gc-sections dropped it from the image
    // entirely, which is how the regression was finally identified: the symbol is
    // present in 2.4.50 through 2.4.55 and absent from 2.4.57 on.
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
      svc->SetConnectionHandle(conn_handle);
    }
    return svc->OnCommand(ctxt);
  }
}

HomeAssistantService::HomeAssistantService()
  : characteristicDefinition {{.uuid = &haTxCharUuid.u,
                               .access_cb = HomeAssistantCallback,
                               .arg = this,
                               .flags = BLE_GATT_CHR_F_NOTIFY,
                               .val_handle = &txHandle},
                              {.uuid = &haRxCharUuid.u,
                               .access_cb = HomeAssistantCallback,
                               .arg = this,
                               .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                               .val_handle = nullptr},
                              {0}},
    serviceDefinition {
      {.type = BLE_GATT_SVC_TYPE_PRIMARY,
       .uuid = &haServiceUuid.u,
       .characteristics = characteristicDefinition},
      {0},
    } {
  // Mutex created in Init() after FreeRTOS starts
}

void HomeAssistantService::Init() {
  // MCUboot confirmation is NOT done here any more. Auto-validating on every
  // boot meant an image that crash-looped after this point could never be
  // reverted — the bootloader will not roll back an image that has set the
  // valid bit (bootloader/README.md), which is exactly how 2.4.41/2.4.42 left
  // the watch with no way home. The swap-corruption case this was protecting
  // against is now handled precisely, by confirming when a DFU starts writing
  // the secondary slot rather than on every boot unconditionally.
  // See ha_bridge/BootConfirm.h.

  // Create mutex after FreeRTOS is running
  dataMutex_ = xSemaphoreCreateMutex();

  // Entities now live in static BSS array — no heap allocation needed.
  // Zero-initialized by C++ static init (BSS section).

  // Register BLE GATT service. Use graceful error handling instead of ASSERT
  // to prevent hard crash on ENOMEM during boot (boot failure fix v1.172).
  int res = ble_gatts_count_cfg(serviceDefinition);
  if (res != 0) {
    NRF_LOG_ERROR("HA GATT count_cfg failed: %d", res);
    return;
  }

  res = ble_gatts_add_svcs(serviceDefinition);
  if (res != 0) {
    NRF_LOG_ERROR("HA GATT add_svcs failed: %d", res);
    return;
  }
}

void HomeAssistantService::SetConnectionHandle(uint16_t conn) {
  connHandle = conn;
}

uint32_t HomeAssistantService::SecondsSinceLastRx() const {
  return (xTaskGetTickCount() - lastRxTick_.load(std::memory_order_relaxed)) / configTICK_RATE_HZ;
}

int HomeAssistantService::OnCommand(struct ble_gatt_access_ctxt* ctxt) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    // Any write is contact: the stale banner counts from the last one.
    lastRxTick_.store(xTaskGetTickCount(), std::memory_order_relaxed);
    // Get data from mbuf
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 0 && onDataReceived_) {
      // Use stack buffer (not static) to avoid race conditions between concurrent BLE writes.
      // 256 bytes suffices: BLE writes are at most MTU-3 bytes (244 for MTU 247).
      // Reduced from 512 to save 256 bytes of BLE task stack — critical for
      // HandleWatchFaceLayout CBOR decode depth (OnCommand → FrameCodec → decode).
      uint8_t buf[256];
      // REQ-010 — REJECT oversize writes instead of silently truncating. A
      // truncated frame can turn legitimate protocol errors (oversize, attacker
      // payloads) into bizarre layout state downstream. Counter exposed via
      // GetTruncatedWriteFailures() and HELLO so the phone observes mismatch.
      if (len > sizeof(buf)) {
        truncatedWriteFailures_.fetch_add(1, std::memory_order_relaxed);
        return 0;  // Do NOT invoke onDataReceived_ with truncated data.
      }
      os_mbuf_copydata(ctxt->om, 0, len, buf);
      onDataReceived_(buf, len);
    }
  }
  return 0;
}

void HomeAssistantService::InvokeAction(uint16_t actionId, const char* gesture) {
  if (onActionInvoked_) {
    onActionInvoked_(actionId, gesture);
  }
}

void HomeAssistantService::SetPendingTileEntity(const char* entityId) {
  std::strncpy(pendingTileEntity_.data(), entityId, pendingTileEntity_.size() - 1);
  pendingTileEntity_[pendingTileEntity_.size() - 1] = '\0';
}

// --- Mutex-protected entity accessors ---
// BLE callbacks (UpdateEntity) and the display task (GetEntityState) can access entities
// concurrently. The dataMutex_ prevents torn reads/writes.

uint32_t HomeAssistantService::EntityKey(const char* entityId) {
  uint32_t key = 2166136261u;
  for (const char* c = entityId; *c != '\0'; ++c) {
    key = (key ^ static_cast<uint8_t>(*c)) * 16777619u;
  }
  return key;
}

uint8_t HomeAssistantService::GetEntityCount() const {
  if (!dataMutex_) return 0;

  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    uint8_t count = entityCount_;
    xSemaphoreGive(dataMutex_);
    return count;
  }
  return 0;
}

HomeAssistantService::Entity HomeAssistantService::GetEntity(uint8_t index) const {
  if (!dataMutex_) return Entity{};

  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    Entity copy{};
    if (index < entityCount_) {
      copy = entities_[index];
    }
    xSemaphoreGive(dataMutex_);
    return copy;
  }
  return Entity{};
}

bool HomeAssistantService::GetEntityState(const char* entityId, char* out, size_t outLen) const {
  if (out == nullptr || outLen == 0) return false;
  out[0] = '\0';
  if (!dataMutex_ || entityId == nullptr || entityId[0] == '\0') return false;
  const uint32_t key = EntityKey(entityId);

  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool found = false;
  for (uint8_t i = 0; i < entityCount_; i++) {
    if (entities_[i].key == key) {
      std::strncpy(out, entities_[i].state.data(), outLen - 1);
      out[outLen - 1] = '\0';
      found = true;
      break;
    }
  }
  xSemaphoreGive(dataMutex_);
  return found;
}

bool HomeAssistantService::UpdateEntity(const char* entityId, const char* state) {
  if (!dataMutex_) return false;
  const uint32_t key = EntityKey(entityId);

  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (entitiesResetPending_.exchange(false, std::memory_order_acq_rel)) {
      entityCount_ = 0;
    }
    Entity* entry = nullptr;
    for (uint8_t i = 0; i < entityCount_; i++) {
      if (entities_[i].key == key) {
        entry = &entities_[i];
        break;
      }
    }
    // Not found — add new entry if space available.
    if (entry == nullptr && entityCount_ < kMaxEntities) {
      entry = &entities_[entityCount_++];
      entry->key = key;
    }
    if (entry != nullptr) {
      std::strncpy(entry->state.data(), state, entry->state.size() - 1);
      entry->state[entry->state.size() - 1] = '\0';
    }
    xSemaphoreGive(dataMutex_);
    return true;
  }
  return false;
}

bool HomeAssistantService::SendNotify(const uint8_t* data, size_t len) {
  // Connection check
  if (connHandle == BLE_HS_CONN_HANDLE_NONE) {
    return false;
  }

  // Allocate mbuf from NimBLE pool — this IS the flow control.
  // If pool is exhausted, allocation fails gracefully (returns null).
  // Previous pendingNotifications_ counter was broken: increment-only with
  // periodic timer reset, causing ACTION_INVOKE messages to be silently
  // dropped after any burst of 5+ notifications within a single tick.
  struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
  if (!om) {
    mbufFailures_++;
    return false;
  }

  // Send notification (mbuf ownership transferred to BLE stack)
  int rc = ble_gattc_notify_custom(connHandle, txHandle, om);
  if (rc != 0) {
    // Counted because the return value was previously discarded, and that made a
    // real failure invisible: WATCHFACE_ACK was lost on every layout apply while
    // mbufFailures_ stayed 0, so there was nothing to distinguish "never sent"
    // from "sent and lost". HandleWatchFaceLayout runs inside the NimBLE GATT
    // write callback (HA_BLE_RX_MAILBOX is 0, so there is no bridge task), and a
    // notify re-entered from that callback after a long decode is what fails.
    if (notifyFailures_ < 255) {
      notifyFailures_++;
    }
    lastNotifyError_ = static_cast<int16_t>(rc);
    return false;
  }
  return true;
}

void HomeAssistantService::PublishLayoutCommitted() {
  // The generation stays even: no reader sees a write window, because no resident
  // copy of the layout exists. Adding 2 gives every
  // committed layout its own generation. The update signal is raised LAST, after
  // the generation, so a display task that takes the signal reads the new value.
  layoutGen_.fetch_add(2, std::memory_order_release);
  hasUpdate_.store(true, std::memory_order_release);
}

uint16_t HomeAssistantService::GetCurrentMtu() const {
  // CRITICAL: Check connection status before querying MTU
  // BLE stack may return invalid value if not connected
  if (connHandle == BLE_HS_CONN_HANDLE_NONE) {
    return 0;  // Not connected
  }

  // Query current MTU from BLE stack
  // WHY: MTU can change after connection negotiation
  uint16_t mtu = ble_att_mtu(connHandle);

  // Fallback to default (247) if query fails or returns 0
  // This handles edge cases where stack doesn't return valid value
  return mtu > 0 ? mtu : 247;
}

namespace {
  // Copies at most `room` bytes of `text`, ending on a whole UTF-8 character. Returns the bytes copied.
  size_t CopyUtf8(char* out, size_t room, const char* text) {
    size_t used = 0;
    while (text[used] != '\0') {
      size_t len = 1;
      while ((static_cast<uint8_t>(text[used + len]) & 0xC0) == 0x80) len++;
      if (used + len > room) break;
      used += len;
    }
    std::memcpy(out, text, used);
    return used;
  }
}

bool HomeAssistantService::IsLinkEncrypted() const {
  if (connHandle == BLE_HS_CONN_HANDLE_NONE) return false;
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(connHandle, &desc) != 0) return false;
  return desc.sec_state.encrypted != 0;
}

namespace {
  // Milliseconds modulo 2^32 on the RTOS clock; LVGL runs on the same ticks (lv_conf.h, LV_TICK_CUSTOM).
  uint32_t TickMs() {
    return Pinetime::Applications::HaBridge::WakeTicksToMs(static_cast<uint32_t>(xTaskGetTickCount()), configTICK_RATE_HZ);
  }
}

void HomeAssistantService::PushNotification(const char* title, const char* body, uint16_t nid) {
  if (notificationManager_ == nullptr || systemTask_ == nullptr) return;
  NotificationManager::Notification notif;
  // "title\0body\0", as the Alert Notification Service builds a phone's alert: the
  // Notifications screen shows what comes before the first '\0' as the title.
  constexpr size_t kChars = NotificationManager::MaximumMessageSize();  // the message holds one more, its end
  char* msg = notif.message.data();
  size_t used = CopyUtf8(msg, kChars - 2, title[0] != '\0' ? title : "Home Assistant");
  msg[used++] = '\0';
  used += CopyUtf8(msg + used, kChars - used, body);
  msg[used] = '\0';
  notif.size = static_cast<uint8_t>(used + 1);
  notif.category = NotificationManager::Categories::SimpleAlert;
  notifySync_.Push(std::move(notif), nid);

  // Burst handling (2.4.104). SystemTask acts on the announcement only when the notification status
  // is On; under Off or Sleep it did nothing before and still does nothing, so those pass through.
  if (systemTask_->GetSettings().GetNotificationStatus() == Pinetime::Controllers::Settings::Notification::On) {
    using namespace Pinetime::Applications::HaBridge;
    const bool screenOff = systemTask_->IsSleeping();
    const uint32_t now = TickMs();
    NotifyVerdict verdict;
    taskENTER_CRITICAL();
    verdict = notifyGate_.Decide(now, screenOff);
    if (verdict.wake) {
      sleepWatch_.Arm(now);
    }
    // The counters are counted in the same critical section the HELLO reads them in, so a read sees
    // the count and the gate's decision together.
    if (!verdict.announce) {
      g_wakeTelemetry.CountNotifyStored();
    } else if (verdict.wake) {
      g_wakeTelemetry.CountNotifyWake();
    }
    taskEXIT_CRITICAL();
    if (!verdict.announce) {
      // Stored in the list (the Push above) and nothing more: the screen stays off and quiet.
      return;
    }
    if (verdict.wake) {
      if (notifyWakeTimer_ != nullptr) {
        xTimerStart(notifyWakeTimer_, 0);
      }
    }
  }
  systemTask_->PushMessage(Pinetime::System::Messages::OnNewNotification);
}

// Runs on the timer daemon, twice a second while a notification wake is being watched. Reads the
// display's idle time without the LVGL lock: it is one 32-bit read of a value only the display task
// writes, and a stale answer makes the watch stand down, which is the safe side.
void HomeAssistantService::PollNotifyWake() {
  using namespace Pinetime::Applications::HaBridge;
  if (systemTask_ == nullptr) return;
  const uint32_t now = TickMs();
  const bool screenOff = systemTask_->IsSleeping();
  const uint32_t idleMs = WakeTicksToMs(lv_disp_get_inactive_time(nullptr), configTICK_RATE_HZ);
  const bool sleepDisabled = systemTask_->IsSleepDisabled();
  NotifyWakeSleepWatch::Verdict verdict;
  taskENTER_CRITICAL();
  verdict = sleepWatch_.Poll(now, screenOff, idleMs, sleepDisabled);
  taskEXIT_CRITICAL();
  if (verdict == NotifyWakeSleepWatch::Verdict::Keep) {
    xTimerStart(notifyWakeTimer_, 0);
  } else if (verdict == NotifyWakeSleepWatch::Verdict::Sleep) {
    // SystemTask::GoToSleep does nothing while a wake lock is held, so a lock taken since the poll wins.
    systemTask_->PushMessage(Pinetime::System::Messages::GoToSleep);
  }
}

void HomeAssistantService::NotifyWakeTimerCb(TimerHandle_t timer) {
  static_cast<HomeAssistantService*>(pvTimerGetTimerID(timer))->PollNotifyWake();
}

// The HELLO's settings word carries the stored brightness as its enum value (WakeTelemetry.h, bits
// 8-10); the B setting is code 5 there.
static_assert(static_cast<uint8_t>(Pinetime::Controllers::BrightnessController::Levels::High) == 4 &&
                static_cast<uint8_t>(Pinetime::Controllers::BrightnessController::Levels::Button) == 5,
              "the HELLO brightness codes are the BrightnessController::Levels values");

Pinetime::Applications::HaBridge::WakeDiag HomeAssistantService::ReadWakeDiag() const {
  using namespace Pinetime::Applications::HaBridge;
  WakeDiag d;
  taskENTER_CRITICAL();
  d.counters = g_wakeTelemetry.Read(TickMs());
  taskEXIT_CRITICAL();
  if (systemTask_ != nullptr) {
    const Pinetime::Controllers::Settings& settings = systemTask_->GetSettings();
    WakeSettings s;
    s.wakeModes = static_cast<uint8_t>(settings.getWakeUpModes().to_ulong());
    s.alwaysOn = settings.GetAlwaysOnDisplaySetting();
    s.notifyStatus = static_cast<uint8_t>(settings.GetNotificationStatus());
    s.brightness = static_cast<uint8_t>(settings.GetBrightness());
    s.timeoutMs = settings.GetScreenTimeOut();
    d.settings = PackWakeSettings(s);
  }
  return d;
}

void HomeAssistantService::OnNotifyDismissed(void* self, uint16_t nid) {
  HomeAssistantService* service = static_cast<HomeAssistantService*>(self);
  if (service->onNotifyDismissed_) {
    service->onNotifyDismissed_(nid);
  }
}

void HomeAssistantService::SetLogbookEntries(const LogbookEntry* entries, uint8_t count) {
  // REQ-017 — mutex-protect logbook writer.
  if (!dataMutex_) return;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
  uint8_t n = (count > kMaxLogbookEntries) ? kMaxLogbookEntries : count;
  for (uint8_t i = 0; i < n; i++) {
    logbookEntries_[i] = entries[i];
  }
  logbookCount_ = n;
  logbookGeneration_++;
  xSemaphoreGive(dataMutex_);
}

uint8_t HomeAssistantService::GetLogbookGeneration() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t v = logbookGeneration_;
  xSemaphoreGive(dataMutex_);
  return v;
}

uint8_t HomeAssistantService::GetLogbookCount() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t n = logbookCount_;
  xSemaphoreGive(dataMutex_);
  return n;
}

const HomeAssistantService::LogbookEntry* HomeAssistantService::GetLogbookEntries() const {
  // REQ-017 — brief lock around the snapshot. As with notifications/calendar/
  // home-control, immutability of the returned pointer is REQ-009 territory.
  if (!dataMutex_) return logbookEntries_.data();
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    xSemaphoreGive(dataMutex_);
  }
  return logbookEntries_.data();
}

void HomeAssistantService::SetCalendarEntries(const CalendarAppEntry* entries, uint8_t count, int8_t dayOffset) {
  // REQ-017 — mutex-protect calendar writer.
  if (!dataMutex_) return;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
  uint8_t n = (count > kMaxCalendarEntries) ? kMaxCalendarEntries : count;
  for (uint8_t i = 0; i < n; i++) calendarEntries_[i] = entries[i];
  calendarCount_ = n;
  calendarDayOffset_ = dayOffset;
  calendarNavPending_ = false;
  calendarGeneration_++;
  xSemaphoreGive(dataMutex_);
}

uint8_t HomeAssistantService::GetCalendarCount() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t n = calendarCount_;
  xSemaphoreGive(dataMutex_);
  return n;
}

const HomeAssistantService::CalendarAppEntry* HomeAssistantService::GetCalendarEntries() const {
  // REQ-017 — see note in GetLogbookEntries.
  if (!dataMutex_) return calendarEntries_.data();
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    xSemaphoreGive(dataMutex_);
  }
  return calendarEntries_.data();
}

int8_t HomeAssistantService::GetCalendarDayOffset() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  int8_t v = calendarDayOffset_;
  xSemaphoreGive(dataMutex_);
  return v;
}

void HomeAssistantService::SetCalendarDayOffset(int8_t offset) {
  if (!dataMutex_) return;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return;
  // The entries held belong to the old day: none are shown under the new day's header until
  // the phone's arrive.
  if (offset != calendarDayOffset_) {
    calendarCount_ = 0;
  }
  calendarDayOffset_ = offset;
  calendarNavPending_ = true;
  calendarGeneration_++;
  xSemaphoreGive(dataMutex_);
}

uint8_t HomeAssistantService::GetCalendarGeneration() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t v = calendarGeneration_;
  xSemaphoreGive(dataMutex_);
  return v;
}

void HomeAssistantService::PublishDashLive(const Pinetime::Applications::HaBridge::DashLive& state) {
  // Stamped before the generation leaves 0, so SecondsSinceDashState never reads a state
  // without its time.
  dashStateTick_.store(xTaskGetTickCount(), std::memory_order_relaxed);
  // Odd while writing, even (and never 0 again) once the copy is in: a reader that saw the
  // same even value before and after its copy read a whole state. No lock: the display task
  // never waits on the GATT callback, and the GATT callback never waits on it.
  dashLiveGen_.fetch_add(1, std::memory_order_acq_rel);
  std::atomic_thread_fence(std::memory_order_release);
  dashLive_ = state;
  std::atomic_thread_fence(std::memory_order_release);
  dashLiveGen_.fetch_add(1, std::memory_order_release);
  dashStateOk_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t HomeAssistantService::SecondsSinceDashState() const {
  if (dashLiveGen_.load(std::memory_order_acquire) == 0) return 0;
  return (xTaskGetTickCount() - dashStateTick_.load(std::memory_order_relaxed)) / configTICK_RATE_HZ;
}

bool HomeAssistantService::ReadDashLive(Pinetime::Applications::HaBridge::DashLive& out, uint32_t* generation) const {
  for (uint8_t attempt = 0; attempt < 4; attempt++) {
    const uint32_t before = dashLiveGen_.load(std::memory_order_acquire);
    if (before == 0) return false;
    if ((before & 1u) != 0) continue;
    out = dashLive_;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (dashLiveGen_.load(std::memory_order_acquire) == before) {
      if (generation != nullptr) *generation = before;
      return true;
    }
  }
  return false;
}

bool HomeAssistantService::IsCalendarNavPending() const {
  if (!dataMutex_) return false;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool v = calendarNavPending_;
  xSemaphoreGive(dataMutex_);
  return v;
}

void HomeAssistantService::SetHomeControlDevices(const HomeControlDevice* devices, uint8_t count) {
  // REQ-017 — mutex-protect home-control writer.
  if (!dataMutex_) return;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
  uint8_t n = (count > kMaxHomeControlDevices) ? kMaxHomeControlDevices : count;
  for (uint8_t i = 0; i < n; i++) homeControlDevices_[i] = devices[i];
  homeControlCount_ = n;
  homeControlGeneration_++;
  xSemaphoreGive(dataMutex_);
}

uint8_t HomeAssistantService::GetHomeControlGeneration() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t v = homeControlGeneration_;
  xSemaphoreGive(dataMutex_);
  return v;
}

uint8_t HomeAssistantService::GetHomeControlCount() const {
  if (!dataMutex_) return 0;
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint8_t n = homeControlCount_;
  xSemaphoreGive(dataMutex_);
  return n;
}

const HomeAssistantService::HomeControlDevice* HomeAssistantService::GetHomeControlDevices() const {
  // REQ-017 — see note in GetLogbookEntries.
  if (!dataMutex_) return homeControlDevices_.data();
  if (xSemaphoreTake(dataMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    xSemaphoreGive(dataMutex_);
  }
  return homeControlDevices_.data();
}

void HomeAssistantService::RequestWake() {
  if (systemTask_ == nullptr) return;
  systemTask_->PushMessage(Pinetime::System::Messages::DisableSleeping);
}

void HomeAssistantService::ReleaseWake() {
  if (systemTask_ == nullptr) return;
  systemTask_->PushMessage(Pinetime::System::Messages::EnableSleeping);
}

void HomeAssistantService::WakeScreen() {
  if (systemTask_ == nullptr) return;
  systemTask_->PushMessage(Pinetime::System::Messages::GoToRunning);
}

void HomeAssistantService::SuspendEarlySleep(uint32_t forMs) {
  const uint32_t now = TickMs();
  taskENTER_CRITICAL();
  sleepWatch_.Suspend(now, forMs);
  taskEXIT_CRITICAL();
  // The poll runs until the suspension has ended, which is when it clears it (WakeTelemetry.h).
  if (forMs > 0 && notifyWakeTimer_ != nullptr) {
    xTimerStart(notifyWakeTimer_, 0);
  }
}

bool HomeAssistantService::SystemAwake() const {
  return systemTask_ == nullptr || !systemTask_->IsSleeping();
}

void HomeAssistantService::BindSystemTask(Pinetime::System::SystemTask& systemTask) {
  systemTask_ = &systemTask;
  // The poll of the early sleep after a notification wake (PushNotification starts it). A one-shot
  // timer, so it costs nothing while no notification wake is being watched.
  if (notifyWakeTimer_ == nullptr) {
    notifyWakeTimer_ = xTimerCreate("hnw", pdMS_TO_TICKS(Pinetime::Applications::HaBridge::NotifyWakeSleepWatch::kPollMs),
                                    pdFALSE, this, &HomeAssistantService::NotifyWakeTimerCb);
  }
}
