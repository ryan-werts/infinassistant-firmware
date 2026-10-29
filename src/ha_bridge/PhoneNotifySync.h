#pragma once

#include <cstdint>

#include "components/ble/NotificationManager.h"

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * Ties the phone's notifications to InfiniTime's notification list (2.4.101, the NOTIFY
       * `nid`): which list entry is which phone notification, so that
       *  - a NOTIFY with a nid the list already holds replaces that entry,
       *  - NOTIFY_REMOVE takes an entry out without a sound, and
       *  - the user dismissing a phone notification on the watch is told to the phone.
       *
       * The table is at most one pair {list id, nid} per list entry: 20 bytes. A pair is dropped
       * before the list removes its entry for the watch's own reasons, so the dismiss hook finds
       * no nid and sends nothing; only a dismiss by the user finds the pair and reports it.
       *
       * Two tasks use it. The NimBLE host task calls Push and Remove; the display task reaches
       * OnManagerDismiss through the list's dismiss hook. The table is guarded by a short critical
       * section that is never held while calling the list's Dismiss or the sink, so the sink may
       * send over BLE.
       *
       * A pair must not outlive its entry, or the list's 8-bit id, handed out again after 256
       * pushes, would match an old nid. The list says nothing when a push pushes out its oldest
       * entry, and the Alert Notification Service pushes without coming through here, so the list's
       * push hook (OnManagerPush) drops the pairs of entries that are gone after every push.
       *
       * On the display: the Notifications screen holds a copy of the text it shows, not a
       * reference into the list. A Remove or a replace of the entry on screen leaves that copy
       * there until the user swipes it away, moves to another entry or leaves the screen; nothing
       * dangles. The screen's next lookup of the id finds nothing and falls back to the newest
       * entry or the empty page, and swiping the stale copy away is a Dismiss of an id that is
       * gone: nothing happens and no event goes to the phone.
       */
      class PhoneNotifySync {
      public:
        using Manager = Pinetime::Controllers::NotificationManager;
        using ListId = Manager::Notification::Id;
        // Called on the display task when the user dismissed the entry of `nid`.
        using DismissSink = void (*)(void* context, uint16_t nid);

        // Hooks the list's Dismiss and Push. Called once, before the first Push.
        void Bind(Manager& manager, DismissSink sink, void* context);

        // Pushes the notification. A nid (not 0) the list already holds first removes that entry,
        // and the new one is then recorded under the nid. nid 0 is a notification with no phone
        // identity (Home Assistant's): it is pushed as before.
        void Push(Manager::Notification&& notif, uint16_t nid);

        // Removes the entry of `nid` with no event to the phone. False when there is none (never
        // pushed, already dismissed, pushed out of the list, or already removed).
        bool Remove(uint16_t nid);

        // Pairs held now.
        uint8_t Count() const;

      private:
        // Id and nid in 4 bytes. A pair with nid 0 is free.
        struct Pair {
          ListId id {0};
          uint16_t nid {0};
        };
        // One per list entry: Manager::TotalNbNotifications, which the manager keeps private.
        static constexpr uint8_t kMaxPairs = 5;

        static void OnManagerDismiss(void* self, ListId id);
        static void OnManagerPush(void* self);

        bool IsLive(ListId id) const;
        // Both need the critical section held by the caller.
        void DropDeadPairs();
        bool TakeLocked(uint16_t nid, ListId& id);

        Manager* manager_ {nullptr};
        DismissSink sink_ {nullptr};
        void* sinkContext_ {nullptr};
        Pair pairs_[kMaxPairs] {};
      };

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
