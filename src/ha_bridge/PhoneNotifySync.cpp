#include "ha_bridge/PhoneNotifySync.h"

#include <utility>

#include <FreeRTOS.h>
#include <task.h>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      namespace {
        // Critical sections nest, and the list takes its own inside this one (IndexOf and the
        // rest): while it is held no other task runs, so the table and the list agree.
        struct CriticalSection {
          CriticalSection() {
            taskENTER_CRITICAL();
          }
          ~CriticalSection() {
            taskEXIT_CRITICAL();
          }
        };
      }  // namespace

      void PhoneNotifySync::Bind(Manager& manager, DismissSink sink, void* context) {
        manager_ = &manager;
        sink_ = sink;
        sinkContext_ = context;
        manager.SetHooks(&PhoneNotifySync::OnManagerDismiss, &PhoneNotifySync::OnManagerPush, this);
      }

      bool PhoneNotifySync::IsLive(ListId id) const {
        return manager_->IndexOf(id) < manager_->NbNotifications();
      }

      // The list drops its oldest entry when a sixth arrives, and says nothing. A pair left for it
      // would name an id the list hands out again after 256 pushes, so the list's push hook drops
      // the pairs whose entry is gone after every push, ours or the Alert Notification Service's.
      void PhoneNotifySync::DropDeadPairs() {
        for (Pair& p : pairs_) {
          if (p.nid != 0 && !IsLive(p.id)) {
            p.nid = 0;
          }
        }
      }

      bool PhoneNotifySync::TakeLocked(uint16_t nid, ListId& id) {
        for (Pair& p : pairs_) {
          if (p.nid == nid) {
            id = p.id;
            p.nid = 0;
            return true;
          }
        }
        return false;
      }

      void PhoneNotifySync::Push(Manager::Notification&& notif, uint16_t nid) {
        if (manager_ == nullptr) return;

        if (nid != 0) {
          ListId oldId = 0;
          bool replaces = false;
          {
            CriticalSection lock;
            replaces = TakeLocked(nid, oldId);
          }
          // The pair is gone, so the hook finds no nid for oldId and reports nothing to the phone.
          // An open Notifications screen keeps showing its copy of the old text (see the header).
          if (replaces) {
            manager_->Dismiss(oldId);
          }
        }

        // The push and the pair are one critical section: a user's dismiss of the new entry cannot
        // fall between them and leave a pair for an entry that is gone. (The list's push hook
        // runs inside it too and only prunes, which nests.)
        CriticalSection lock;
        const ListId id = manager_->Push(std::move(notif));
        if (nid == 0) return;  // not a phone notification
        for (Pair& p : pairs_) {
          if (p.nid == 0) {
            p.id = id;
            p.nid = nid;
            return;
          }
        }
      }

      bool PhoneNotifySync::Remove(uint16_t nid) {
        if (manager_ == nullptr || nid == 0) return false;

        ListId id = 0;
        bool found = false;
        {
          CriticalSection lock;
          found = TakeLocked(nid, id);
        }
        if (!found) return false;

        // An open Notifications screen keeps showing its copy of this entry until the user moves
        // on (see the header); no crash, and swiping it away is a Dismiss of an id that is gone.
        manager_->Dismiss(id);
        // Nothing is left to read, so the watch face's "new notification" mark goes too.
        if (manager_->IsEmpty()) {
          manager_->ClearNewNotificationFlag();
        }
        return true;
      }

      void PhoneNotifySync::OnManagerPush(void* self) {
        PhoneNotifySync* sync = static_cast<PhoneNotifySync*>(self);
        CriticalSection lock;
        sync->DropDeadPairs();
      }

      void PhoneNotifySync::OnManagerDismiss(void* self, ListId id) {
        PhoneNotifySync* sync = static_cast<PhoneNotifySync*>(self);
        uint16_t nid = 0;
        {
          CriticalSection lock;
          for (Pair& p : sync->pairs_) {
            if (p.nid != 0 && p.id == id) {
              nid = p.nid;
              p.nid = 0;
              break;
            }
          }
        }
        // The lock is released: the sink sends over BLE.
        if (nid != 0 && sync->sink_ != nullptr) {
          sync->sink_(sync->sinkContext_, nid);
        }
      }

      uint8_t PhoneNotifySync::Count() const {
        uint8_t n = 0;
        for (const Pair& p : pairs_) {
          if (p.nid != 0) ++n;
        }
        return n;
      }

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
