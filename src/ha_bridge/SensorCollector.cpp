#include "SensorCollector.h"
#include "components/motion/MotionController.h"
#include "components/heartrate/HeartRateController.h"
#include "components/battery/BatteryController.h"
#include "systemtask/SystemTask.h"
#include <chrono>

namespace Pinetime {
namespace Applications {
namespace HaBridge {

SensorCollector::SensorCollector(
  Pinetime::Controllers::MotionController& motionController,
  Pinetime::Controllers::HeartRateController& heartRateController,
  Pinetime::Controllers::Battery& batteryController
) : motionController(motionController),
    heartRateController(heartRateController),
    batteryController(batteryController) {
}

uint32_t SensorCollector::GetSteps() const {
  // MotionController::NbSteps() returns the daily step count
  return motionController.NbSteps();
}

uint8_t SensorCollector::GetHeartRate() const {
  // HeartRateController::HeartRate() returns current BPM measurement
  // Returns 0 if no valid measurement
  return heartRateController.HeartRate();
}

uint8_t SensorCollector::GetHeartRateState() const {
  // HeartRateController::State() returns measurement state:
  // 0 = Stopped - Not measuring
  // 1 = NotEnough - Insufficient data
  // 2 = NoTouch - Sensor not in contact
  // 3 = Running - Active measurement with valid data
  auto state = heartRateController.State();
  return static_cast<uint8_t>(state);
}

uint8_t SensorCollector::GetBatteryPercent() const {
  // Battery::PercentRemaining() returns battery level 0-100
  return batteryController.PercentRemaining();
}

bool SensorCollector::IsPowerPresent() const {
  return batteryController.IsPowerPresent();
}

uint16_t SensorCollector::GetBatteryMillivolts() const {
  // Battery::Voltage() is the last MeasureVoltage() result, in millivolts
  return batteryController.Voltage();
}

bool SensorCollector::IsBatteryCharging() const {
  // Battery::IsCharging() returns true if charging cable connected
  return batteryController.IsCharging();
}

void SensorCollector::GetMotion(int16_t& x, int16_t& y, int16_t& z) const {
  // MotionController provides accelerometer data
  // Methods: X(), Y(), Z() return raw 16-bit accelerometer values
  // Note: Accelerometer might not be continuously active (depends on wake state)
  x = motionController.X();
  y = motionController.Y();
  z = motionController.Z();
}

uint64_t SensorCollector::GetTimestamp() const {
  // InfiniTime uses FreeRTOS tick count, not Unix epoch
  // For now, use tick count converted to milliseconds
  // Phone/HA can use this as relative timestamp or convert to absolute time

  // FreeRTOS tick rate is typically 1024 Hz in InfiniTime
  // xTaskGetTickCount() returns ticks since boot
  // Convert to milliseconds: ticks * 1000 / configTICK_RATE_HZ

  // Note: In actual InfiniTime integration, include "FreeRTOS.h" and "task.h"
  // For now, use placeholder that will be resolved during integration

  // This will need to be updated during firmware integration to:
  // uint64_t ticks = xTaskGetTickCount();
  // return (ticks * 1000ULL) / configTICK_RATE_HZ;

  // Placeholder: return 0 (will be fixed during integration with actual InfiniTime)
  return 0; // TODO: Replace with actual tick count conversion during InfiniTime integration
}

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
