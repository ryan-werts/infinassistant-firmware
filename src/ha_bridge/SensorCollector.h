#pragma once
#include <cstdint>

namespace Pinetime {
  namespace Controllers {
    class MotionController;
    class HeartRateController;
    class Battery;
  }
}

namespace Pinetime {
namespace Applications {
namespace HaBridge {

/**
 * SensorCollector gathers sensor data from InfiniTime controllers
 * for transmission to the phone via SENSOR_REPORT messages.
 *
 * Collects:
 * - Steps (daily step count)
 * - Heart rate (current BPM + measurement state)
 * - Battery (percentage + charging status)
 * - Motion (accelerometer X/Y/Z)
 * - Timestamp
 */
class SensorCollector {
public:
  SensorCollector(
    Pinetime::Controllers::MotionController& motionController,
    Pinetime::Controllers::HeartRateController& heartRateController,
    Pinetime::Controllers::Battery& batteryController
  );

  // Get current step count
  uint32_t GetSteps() const;

  // Get current heart rate (BPM)
  uint8_t GetHeartRate() const;

  // Get heart rate measurement state (0=Stopped, 1=NotEnough, 2=NoTouch, 3=Running)
  uint8_t GetHeartRateState() const;

  // Get battery level (0-100%)
  uint8_t GetBatteryPercent() const;

  // Get battery charging status
  bool IsBatteryCharging() const;

  // True while a power source is connected, including a full battery on the charger (which is not
  // charging)
  bool IsPowerPresent() const;

  // The battery voltage in millivolts as of the last measurement (every 10 minutes), 0 before the first
  uint16_t GetBatteryMillivolts() const;

  // Get accelerometer motion data (X, Y, Z axis)
  void GetMotion(int16_t& x, int16_t& y, int16_t& z) const;

  // Get current timestamp (milliseconds since epoch or system time)
  uint64_t GetTimestamp() const;

private:
  Pinetime::Controllers::MotionController& motionController;
  Pinetime::Controllers::HeartRateController& heartRateController;
  Pinetime::Controllers::Battery& batteryController;
};

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
