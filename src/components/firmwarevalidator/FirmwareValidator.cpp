#include "components/firmwarevalidator/FirmwareValidator.h"

#include <hal/nrf_rtc.h>
#include "drivers/InternalFlash.h"

using namespace Pinetime::Controllers;

bool FirmwareValidator::IsValidated() const {
  // MCUboot sets image_ok as one byte, 0x01, padded with the erased value to its write
  // alignment, so after a permanent install or a revert the word reads 0xFFFFFF01. Compared as
  // a whole word it never matched, and the image took every such boot for a test boot and wrote
  // image_ok again. Compare the flag byte, as MCUboot does (little-endian: the word's low byte).
  auto* imageOkPtr = reinterpret_cast<const volatile uint8_t*>(validBitAdress);
  return (*imageOkPtr) == validBitValue;
}

void FirmwareValidator::Validate() {
  if (!IsValidated())
    Pinetime::Drivers::InternalFlash::WriteWord(validBitAdress, validBitValue);
}

void FirmwareValidator::Reset() {
  NVIC_SystemReset();
}
