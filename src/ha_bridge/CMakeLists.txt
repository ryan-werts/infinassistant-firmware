# PineTime HA Bridge — InfiniTime ha_bridge component
# Called via add_subdirectory(ha_bridge) from src/CMakeLists.txt.
# CMAKE_CURRENT_SOURCE_DIR = InfiniTime/src/ha_bridge/

cmake_minimum_required(VERSION 3.10)

# ARM Cortex-M4 flags required for all ha_bridge targets.
# src/CMakeLists.txt applies COMMON_FLAGS only to specific named targets;
# these static libraries need the same ABI flags to link cleanly.
set(HA_ARM_FLAGS -mthumb -mabi=aapcs -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
  -ffunction-sections -fdata-sections -fno-strict-aliasing --specs=nano.specs)

# TinyCBOR is already added by src/CMakeLists.txt.
# Guard against double-add if invoked standalone.
if(NOT TARGET tinycbor)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../external/tinycbor/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/tinycbor
                     ${CMAKE_CURRENT_BINARY_DIR}/tinycbor)
  else()
    message(WARNING "TinyCBOR not found at external/tinycbor")
  endif()
endif()

# HA Bridge protocol library (FrameCodec; ProtocolMessages.h is constants only)
add_library(ha_bridge_protocol STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/FrameCodec.cpp
  # MCUboot confirmation policy. Lives in the lowest-level library because its
  # callers are core InfiniTime files (SystemTask, DisplayApp, DfuService),
  # not the HA bridge itself, and every executable links ha_bridge.
  ${CMAKE_CURRENT_SOURCE_DIR}/BootConfirm.cpp
  # Boot black box (diagnostic). Only the application pulls it in: see BootBox.cpp.
  ${CMAKE_CURRENT_SOURCE_DIR}/BootBox.cpp
)

target_include_directories(ha_bridge_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../external/tinycbor/src
)

target_link_libraries(ha_bridge_protocol PUBLIC tinycbor)
target_compile_features(ha_bridge_protocol PUBLIC cxx_std_17)
target_compile_options(ha_bridge_protocol PRIVATE ${HA_ARM_FLAGS})

# HA Bridge BLE service (HomeAssistantService)
add_library(ha_bridge_ble STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/HomeAssistantService.cpp
  # Which list entry is which phone notification (2.4.101). Its only caller is
  # HomeAssistantService.cpp, so it sits in this archive, ahead of the --start-group block below.
  ${CMAKE_CURRENT_SOURCE_DIR}/PhoneNotifySync.cpp
)

target_include_directories(ha_bridge_ble PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ha_bridge_ble PUBLIC ha_bridge_protocol nimble)
target_compile_features(ha_bridge_ble PUBLIC cxx_std_17)
target_compile_options(ha_bridge_ble PRIVATE ${HA_ARM_FLAGS})

# BridgeController + SensorCollector
add_library(ha_bridge_controller STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/BridgeController.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/SensorCollector.cpp
  # The live watch-face layout decoder. Its own TU so tools/fuzz can target it
  # on the host (it needs tinycbor and the layout structs, nothing else), but it
  # belongs in the SAME archive as BridgeController.cpp, its only caller, and
  # inside the --start-group block below.
  #
  # It sat in ha_bridge_protocol from 2.4.51 to 2.4.55 and every one of those
  # builds failed to get past LoadNewScreen. ha_bridge_protocol is listed first
  # and OUTSIDE the group, so the linker reaches it before anything references
  # DecodeScreenInline — exactly the archive-ordering trap the group below was
  # added to work around for nrf-sdk.
  ${CMAKE_CURRENT_SOURCE_DIR}/LayoutDecoder.cpp
  # The layout-level decode (key loop, defaults, range and version checks) that
  # used to sit in BridgeController::HandleWatchFaceLayout. Same rule as
  # LayoutDecoder.cpp above: this archive, never ha_bridge_protocol or
  # ha_bridge_ble, so the --start-group block below resolves it.
  ${CMAKE_CURRENT_SOURCE_DIR}/LayoutDocumentDecoder.cpp
  # The DASHBOARD_STATE decoder (2.4.96). Same rule as the decoders above.
  ${CMAKE_CURRENT_SOURCE_DIR}/DashboardState.cpp
  # The layout commit logic (validate, step-machine write, commit, abort and the
  # persisted-record parser). Same rule as the decoders above: this archive, so
  # the --start-group block below resolves it. It reaches the store only through
  # LayoutStore.h; the flash-backed store is a separate unit.
  ${CMAKE_CURRENT_SOURCE_DIR}/LayoutCommit.cpp
  # The flash-backed LayoutStore (one littlefs record file, replaced in place).
  # Same archive as LayoutCommit.cpp, which reaches it only through LayoutStore.h,
  # so the --start-group block below resolves both.
  ${CMAKE_CURRENT_SOURCE_DIR}/LayoutStoreFlash.cpp
  # The active-screen view (materialization from the store, the view lifecycle and
  # the common-section decisions). Same archive: HomeAssistant.cpp in
  # ha_bridge_app calls it, and the --start-group block below resolves it.
  ${CMAKE_CURRENT_SOURCE_DIR}/ActiveScreenView.cpp
  # The dashboard screen's decisions (v8, 2.4.98). Same archive and the same reason:
  # HomeAssistant.cpp in ha_bridge_app calls it.
  ${CMAKE_CURRENT_SOURCE_DIR}/DashboardV8.cpp
  # The NOTIFY and NOTIFY_REMOVE decode (2.4.101). Same archive as BridgeController.cpp, its
  # only caller; its own TU so tools/fuzz runs it on the host.
  ${CMAKE_CURRENT_SOURCE_DIR}/NotifyDecoder.cpp
)

target_include_directories(ha_bridge_controller PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ha_bridge_controller PUBLIC ha_bridge_ble)
target_compile_features(ha_bridge_controller PUBLIC cxx_std_17)
target_compile_options(ha_bridge_controller PRIVATE ${HA_ARM_FLAGS})

# HA Bridge UI screens (HomeAssistant watch face + standalone apps)
add_library(ha_bridge_app STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../displayapp/screens/HomeAssistant.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../displayapp/screens/HaLogbook.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../displayapp/screens/HaCalendar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../displayapp/screens/HaHomeControl.cpp
)

target_include_directories(ha_bridge_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ha_bridge_app PUBLIC ha_bridge_ble lvgl)
target_compile_features(ha_bridge_app PUBLIC cxx_std_17)
target_compile_options(ha_bridge_app PRIVATE ${HA_ARM_FLAGS})

# Top-level interface target
add_library(ha_bridge INTERFACE)

# BridgeController.cpp calls xTaskNotifyGive / ulTaskNotifyTake, which
# resolve to xTaskGenericNotify / ulTaskNotifyTake in nrf-sdk (tasks.c).
# The top-level CMakeLists places nrf-sdk before ha_bridge in the link
# line, so the linker discards those symbols before it sees the references.
# Fix: wrap ha_bridge_controller in a --start-group/--end-group block that
# explicitly includes the nrf-sdk archive by file path (not target name) so
# CMake's deduplication does not remove it from inside the group.
target_link_libraries(ha_bridge INTERFACE
  ha_bridge_protocol
  ha_bridge_ble
  -Wl,--start-group
  ha_bridge_controller
  ha_bridge_app
  $<TARGET_FILE:nrf-sdk>
  -Wl,--end-group
)
