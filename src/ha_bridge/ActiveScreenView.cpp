#include "ha_bridge/ActiveScreenView.h"

#include <FreeRTOS.h>
#include <portable.h>  // pvPortMalloc / vPortFree: the heap the reassembly buffer comes from

#include <cstring>
#include <new>

#include "ha_bridge/LayoutCommit.h"
#include "ha_bridge/LayoutStore.h"

// C8c: materialization of the active screen from the store, the view lifecycle
// and the common-section decisions (REQ009-FR-C8c). See ActiveScreenView.h for
// the contract. No LVGL object, no NimBLE, no file system: the store is reached
// through the LayoutStore seam, the heap phase is an injected atomic byte.

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      // ---------------------------------------------------------------------
      // Common-section decisions
      // ---------------------------------------------------------------------

      bool Is24Hour(const ActiveScreenView* v) {
        if (!LayoutPresent(v)) return true;
        return v->common.timeFormatLead == 'H';
      }

      bool IsBannerStale(bool haStale, uint32_t secondsSinceContact) {
        return haStale || secondsSinceContact > kBannerNoContactSec;
      }

      NextEventDecision DecideNextEvent(const ActiveScreenView* v, uint16_t year, int64_t nowSec,
                                        NextEventThrottle& throttle) {
        const bool present = LayoutPresent(v);
        return DecideNextEventAt(present ? v->common.eventStartEpoch.data() : nullptr,
                                 present ? v->common.eventCount : 0, present, year, nowSec, throttle);
      }

      uint32_t StatusFingerprint(const ActiveScreenView::DashView& dash) {
        uint32_t hash = kFnvOffset;
        const uint8_t groups = dash.statusGroupCount < dash.statusGroups.size()
                                   ? dash.statusGroupCount
                                   : static_cast<uint8_t>(dash.statusGroups.size());
        for (uint8_t g = 0; g < groups; g++) {
          const auto& grp = dash.statusGroups[g];
          const uint8_t count = grp.glyphCount < grp.glyphs.size() ? grp.glyphCount
                                                                   : static_cast<uint8_t>(grp.glyphs.size());
          hash = FnvAdd(hash, count);
          for (uint8_t i = 0; i < count; i++) {
            hash = FnvAdd(hash, static_cast<uint8_t>(grp.glyphs[i].symbol));
          }
        }
        return hash;
      }

      NextEventDecision DecideNextEventAt(const uint32_t* startEpochs, uint8_t count, bool layoutPresent,
                                          uint16_t year, int64_t nowSec, NextEventThrottle& throttle) {
        NextEventDecision d;
        if (year < kMinValidYear || !layoutPresent) return d;

        // Once-per-minute bucket: a new UTC minute re-arms the vibration.
        const int32_t nowMinute = static_cast<int32_t>(nowSec / 60);
        if (nowMinute != throttle.lastMinute) {
          throttle.lastMinute = nowMinute;
          throttle.vibrated = false;
        }
        if (nowSec <= 0) return d;

        for (uint8_t i = 0; startEpochs != nullptr && i < count && i < 2; i++) {
          const uint32_t epoch = startEpochs[i];
          if (epoch == 0) continue;
          const int64_t delta = static_cast<int64_t>(epoch) - nowSec;
          if (delta > 0 && static_cast<uint64_t>(delta) <= kNextEventWindowSec) {
            uint32_t minsLeft = static_cast<uint32_t>(delta) / 60u;
            if (minsLeft == 0) minsLeft = 1u;  // "1m" minimum
            d.show = true;
            d.minutes = minsLeft;
            return d;
          }
          if (delta <= 0 && delta >= -60) {
            // The epoch has just passed: one vibration, and "0m" for the rest of the window.
            if (!throttle.vibrated) {
              throttle.vibrated = true;
              d.requestVibration = true;
            }
            d.show = true;
            d.minutes = 0;
            return d;
          }
        }
        return d;
      }

      ScreenGroup ChooseGroup(const ActiveScreenView* v) {
        if (!LayoutPresent(v) || v->common.screenCount == 0) return ScreenGroup::Default;
        if (v->common.screenType == 0) {
          return v->screen.dashView.hasDashboard ? ScreenGroup::Dashboard : ScreenGroup::NoData;
        }
        return ScreenGroup::Tiles;
      }

      // ---------------------------------------------------------------------
      // Sanitizing a view read from the store
      // ---------------------------------------------------------------------

      namespace {

        // The read-only LayoutStore the header and section readers take, over the source's
        // ReadAt. It lives on the stack for one materialization (8 B), never on the heap.
        class SourceStore final : public LayoutStore {
         public:
          explicit SourceStore(ViewSource& s) : s_(s) {
          }
          bool OpenForReplace() override {
            return false;
          }
          bool Write(const uint8_t*, size_t) override {
            return false;
          }
          bool Commit() override {
            return false;
          }
          void Abort() override {
          }
          int ReadAt(size_t offset, uint8_t* out, size_t len) override {
            return s_.ReadAt(offset, out, len);
          }

         private:
          ViewSource& s_;
        };

        // A record forged through the file system service can pass both CRCs, so the view is
        // made safe for the display code whatever the bytes held: every char array ends in a
        // NUL, every bool is 0 or 1, every count is within its array.
        template <std::size_t N>
        void Terminate(std::array<char, N>& a) {
          a[N - 1] = '\0';
        }

        // A bool is read as a byte first: any other value is not a valid bool object.
        void NormalizeBool(bool& b) {
          uint8_t* raw = reinterpret_cast<uint8_t*>(&b);
          *raw = (*raw != 0) ? 1u : 0u;
        }

        void CapCount(uint8_t& count, std::size_t max) {
          if (count > max) count = static_cast<uint8_t>(max);
        }

        void SanitizeTile(ActiveScreenView::TileView& t) {
          Terminate(t.defaultActiveColor);
          Terminate(t.defaultInactiveColor);
          CapCount(t.tileCount, t.tiles.size());
          for (auto& tile : t.tiles) {
            Terminate(tile.label);
            Terminate(tile.icon);
            Terminate(tile.entityId);
            Terminate(tile.color);
            NormalizeBool(tile.colorEnabled);
            NormalizeBool(tile.confirm);
          }
        }

        void SanitizeDash(ActiveScreenView::DashView& d) {
          NormalizeBool(d.hasDashboard);
          for (auto& e : d.events) {
            Terminate(e.time);
            Terminate(e.title);
          }
          Terminate(d.configurableValue.label);
          Terminate(d.configurableValue.value);
          NormalizeBool(d.configurableValue.hasValue);
          for (auto& m : d.inlineMetrics) {
            Terminate(m.label);
            Terminate(m.value);
            NormalizeBool(m.enabled);
          }
          Terminate(d.weatherCondition);
          CapCount(d.statusGroupCount, d.statusGroups.size());
          for (auto& g : d.statusGroups) {
            CapCount(g.glyphCount, g.glyphs.size());
            for (auto& glyph : g.glyphs) NormalizeBool(glyph.active);
          }
          CapCount(d.sensorGroupCount, d.sensorGroups.size());
          for (auto& g : d.sensorGroups) CapCount(g.valueCount, g.values.size());
        }

      } // namespace

      // ---------------------------------------------------------------------
      // The lifecycle
      // ---------------------------------------------------------------------

      void ActiveScreenModel::ClearView() {
        if (view_ == nullptr) return;
        view_->common = ActiveScreenView::Common {};
        std::memset(static_cast<void*>(&view_->screen), 0, sizeof(view_->screen));
      }

      ActiveScreenModel::Outcome ActiveScreenModel::Defer(uint8_t screen) {
        pending_ = true;
        wantScreen_ = screen;
        return Outcome::Deferred;
      }

      ActiveScreenModel::Outcome ActiveScreenModel::Activate(uint8_t screen) {
        Deactivate();
        active_ = true;
        readRetries_ = 0;
        allocBaseline_ = allocFailures_;
        return Materialize(screen);
      }

      ActiveScreenModel::Outcome ActiveScreenModel::ActivateDeferred(uint8_t screen) {
        Deactivate();
        active_ = true;
        readRetries_ = 0;
        allocBaseline_ = allocFailures_;
        return Defer(screen);
      }

      void ActiveScreenModel::EndMaterialization() {
        ReleaseHeapPhaseFrom(source_.PhaseCell(), HeapPhase::Activation);
      }

      void ActiveScreenModel::Deactivate() {
        // A render in flight ends with the face: never leave the phase held.
        EndMaterialization();
        if (view_ != nullptr) {
          view_->~ActiveScreenView();
          vPortFree(view_);
          view_ = nullptr;
        }
        active_ = false;
        pending_ = false;
        readRetries_ = 0;
        screen_ = 0;
        wantScreen_ = 0;
      }

      ActiveScreenModel::Outcome ActiveScreenModel::Refresh() {
        if (!active_) return Outcome::Inactive;
        if (pending_) return Materialize(wantScreen_);
        if (source_.UpdatePending()) return Materialize(screen_);
        return Outcome::Unchanged;
      }

      ActiveScreenModel::Outcome ActiveScreenModel::Materialize(uint8_t screen) {
        if (!active_) return Outcome::Inactive;
        // The store is not readable while the system sleeps: defer without taking the phase or
        // reading. The signal stays raised and the request stays owed, so the first Refresh() after
        // the system runs again serves it.
        if (!source_.StoreReadable()) return Defer(screen);
        // Every materialization holds the phase from before the store read until the caller's
        // render is done (EndMaterialization). A held phase is a transfer, a pending write, or a
        // render still in flight: nothing has been read or allocated, the signal is untouched.
        if (!TryEnterHeapPhase(source_.PhaseCell(), HeapPhase::Activation)) return Defer(screen);
        const Outcome out = MaterializeLocked(screen);
        // A deferral draws nothing, so it gives the phase back at once; every other outcome is
        // drawn by the caller and keeps the hold until EndMaterialization().
        if (out == Outcome::Deferred) ReleaseHeapPhaseFrom(source_.PhaseCell(), HeapPhase::Activation);
        return out;
      }

      ActiveScreenModel::Outcome ActiveScreenModel::MaterializeLocked(uint8_t screen) {
        // Seqlock: capture the generation, defer on odd.
        const uint32_t genBefore = source_.LayoutGen();
        if ((genBefore & 1u) != 0u) return Defer(screen);

        if (view_ == nullptr) {
          void* mem = pvPortMalloc(sizeof(ActiveScreenView));
          if (mem == nullptr) {
            // Defined state: no view, the caller draws the safe face. Retried by Refresh();
            // nothing latches (the per-activation baseline, never a reset counter).
            ++allocFailures_;
            pending_ = true;
            wantScreen_ = screen;
            return Outcome::NoMemory;
          }
          view_ = new (mem) ActiveScreenView();
        }

        // One header read, then one section read; both are staged, so a read that fails
        // or is discarded leaves the view holding the last materialized screen.
        LayoutRecordHeader header;
        alignas(4) uint8_t image[sizeof(ViewTile) > sizeof(ViewDash) ? sizeof(ViewTile) : sizeof(ViewDash)];
        size_t imageLen = 0;
        uint8_t want = screen;
        uint8_t section = kNoSection;

        SourceStore store(source_);
        RecordStatus status = ReadLayoutHeader(store, header);
        if (status == RecordStatus::Ok) {
          if (want >= header.screenCount) want = 0;
          section = SectionForScreen(header, want);
          if (section != kNoSection) {
            status = ReadLayoutSection(store, header, section, image, sizeof(image), &imageLen);
          }
        }

        // Re-check: a generation that moved during the read discards it. The signal is not
        // consumed, and the request stays owed.
        if (source_.LayoutGen() != genBefore) return Defer(screen);

        if (status == RecordStatus::ReadError && readRetries_ < kMaxReadRetries) {
          // A transient I/O error is not "no layout": the view stays as it is, the signal stays
          // raised and the request stays owed. Bounded, so an error that never clears ends as
          // no layout below.
          ++readRetries_;
          return Defer(screen);
        }

        pending_ = false;
        readRetries_ = 0;
        if (status != RecordStatus::Ok) {
          // No layout: Absent, a corrupt or short record (REQ009-FR-B5(f)), or a read error that
          // outlasted the retries.
          ClearView();
          screen_ = 0;
          source_.TakeUpdate();
          return Outcome::NoLayout;
        }

        std::memcpy(&view_->common, header.common, sizeof(view_->common));
        const bool isTile = header.screenCount != 0 && (header.flags & (1u << (kFlagTileShift + want))) != 0;
        view_->common.screenType = isTile ? 1 : 0;
        std::memset(static_cast<void*>(&view_->screen), 0, sizeof(view_->screen));
        if (section != kNoSection) {
          // The image length is sizeof(DashView) or sizeof(TileView), by the section map.
          std::memcpy(static_cast<void*>(&view_->screen), image, imageLen);
        }
        // The CRCs passed, but a record forged through the file system service passes them too.
        CapCount(view_->common.eventCount, ActiveScreenView::kEventSlots);
        if (section != kNoSection) {
          if (isTile) {
            SanitizeTile(view_->screen.tileView);
          } else {
            SanitizeDash(view_->screen.dashView);
          }
        }
        screen_ = want;
        source_.TakeUpdate();
        return Outcome::Ready;
      }

    } // namespace HaBridge
  }   // namespace Applications
} // namespace Pinetime
