/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ESP32S3 variant -- uses the built-in TWAI (CAN) peripheral instead of MCP2515.

    Two supported boards (selected via PlatformIO build_flags):
      * Waveshare ESP32-S3-RS485-CAN -- single CAN, TWAI only.
      * LILYGO T-2CAN -- dual CAN:
          - CAN A / CAN1: ESP32-S3 native TWAI (GPIO7/GPIO6) -- FSD activation and
            speed-limit modification. This is the primary, time-critical bus.
          - CAN B / CAN2: MCP2515 over SPI (GPIO12/11/13/10/9) -- second auxiliary
            bus (basic RX/TX/counters + optional 0x339 service-mode burst).

    WiFi/Bluetooth stay powered down unless the light WebUI is compiled in
    (-DENABLE_LIGHT_WEBUI), in which case a SoftAP + tiny parameter page run on
    a dedicated low-priority task so the CAN fast path is never blocked.
*/

#include <algorithm>
#include <cmath>
#include <cstring>
#include <driver/twai.h>
#include <esp_system.h>
#include <esp_sleep.h>
#ifdef ENABLE_LIGHT_WEBUI
#include <esp_heap_caps.h>
#endif

// ---- CAN B (MCP2515) -- optional secondary bus ----
// The autowp MCP2515 library provides its own `struct can_frame` with the same
// field layout (can_id / can_dlc / data[8]) the handler already expects, so we
// only define our own copy when the library is absent. Defining both in one
// translation unit would be a redefinition error -- hence the #else.
#ifdef ENABLE_CANB_MCP2515
#include <SPI.h>
#include <mcp2515.h>
#else
struct can_frame {
  uint32_t can_id;
  uint8_t  can_dlc;
  uint8_t  data[8];
};
#endif

// ---- Light WebUI -- optional ----
#ifdef ENABLE_LIGHT_WEBUI
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#endif

// This build supports the HW3 car only.

// Pin assignments -- overridable via PlatformIO build_flags (-D...).
// Defaults match the Waveshare ESP32-S3-RS485-CAN board.
#ifndef TWAI_TX_PIN
#define TWAI_TX_PIN GPIO_NUM_15
#endif
#ifndef TWAI_RX_PIN
#define TWAI_RX_PIN GPIO_NUM_16
#endif
#ifndef PIN_LED
#define PIN_LED 14
#endif

// TWAI RX/TX queue depths -- overridable via build_flags.
#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 64
#endif
#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif

// ---- CAN B (MCP2515) pin/clock fallbacks -- overridable via build_flags ----
#ifdef ENABLE_CANB_MCP2515
#ifndef MCP2515_SCK
#define MCP2515_SCK 12
#endif
#ifndef MCP2515_MOSI
#define MCP2515_MOSI 11
#endif
#ifndef MCP2515_MISO
#define MCP2515_MISO 13
#endif
#ifndef MCP2515_CS
#define MCP2515_CS 10
#endif
#ifndef MCP2515_RST
#define MCP2515_RST 9
#endif
#ifndef MCP2515_INT
#define MCP2515_INT 8
#endif
#ifndef MCP2515_CLOCK
#define MCP2515_CLOCK MCP_16MHZ
#endif
// SocketCAN-style id flags (defined by the autowp can.h; guarded for safety).
#ifndef CAN_EFF_FLAG
#define CAN_EFF_FLAG 0x80000000UL
#endif
#ifndef CAN_RTR_FLAG
#define CAN_RTR_FLAG 0x40000000UL
#endif
#ifndef CAN_SFF_MASK
#define CAN_SFF_MASK 0x000007FFUL
#endif
#endif  // ENABLE_CANB_MCP2515
#ifndef CAN_SFF_MASK
#define CAN_SFF_MASK 0x000007FFUL
#endif

// ---- Speed-offset encoding constants (wire format, not user-tunable) ----
constexpr int LOW_SPEED_MAX_PCT_LIMIT_KPH = 50;
constexpr int MAX_SPEED_OFFSET_KPH = 25;     // absolute pre-clamp on the computed offset
constexpr int MAX_SPEED_OFFSET_PCT = 50;     // PCT4 wire cap (matches dev kHw3SpeedOffsetMaxPct)
constexpr int OFFSET_PCT4_RAW_PER_PCT = 4;

constexpr uint32_t TWAI_ALERT_MASK =
  TWAI_ALERT_BUS_OFF |
  TWAI_ALERT_BUS_RECOVERED |
  TWAI_ALERT_RECOVERY_IN_PROGRESS |
  TWAI_ALERT_ERR_PASS |
  TWAI_ALERT_BUS_ERROR |
  TWAI_ALERT_TX_FAILED |
  TWAI_ALERT_RX_QUEUE_FULL |
  TWAI_ALERT_RX_FIFO_OVERRUN;
constexpr uint32_t TWAI_TX_WAIT_MS = 5;
constexpr uint32_t TWAI_RX_WAIT_MS = 1;
constexpr uint32_t TWAI_TX_RETRY_DELAY_MS = 1;
constexpr uint8_t TWAI_TX_RETRY_COUNT = 1;
constexpr uint8_t TWAI_RX_SCAN_LIMIT = 8;

constexpr uint8_t CANB_FILTER_ALL = 0;
constexpr uint8_t CANB_FILTER_FEATURE = 1;
constexpr uint8_t NAG_KILLER_MODE_B = 1;
constexpr uint8_t NAG_KILLER_MODE_C = 2;
constexpr uint8_t NAG_KILLER_MODE_DOC = 3;
constexpr uint16_t NAG_KILLER_TORQUE_MAX_CX100 = 280; // 2.80 Nm
constexpr uint16_t NAG_KILLER_TORQUE_RAW_BASE = 2050;
constexpr uint16_t NAG_KILLER_TORQUE_RAW_MIN =
    NAG_KILLER_TORQUE_RAW_BASE - NAG_KILLER_TORQUE_MAX_CX100;
constexpr uint16_t NAG_KILLER_TORQUE_RAW_MAX =
    NAG_KILLER_TORQUE_RAW_BASE + NAG_KILLER_TORQUE_MAX_CX100;

// ---- Runtime configuration (WebUI-tunable; defaults match the legacy constants) ----
// CAN A reads this every relevant frame, so updates must stay cheap. The legacy
// firmware used plain constexpr values; the defaults below are byte-for-byte
// equivalent, so behaviour with no WebUI is unchanged.
struct RuntimeConfig {
  bool fsdEnabled = true;
  bool autoSpeedOffsetEnabled = true;
  bool cabinCameraDisableEnabled = false; // when enabled, write 0x3FD mux1 bit43 to 0
  bool cabinCameraTelemetryDisableEnabled = false; // when enabled, write 0x3FD mux1 bit48 to 0
  uint8_t slewPctPerSec = 5;
  uint8_t lowSpeedMaxPctRaw = 200;    // = MAX_SPEED_OFFSET_PCT * OFFSET_PCT4_RAW_PER_PCT

  uint16_t targetBelow60 = 60;
  uint16_t target60 = 80;
  uint16_t target70 = 85;
  uint16_t target80 = 90;
  uint16_t target90 = 100;
  uint16_t target100 = 120;
  uint16_t target120 = 140;

  bool canbEnabled = true;
  bool canbServiceModeEnabled = false;
  uint8_t canbFilterMode = CANB_FILTER_ALL; // 0=capture/debug all, 1=current feature IDs
  bool highBeamStrobeEnabled = false; // arms double-pull flash-to-pass trigger
  bool rearFogBrakeStrobeEnabled = false; // arms brake-triggered 0x273 rear fog burst
  bool reverseStrobeEnabled = false;  // arms reverse-gear hazard + rear-fog burst
  bool batteryPreheatEnabled = false; // sends fixed UI_tripPlanning 0x082 every 500 ms
  bool dndEnabled = false;             // volume DND master switch
  bool dndVolumeEnabled = false;       // left scroll up/down on 0x3C2
  bool nagKillerEnabled = false;       // experimental steering torque echo
  bool nagKillerTest052Enabled = false; // force-send configured 0x052 torque on live target frames
  bool nagKillerTest370Enabled = false; // force-send configured 0x370 torque on live target frames
  uint8_t nagKillerMode = NAG_KILLER_MODE_B; // 1=Mode B, 2=Mode C, 3=doc state machine
  uint16_t nagKillerBurstMs = 1000;    // Mode B injection window
  uint16_t nagKillerPauseMs = 1500;    // Mode B rest window
  uint16_t nagKillerBPos1Cx100 = 180;  // Mode B + torque #1, centi-Nm
  uint16_t nagKillerBPos2Cx100 = 150;  // Mode B + torque #2, centi-Nm
  uint16_t nagKillerBNeg1Cx100 = 150;  // Mode B - torque #1, centi-Nm
  uint16_t nagKillerBNeg2Cx100 = 180;  // Mode B - torque #2, centi-Nm
  uint16_t nagKillerCNegCx100 = 180;   // Mode C negative endpoint, centi-Nm
  uint16_t nagKillerCPosCx100 = 180;   // Mode C positive endpoint, centi-Nm
  bool scrollGearSimEnabled = true;    // show brake + right-scroll D/R intent only
  bool scrollGearInjectEnabled = false; // experimental: inject 0x229 right-stalk D/R request
  bool can1ReceiveOnly = false;        // bus=1/TWAI/physical CANB RX-only: gate TWAI TX
  bool lockDeepSleepEnabled = false;    // enter deep sleep immediately after a validated vehicle lock signal
};

static RuntimeConfig g_config;

static uint8_t normalizeCanBFilterMode(uint8_t mode) {
  // Legacy saved value 2 used to mean "minimum"; map it to feature IDs.
  return mode == CANB_FILTER_FEATURE || mode == 2 ? CANB_FILTER_FEATURE : CANB_FILTER_ALL;
}

static uint8_t normalizeNagKillerMode(uint8_t mode) {
  if (mode == NAG_KILLER_MODE_DOC) return NAG_KILLER_MODE_DOC;
  return mode == NAG_KILLER_MODE_C ? NAG_KILLER_MODE_C : NAG_KILLER_MODE_B;
}

static uint16_t clampNagKillerBurstMs(uint16_t value) {
  if (value < 50) return 50;
  if (value > 10000) return 10000;
  return value;
}

static uint16_t clampNagKillerPauseMs(uint16_t value) {
  if (value > 10000) return 10000;
  return value;
}

static uint16_t clampNagKillerTorqueCx100(uint16_t value) {
  if (value > NAG_KILLER_TORQUE_MAX_CX100) return NAG_KILLER_TORQUE_MAX_CX100;
  return value;
}

static float nagKillerTorqueCx100ToNm(uint16_t value) {
  return static_cast<float>(clampNagKillerTorqueCx100(value)) / 100.0f;
}

// ---- Runtime status snapshot (CAN fast path writes, WebUI reads) ----
struct RuntimeStatus {
  uint32_t can1Rx = 0;
  uint32_t can1Tx = 0;
  uint32_t can1TxFail = 0;
  uint32_t twaiBusOffCount = 0;
  uint8_t twaiState = 0;

  uint32_t canbRx = 0;
  uint32_t canbTx = 0;
  uint32_t canbTxFail = 0;
  uint32_t canbLastId = 0;
  uint8_t canbErrorFlags = 0;
  uint32_t canbRxOverflowCount = 0;
  uint8_t highBeamStrobeActive = 0;
  uint8_t highBeamStrobeRemaining = 0;
  uint8_t rearFogBrakeStrobeActive = 0;
  uint8_t rearFogBrakeStrobeRemaining = 0;
  uint8_t reverseStrobeActive = 0;
  uint8_t reverseStrobeRemaining = 0;
  uint8_t batteryPreheatActive = 0;
  uint32_t batteryPreheatTxCount = 0;
  uint32_t batteryPreheatAgeMs = 0;
  uint8_t batteryPreheatFeedbackSeen = 0;
  uint8_t batteryPreheatFeedbackBus = 0;
  uint32_t batteryPreheatFeedbackAgeMs = 0;
  uint8_t batteryPreheatUiTripActive = 0;
  uint8_t batteryPreheatUiNavToSupercharger = 0;
  uint8_t batteryPreheatUiFastChargerType = 0;
  uint8_t batteryPreheatUiState = 0;
  uint8_t batteryPreheatUiRequestHeat = 0;
  int batteryPreheatUiPowerW = -32768;
  int batteryPreheatUiTargetCx100 = -32768;
  int batteryPreheatUiAmbientCx100 = -32768;
  int batteryPreheatUiChargeTargetCx10 = -32768;
  int batteryPreheatUiEnergyAtDestination = -32768;
  char batteryPreheatFeedbackPayload[24] = "-";
  uint8_t dndHandsOnState = 0;
  uint8_t dndWarningActive = 0;
  uint8_t dndActionActive = 0;
  uint8_t dndActionType = 0; // 0=none, 1=volume
  uint8_t dndBlocked = 0;    // 0=ok, 1=disabled, 2=canb, 3=no_cache
  uint32_t dndTxCount = 0;
  uint32_t dndLastTriggerAgeMs = 0;
  uint32_t dndScrollCacheAgeMs = 0;
  uint8_t nagKillerMode = 0;
  uint8_t nagKillerActive = 0;
  uint8_t nagKillerBlocked = 0;
  uint8_t nagKillerBurstActive = 0;
  uint32_t nagKillerTargetId = 0;
  uint32_t nagKillerRxCount = 0;
  uint32_t nagKillerTxCount = 0;
  uint32_t nagKillerTxFail = 0;
  uint32_t nagKillerLastRxAgeMs = 0;
  uint32_t nagKillerLastTxAgeMs = 0;
  uint32_t nagKillerApAgeMs = 0;
  uint32_t nagKillerSteeringAgeMs = 0;
  uint8_t nagKillerApState = 15;
  uint8_t nagKillerHandsOnState = 0;
  uint8_t nagKillerTargetHandsOn = 0;
  uint8_t nagKillerSetHandsOn = 0;
  int nagKillerRealTorqueCx100 = -32768;
  int nagKillerLastTorqueCx100 = -32768;
  int nagKillerSteeringDegCx10 = -32768;
  uint8_t bmsTempFrameSeen = 0;
  uint32_t bmsTempFrameId = 0;
  uint8_t bmsTempFrameBus = 0;
  uint8_t bmsTempFrameMux = 0;
  uint32_t bmsTempFrameAgeMs = 0;
  char bmsTempFramePayload[24] = "-";
  uint8_t bmsTempDecodedSeen = 0;
  uint8_t bmsTempDecodedMux = 255;
  uint8_t bmsTempDecodedCount = 0;
  uint32_t bmsTempDecodedAgeMs = 0;
  int bmsTempLatest1Cx100 = -32768;
  int bmsTempLatest2Cx100 = -32768;
  int bmsTempLatest3Cx100 = -32768;
  int bmsTempMinCx100 = -32768;
  int bmsTempAvgCx100 = -32768;
  int bmsTempMaxCx100 = -32768;
  int8_t rightScrollTicks = 0;
  uint8_t rightStalkStatus = 0;
  uint8_t rightStalkCounter = 0;
  uint8_t currentGear = 0;
  uint8_t dasAutopilotState = 15;    // 0/1/2 allow normal driving; 3..6 active AP/FSD
  uint8_t brakeActive = 0;
  int8_t scrollGearIntent = 0;       // -1=R, 0=none, 1=D
  uint8_t scrollGearDryRun = 0;
  uint8_t scrollGearInjectActive = 0;
  uint8_t scrollGearInjectTarget = 0; // 2=R, 4=D
  uint8_t scrollGearInjectOk = 0;
  uint8_t scrollGearInjectBlocked = 0;
  int vehicleSpeedKph = 0;
  uint8_t lockSleepArmed = 0;
  uint8_t lockSleepTriggered = 0;
  uint32_t lockSleepLastId = 0;
  uint8_t lockSleepSource = 0; // 1=0x339 VCSEC simplified lock status
  uint32_t lockSleepAgeMs = 0;
  uint8_t lockSleep339Seen = 0;
  uint8_t lockSleep339SimpleStatus = 255;
  uint32_t lockSleep339AgeMs = 0;
  uint8_t lockSleepCabinEmpty = 0;
  uint32_t lockSleep339StableAgeMs = 0;
  uint8_t lockSleepBlocked = 0; // 0=ok, 1=unlocked, 2=cabin_active, 3=stabilizing

  int fusedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t offsetRaw = 0;
};

static RuntimeStatus g_status;

// Config is shared between the CAN core and the WebUI task. A short spinlock
// guards writes; the CAN path takes a one-shot consistent copy per frame. With
// no WebUI compiled in there are no writers, so we skip the lock entirely.
#ifdef ENABLE_LIGHT_WEBUI
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
#endif

static inline RuntimeConfig configSnapshot() {
#ifdef ENABLE_LIGHT_WEBUI
  portENTER_CRITICAL(&g_cfgMux);
  RuntimeConfig c = g_config;
  portEXIT_CRITICAL(&g_cfgMux);
  return c;
#else
  return g_config;
#endif
}

static inline void applyBuildModeGuards(RuntimeConfig& c) {
  (void)c;  // no build-mode overrides on this branch
}

#ifdef ENABLE_LIGHT_WEBUI
#ifndef REC_CAP
#define REC_CAP 4000
#endif
static constexpr uint32_t REC_TARGET_CAP = REC_CAP;
static constexpr uint32_t REC_MAX_DURATION_MS = 60000UL;
static constexpr uint8_t REC_FILTER_MAX = 16;

struct RecFrame {
  uint32_t ts;
  char dir;
  uint8_t bus;
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
};

static portMUX_TYPE g_recMux = portMUX_INITIALIZER_UNLOCKED;
static RecFrame* recBuf = nullptr;
static uint32_t recCapacity = 0;
static size_t recBufferBytes = 0;
static bool recPsramReady = false;
static volatile bool recActive = false;
static volatile uint32_t recCount = 0;
static volatile uint32_t recDropped = 0;
static volatile uint32_t recBus1Count = 0;
static volatile uint32_t recBus2Count = 0;
static volatile uint8_t recStopReason = 0; // 0=none/manual, 1=full, 2=timeout
static volatile bool recSaved = false;
static uint32_t recStartMs = 0;
static uint32_t recFilterIds[REC_FILTER_MAX];
static uint8_t recFilterCount = 0;
static uint32_t recExcludeIds[REC_FILTER_MAX];
static uint8_t recExcludeCount = 0;

static bool recIdInList(uint32_t id, const uint32_t* ids, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (ids[i] == id) return true;
  }
  return false;
}

static bool recFramePassesFilter(uint32_t id) {
  if (recFilterCount > 0 && !recIdInList(id, recFilterIds, recFilterCount)) return false;
  if (recExcludeCount > 0 && recIdInList(id, recExcludeIds, recExcludeCount)) return false;
  return true;
}

static void setupRecorderBuffer() {
  recPsramReady = psramInit();
  if (!recPsramReady) return;

  recBufferBytes = static_cast<size_t>(REC_TARGET_CAP) * sizeof(RecFrame);
  recBuf = static_cast<RecFrame*>(heap_caps_malloc(recBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!recBuf) {
    recBufferBytes = 0;
    recCapacity = 0;
    return;
  }
  recCapacity = REC_TARGET_CAP;
}

static void recordCanFrame(const can_frame& frame, char dir, uint8_t bus) {
  if (!recActive) return;
  if (!recBuf || recCapacity == 0) return;
  if (millis() - recStartMs >= REC_MAX_DURATION_MS) {
    portENTER_CRITICAL(&g_recMux);
    recActive = false;
    recSaved = true;
    recStopReason = 2;
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  const uint32_t id = frame.can_id & CAN_SFF_MASK;
  if (!recFramePassesFilter(id)) return;

  portENTER_CRITICAL(&g_recMux);
  if (!recActive) {
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  uint32_t idx = recCount;
  if (idx >= recCapacity) {
    recActive = false;
    recSaved = true;
    recDropped++;
    recStopReason = 1;
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  RecFrame& r = recBuf[idx];
  r.ts = millis();
  r.dir = dir;
  r.bus = bus;
  r.id = id;
  r.dlc = frame.can_dlc <= 8 ? frame.can_dlc : 8;
  memset(r.data, 0, sizeof(r.data));
  memcpy(r.data, frame.data, r.dlc);
  recCount = idx + 1;
  if (bus == 1) recBus1Count++;
  if (bus == 2) recBus2Count++;
  if (recCount >= recCapacity) {
    recActive = false;
    recSaved = true;
    recStopReason = 1;
  }
  portEXIT_CRITICAL(&g_recMux);
}
#else
static inline void recordCanFrame(const can_frame&, char, uint8_t) {}
#endif

// ---- TWAI helpers ----

static bool twaiRecoveryInProgress = false;
static uint32_t batteryPreheatLastSendMs = 0;
static volatile bool canTxInhibitedForSleep = false;
static bool lockDeepSleepPending = false;
static uint32_t lockDeepSleepPendingMs = 0;
static uint32_t lockSleepLastSignalMs = 0;
static uint32_t lockSleep339LastRxMs = 0;
static uint32_t lockSleep339StableStartMs = 0;
static bool lockSleepDriverKnown = false;
static bool lockSleepDriverPresent = false;
static uint32_t lockSleepDriverSeenMs = 0;
static int8_t lockSleepSeatDriver = -1;
static int8_t lockSleepSeatPassenger = -1;
static int8_t lockSleepSeatRearLeft = -1;
static int8_t lockSleepSeatRearCenter = -1;
static int8_t lockSleepSeatRearRight = -1;
static uint32_t lockSleepSeatSeenMs = 0;
static uint32_t batteryPreheatFeedbackLastRxMs = 0;
static uint32_t nagKillerLastRxMs = 0;
static uint32_t nagKillerLastTxMs = 0;
static uint32_t nagKillerLastApMs = 0;
static uint32_t nagKillerApActiveEnterMs = 0;
static uint32_t nagKillerLastSteeringMs = 0;
static uint32_t nagKillerState2EnterMs = 0;
static uint32_t nagKillerState3EnterMs = 0;
static uint32_t nagKillerDocState1EnterMs = 0;
static uint32_t nagKillerDocState2EnterMs = 0;
static uint32_t nagKillerDocState3EnterMs = 0;
static uint32_t nagKillerDocState2HoldUntilMs = 0;
static float nagKillerDocState1HoldTorqueNm = 0.0f;
static float nagKillerDocState2HoldTorqueNm = 0.0f;
static float nagKillerDocMildTorqueNm = 1.25f;
static float nagKillerDocLastTorqueNm = 0.0f;
static uint8_t nagKillerDocState1HoldHandsLevel = 0;
static uint8_t nagKillerDocState2HoldHandsLevel = 0;
static uint8_t nagKillerDocLastHandsLevel = 0;
static bool nagKillerDocState2Level2WasActive = false;
static uint16_t nagKillerWalkSeed = 0xACE1;
static float nagKillerLastModeCTorqueNm = 0.5f;
static float nagKillerSteeringAngleDeg = 0.0f;
static uint8_t nagKillerApState = 15;
static uint8_t nagKillerHandsOnState = 0;
static uint8_t nagKillerPrevHandsOnState = 255;
static uint8_t nagKillerModeBTorqueIndex = 0;
static uint32_t nagKillerModeBLastChangeMs = 0;
static uint32_t bms712TempLastRxMs = 0;
static int16_t bms712TempCx100[12] = {};
static uint16_t bms712TempValidMask = 0;

static bool isRelevantCanId(uint32_t canId);
static void serviceTwaiAlerts();
static void serviceBatteryPreheat(const RuntimeConfig& cfg);
static void handleBatteryPreheatFeedbackFrame(const can_frame& frame, uint8_t bus);
#ifdef ENABLE_CANB_MCP2515
static bool canb_send(const can_frame& frame);
#endif

static bool twai_send(const can_frame& frame) {
  if (canTxInhibitedForSleep) return false;
  if (g_config.can1ReceiveOnly) return false;  // bus=1/TWAI/physical CANB RX-only
  if (frame.can_dlc > 8) return false;

  twai_message_t msg = {};
  msg.identifier = frame.can_id;
  msg.data_length_code = frame.can_dlc;
  memcpy(msg.data, frame.data, frame.can_dlc);

  for (uint8_t attempt = 0; attempt <= TWAI_TX_RETRY_COUNT; ++attempt) {
    serviceTwaiAlerts();
    if (twaiRecoveryInProgress) {
      g_status.can1TxFail++;
      return false;
    }

    if (twai_transmit(&msg, pdMS_TO_TICKS(TWAI_TX_WAIT_MS)) == ESP_OK) {
      g_status.can1Tx++;
      recordCanFrame(frame, 'T', 1);
      return true;
    }

    serviceTwaiAlerts();
    if (attempt < TWAI_TX_RETRY_COUNT) delay(TWAI_TX_RETRY_DELAY_MS);
  }
  g_status.can1TxFail++;
  return false;
}

static bool nagKillerSendFrame(const can_frame& frame) {
#ifdef ENABLE_CANB_MCP2515
  return canb_send(frame);  // T-2CAN: Nag-Killer lives on bus=2 / MCP2515 / physical CANA.
#else
  return twai_send(frame);
#endif
}

static bool twai_recv(can_frame& frame) {
  twai_message_t msg;

  for (uint8_t i = 0; i < TWAI_RX_SCAN_LIMIT; ++i) {
    TickType_t waitTicks = (i == 0) ? pdMS_TO_TICKS(TWAI_RX_WAIT_MS) : 0;
    if (twai_receive(&msg, waitTicks) != ESP_OK) return false;
    if (msg.extd || msg.rtr || msg.data_length_code > 8) {
      continue;
    }
    if (!isRelevantCanId(msg.identifier)) continue;

    frame.can_id  = msg.identifier;
    frame.can_dlc = msg.data_length_code;
    memset(frame.data, 0, sizeof(frame.data));
    memcpy(frame.data, msg.data, frame.can_dlc);
    g_status.can1Rx++;
    return true;
  }
  return false;
}

static void serviceTwaiAlerts() {
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) == ESP_OK) {
    if (alerts & TWAI_ALERT_BUS_OFF) {
      if (!twaiRecoveryInProgress) {
        twaiRecoveryInProgress = true;
        g_status.twaiBusOffCount++;
        twai_initiate_recovery();
      }
      return;
    }

    if (alerts & TWAI_ALERT_RECOVERY_IN_PROGRESS) {
      twaiRecoveryInProgress = true;
      return;
    }

    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
      twaiRecoveryInProgress = twai_start() != ESP_OK;
      return;
    }
  }

  twai_status_info_t statusInfo = {};
  if (twai_get_status_info(&statusInfo) != ESP_OK) return;

  if (statusInfo.state == TWAI_STATE_BUS_OFF) {
    if (!twaiRecoveryInProgress) {
      twaiRecoveryInProgress = true;
      g_status.twaiBusOffCount++;
      twai_initiate_recovery();
    }
    return;
  }

  if (statusInfo.state == TWAI_STATE_RECOVERING) {
    twaiRecoveryInProgress = true;
    return;
  }

  if (twaiRecoveryInProgress && statusInfo.state == TWAI_STATE_STOPPED) {
    twaiRecoveryInProgress = twai_start() != ESP_OK;
    return;
  }

  if (statusInfo.state == TWAI_STATE_RUNNING) {
    twaiRecoveryInProgress = false;
  }
  g_status.twaiState = static_cast<uint8_t>(statusInfo.state);
}

// ---- CAN IDs ----

constexpr uint32_t CAN_ID_FOLLOW_DISTANCE = 1016;
constexpr uint32_t CAN_ID_AP_CONTROL = 1021;
constexpr uint32_t CAN_ID_UI_TRIP_PLANNING = 0x082;
constexpr uint32_t CAN_ID_NAG_MODE_B_TARGET = 0x052;
constexpr uint32_t CAN_ID_NAG_MODE_C_TARGET = 0x370;
constexpr uint32_t CAN_ID_NAG_STEERING_ANGLE = 0x129;
constexpr uint32_t CAN_ID_EPAS_SYS_STATUS = 0x313;
constexpr uint32_t CAN_ID_BMS_THERMAL_STATUS = 0x312;
constexpr uint32_t CAN_ID_BMS_LOG1 = 0x374;
constexpr uint32_t CAN_ID_BMS_PACK_TEMPERATURES = 0x712;
constexpr uint32_t CAN_ID_DAS_STATUS = 0x399;
constexpr uint32_t CAN_ID_DRIVER_OCCUPANCY = 0x3A1;
constexpr uint32_t CAN_ID_BRAKE_PEDAL = 0x145;
constexpr uint32_t CAN_ID_RCM_INERTIAL2_CH = 0x111;
constexpr uint32_t CAN_ID_RCM_INERTIAL2_ETH = 0x116;
constexpr uint32_t CAN_ID_DI_SYSTEM_STATUS = 0x118;
constexpr uint32_t CAN_ID_DI_CHASSIS_CONTROL = 0x148;
constexpr uint32_t CAN_ID_ESP_BRAKE_TORQUE = 0x185;
constexpr uint32_t CAN_ID_DIF_TORQUE = 0x186;
constexpr uint32_t CAN_ID_VEHICLE_SPEED = 0x257;

static inline bool isRelevantCanId(uint32_t canId) {
  return canId == CAN_ID_UI_TRIP_PLANNING ||
         canId == CAN_ID_NAG_MODE_B_TARGET ||
         canId == CAN_ID_NAG_MODE_C_TARGET ||
         canId == CAN_ID_NAG_STEERING_ANGLE ||
         canId == CAN_ID_EPAS_SYS_STATUS ||
         canId == CAN_ID_BRAKE_PEDAL ||
         canId == CAN_ID_RCM_INERTIAL2_CH ||
         canId == CAN_ID_RCM_INERTIAL2_ETH ||
         canId == CAN_ID_DI_SYSTEM_STATUS ||
         canId == CAN_ID_DI_CHASSIS_CONTROL ||
         canId == CAN_ID_ESP_BRAKE_TORQUE ||
         canId == CAN_ID_DIF_TORQUE ||
         canId == CAN_ID_VEHICLE_SPEED ||
         canId == CAN_ID_BMS_THERMAL_STATUS ||
         canId == CAN_ID_BMS_LOG1 ||
         canId == CAN_ID_BMS_PACK_TEMPERATURES ||
         canId == CAN_ID_DAS_STATUS ||
         canId == CAN_ID_DRIVER_OCCUPANCY ||
         canId == CAN_ID_FOLLOW_DISTANCE ||
         canId == CAN_ID_AP_CONTROL;
}

// Battery-preheat 0x082 UI_tripPlanning. This is the fixed payload proven on
// the vehicle; the old dynamic-template and companion-frame replay paths were
// removed so this switch has exactly one CAN behavior.
static const uint8_t BATTERY_PREHEAT_ON[8] = {0xAF, 0x50, 0x94, 0x39, 0xFF, 0x03, 0x83, 0x05};
static const uint8_t BATTERY_PREHEAT_OFF[8] = {0x01, 0x50, 0x94, 0x39, 0xFF, 0x03, 0x83, 0x05};
constexpr uint32_t BATTERY_PREHEAT_PERIOD_MS = 500UL;
constexpr int TEMPERATURE_SNA_CX100 = -32768;
static uint8_t batteryPreheatOffFramesLeft = 0;
static uint32_t bmsTempLastRxMs = 0;

static void formatPayload8(const can_frame& frame, char out[24]) {
  static const char hex[] = "0123456789ABCDEF";
  uint8_t pos = 0;
  const uint8_t n = frame.can_dlc < 8 ? frame.can_dlc : 8;
  for (uint8_t i = 0; i < n; ++i) {
    if (i != 0) out[pos++] = ' ';
    out[pos++] = hex[(frame.data[i] >> 4) & 0x0F];
    out[pos++] = hex[frame.data[i] & 0x0F];
  }
  out[pos] = '\0';
}

static int8_t signedByte(uint8_t raw) {
  return raw >= 0x80 ? static_cast<int8_t>(static_cast<int>(raw) - 256) : static_cast<int8_t>(raw);
}

static void handleBatteryPreheatFeedbackFrame(const can_frame& frame, uint8_t bus) {
  if (frame.can_id != CAN_ID_UI_TRIP_PLANNING || frame.can_dlc < 8) return;

  batteryPreheatFeedbackLastRxMs = millis();
  g_status.batteryPreheatFeedbackSeen = 1;
  g_status.batteryPreheatFeedbackBus = bus;
  g_status.batteryPreheatUiTripActive = frame.data[0] & 0x01;
  g_status.batteryPreheatUiNavToSupercharger = (frame.data[0] >> 1) & 0x01;
  g_status.batteryPreheatUiFastChargerType = (frame.data[0] >> 2) & 0x07;
  g_status.batteryPreheatUiState = (frame.data[0] >> 5) & 0x03;
  g_status.batteryPreheatUiRequestHeat = (frame.data[0] >> 7) & 0x01;
  g_status.batteryPreheatUiPowerW =
      frame.data[1] == 0x7F ? TEMPERATURE_SNA_CX100 : static_cast<int>(signedByte(frame.data[1])) * 125;
  g_status.batteryPreheatUiTargetCx100 =
      frame.data[2] == 0xFF ? TEMPERATURE_SNA_CX100 : static_cast<int>(frame.data[2]) * 25;
  g_status.batteryPreheatUiAmbientCx100 =
      (frame.data[3] == 0x80 || frame.data[3] == 0xFF)
          ? TEMPERATURE_SNA_CX100
          : static_cast<int>(signedByte(frame.data[3])) * 50;

  const uint16_t chargeTargetRaw =
      static_cast<uint16_t>((static_cast<uint16_t>(frame.data[5] & 0x03) << 8) | frame.data[4]);
  const uint16_t energyRaw =
      static_cast<uint16_t>((static_cast<uint16_t>(frame.data[7]) << 8) | frame.data[6]);
  g_status.batteryPreheatUiChargeTargetCx10 =
      chargeTargetRaw == 0x03FF ? TEMPERATURE_SNA_CX100 : static_cast<int>(chargeTargetRaw);
  g_status.batteryPreheatUiEnergyAtDestination =
      (energyRaw == 0x8000 || energyRaw == 0x8001 || energyRaw == 0xFFFF)
          ? TEMPERATURE_SNA_CX100
          : static_cast<int>(static_cast<int16_t>(energyRaw));
  formatPayload8(frame, g_status.batteryPreheatFeedbackPayload);
}

static void sendBatteryPreheatFrame(const uint8_t payload[8]) {
  can_frame f = {};
  f.can_id = CAN_ID_UI_TRIP_PLANNING;
  f.can_dlc = 8;
  memcpy(f.data, payload, 8);
#ifdef ENABLE_CANB_MCP2515
  canb_send(f);
#else
  twai_send(f);
#endif
}

static void serviceBatteryPreheat(const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  g_status.batteryPreheatAgeMs =
      batteryPreheatLastSendMs == 0 ? 0 : (now - batteryPreheatLastSendMs);

#ifdef ENABLE_CANB_MCP2515
  if (!cfg.canbEnabled) {
    g_status.batteryPreheatActive = 0;
    batteryPreheatLastSendMs = 0;
    batteryPreheatOffFramesLeft = 0;
    return;
  }
#endif

  if (!cfg.batteryPreheatEnabled) {
    g_status.batteryPreheatActive = 0;
    // On the OFF edge, emit a few OFF frames so the request clears.
    if (batteryPreheatOffFramesLeft > 0) {
      if (batteryPreheatLastSendMs == 0 ||
          (now - batteryPreheatLastSendMs) >= BATTERY_PREHEAT_PERIOD_MS) {
        batteryPreheatLastSendMs = now;
        sendBatteryPreheatFrame(BATTERY_PREHEAT_OFF);
        batteryPreheatOffFramesLeft--;
      }
    } else {
      batteryPreheatLastSendMs = 0;
    }
    return;
  }

  g_status.batteryPreheatActive = 1;
  batteryPreheatOffFramesLeft = 3;  // arm OFF frames for the next disable edge
  if (batteryPreheatLastSendMs != 0 &&
      (now - batteryPreheatLastSendMs) < BATTERY_PREHEAT_PERIOD_MS) {
    return;
  }
  batteryPreheatLastSendMs = now;
  sendBatteryPreheatFrame(BATTERY_PREHEAT_ON);
  g_status.batteryPreheatTxCount++;
}

// Hardware acceptance filter -- a coarse pre-filter so the controller only
// enqueues the IDs near our targets instead of the whole bus; isRelevantCanId()
// still does the exact ID match. Derived from the IDs we touch:
//   0x145, 0x399, 0x3F8, 0x3FD.
// Adding 0x145 widens this coarse filter; software filtering above still drops
// every unrelated ID. Standard 11-bit IDs sit in bits [31:21];
// in twai_filter_config_t a mask bit of 1 means "don't care".
constexpr uint32_t CAN_ACCEPT_CODE = 0;
constexpr uint32_t CAN_ACCEPT_MASK = 0xFFFFFFFF;

// ---- Unified speed compensation ----

struct UnifiedSpeedCompensationPort {
  bool hasFusedSpeedLimit = false;
  int fusedSpeedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t speedOffsetRaw = 0;
};

struct SpeedLimitMonitor {
  can_frame dasStatusFrame{};
  bool hasDasStatusFrame = false;

  void update(const can_frame& frame) {
    if (frame.can_id == CAN_ID_DAS_STATUS && frame.can_dlc >= 1) {
      // Cache the latest 0x399 DAS_status frame for speed/AP state checks.
      dasStatusFrame = frame;
      hasDasStatusFrame = true;
      g_status.dasAutopilotState = static_cast<uint8_t>(frame.data[0] & 0x0F);
    }
  }

  bool getFusedSpeedLimitValue(int& limitValue) const {
    if (!hasDasStatusFrame || dasStatusFrame.can_dlc < 2) return false;
    const uint64_t fusedLimitRaw = (static_cast<uint64_t>(dasStatusFrame.data[1]) & 0x1F);
    // Low 5 bits encode fused speed limit in 5 kph units; 0 and 31 are invalid.
    if (fusedLimitRaw == 0 || fusedLimitRaw == 31) return false;
    limitValue = static_cast<int>(fusedLimitRaw * 5ULL);
    return true;
  }

  bool getAutopilotState(uint8_t& state) const {
    if (!hasDasStatusFrame || dasStatusFrame.can_dlc < 1) return false;
    state = static_cast<uint8_t>(dasStatusFrame.data[0] & 0x0F);
    return true;
  }
};

SpeedLimitMonitor speedLimitMonitor;

constexpr uint8_t NAG_BLOCK_NONE = 0;
constexpr uint8_t NAG_BLOCK_DISABLED = 1;
constexpr uint8_t NAG_BLOCK_MODE = 2;
constexpr uint8_t NAG_BLOCK_AP_STATE = 3;
constexpr uint8_t NAG_BLOCK_TARGET_HO = 4;
constexpr uint8_t NAG_BLOCK_REST = 5;
constexpr uint8_t NAG_BLOCK_STALE_AP = 6;
constexpr uint8_t NAG_BLOCK_STALE_STEER = 7;
constexpr uint8_t NAG_BLOCK_STEER_ANGLE = 8;
constexpr uint8_t NAG_BLOCK_HANDS_STATE = 9;
constexpr uint8_t NAG_BLOCK_TX = 10;
constexpr uint8_t NAG_BLOCK_DLC = 11;
constexpr uint32_t NAG_KILLER_MODE_C_STARTUP_A_MS = 5000UL;
constexpr float NAG_KILLER_MODE_A_TORQUE_NM = 1.80f;

static uint32_t nagKillerTargetIdForMode(uint8_t mode) {
  const uint8_t normalized = normalizeNagKillerMode(mode);
  return normalized == NAG_KILLER_MODE_C || normalized == NAG_KILLER_MODE_DOC
    ? CAN_ID_NAG_MODE_C_TARGET
    : CAN_ID_NAG_MODE_B_TARGET;
}

static bool nagKillerDocStrongHandsState(uint8_t state) {
  return state >= 3 && state <= 5;
}

static uint8_t nagKillerHandsLevelForTorque(float torqueNm) {
  const float mag = fabsf(torqueNm);
  if (mag >= 2.0f) return 2;
  if (mag >= 1.0f) return 1;
  return 0;
}

static void nagKillerRememberDocOutput(float torqueNm, uint8_t handsLevel) {
  nagKillerDocLastTorqueNm = torqueNm;
  nagKillerDocLastHandsLevel = handsLevel;
}

static int nagKillerTorqueCx100(uint8_t b2, uint8_t b3) {
  const uint16_t raw = static_cast<uint16_t>(((b2 & 0x0F) << 8) | b3);
  return static_cast<int>(raw) - NAG_KILLER_TORQUE_RAW_BASE;
}

static void nagKillerNmToBytes(float nm, uint8_t& b2, uint8_t& b3) {
  if (nm > 2.8f) nm = 2.8f;
  if (nm < -2.8f) nm = -2.8f;
  uint16_t raw = static_cast<uint16_t>((nm + 20.5f) * 100.0f + 0.5f);
  if (raw > NAG_KILLER_TORQUE_RAW_MAX) raw = NAG_KILLER_TORQUE_RAW_MAX;
  if (raw < NAG_KILLER_TORQUE_RAW_MIN) raw = NAG_KILLER_TORQUE_RAW_MIN;
  b2 = static_cast<uint8_t>((raw >> 8) & 0x0F);
  b3 = static_cast<uint8_t>(raw & 0xFF);
}

static float nagKillerModeBTorqueNm(const RuntimeConfig& cfg, uint8_t index) {
  switch (index & 0x03) {
    case 0: return nagKillerTorqueCx100ToNm(cfg.nagKillerBPos1Cx100);
    case 1: return nagKillerTorqueCx100ToNm(cfg.nagKillerBPos2Cx100);
    case 2: return -nagKillerTorqueCx100ToNm(cfg.nagKillerBNeg1Cx100);
    default: return -nagKillerTorqueCx100ToNm(cfg.nagKillerBNeg2Cx100);
  }
}

static float nagKillerModeCSweepTorqueNm(const RuntimeConfig& cfg, uint32_t now) {
  const float negNm = -nagKillerTorqueCx100ToNm(cfg.nagKillerCNegCx100);
  const float posNm = nagKillerTorqueCx100ToNm(cfg.nagKillerCPosCx100);
  const float spanNm = posNm - negNm;
  const uint32_t phase = now % 1000UL;
  if (phase < 500UL) {
    return negNm + (static_cast<float>(phase) / 500.0f) * spanNm;
  }
  return posNm - (static_cast<float>(phase - 500UL) / 500.0f) * spanNm;
}

static bool nagKillerApStateActive(uint8_t state) {
  return state >= 3 && state <= 6;
}

static uint8_t nagKillerChecksum(const can_frame& frame) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < 7; ++i) sum += frame.data[i];
  return static_cast<uint8_t>((sum + 0x73) & 0xFF);
}

static bool nagKillerApContextFresh(uint32_t now) {
  return nagKillerLastApMs != 0 && (now - nagKillerLastApMs) <= 1000UL;
}

static bool nagKillerSteeringContextFresh(uint32_t now) {
  return nagKillerLastSteeringMs != 0 && (now - nagKillerLastSteeringMs) <= 1000UL;
}

static void handleNagKillerContextFrame(const can_frame& frame) {
  const uint32_t now = millis();
  if (frame.can_id == CAN_ID_DAS_STATUS && frame.can_dlc >= 6) {
    const bool previousApFresh = nagKillerApContextFresh(now);
    const bool previousApActive = nagKillerApStateActive(nagKillerApState);
    nagKillerApState = static_cast<uint8_t>(frame.data[0] & 0x0F);
    const uint8_t handsOnState = static_cast<uint8_t>((frame.data[5] >> 2) & 0x0F);
    nagKillerLastApMs = now;
    const bool currentApActive = nagKillerApStateActive(nagKillerApState);
    if (currentApActive) {
      if (!previousApActive || !previousApFresh || nagKillerApActiveEnterMs == 0) {
        nagKillerApActiveEnterMs = now;
      }
    } else {
      nagKillerApActiveEnterMs = 0;
    }
    if (handsOnState != nagKillerHandsOnState) {
      const uint8_t previousHandsOnState = nagKillerHandsOnState;
      nagKillerPrevHandsOnState = previousHandsOnState;
      nagKillerHandsOnState = handsOnState;
      if (handsOnState == 2) {
        nagKillerState2EnterMs = now;
      } else {
        nagKillerState2EnterMs = 0;
      }
      if (handsOnState == 3) {
        nagKillerState3EnterMs = now;
      } else {
        nagKillerState3EnterMs = 0;
      }

      if (previousHandsOnState != 1 && handsOnState == 1) {
        nagKillerDocState1EnterMs = now;
        nagKillerDocState1HoldTorqueNm = nagKillerDocLastTorqueNm;
        nagKillerDocState1HoldHandsLevel = nagKillerDocLastHandsLevel;
      }
      if (handsOnState != 1) {
        nagKillerDocState1EnterMs = 0;
        nagKillerDocState1HoldTorqueNm = 0.0f;
        nagKillerDocState1HoldHandsLevel = 0;
      }

      if (previousHandsOnState != 2 && handsOnState == 2) {
        nagKillerDocState2EnterMs = now;
      }
      if (handsOnState != 2) {
        nagKillerDocState2EnterMs = 0;
        nagKillerDocState2HoldUntilMs = 0;
        nagKillerDocState2HoldTorqueNm = 0.0f;
        nagKillerDocState2HoldHandsLevel = 0;
        nagKillerDocState2Level2WasActive = false;
      }

      if (!nagKillerDocStrongHandsState(previousHandsOnState) && nagKillerDocStrongHandsState(handsOnState)) {
        nagKillerDocState3EnterMs = now;
      }
      if (!nagKillerDocStrongHandsState(handsOnState)) {
        nagKillerDocState3EnterMs = 0;
      }
    }
    g_status.nagKillerApState = nagKillerApState;
    g_status.nagKillerHandsOnState = nagKillerHandsOnState;
    return;
  }

  if (frame.can_id == CAN_ID_NAG_STEERING_ANGLE && frame.can_dlc >= 4) {
    const uint16_t raw14 = static_cast<uint16_t>(((static_cast<uint16_t>(frame.data[3]) << 8) |
                                                 frame.data[2]) & 0x3FFF);
    nagKillerSteeringAngleDeg = static_cast<float>(raw14) * 0.1f - 819.2f;
    nagKillerLastSteeringMs = now;
    g_status.nagKillerSteeringDegCx10 = static_cast<int>(nagKillerSteeringAngleDeg * 10.0f);
  }
}

static bool decideNagKillerDocTorque(uint32_t now, float& torqueNm, uint8_t& handsLevel) {
  if (!nagKillerSteeringContextFresh(now)) {
    g_status.nagKillerBlocked = NAG_BLOCK_STALE_STEER;
    return false;
  }

  const bool useNegative = nagKillerSteeringAngleDeg > 0.0f;
  if (nagKillerHandsOnState == 1) {
    if (nagKillerDocState1EnterMs != 0 && (now - nagKillerDocState1EnterMs) < 500UL) {
      torqueNm = nagKillerDocState1HoldTorqueNm;
      handsLevel = nagKillerDocState1HoldHandsLevel;
      nagKillerRememberDocOutput(torqueNm, handsLevel);
      return true;
    }
    g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
    return false;
  }

  if (nagKillerHandsOnState == 2) {
    if (nagKillerDocState2EnterMs == 0 || (now - nagKillerDocState2EnterMs) < 2000UL) {
      g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
      return false;
    }

    if (nagKillerDocState2HoldUntilMs != 0 && now < nagKillerDocState2HoldUntilMs) {
      torqueNm = nagKillerDocState2HoldTorqueNm;
      handsLevel = nagKillerDocState2HoldHandsLevel;
      nagKillerRememberDocOutput(torqueNm, handsLevel);
      return true;
    }

    const float minNm = useNegative ? -2.0f : 0.5f;
    const float maxNm = useNegative ? -0.5f : 2.0f;
    if (nagKillerDocMildTorqueNm < minNm || nagKillerDocMildTorqueNm > maxNm) {
      nagKillerDocMildTorqueNm = (minNm + maxNm) * 0.5f;
    }
    nagKillerWalkSeed = static_cast<uint16_t>(nagKillerWalkSeed * 1103U + 12345U);
    nagKillerDocMildTorqueNm += (static_cast<int>(nagKillerWalkSeed % 25U) - 12) * 0.01f;
    if (nagKillerDocMildTorqueNm < minNm) nagKillerDocMildTorqueNm = minNm;
    if (nagKillerDocMildTorqueNm > maxNm) nagKillerDocMildTorqueNm = maxNm;

    torqueNm = nagKillerDocMildTorqueNm;
    handsLevel = nagKillerHandsLevelForTorque(torqueNm);
    const bool level2Active = handsLevel >= 2;
    if (level2Active && !nagKillerDocState2Level2WasActive) {
      nagKillerDocState2HoldUntilMs = now + 1000UL;
      nagKillerDocState2HoldTorqueNm = torqueNm;
      nagKillerDocState2HoldHandsLevel = 2;
      handsLevel = 2;
    }
    nagKillerDocState2Level2WasActive = level2Active;
    nagKillerRememberDocOutput(torqueNm, handsLevel);
    return true;
  }

  if (nagKillerDocStrongHandsState(nagKillerHandsOnState)) {
    if (nagKillerDocState3EnterMs == 0 || (now - nagKillerDocState3EnterMs) < 1000UL) {
      g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
      return false;
    }

    const uint32_t activeMs = now - nagKillerDocState3EnterMs - 1000UL;
    const uint32_t phase = activeMs % 1500UL;
    float magNm = 2.1f;
    if (phase < 500UL) {
      magNm = (static_cast<float>(phase) / 500.0f) * 2.1f;
    }
    torqueNm = useNegative ? -magNm : magNm;
    handsLevel = nagKillerHandsLevelForTorque(torqueNm);
    nagKillerRememberDocOutput(torqueNm, handsLevel);
    return true;
  }

  g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
  return false;
}

static bool decideNagKillerTorque(const RuntimeConfig& cfg,
                                  uint32_t targetId,
                                  bool testOverride,
                                  uint8_t& outB2,
                                  uint8_t& outB3,
                                  uint8_t& outHandsOnLevel,
                                  bool& outForceHandsOnLevel) {
  const uint32_t now = millis();
  const uint8_t mode = normalizeNagKillerMode(cfg.nagKillerMode);
  g_status.nagKillerMode = mode;
  g_status.nagKillerBurstActive = 0;
  outHandsOnLevel = 0;
  outForceHandsOnLevel = false;

  if (testOverride) {
    float torqueNm = 0.0f;
    if (targetId == CAN_ID_NAG_MODE_B_TARGET) {
      if (now - nagKillerModeBLastChangeMs >= 200UL) {
        nagKillerModeBTorqueIndex = static_cast<uint8_t>((nagKillerModeBTorqueIndex + 1) & 0x03);
        nagKillerModeBLastChangeMs = now;
      }
      torqueNm = nagKillerModeBTorqueNm(cfg, nagKillerModeBTorqueIndex);
      outHandsOnLevel = 1;
    } else if (targetId == CAN_ID_NAG_MODE_C_TARGET) {
      torqueNm = nagKillerModeCSweepTorqueNm(cfg, now);
      outHandsOnLevel = nagKillerHandsLevelForTorque(torqueNm);
    } else {
      g_status.nagKillerBlocked = NAG_BLOCK_MODE;
      return false;
    }

    nagKillerNmToBytes(torqueNm, outB2, outB3);
    outForceHandsOnLevel = outHandsOnLevel > 0;
    g_status.nagKillerBurstActive = 1;
    g_status.nagKillerBlocked = NAG_BLOCK_NONE;
    return true;
  }

  if (!cfg.nagKillerEnabled) {
    g_status.nagKillerBlocked = NAG_BLOCK_DISABLED;
    return false;
  }

  if (!nagKillerApContextFresh(now)) {
    g_status.nagKillerBlocked = NAG_BLOCK_STALE_AP;
    return false;
  }
  if (!nagKillerApStateActive(nagKillerApState)) {
    g_status.nagKillerBlocked = NAG_BLOCK_AP_STATE;
    return false;
  }

  if (mode == NAG_KILLER_MODE_B) {
    uint32_t cycleMs = static_cast<uint32_t>(cfg.nagKillerBurstMs) + cfg.nagKillerPauseMs;
    if (cycleMs == 0) cycleMs = 1;
    const uint32_t phase = now % cycleMs;
    if (phase >= cfg.nagKillerBurstMs) {
      g_status.nagKillerBlocked = NAG_BLOCK_REST;
      return false;
    }

    if (now - nagKillerModeBLastChangeMs >= 200UL) {
      nagKillerModeBTorqueIndex = static_cast<uint8_t>((nagKillerModeBTorqueIndex + 1) & 0x03);
      nagKillerModeBLastChangeMs = now;
    }
    nagKillerNmToBytes(nagKillerModeBTorqueNm(cfg, nagKillerModeBTorqueIndex), outB2, outB3);
    outHandsOnLevel = 1;
    outForceHandsOnLevel = true;
    g_status.nagKillerBurstActive = 1;
    g_status.nagKillerBlocked = NAG_BLOCK_NONE;
    return true;
  }

  if (mode == NAG_KILLER_MODE_C) {
    if (nagKillerApActiveEnterMs != 0 &&
        (now - nagKillerApActiveEnterMs) < NAG_KILLER_MODE_C_STARTUP_A_MS) {
      nagKillerNmToBytes(NAG_KILLER_MODE_A_TORQUE_NM, outB2, outB3);
      outHandsOnLevel = 1;
      outForceHandsOnLevel = true;
      g_status.nagKillerBurstActive = 1;
      g_status.nagKillerBlocked = NAG_BLOCK_NONE;
      return true;
    }

    if (!nagKillerSteeringContextFresh(now)) {
      g_status.nagKillerBlocked = NAG_BLOCK_STALE_STEER;
      return false;
    }
    if (fabsf(nagKillerSteeringAngleDeg) > 5.0f) {
      g_status.nagKillerBlocked = NAG_BLOCK_STEER_ANGLE;
      return false;
    }

    float torqueNm = 0.0f;
    uint8_t handsLevel = 0;
    const float cNegMaxNm = nagKillerTorqueCx100ToNm(cfg.nagKillerCNegCx100);
    const float cPosMaxNm = nagKillerTorqueCx100ToNm(cfg.nagKillerCPosCx100);
    if (nagKillerHandsOnState == 1) {
      g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
      return false;
    } else if (nagKillerHandsOnState == 2) {
      if (nagKillerState2EnterMs == 0 || (now - nagKillerState2EnterMs) < 2000UL) {
        g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
        return false;
      }
      nagKillerWalkSeed = static_cast<uint16_t>(nagKillerWalkSeed * 1103U + 12345U);
      const float delta = (static_cast<int>(nagKillerWalkSeed & 0x1F) - 16) * 0.05f;
      const bool useNegative = nagKillerSteeringAngleDeg > 0.0f;
      const float maxMag = useNegative ? cNegMaxNm : cPosMaxNm;
      const float minMag = maxMag < 0.5f ? 0.0f : 0.5f;
      float mag = fabsf(nagKillerLastModeCTorqueNm) + delta;
      if (mag < minMag) mag = minMag;
      if (mag > maxMag) mag = maxMag;
      torqueNm = useNegative ? -mag : mag;
      nagKillerLastModeCTorqueNm = torqueNm;
      handsLevel = fabsf(torqueNm) >= 1.0f ? 1 : 0;
    } else if (nagKillerHandsOnState == 3) {
      if (nagKillerState3EnterMs == 0 || (now - nagKillerState3EnterMs) < 1000UL) {
        g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
        return false;
      }
      torqueNm = nagKillerModeCSweepTorqueNm(cfg, now - nagKillerState3EnterMs - 1000UL);
      nagKillerLastModeCTorqueNm = torqueNm;
      handsLevel = fabsf(torqueNm) >= 1.0f ? 1 : 0;
    } else {
      g_status.nagKillerBlocked = NAG_BLOCK_HANDS_STATE;
      return false;
    }

    nagKillerNmToBytes(torqueNm, outB2, outB3);
    outHandsOnLevel = handsLevel;
    outForceHandsOnLevel = handsLevel > 0;
    g_status.nagKillerBlocked = NAG_BLOCK_NONE;
    return true;
  }

  if (mode == NAG_KILLER_MODE_DOC) {
    float torqueNm = 0.0f;
    uint8_t handsLevel = 0;
    if (!decideNagKillerDocTorque(now, torqueNm, handsLevel)) return false;
    nagKillerNmToBytes(torqueNm, outB2, outB3);
    outHandsOnLevel = handsLevel;
    outForceHandsOnLevel = true;
    g_status.nagKillerBurstActive = 1;
    g_status.nagKillerBlocked = NAG_BLOCK_NONE;
    return true;
  }

  g_status.nagKillerBlocked = NAG_BLOCK_MODE;
  return false;
}

static void handleNagKillerTargetFrame(const can_frame& frame, const RuntimeConfig& cfg) {
  const uint8_t mode = normalizeNagKillerMode(cfg.nagKillerMode);
  const uint32_t targetId = nagKillerTargetIdForMode(mode);
  const bool test052 = cfg.nagKillerTest052Enabled && frame.can_id == CAN_ID_NAG_MODE_B_TARGET;
  const bool test370 = cfg.nagKillerTest370Enabled && frame.can_id == CAN_ID_NAG_MODE_C_TARGET;
  const bool testOverride = test052 || test370;
  const uint32_t activeTargetId = testOverride ? frame.can_id : targetId;
  g_status.nagKillerMode = mode;
  g_status.nagKillerTargetId = activeTargetId;
  g_status.nagKillerActive = 0;

  if (frame.can_id != activeTargetId) return;
  nagKillerLastRxMs = millis();
  g_status.nagKillerRxCount++;

  if (frame.can_dlc < 8) {
    g_status.nagKillerBlocked = NAG_BLOCK_DLC;
    return;
  }

  const uint8_t targetHandsOn = static_cast<uint8_t>((frame.data[4] >> 6) & 0x03);
  const int realTorque = nagKillerTorqueCx100(frame.data[2], frame.data[3]);
  g_status.nagKillerTargetHandsOn = targetHandsOn;
  g_status.nagKillerRealTorqueCx100 = realTorque;
  if (!testOverride &&
      ((mode == NAG_KILLER_MODE_DOC && targetHandsOn != 0) ||
       (mode != NAG_KILLER_MODE_DOC && targetHandsOn > 1))) {
    g_status.nagKillerBlocked = NAG_BLOCK_TARGET_HO;
    return;
  }

  uint8_t b2 = 0;
  uint8_t b3 = 0;
  uint8_t handsOnLevel = 0;
  bool forceHandsOnLevel = false;
  if (!decideNagKillerTorque(cfg, activeTargetId, testOverride, b2, b3, handsOnLevel, forceHandsOnLevel)) return;

  can_frame echo = frame;
  echo.can_id = activeTargetId;
  echo.can_dlc = 8;
  echo.data[2] = static_cast<uint8_t>((echo.data[2] & 0xF0) | (b2 & 0x0F));
  echo.data[3] = b3;
  if (forceHandsOnLevel) {
    echo.data[4] = static_cast<uint8_t>((echo.data[4] & ~0xC0) | ((handsOnLevel & 0x03) << 6));
  }
  echo.data[6] = static_cast<uint8_t>((echo.data[6] & 0xF0) |
                                      (((echo.data[6] & 0x0F) + 1) & 0x0F));
  echo.data[7] = nagKillerChecksum(echo);

  if (nagKillerSendFrame(echo)) {
    nagKillerLastTxMs = millis();
    g_status.nagKillerTxCount++;
    g_status.nagKillerLastTorqueCx100 = nagKillerTorqueCx100(b2, b3);
    g_status.nagKillerSetHandsOn = forceHandsOnLevel ? handsOnLevel : 0;
    g_status.nagKillerActive = 1;
    g_status.nagKillerBlocked = NAG_BLOCK_NONE;
  } else {
    g_status.nagKillerTxFail++;
    g_status.nagKillerBlocked = NAG_BLOCK_TX;
  }
}

#ifdef ENABLE_CANB_MCP2515
static void handleRearFogPedalBrakeEdge(bool brakeActive, const RuntimeConfig& cfg);
static void handleRearFogBrakeLampState(bool brakeActive, const RuntimeConfig& cfg);
static void handleRearFogCanADecelFrame(const can_frame& frame, const RuntimeConfig& cfg);
#endif

// ---- Bit-level helpers ----

inline uint8_t readMuxID(const can_frame& frame) { return frame.data[0] & 0x07; }

inline void setSpeedProfileV12V13(can_frame& frame, int profile) {
  frame.data[6] &= ~0x06;
  frame.data[6] |= (profile << 1);
}

inline void setBit(can_frame& frame, int bit, bool value) {
  int byteIndex = bit / 8;
  int bitIndex = bit % 8;
  uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
  if (value) frame.data[byteIndex] |= mask;
  else frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
}

inline int clampOffsetKph(int value) { return std::max(std::min(value, MAX_SPEED_OFFSET_KPH), 0); }

inline uint8_t encodeSpeedOffsetRawPct4(int offsetKph, int fusedSpeedLimitKph) {
  if (fusedSpeedLimitKph <= 0) return 0;
  // PCT4 wire encoding: 1 percent = 4 raw units.
  int pct = (offsetKph * 100 + fusedSpeedLimitKph / 2) / fusedSpeedLimitKph;
  pct = std::max(std::min(pct, MAX_SPEED_OFFSET_PCT), 0);
  return static_cast<uint8_t>(pct * OFFSET_PCT4_RAW_PER_PCT);
}

inline int getTargetSpeedForLimit(int fusedSpeedLimitValue, const RuntimeConfig& cfg) {
  if (fusedSpeedLimitValue < 60)  return cfg.targetBelow60;
  if (fusedSpeedLimitValue < 70)  return cfg.target60;
  if (fusedSpeedLimitValue < 80)  return cfg.target70;
  if (fusedSpeedLimitValue < 90)  return cfg.target80;
  if (fusedSpeedLimitValue < 100) return cfg.target90;
  if (fusedSpeedLimitValue < 120) return cfg.target100;
  if (fusedSpeedLimitValue < 140) return cfg.target120;
  return fusedSpeedLimitValue;
}

inline uint8_t readSpeedOffsetRaw(const can_frame& frame) {
  return static_cast<uint8_t>(((frame.data[1] & 0x3F) << 2) | ((frame.data[0] >> 6) & 0x03));
}

inline void writeSpeedOffsetRaw(can_frame& frame, uint8_t raw) {
  frame.data[0] = static_cast<uint8_t>((frame.data[0] & ~0xC0) | ((raw & 0x03) << 6));
  // 0x3FD mux 2 stores offset raw in data[0] bits 6-7 and data[1] bits 0-5.
  frame.data[1] = static_cast<uint8_t>((frame.data[1] & ~0x3F) | (raw >> 2));
}

struct OffsetSlewLimiter {
  uint8_t lastRaw = 0;
  uint32_t lastSentMs = 0;

  uint8_t apply(uint8_t targetRaw, uint8_t slewPctPerSec) {
    uint8_t shapedRaw = targetRaw;
    const uint32_t now = millis();

    if (targetRaw < lastRaw && lastSentMs != 0) {
      // Only limit downward offset changes; upward changes pass immediately.
      const uint32_t rateRawPerSec =
        static_cast<uint32_t>(slewPctPerSec) * OFFSET_PCT4_RAW_PER_PCT;
      const uint32_t elapsedMs = now - lastSentMs;
      const uint32_t maxDrop = (rateRawPerSec * elapsedMs + 500U) / 1000U;
      const uint8_t floorRaw = lastRaw > maxDrop ? static_cast<uint8_t>(lastRaw - maxDrop) : 0;
      if (targetRaw < floorRaw) shapedRaw = floorRaw;
    }

    lastRaw = shapedRaw;
    lastSentMs = now;
    return shapedRaw;
  }
};

// ---- HW3 car handler ----

struct HW3Handler {
  int speedProfile = 1;
  UnifiedSpeedCompensationPort unifiedSpeedCompensation{};
  OffsetSlewLimiter offsetSlewLimiter{};

  void refreshUnifiedSpeedCompensation(const RuntimeConfig& cfg) {
    unifiedSpeedCompensation.hasFusedSpeedLimit = false;
    // Refresh once per relevant CAN A frame; mux 2 then writes the cached offset.
    unifiedSpeedCompensation.fusedSpeedLimitKph = 0;
    unifiedSpeedCompensation.targetSpeedKph = 0;
    unifiedSpeedCompensation.offsetKph = 0;
    unifiedSpeedCompensation.speedOffsetRaw = 0;
    if (cfg.autoSpeedOffsetEnabled) {
      int fusedSpeedLimitValue = 0;
      if (speedLimitMonitor.getFusedSpeedLimitValue(fusedSpeedLimitValue)) {
        unifiedSpeedCompensation.hasFusedSpeedLimit = true;
        unifiedSpeedCompensation.fusedSpeedLimitKph = fusedSpeedLimitValue;
        unifiedSpeedCompensation.targetSpeedKph = getTargetSpeedForLimit(fusedSpeedLimitValue, cfg);
        int desiredOffsetKph = unifiedSpeedCompensation.targetSpeedKph > fusedSpeedLimitValue
          ? (unifiedSpeedCompensation.targetSpeedKph - fusedSpeedLimitValue)
          : 0;
        // WebUI target speed is still protected by kph and percentage caps.
        unifiedSpeedCompensation.offsetKph = clampOffsetKph(desiredOffsetKph);
        unifiedSpeedCompensation.speedOffsetRaw =
          fusedSpeedLimitValue < LOW_SPEED_MAX_PCT_LIMIT_KPH
            ? cfg.lowSpeedMaxPctRaw
            : encodeSpeedOffsetRawPct4(
                unifiedSpeedCompensation.offsetKph,
                unifiedSpeedCompensation.fusedSpeedLimitKph);
      }
    }

    g_status.fusedLimitKph = unifiedSpeedCompensation.fusedSpeedLimitKph;
    g_status.targetSpeedKph = unifiedSpeedCompensation.targetSpeedKph;
    g_status.offsetKph = unifiedSpeedCompensation.offsetKph;
  }

  void handelMessage(can_frame& frame, const RuntimeConfig& cfg) {
#ifdef ENABLE_CANB_MCP2515
    handleRearFogCanADecelFrame(frame, cfg);
#endif
    if (frame.can_id == CAN_ID_BRAKE_PEDAL) {
      if (frame.can_dlc < 4) return;
      return;
    }

    if (frame.can_id == CAN_ID_FOLLOW_DISTANCE) {
      if (frame.can_dlc < 6) return;
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      // Reuse the 0x3F8 follow-distance setting as the driving style selector.
      switch (followDistance) {
        case 1: speedProfile = 2; break;
        case 2: speedProfile = 1; break;
        case 3: speedProfile = 0; break;
      }
      return;
    }
    if (frame.can_id == CAN_ID_AP_CONTROL) {
      if (frame.can_dlc < 8) return;
      auto index = readMuxID(frame);
      if (index == 0 && cfg.fsdEnabled) {
        setBit(frame, 46, true);
        // 0x3FD mux 0 enables the FSD/AP bit and writes the current drive style.
        setSpeedProfileV12V13(frame, speedProfile);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        if (cfg.cabinCameraDisableEnabled) setBit(frame, 43, false);
        if (cfg.cabinCameraTelemetryDisableEnabled) setBit(frame, 48, false);
        // 0x3FD mux 1 keeps bit 19 clear and can optionally clear cabin camera bits.
        twai_send(frame);
      }
      if (index == 2 && cfg.fsdEnabled) {
        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
          ? unifiedSpeedCompensation.speedOffsetRaw
          : readSpeedOffsetRaw(frame);
        // 0x3FD mux 2 writes speed offset, or preserves stock offset if no valid limit.
        speedOffsetRaw = offsetSlewLimiter.apply(speedOffsetRaw, cfg.slewPctPerSec);
        g_status.offsetRaw = speedOffsetRaw;
        writeSpeedOffsetRaw(frame, speedOffsetRaw);
        twai_send(frame);
      }
    }
  }
};

HW3Handler handler;

// ============================================================================
// CAN B (MCP2515) secondary bus -- basic comms + non-blocking service-mode burst
// ============================================================================
#ifdef ENABLE_CANB_MCP2515

static MCP2515 canb(MCP2515_CS);
static bool canbReady = false;
static uint8_t canbHardwareFilterMode = CANB_FILTER_ALL;
static uint32_t canbRxCount = 0;
static uint32_t canbTxCount = 0;
static uint32_t canbTxFailCount = 0;
static uint32_t canbLastId = 0;
static uint32_t canbRxOverflowCount = 0;

// 0x339 VCSEC service-mode burst state (RAM-only, OFF on boot). Each toggle
// queues 4 frames at 10ms spacing, scheduled with millis() -- never delay().
// volatile: setCanBServiceMode() may run on the WebUI task (core 0) while
// serviceCanBScheduledTx() runs on the CAN loop (core 1).
static volatile bool canbServiceModeActive = false;
static volatile uint8_t canbServiceBurstRemaining = 0;
static volatile uint8_t canbServiceBurstByte5 = 0;
static volatile uint32_t canbLastServiceBurstMs = 0;

// CAN B feature IDs:
//   0x249: SCCMLeftStalk command. status=1 PULL triggers high-light strobe;
//          injected strobe uses only status=1 PULL and status=0 idle.
//   0x229: SCCM_rightStalk. Captured D/R request source on bus2; optional
//          experimental scroll-to-gear injection uses this frame.
//   0x273: body lighting frame used for brake/fog context and rear-fog strobe.
constexpr uint32_t CANB_ID_SCCM_RIGHT_STALK = 0x229;
constexpr uint32_t CANB_ID_STW_ACTN_RQ = 0x249;
constexpr uint32_t CANB_ID_BODY_LIGHTING = 0x273;
constexpr uint32_t CANB_ID_VCSEC_STATUS = 0x339;
// 0x3C2 VCLEFT_switchStatus: byte0 bit3 = hazardButtonPressed on mux0;
// mux1 data[2]/data[3] are left/right scroll ticks (6-bit signed).
constexpr uint32_t CANB_ID_VCLEFT_SWITCH = 0x3C2;
constexpr uint8_t HIGH_BEAM_STROBE_PULSES = 8;
constexpr uint8_t REAR_FOG_PEDAL_STROBE_PULSES = 3;
constexpr uint8_t REAR_FOG_BODY_STROBE_PULSES = 6;
constexpr uint8_t REVERSE_STROBE_PULSES = 4;
constexpr uint8_t REAR_FOG_PRIORITY_PEDAL = 1;
constexpr uint8_t REAR_FOG_PRIORITY_BODY = 2;
constexpr uint8_t REAR_FOG_PRIORITY_REVERSE = 3;
constexpr uint16_t HIGH_BEAM_STROBE_INTERVAL_MS = 75;
constexpr uint16_t HIGH_BEAM_STROBE_RESEND_MS = 45;
constexpr uint16_t REAR_FOG_STROBE_INTERVAL_MS = 135;
// Hazard (0x3C2 bit3) is a momentary TOGGLE button: one click toggles hazards
// on/off. Reverse = one click ON -> hold REVERSE_HAZARD_ON_MS (car flashes
// continuously) -> one click OFF. (Pulsing it would just toggle on/off/on/off.)
constexpr uint16_t REVERSE_HAZARD_CLICK_MS = 200;   // length of one button "press"
constexpr uint16_t REVERSE_HAZARD_ON_MS = 2500;     // hazards stay on ~2.5s of continuous flashing
constexpr uint16_t REAR_FOG_MILD_DECEL_HOLD_MS = 300;
constexpr uint16_t REAR_FOG_HARD_DECEL_HOLD_MS = 150;
constexpr uint16_t REAR_FOG_DECEL_RECENT_MS = 800;
constexpr uint16_t SCROLL_GEAR_BRAKE_HOLD_MS = 150;
constexpr uint16_t SCROLL_GEAR_FRAME_INTERVAL_MS = 50;  // ~20Hz, matches/dominates real 0x229 (10Hz)
constexpr uint16_t SCROLL_GEAR_IDLE_FRAMES = 3;
constexpr uint16_t SCROLL_GEAR_STATUS_FRAMES = 6;        // sustain detent longer (manual D=5,R=~7 frames)
constexpr uint16_t SCROLL_GEAR_COOLDOWN_MS = 400;  // responsive R<->D; one shift takes ~0.1-0.3s
constexpr uint16_t DND_SCROLL_STEP_MS = 50;
constexpr uint16_t DND_SCROLL_CACHE_MAX_AGE_MS = 1000;
constexpr uint16_t DND_VOLUME_AUTO_MIN_MS = 1000;
constexpr uint16_t DND_VOLUME_AUTO_MAX_MS = 5000;
constexpr uint16_t LOCK_SLEEP_STABLE_MS = 5000;
constexpr uint16_t LOCK_SLEEP_RECENT_ACTIVITY_MS = 5000;
constexpr uint16_t LOCK_SLEEP_OCCUPANCY_FRESH_MS = 30000;
constexpr float SCROLL_GEAR_MAX_SPEED_KPH = 2.0f;
constexpr float LOCK_SLEEP_MAX_SPEED_KPH = 1.0f;
constexpr float REAR_FOG_MILD_DECEL_THRESHOLD = -0.80f;
constexpr float REAR_FOG_HARD_DECEL_THRESHOLD = -2.50f;
constexpr float REAR_FOG_VERY_HARD_DECEL_THRESHOLD = -3.50f;
constexpr uint16_t HIGH_BEAM_DOUBLE_PULL_WINDOW_MS = 1200;
constexpr uint8_t STALK_STATUS_IDLE = 0;
constexpr uint8_t STALK_STATUS_PULL = 1;
constexpr uint8_t STALK_TURN_IDLE = 0;
constexpr uint8_t STALK_TURN_HAZARD = 6;
constexpr uint8_t RIGHT_STALK_IDLE = 0;
constexpr uint8_t RIGHT_STALK_R_STAGE1 = 1;
constexpr uint8_t RIGHT_STALK_R_STAGE2 = 2;
constexpr uint8_t RIGHT_STALK_D_STAGE1 = 3;
constexpr uint8_t RIGHT_STALK_D_STAGE2 = 4;
constexpr uint8_t GEAR_R = 2;
constexpr uint8_t GEAR_N = 3;
constexpr uint8_t GEAR_D = 4;
constexpr uint8_t DAS_AP_STATE_DISABLED = 0;
constexpr uint8_t DAS_AP_STATE_UNAVAILABLE = 1;
constexpr uint8_t DAS_AP_STATE_AVAILABLE = 2;
constexpr uint8_t VCLEFT_HAZARD_BUTTON_MASK = 0x08;  // 0x3C2 byte0 bit3
// 0x3C2 is multiplexed by byte0 bits0..1. Capture: mux0 carries hazardButton +
// counter/CRC (data[3] is 0x55 filler); mux1 carries rightScrollTicks in data[3]
// (6-bit signed, idle 0). Reading data[3] without checking the mux misreads
// mux0's 0x55 as +21 ticks.
constexpr uint8_t VCLEFT_MUX_MASK = 0x03;
constexpr uint8_t VCLEFT_MUX_HAZARD = 0x00;  // mux0: hazard button
constexpr uint8_t VCLEFT_MUX_SCROLL = 0x01;  // mux1: right scroll ticks
constexpr uint8_t DND_ACTION_NONE = 0;
constexpr uint8_t DND_ACTION_VOLUME = 1;
constexpr uint8_t DND_BLOCK_NONE = 0;
constexpr uint8_t DND_BLOCK_DISABLED = 1;
constexpr uint8_t DND_BLOCK_CANB = 2;
constexpr uint8_t DND_BLOCK_NO_CACHE = 3;
constexpr uint8_t LOCK_SLEEP_BLOCK_NONE = 0;
constexpr uint8_t LOCK_SLEEP_BLOCK_UNLOCKED = 1;
constexpr uint8_t LOCK_SLEEP_BLOCK_CABIN_ACTIVE = 2;
constexpr uint8_t LOCK_SLEEP_BLOCK_STABILIZING = 3;
constexpr int8_t LOCK_SLEEP_SEAT_UNKNOWN = -1;
constexpr int8_t LOCK_SLEEP_SEAT_EMPTY = 0;
constexpr int8_t LOCK_SLEEP_SEAT_OCCUPIED = 1;
constexpr uint8_t REAR_FOG_MASK = 0x80;
constexpr uint8_t REAR_FOG_OFF = 0x10;
constexpr uint8_t REAR_FOG_ON = 0x90;
static can_frame canbLastStwActnRqFrame{};
static bool canbHasLastStwActnRqFrame = false;
static can_frame canbLastRightStalkFrame{};
static bool canbHasLastRightStalkFrame = false;
static volatile uint8_t rightStalkTxCounter = 0;
static can_frame canbLastBodyLightingFrame{};
static bool canbHasLastBodyLightingFrame = false;
static can_frame canbLastVcleftSwitchFrame{};
static bool canbHasLastVcleftSwitchFrame = false;
static can_frame canbLastVcleftMux0Frame{};      // last 0x3C2 mux0 frame (hazard + counter/CRC)
static bool canbHasLastVcleftMux0Frame = false;
static can_frame canbLastVcleftMux1Frame{};      // last 0x3C2 mux1 frame (scroll wheels)
static bool canbHasLastVcleftMux1Frame = false;
static uint32_t canbLastVcleftMux1Ms = 0;
static volatile uint8_t highBeamStalkLastCounter = 0;

static volatile bool highBeamStrobeActive = false;
// Non-blocking light-effect state. loop() only emits frames when millis() reaches
// the next phase edge, so CAN A / FSD activation never waits on a delay().
static volatile bool highBeamStrobeManualTrigger = false;
static volatile bool highBeamStrobeOutputOn = false;
static volatile uint8_t highBeamStrobePulsesRemaining = 0;
static volatile uint32_t highBeamStrobeLastToggleMs = 0;
static volatile uint32_t highBeamStrobeLastSendMs = 0;
static bool highBeamLastPullDown = false;
static uint8_t highBeamPullCount = 0;
static uint32_t highBeamLastPullMs = 0;
static volatile bool rearFogBrakeStrobeActive = false;
static volatile bool rearFogBrakeStrobeManualTrigger = false;
static volatile bool rearFogBrakeStrobeOutputOn = false;
static volatile uint8_t rearFogBrakeStrobePulsesRemaining = 0;
static volatile uint8_t rearFogBrakeStrobePriority = 0;
static volatile uint32_t rearFogBrakeStrobeLastToggleMs = 0;
static bool rearFogLastPedalBrakeActive = false;
static bool rearFogLastBrakeActive = false;
static uint32_t rearFogRecentPedalBrakeUntilMs = 0;
static uint32_t rearFogRecentBrakeLampUntilMs = 0;
static uint32_t rearFogRecentBrakeTorqueUntilMs = 0;
static uint32_t rearFogRecentRegenUntilMs = 0;
static uint32_t rearFogRecentNegTorqueUntilMs = 0;
static uint32_t rearFogRecentSpeedFallingUntilMs = 0;
static uint32_t rearFogMildDecelStartMs = 0;
static uint32_t rearFogHardDecelStartMs = 0;
static bool rearFogMildDecelTriggered = false;
static bool rearFogHardDecelTriggered = false;
static float rearFogLastVehicleSpeedKph = -1.0f;
static volatile bool reverseStrobeActive = false;
static volatile uint8_t reverseStrobePhase = 0;       // 0 idle, 1 ON-press, 2 hold(flashing), 3 OFF-press
static volatile uint32_t reverseStrobePhaseEnd = 0;
static volatile bool reverseHazardLatchedOn = false;  // true after our ON click until the matching OFF click is sent
static uint8_t lastDIGearRaw = 0;
static volatile bool g_brakePedalActive = false;
static volatile uint32_t g_brakePedalActiveSinceMs = 0;
static volatile float g_vehicleSpeedKph = 0.0f;
static volatile bool g_vehicleSpeedValid = false;
static volatile bool scrollGearShiftActive = false;
static volatile uint8_t scrollGearTargetGear = 0;
static volatile uint8_t scrollGearPhaseIndex = 0;
static volatile uint8_t scrollGearIdleFramesRemaining = 0;
static volatile uint32_t scrollGearNextTxMs = 0;
static volatile uint32_t scrollGearCooldownUntilMs = 0;
static volatile bool scrollGearLatched = false;
static volatile uint8_t scrollGearLastBlocked = 0;
static volatile bool dndActionActive = false;
static volatile uint8_t dndActionType = DND_ACTION_NONE;
static volatile uint8_t dndActionStep = 0;
static volatile uint32_t dndNextStepMs = 0;
static uint32_t dndLastTriggerMs = 0;
static uint32_t dndVolumeNextAutoMs = 0;

// CAN B read budget per loop pass -- bounded so it can never starve CAN A.
constexpr uint8_t CANB_RX_SCAN_LIMIT = 4;
constexpr uint8_t CANB_RX_SCAN_LIMIT_ACTIVE = 24;
constexpr uint32_t CANB_RX_DRAIN_TIME_US = 900;

static void setupCanB();
static bool applyCanBFilters(uint8_t mode);
static bool canb_recv(can_frame& frame);
static bool canb_send(const can_frame& frame);
static void drainCanBWithBudget();
static void updateCanBErrorStatus();
static void handleCanBFrame(const can_frame& frame);
static void setCanBServiceMode(bool enabled);
static void serviceCanBScheduledTx();
static void serviceHighBeamStrobe(const RuntimeConfig& cfg);
static void serviceReverseStrobe(const RuntimeConfig& cfg);
static void serviceRearFogBrakeStrobe(const RuntimeConfig& cfg);
static void serviceScrollGearShift(const RuntimeConfig& cfg);
static void handleDndHandsOnFrame(const can_frame& frame);
static void serviceDndScrollAction(const RuntimeConfig& cfg);
static void serviceDndVolumeAuto(const RuntimeConfig& cfg);
static void handleVcleftSwitchFrame(const can_frame& frame, const RuntimeConfig& cfg, bool cacheHazardFrame);
static void requestLockDeepSleep(const can_frame& frame, uint8_t source);
static void serviceLockDeepSleep();

static void setupCanB() {
  canbReady = false;
  pinMode(MCP2515_INT, INPUT_PULLUP);

  // Hard reset the MCP2515 via its RST line: high / low / high.
  pinMode(MCP2515_RST, OUTPUT);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);
  digitalWrite(MCP2515_RST, LOW);
  delay(10);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);

  SPI.begin(MCP2515_SCK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);

  // reset()/setNormalMode() return types vary across library versions, so we
  // call them as statements and only gate on setBitrate(), which is the
  // meaningful failure point. A failure here just leaves canbReady = false; it
  // must never block or disturb CAN A / the FSD pipeline.
  canb.reset();
  delay(10);
  if (canb.setBitrate(CAN_500KBPS, MCP2515_CLOCK) != MCP2515::ERROR_OK) return;
  if (!applyCanBFilters(configSnapshot().canbFilterMode)) return;
  canbReady = true;
}

static bool applyCanBFilters(uint8_t mode) {
  mode = normalizeCanBFilterMode(mode);
  if (mode == CANB_FILTER_FEATURE) {
    // Coarse feature filter for the current CANA feature set. MCP2515 only has
    // six filters, so the masks group nearby IDs while still excluding most
    // unrelated 11-bit traffic.
    if (canb.setFilterMask(MCP2515::MASK0, false, 0x42F) != MCP2515::ERROR_OK) return false;
    // Covers: 0x052, 0x082, 0x3C2.
    if (canb.setFilter(MCP2515::RXF0, false, 0x002) != MCP2515::ERROR_OK) return false;
    // Covers: 0x129, 0x229, 0x339.
    if (canb.setFilter(MCP2515::RXF1, false, 0x029) != MCP2515::ERROR_OK) return false;

    if (canb.setFilterMask(MCP2515::MASK1, false, 0x60F) != MCP2515::ERROR_OK) return false;
    // Covers: 0x229, 0x249, 0x339, 0x399.
    if (canb.setFilter(MCP2515::RXF2, false, 0x209) != MCP2515::ERROR_OK) return false;
    // Covers: 0x273, 0x313.
    if (canb.setFilter(MCP2515::RXF3, false, 0x203) != MCP2515::ERROR_OK) return false;
    // Covers: 0x370.
    if (canb.setFilter(MCP2515::RXF4, false, 0x200) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF5, false, 0x200) != MCP2515::ERROR_OK) return false;
  } else {
    if (canb.setFilterMask(MCP2515::MASK0, false, 0x000) != MCP2515::ERROR_OK) return false;
    // All-pass mode is useful for capture and unknown-ID debugging.
    if (canb.setFilter(MCP2515::RXF0, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF1, false, 0x000) != MCP2515::ERROR_OK) return false;

    if (canb.setFilterMask(MCP2515::MASK1, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF2, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF3, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF4, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF5, false, 0x000) != MCP2515::ERROR_OK) return false;
  }

  if (canb.setNormalMode() != MCP2515::ERROR_OK) return false;
  canbHardwareFilterMode = mode;
  return true;
}

static bool canb_recv(can_frame& frame) {
  if (!canbReady) return false;
  if (canb.readMessage(&frame) != MCP2515::ERROR_OK) return false;

  // Stage 1: ignore extended and remote frames; clamp DLC defensively.
  if (frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) return false;
  frame.can_id &= CAN_SFF_MASK;
  if (frame.can_dlc > 8) frame.can_dlc = 8;

  canbRxCount++;
  canbLastId = frame.can_id;
  g_status.canbRx = canbRxCount;
  g_status.canbLastId = canbLastId;
  recordCanFrame(frame, 'R', 2);
  return true;
}

static bool canb_send(const can_frame& frame) {
  if (canTxInhibitedForSleep) return false;
  if (!canbReady) return false;
  if (frame.can_dlc > 8) return false;
  // One short retry on a busy/failed mailbox; no blocking delay so a stuck
  // CAN B can never stall CAN A.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (canb.sendMessage(&frame) == MCP2515::ERROR_OK) {
      canbTxCount++;
      g_status.canbTx = canbTxCount;
      recordCanFrame(frame, 'T', 2);
      return true;
    }
  }
  canbTxFailCount++;
  g_status.canbTxFail = canbTxFailCount;
  return false;
}

static void updateCanBErrorStatus() {
  if (!canbReady) return;
  const uint8_t flags = canb.getErrorFlags();
  g_status.canbErrorFlags = flags;
  if (flags & (MCP2515::EFLG_RX0OVR | MCP2515::EFLG_RX1OVR)) {
    canbRxOverflowCount++;
    g_status.canbRxOverflowCount = canbRxOverflowCount;
    canb.clearRXnOVRFlags();
  }
}

static uint8_t readStalkStatus(const can_frame& frame) {
  if (frame.can_dlc < 2) return STALK_STATUS_IDLE;
  return static_cast<uint8_t>((frame.data[1] >> 4) & 0x03);
}

static uint8_t stalkCrc249(uint8_t counter, uint8_t status) {
  static const uint8_t base[16] = {
    0x9B, 0xE8, 0x2A, 0xD3, 0xD3, 0x83, 0x4C, 0x5E,
    0x3F, 0x5E, 0xE2, 0x28, 0x3A, 0x13, 0xAF, 0xCE
  };
  static const uint8_t offset[8] = {0x00, 0x76, 0xEC, 0x00, 0xF7, 0x00, 0x00, 0x00};
  return static_cast<uint8_t>(base[counter & 0x0F] ^ offset[status & 0x07]);
}

static can_frame highBeamFrame(uint8_t status) {
  can_frame f = {};
  f.can_id = CANB_ID_STW_ACTN_RQ;
  f.can_dlc = 4;
  const uint8_t counter = static_cast<uint8_t>((highBeamStalkLastCounter + 1) & 0x0F);
  f.data[0] = stalkCrc249(counter, status);
  f.data[1] = static_cast<uint8_t>(((status & 0x07) << 4) | counter);
  highBeamStalkLastCounter = counter;
  return f;
}

static void setBrakePedalActive(bool active) {
  const uint32_t now = millis();
  if (active && !g_brakePedalActive) g_brakePedalActiveSinceMs = now;
  if (!active) g_brakePedalActiveSinceMs = 0;
  g_brakePedalActive = active;
  g_status.brakeActive = active ? 1 : 0;
}

static uint8_t rightStalkCrc229(uint8_t counter, uint8_t status) {
  static const uint8_t base[16] = {
    0x46, 0x44, 0x52, 0x6D, 0x43, 0x41, 0xDD, 0xF9,
    0x4C, 0xA5, 0xF6, 0x8C, 0x49, 0x2F, 0x31, 0x3B
  };
  static const uint8_t offset[8] = {
    0x00, 0xE0, 0xEF, 0x0F, 0xF1, 0x00, 0x00, 0x00
  };
  return static_cast<uint8_t>(base[counter & 0x0F] ^ offset[status & 0x07]);
}

static uint8_t rightStalkNextCounter() {
  rightStalkTxCounter = static_cast<uint8_t>((rightStalkTxCounter + 1) & 0x0F);
  g_status.rightStalkCounter = rightStalkTxCounter;
  return rightStalkTxCounter;
}

static can_frame rightStalkFrame(uint8_t status) {
  can_frame f = {};
  f.can_id = CANB_ID_SCCM_RIGHT_STALK;
  f.can_dlc = 3;  // real SCCM_rightStalk is DLC=3 (cap80); DLC=8 was rejected
  const uint8_t counter = rightStalkNextCounter();
  f.data[0] = rightStalkCrc229(counter, status);
  f.data[1] = static_cast<uint8_t>(((status & 0x07) << 4) | counter);
  return f;
}

static bool scrollGearFsdStateOk(const RuntimeConfig& cfg) {
  if (!cfg.fsdEnabled) return true;
  uint8_t apState = 15;
  if (!speedLimitMonitor.getAutopilotState(apState)) return false;
  return apState == DAS_AP_STATE_DISABLED ||
         apState == DAS_AP_STATE_UNAVAILABLE ||
         apState == DAS_AP_STATE_AVAILABLE;
}

static bool scrollGearSafetyOk(uint8_t targetGear, const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  if (targetGear != GEAR_D && targetGear != GEAR_R) {
    scrollGearLastBlocked = 1;
    return false;
  }
  if (!scrollGearFsdStateOk(cfg)) {
    scrollGearLastBlocked = 6;
    return false;
  }
  if (!g_brakePedalActive || g_brakePedalActiveSinceMs == 0 ||
      (now - g_brakePedalActiveSinceMs) < SCROLL_GEAR_BRAKE_HOLD_MS) {
    scrollGearLastBlocked = 2;
    return false;
  }
  if (!g_vehicleSpeedValid || g_vehicleSpeedKph > SCROLL_GEAR_MAX_SPEED_KPH) {
    scrollGearLastBlocked = 3;
    return false;
  }
  if (g_status.currentGear == targetGear) {
    scrollGearLastBlocked = 4;
    return false;
  }
  if ((int32_t)(now - scrollGearCooldownUntilMs) < 0) {
    scrollGearLastBlocked = 5;
    return false;
  }
  scrollGearLastBlocked = 0;
  return true;
}

static void requestScrollGearShift(uint8_t targetGear, const RuntimeConfig& cfg) {
  g_status.scrollGearIntent = (targetGear == GEAR_D) ? 1 : (targetGear == GEAR_R ? -1 : 0);
  g_status.scrollGearDryRun = cfg.scrollGearSimEnabled ? 1 : 0;
  g_status.scrollGearInjectBlocked = 0;
  g_status.scrollGearInjectOk = 0;
  g_status.scrollGearInjectTarget = targetGear;

  if (!cfg.scrollGearInjectEnabled) return;
  if (scrollGearShiftActive) return;
  if (!scrollGearSafetyOk(targetGear, cfg)) {
    g_status.scrollGearInjectBlocked = scrollGearLastBlocked;
    return;
  }

  scrollGearShiftActive = true;
  scrollGearTargetGear = targetGear;
  scrollGearPhaseIndex = 0;
  scrollGearIdleFramesRemaining = 0;
  scrollGearNextTxMs = 0;
  g_status.scrollGearInjectActive = 1;
}

static void serviceScrollGearShift(const RuntimeConfig& cfg) {
  if (!scrollGearShiftActive) {
    g_status.scrollGearInjectActive = 0;
    return;
  }
  if (!canbReady || !cfg.canbEnabled || !cfg.scrollGearInjectEnabled ||
      !scrollGearSafetyOk(scrollGearTargetGear, cfg)) {
    canb_send(rightStalkFrame(RIGHT_STALK_IDLE));
    scrollGearShiftActive = false;
    scrollGearCooldownUntilMs = millis() + SCROLL_GEAR_COOLDOWN_MS;
    g_status.scrollGearInjectActive = 0;
    g_status.scrollGearInjectBlocked = scrollGearLastBlocked;
    return;
  }

  const uint32_t now = millis();
  if (scrollGearNextTxMs != 0 && (int32_t)(now - scrollGearNextTxMs) < 0) return;

  uint8_t status = RIGHT_STALK_IDLE;
  if (scrollGearPhaseIndex < SCROLL_GEAR_STATUS_FRAMES) {
    if (scrollGearTargetGear == GEAR_R) {
      status = (scrollGearPhaseIndex == 0) ? RIGHT_STALK_R_STAGE1 : RIGHT_STALK_R_STAGE2;
    } else {
      status = (scrollGearPhaseIndex == 0) ? RIGHT_STALK_D_STAGE1 : RIGHT_STALK_D_STAGE2;
    }
    scrollGearPhaseIndex++;
  } else if (scrollGearIdleFramesRemaining < SCROLL_GEAR_IDLE_FRAMES) {
    status = RIGHT_STALK_IDLE;
    scrollGearIdleFramesRemaining++;
  } else {
    scrollGearShiftActive = false;
    scrollGearCooldownUntilMs = now + SCROLL_GEAR_COOLDOWN_MS;
    g_status.scrollGearInjectActive = 0;
    g_status.scrollGearInjectOk = (g_status.currentGear == scrollGearTargetGear) ? 1 : 0;
    return;
  }

  canb_send(rightStalkFrame(status));
  scrollGearNextTxMs = now + SCROLL_GEAR_FRAME_INTERVAL_MS;
}

// Hazard via VCLEFT_switchStatus (0x3C2): reuse the latest live frame and only
// set/clear byte0 bit3 (hazardButtonPressed), leaving the counter/CRC bytes as
// the car last sent them. Capture: press = 08 55 55 55 00 00 59 45.
// NOTE: hazard is a momentary BUTTON (toggles hazards); pulse behaviour still
// needs on-vehicle validation.
static can_frame vcleftHazardFrame(bool pressed) {
  can_frame f = {};
  if (canbHasLastVcleftMux0Frame) {
    f = canbLastVcleftMux0Frame;  // reuse a real mux0 frame (its counter/CRC bytes)
  } else {
    // Captured mux0 idle VCLEFT_switchStatus (hazard released).
    static const uint8_t baseData[8] = {0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x59, 0x45};
    f.can_dlc = 8;
    memcpy(f.data, baseData, sizeof(f.data));
  }
  f.can_id = CANB_ID_VCLEFT_SWITCH;
  if (f.can_dlc < 4) f.can_dlc = 4;
  f.data[0] = static_cast<uint8_t>(f.data[0] & static_cast<uint8_t>(~VCLEFT_MUX_MASK));  // force mux0
  f.data[0] = pressed
    ? static_cast<uint8_t>(f.data[0] | VCLEFT_HAZARD_BUTTON_MASK)
    : static_cast<uint8_t>(f.data[0] & static_cast<uint8_t>(~VCLEFT_HAZARD_BUTTON_MASK));
  return f;
}

static bool dndScrollCacheFresh() {
  return canbHasLastVcleftMux1Frame &&
         (millis() - canbLastVcleftMux1Ms) <= DND_SCROLL_CACHE_MAX_AGE_MS;
}

static uint32_t dndVolumeAutoDelayMs() {
  constexpr uint32_t range = DND_VOLUME_AUTO_MAX_MS - DND_VOLUME_AUTO_MIN_MS + 1UL;
  return DND_VOLUME_AUTO_MIN_MS + (esp_random() % range);
}

static bool dndFsdActive() {
  uint8_t apState = 15;
  if (!speedLimitMonitor.getAutopilotState(apState)) return false;
  return apState > DAS_AP_STATE_AVAILABLE && apState <= 6;
}

static bool dndActionAllowed(const RuntimeConfig& cfg) {
  if (!cfg.dndEnabled || !cfg.dndVolumeEnabled) {
    g_status.dndBlocked = DND_BLOCK_DISABLED;
    return false;
  }
  if (!cfg.canbEnabled || !canbReady) {
    g_status.dndBlocked = DND_BLOCK_CANB;
    return false;
  }
  if (!dndScrollCacheFresh()) {
    g_status.dndBlocked = DND_BLOCK_NO_CACHE;
    return false;
  }
  g_status.dndBlocked = DND_BLOCK_NONE;
  return true;
}

static bool sendDndVolumeFrame(uint8_t cmd) {
  if (!dndScrollCacheFresh()) {
    g_status.dndBlocked = DND_BLOCK_NO_CACHE;
    return false;
  }

  can_frame f = canbLastVcleftMux1Frame;
  f.can_id = CANB_ID_VCLEFT_SWITCH;
  f.can_dlc = 8;
  f.data[0] = static_cast<uint8_t>((f.data[0] & static_cast<uint8_t>(~VCLEFT_MUX_MASK)) |
                                   VCLEFT_MUX_SCROLL);
  f.data[2] = 0;
  f.data[3] = 0;
  // Capture 2026-06-13 showed mux1 scroll frames keep data[0]/data[7] stable
  // while data[2] changes, so only change the left-scroll tick byte here.
  f.data[2] = cmd;

  if (!canb_send(f)) {
    g_status.dndBlocked = DND_BLOCK_CANB;
    return false;
  }
  g_status.dndTxCount++;
  return true;
}

static bool startDndVolumeAction(const RuntimeConfig& cfg) {
  if (dndActionActive || !dndActionAllowed(cfg)) return false;

  const uint32_t now = millis();
  dndActionActive = true;
  dndActionType = DND_ACTION_VOLUME;
  dndActionStep = 0;
  dndNextStepMs = 0;
  dndLastTriggerMs = now;
  g_status.dndActionActive = 1;
  g_status.dndActionType = DND_ACTION_VOLUME;
  g_status.dndBlocked = DND_BLOCK_NONE;
  return true;
}

static void handleDndHandsOnFrame(const can_frame& frame) {
  if (frame.can_id != CAN_ID_DAS_STATUS || frame.can_dlc < 6) return;

  const uint8_t handsOnState = static_cast<uint8_t>((frame.data[5] >> 2) & 0x0F);
  g_status.dndHandsOnState = handsOnState;
  g_status.dndWarningActive = handsOnState >= 3 ? 1 : 0;

  if (handsOnState <= 2 && !dndActionActive) g_status.dndBlocked = DND_BLOCK_NONE;
}

static void serviceDndScrollAction(const RuntimeConfig& cfg) {
  if (!dndActionActive) {
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  if (!cfg.dndEnabled || !cfg.dndVolumeEnabled || !cfg.canbEnabled || !canbReady) {
    dndActionActive = false;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    g_status.dndBlocked = (!cfg.dndEnabled || !cfg.dndVolumeEnabled) ? DND_BLOCK_DISABLED : DND_BLOCK_CANB;
    return;
  }

  const uint32_t now = millis();
  if (dndNextStepMs != 0 && (int32_t)(now - dndNextStepMs) < 0) return;

  static const uint8_t sequence[4] = {0x01, 0x00, 0x3F, 0x00};
  if (dndActionStep >= sizeof(sequence)) {
    dndActionActive = false;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  if (!sendDndVolumeFrame(sequence[dndActionStep])) {
    dndActionActive = false;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  dndActionStep++;
  if (dndActionStep >= sizeof(sequence)) {
    dndActionActive = false;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
  } else {
    dndNextStepMs = now + DND_SCROLL_STEP_MS;
    g_status.dndActionActive = 1;
    g_status.dndActionType = DND_ACTION_VOLUME;
  }
}

static void serviceDndVolumeAuto(const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  if (!cfg.dndEnabled || !cfg.dndVolumeEnabled || !dndFsdActive()) {
    dndVolumeNextAutoMs = 0;
    return;
  }

  if (dndVolumeNextAutoMs == 0) {
    dndVolumeNextAutoMs = now + dndVolumeAutoDelayMs();
    return;
  }
  if ((int32_t)(now - dndVolumeNextAutoMs) < 0) return;
  if (dndActionActive) {
    dndVolumeNextAutoMs = now + dndVolumeAutoDelayMs();
    return;
  }

  if (startDndVolumeAction(cfg)) {
    dndVolumeNextAutoMs = now + dndVolumeAutoDelayMs();
  } else {
    dndVolumeNextAutoMs = now + DND_SCROLL_STEP_MS;
  }
}

static uint8_t teslaCanChecksum(uint16_t canId, const uint8_t* data, uint8_t len) {
  uint8_t checksum = static_cast<uint8_t>((canId & 0xFF) + ((canId >> 8) & 0xFF));
  for (uint8_t i = 0; i + 1 < len; ++i) checksum += data[i];
  return checksum;
}

static can_frame rearFogFrame(bool fogOn) {
  can_frame f = {};
  static const uint8_t baseData[8] = {0x81, 0xE1, 0x10, 0x40, 0x0B, 0x03, 0x30, 0x12};
  if (canbHasLastBodyLightingFrame) {
    f = canbLastBodyLightingFrame;
  } else {
    f.can_id = CANB_ID_BODY_LIGHTING;
    f.can_dlc = 8;
    memcpy(f.data, baseData, sizeof(f.data));
  }
  f.can_id = CANB_ID_BODY_LIGHTING;
  if (f.can_dlc < 8) f.can_dlc = 8;
  f.data[2] = fogOn
    ? static_cast<uint8_t>(f.data[2] | REAR_FOG_MASK)
    : static_cast<uint8_t>(f.data[2] & ~REAR_FOG_MASK);
  return f;
}

static void stopHighBeamStrobe(bool sendIdle) {
  if (sendIdle && canbReady) {
    canb_send(highBeamFrame(STALK_STATUS_IDLE));
  }
  highBeamStrobeActive = false;
  highBeamStrobeManualTrigger = false;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = 0;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
  g_status.highBeamStrobeActive = 0;
  g_status.highBeamStrobeRemaining = 0;
}

static void startHighBeamStrobe(bool manualTrigger = false) {
  highBeamStrobeActive = true;
  highBeamStrobeManualTrigger = manualTrigger;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = HIGH_BEAM_STROBE_PULSES;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
  g_status.highBeamStrobeActive = 1;
  g_status.highBeamStrobeRemaining = HIGH_BEAM_STROBE_PULSES;
}

static void stopRearFogBrakeStrobe(bool sendOff) {
  if (sendOff && canbReady) {
    canb_send(rearFogFrame(false));
  }
  rearFogBrakeStrobeActive = false;
  rearFogBrakeStrobeManualTrigger = false;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = 0;
  rearFogBrakeStrobePriority = 0;
  rearFogBrakeStrobeLastToggleMs = 0;
  g_status.rearFogBrakeStrobeActive = 0;
  g_status.rearFogBrakeStrobeRemaining = 0;
}

static void startRearFogBrakeStrobe(uint8_t pulses, uint8_t priority, bool manualTrigger = false) {
  if (rearFogBrakeStrobeActive && priority < rearFogBrakeStrobePriority) return;
  rearFogBrakeStrobeActive = true;
  rearFogBrakeStrobeManualTrigger = manualTrigger;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = pulses;
  rearFogBrakeStrobePriority = priority;
  rearFogBrakeStrobeLastToggleMs = 0;
  g_status.rearFogBrakeStrobeActive = 1;
  g_status.rearFogBrakeStrobeRemaining = pulses;
}

static void stopReverseStrobe(bool sendOff) {
  if (sendOff && canbReady) {
    canb_send(vcleftHazardFrame(false));  // release any in-progress hazard button press
    if (reverseHazardLatchedOn) {
      canb_send(vcleftHazardFrame(true));   // toggle hazards back off if this feature turned them on
      canb_send(vcleftHazardFrame(false));  // release the OFF click
    }
    canb_send(rearFogFrame(false));
  }
  reverseStrobeActive = false;
  reverseStrobePhase = 0;
  reverseStrobePhaseEnd = 0;
  reverseHazardLatchedOn = false;
  g_status.reverseStrobeActive = 0;
  g_status.reverseStrobeRemaining = 0;
}

static void startReverseStrobe() {
  if (!canbReady) return;
  if (reverseStrobePhase != 0 || reverseStrobeActive) return;
  reverseStrobeActive = true;
  reverseStrobePhase = 1;                                 // ON-press
  reverseStrobePhaseEnd = millis() + REVERSE_HAZARD_CLICK_MS;
  canb_send(vcleftHazardFrame(true));                     // single press edge -> hazards ON
  reverseHazardLatchedOn = true;
  g_status.reverseStrobeActive = 1;
  g_status.reverseStrobeRemaining = 1;
  startRearFogBrakeStrobe(REVERSE_STROBE_PULSES, REAR_FOG_PRIORITY_REVERSE, true);
}

static bool readBitsLE(const can_frame& frame, uint8_t startBit, uint8_t length, uint32_t& value) {
  if (length == 0 || length > 32) return false;
  if (static_cast<uint16_t>(startBit) + length > static_cast<uint16_t>(frame.can_dlc) * 8U) return false;
  uint32_t raw = 0;
  for (uint8_t i = 0; i < length; ++i) {
    const uint8_t bit = static_cast<uint8_t>(startBit + i);
    if ((frame.data[bit / 8] >> (bit % 8)) & 0x01) {
      raw |= (1UL << i);
    }
  }
  value = raw;
  return true;
}

static bool readSignedBitsLE(const can_frame& frame, uint8_t startBit, uint8_t length, int32_t& value) {
  uint32_t raw = 0;
  if (!readBitsLE(frame, startBit, length, raw)) return false;
  if (length < 32 && (raw & (1UL << (length - 1)))) {
    raw |= (~0UL << length);
  }
  value = static_cast<int32_t>(raw);
  return true;
}

static bool lockSleepSignalFresh(uint32_t seenMs, uint32_t maxAgeMs) {
  return seenMs != 0 && (millis() - seenMs) <= maxAgeMs;
}

static int8_t lockSleepSeatSwitchState(uint8_t raw) {
  if (raw == 1) return LOCK_SLEEP_SEAT_EMPTY;
  if (raw == 2) return LOCK_SLEEP_SEAT_OCCUPIED;
  return LOCK_SLEEP_SEAT_UNKNOWN;
}

static void lockSleepSetSeatState(int8_t& slot, int8_t state) {
  if (state == LOCK_SLEEP_SEAT_UNKNOWN) return;
  slot = state;
  lockSleepSeatSeenMs = millis();
}

static bool lockSleepDriverFresh() {
  return lockSleepSignalFresh(lockSleepDriverSeenMs, LOCK_SLEEP_OCCUPANCY_FRESH_MS);
}

static bool lockSleepSeatFresh() {
  return lockSleepSignalFresh(lockSleepSeatSeenMs, LOCK_SLEEP_OCCUPANCY_FRESH_MS);
}

static bool lockSleepSeatOccupied() {
  return lockSleepSeatFresh() &&
         (lockSleepSeatDriver == LOCK_SLEEP_SEAT_OCCUPIED ||
          lockSleepSeatPassenger == LOCK_SLEEP_SEAT_OCCUPIED ||
          lockSleepSeatRearLeft == LOCK_SLEEP_SEAT_OCCUPIED ||
          lockSleepSeatRearCenter == LOCK_SLEEP_SEAT_OCCUPIED ||
          lockSleepSeatRearRight == LOCK_SLEEP_SEAT_OCCUPIED);
}

static bool lockSleepAllKnownSeatsEmpty() {
  return lockSleepSeatFresh() &&
         lockSleepSeatDriver == LOCK_SLEEP_SEAT_EMPTY &&
         lockSleepSeatPassenger == LOCK_SLEEP_SEAT_EMPTY &&
         lockSleepSeatRearLeft == LOCK_SLEEP_SEAT_EMPTY &&
         lockSleepSeatRearCenter == LOCK_SLEEP_SEAT_EMPTY &&
         lockSleepSeatRearRight == LOCK_SLEEP_SEAT_EMPTY;
}

static bool lockSleepCabinEmptyReady() {
  if (lockSleepSeatOccupied()) return false;
  if (lockSleepAllKnownSeatsEmpty()) return true;
  if (lockSleepDriverKnown && lockSleepDriverFresh()) return !lockSleepDriverPresent;
  return false;
}

static void updateLockSleepCabinStatus() {
  g_status.lockSleepCabinEmpty = lockSleepCabinEmptyReady() ? 1 : 0;
}

static void observeLockSleepOccupancyFrame(const can_frame& frame) {
  if (frame.can_id != CAN_ID_DRIVER_OCCUPANCY || frame.can_dlc < 2) return;

  uint32_t raw = 0;
  if (readBitsLE(frame, 7, 1, raw)) {
    lockSleepDriverKnown = true;
    lockSleepDriverPresent = raw != 0;
    lockSleepDriverSeenMs = millis();
    lockSleepSetSeatState(lockSleepSeatDriver,
                          lockSleepDriverPresent ? LOCK_SLEEP_SEAT_OCCUPIED : LOCK_SLEEP_SEAT_EMPTY);
  }
  if (readBitsLE(frame, 8, 1, raw)) {
    lockSleepSetSeatState(lockSleepSeatPassenger,
                          raw != 0 ? LOCK_SLEEP_SEAT_OCCUPIED : LOCK_SLEEP_SEAT_EMPTY);
  }
  if (frame.can_dlc >= 6) {
    if (readBitsLE(frame, 36, 2, raw) && raw == 1) {
      lockSleepSetSeatState(lockSleepSeatRearLeft, LOCK_SLEEP_SEAT_OCCUPIED);
    }
    if (readBitsLE(frame, 38, 2, raw) && raw == 1) {
      lockSleepSetSeatState(lockSleepSeatRearCenter, LOCK_SLEEP_SEAT_OCCUPIED);
    }
    if (readBitsLE(frame, 40, 2, raw) && raw == 1) {
      lockSleepSetSeatState(lockSleepSeatRearRight, LOCK_SLEEP_SEAT_OCCUPIED);
    }
  }
  updateLockSleepCabinStatus();
}

static void observeLockSleepVcleftSeats(const can_frame& frame) {
  if (frame.can_id != CANB_ID_VCLEFT_SWITCH || frame.can_dlc < 8) return;

  uint32_t raw = 0;
  if (!readBitsLE(frame, 0, 2, raw) || raw != VCLEFT_MUX_HAZARD) return;

  if (readBitsLE(frame, 50, 2, raw)) {
    lockSleepSetSeatState(lockSleepSeatDriver, lockSleepSeatSwitchState(static_cast<uint8_t>(raw)));
  }
  if (readBitsLE(frame, 54, 2, raw)) {
    lockSleepSetSeatState(lockSleepSeatRearCenter, lockSleepSeatSwitchState(static_cast<uint8_t>(raw)));
  }
  if (readBitsLE(frame, 56, 2, raw)) {
    lockSleepSetSeatState(lockSleepSeatRearLeft, lockSleepSeatSwitchState(static_cast<uint8_t>(raw)));
  }
  if (readBitsLE(frame, 58, 2, raw)) {
    lockSleepSetSeatState(lockSleepSeatRearRight, lockSleepSeatSwitchState(static_cast<uint8_t>(raw)));
  }
  updateLockSleepCabinStatus();
}

static void observeLockSleepFrame(const can_frame& frame) {
  if (frame.can_id != CANB_ID_VCSEC_STATUS) return;

  const uint32_t now = millis();
  if (lockSleep339LastRxMs != 0 &&
      (now - lockSleep339LastRxMs) > LOCK_SLEEP_RECENT_ACTIVITY_MS) {
    lockSleep339StableStartMs = 0;
    g_status.lockSleep339StableAgeMs = 0;
  }
  lockSleep339LastRxMs = now;
  g_status.lockSleep339Seen = 1;

  uint32_t raw = 0;
  g_status.lockSleep339SimpleStatus =
      readBitsLE(frame, 54, 2, raw) ? static_cast<uint8_t>(raw) : 255;
}

static void resetLockSleepCandidate(uint8_t blockReason) {
  lockSleep339StableStartMs = 0;
  g_status.lockSleep339StableAgeMs = 0;
  g_status.lockSleepBlocked = blockReason;
}

static void serviceLockSleepCandidate(const can_frame& frame) {
  if (frame.can_id != CANB_ID_VCSEC_STATUS || frame.can_dlc < 7) return;
  if (!g_config.lockDeepSleepEnabled || lockDeepSleepPending) {
    resetLockSleepCandidate(LOCK_SLEEP_BLOCK_NONE);
    return;
  }

  uint32_t raw = 0;
  if (!readBitsLE(frame, 54, 2, raw)) {
    resetLockSleepCandidate(LOCK_SLEEP_BLOCK_UNLOCKED);
    return;
  }

  const uint8_t simpleStatus = static_cast<uint8_t>(raw);
  if (simpleStatus != 2) {
    resetLockSleepCandidate(LOCK_SLEEP_BLOCK_UNLOCKED);
    return;
  }

  updateLockSleepCabinStatus();
  if (!lockSleepCabinEmptyReady()) {
    resetLockSleepCandidate(LOCK_SLEEP_BLOCK_CABIN_ACTIVE);
    return;
  }

  const uint32_t now = millis();
  if (lockSleep339StableStartMs == 0) lockSleep339StableStartMs = now;
  g_status.lockSleep339StableAgeMs = now - lockSleep339StableStartMs;
  g_status.lockSleepLastId = frame.can_id;
  g_status.lockSleepSource = 1;

  if (g_status.lockSleep339StableAgeMs < LOCK_SLEEP_STABLE_MS) {
    g_status.lockSleepBlocked = LOCK_SLEEP_BLOCK_STABILIZING;
    return;
  }

  g_status.lockSleepBlocked = LOCK_SLEEP_BLOCK_NONE;
  requestLockDeepSleep(frame, 1);
}

static void requestLockDeepSleep(const can_frame& frame, uint8_t source) {
  if (!g_config.lockDeepSleepEnabled || lockDeepSleepPending) return;
  canTxInhibitedForSleep = true;
  lockDeepSleepPending = true;
  lockDeepSleepPendingMs = millis();
  lockSleepLastSignalMs = lockDeepSleepPendingMs;
  g_status.lockSleepTriggered = 1;
  g_status.lockSleepLastId = frame.can_id;
  g_status.lockSleepSource = source;
}

static void serviceLockDeepSleep() {
  if (!lockDeepSleepPending) return;
  const uint32_t now = millis();
  if (now - lockDeepSleepPendingMs < 50) return;

  canTxInhibitedForSleep = true;
  g_status.highBeamStrobeActive = 0;
  g_status.highBeamStrobeRemaining = 0;
  g_status.rearFogBrakeStrobeActive = 0;
  g_status.rearFogBrakeStrobeRemaining = 0;
  g_status.reverseStrobeActive = 0;
  g_status.reverseStrobeRemaining = 0;
  g_status.batteryPreheatActive = 0;
  g_status.scrollGearInjectActive = 0;
  g_status.dndActionActive = 0;
  g_status.dndActionType = DND_ACTION_NONE;

#ifdef ENABLE_LIGHT_WEBUI
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
#endif

  twai_stop();
  twai_driver_uninstall();
  digitalWrite(PIN_LED, HIGH);
  delay(20);
  esp_deep_sleep_start();
}

static bool decodeBms712TempCx100(uint8_t lo, uint8_t hi, int16_t& out) {
  const uint16_t raw = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
  if (raw == 0 || raw == 0x8000 || raw == 0xFFFF || raw > 12000) return false;
  out = static_cast<int16_t>(raw);
  return true;
}

static void refreshBms712TemperatureSummary() {
  int sum = 0;
  int minValue = 32767;
  int maxValue = -32768;
  uint8_t count = 0;

  for (uint8_t i = 0; i < 12; ++i) {
    if ((bms712TempValidMask & (1U << i)) == 0) continue;
    const int value = bms712TempCx100[i];
    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;
    sum += value;
    count++;
  }

  g_status.bmsTempDecodedCount = count;
  if (count == 0) {
    g_status.bmsTempMinCx100 = TEMPERATURE_SNA_CX100;
    g_status.bmsTempAvgCx100 = TEMPERATURE_SNA_CX100;
    g_status.bmsTempMaxCx100 = TEMPERATURE_SNA_CX100;
    return;
  }

  g_status.bmsTempMinCx100 = minValue;
  g_status.bmsTempAvgCx100 = static_cast<int>((sum + (count / 2)) / count);
  g_status.bmsTempMaxCx100 = maxValue;
}

static void handleBms712TemperatureFrame(const can_frame& frame) {
  if (frame.can_id != CAN_ID_BMS_PACK_TEMPERATURES || frame.can_dlc < 8) return;

  const uint8_t mux = static_cast<uint8_t>(frame.data[0] & 0x0F);
  if (mux > 3) return;

  int16_t temps[3] = {};
  const bool valid0 = decodeBms712TempCx100(frame.data[2], frame.data[3], temps[0]);
  const bool valid1 = decodeBms712TempCx100(frame.data[4], frame.data[5], temps[1]);
  const bool valid2 = decodeBms712TempCx100(frame.data[6], frame.data[7], temps[2]);
  if (!valid0 && !valid1 && !valid2) return;

  const uint8_t base = static_cast<uint8_t>(mux * 3);
  const bool valid[3] = {valid0, valid1, valid2};
  for (uint8_t i = 0; i < 3; ++i) {
    const uint16_t bit = static_cast<uint16_t>(1U << (base + i));
    if (valid[i]) {
      bms712TempCx100[base + i] = temps[i];
      bms712TempValidMask |= bit;
    } else {
      bms712TempValidMask &= static_cast<uint16_t>(~bit);
    }
  }

  bms712TempLastRxMs = millis();
  g_status.bmsTempDecodedSeen = 1;
  g_status.bmsTempDecodedMux = mux;
  g_status.bmsTempLatest1Cx100 = valid0 ? temps[0] : TEMPERATURE_SNA_CX100;
  g_status.bmsTempLatest2Cx100 = valid1 ? temps[1] : TEMPERATURE_SNA_CX100;
  g_status.bmsTempLatest3Cx100 = valid2 ? temps[2] : TEMPERATURE_SNA_CX100;
  refreshBms712TemperatureSummary();
}

static void handleBatteryTempDiagFrame(const can_frame& frame, uint8_t bus) {
  if (frame.can_id != CAN_ID_BMS_THERMAL_STATUS &&
      frame.can_id != CAN_ID_BMS_PACK_TEMPERATURES &&
      frame.can_id != CAN_ID_BMS_LOG1) {
    return;
  }
  bmsTempLastRxMs = millis();
  g_status.bmsTempFrameSeen = 1;
  g_status.bmsTempFrameId = frame.can_id;
  g_status.bmsTempFrameBus = bus;
  g_status.bmsTempFrameMux = frame.can_dlc > 0 ? static_cast<uint8_t>(frame.data[0] & 0x0F) : 0;
  formatPayload8(frame, g_status.bmsTempFramePayload);
  handleBms712TemperatureFrame(frame);
}

static void handleRearFogDecelAccel(float accel, const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  if (!canbReady || !cfg.canbEnabled || !cfg.rearFogBrakeStrobeEnabled) {
    rearFogMildDecelStartMs = 0;
    rearFogHardDecelStartMs = 0;
    rearFogMildDecelTriggered = false;
    rearFogHardDecelTriggered = false;
    return;
  }

  const bool recentPedalBrake = now < rearFogRecentPedalBrakeUntilMs;
  const bool recentBrakeLamp = now < rearFogRecentBrakeLampUntilMs;
  const bool recentBrakeTorque = now < rearFogRecentBrakeTorqueUntilMs;
  const bool recentRegen = now < rearFogRecentRegenUntilMs;
  const bool recentNegTorque = now < rearFogRecentNegTorqueUntilMs;
  const bool recentSpeedFalling = now < rearFogRecentSpeedFallingUntilMs;

  const bool hardAux = recentBrakeLamp || recentBrakeTorque || recentPedalBrake;
  const bool mildAux = hardAux || recentRegen || recentNegTorque || recentSpeedFalling;
  const bool hardCandidate =
    (accel <= REAR_FOG_HARD_DECEL_THRESHOLD && hardAux) ||
    (accel <= REAR_FOG_VERY_HARD_DECEL_THRESHOLD);
  const bool mildCandidate =
    !hardCandidate && accel <= REAR_FOG_MILD_DECEL_THRESHOLD && mildAux;

  if (!hardCandidate) {
    rearFogHardDecelStartMs = 0;
    rearFogHardDecelTriggered = false;
  } else {
    if (rearFogHardDecelStartMs == 0) rearFogHardDecelStartMs = now;
    if (!rearFogHardDecelTriggered &&
        (now - rearFogHardDecelStartMs) >= REAR_FOG_HARD_DECEL_HOLD_MS) {
      startRearFogBrakeStrobe(REAR_FOG_BODY_STROBE_PULSES, REAR_FOG_PRIORITY_BODY);
      rearFogHardDecelTriggered = true;
      rearFogMildDecelTriggered = true;
    }
  }

  if (!mildCandidate || hardCandidate) {
    rearFogMildDecelStartMs = 0;
    if (!mildCandidate) rearFogMildDecelTriggered = false;
    return;
  }

  if (rearFogMildDecelStartMs == 0) rearFogMildDecelStartMs = now;
  if (!rearFogMildDecelTriggered &&
      (now - rearFogMildDecelStartMs) >= REAR_FOG_MILD_DECEL_HOLD_MS) {
    startRearFogBrakeStrobe(REAR_FOG_PEDAL_STROBE_PULSES, REAR_FOG_PRIORITY_PEDAL);
    rearFogMildDecelTriggered = true;
  }
}

static void serviceHighBeamStrobe(const RuntimeConfig& cfg) {
  if (!canbReady) return;
  // 0x249 high-light strobe: alternate PULL(status=1) and idle(status=0).
  if (!cfg.canbEnabled) {
    if (highBeamStrobeActive || highBeamStrobeOutputOn) stopHighBeamStrobe(false);
    return;
  }

  if (!cfg.highBeamStrobeEnabled && !highBeamStrobeManualTrigger) {
    if (highBeamStrobeActive || highBeamStrobeOutputOn) stopHighBeamStrobe(true);
    highBeamPullCount = 0;
    highBeamLastPullDown = false;
    return;
  }

  if (!highBeamStrobeActive) return;

  const uint32_t now = millis();
  if (highBeamStrobeLastToggleMs != 0 &&
      (now - highBeamStrobeLastToggleMs) < HIGH_BEAM_STROBE_INTERVAL_MS) {
    if (highBeamStrobeLastSendMs == 0 ||
        (now - highBeamStrobeLastSendMs) >= HIGH_BEAM_STROBE_RESEND_MS) {
      canb_send(highBeamFrame(highBeamStrobeOutputOn ? STALK_STATUS_PULL : STALK_STATUS_IDLE));
      highBeamStrobeLastSendMs = now;
    }
    g_status.highBeamStrobeActive = 1;
    g_status.highBeamStrobeRemaining = highBeamStrobePulsesRemaining;
    return;
  }
  highBeamStrobeLastToggleMs = now;

  if (!highBeamStrobeOutputOn) {
    canb_send(highBeamFrame(STALK_STATUS_PULL));
    highBeamStrobeOutputOn = true;
  } else {
    canb_send(highBeamFrame(STALK_STATUS_IDLE));
    highBeamStrobeOutputOn = false;
    if (highBeamStrobePulsesRemaining > 0) highBeamStrobePulsesRemaining--;
    if (highBeamStrobePulsesRemaining == 0) {
      stopHighBeamStrobe(false);
      return;
    }
  }
  highBeamStrobeLastSendMs = now;

  g_status.highBeamStrobeActive = 1;
  g_status.highBeamStrobeRemaining = highBeamStrobePulsesRemaining;
}

static void serviceReverseStrobe(const RuntimeConfig& cfg) {
  if (!canbReady) return;
  if (!cfg.canbEnabled) {
    if (reverseStrobePhase != 0) stopReverseStrobe(false);
    return;
  }
  if (!cfg.reverseStrobeEnabled) {
    if (reverseStrobePhase != 0) stopReverseStrobe(true);
    lastDIGearRaw = 0;
    return;
  }
  if (reverseStrobePhase == 0) return;

  const uint32_t now = millis();
  if ((int32_t)(now - reverseStrobePhaseEnd) < 0) {
    g_status.reverseStrobeActive = 1;
    return;  // current phase still running
  }

  switch (reverseStrobePhase) {
    case 1:  // ON-press done -> release button; hold while the car flashes
      canb_send(vcleftHazardFrame(false));
      reverseStrobePhase = 2;
      reverseStrobePhaseEnd = now + REVERSE_HAZARD_ON_MS;
      break;
    case 2:  // hold done -> press again to toggle hazards back off
      canb_send(vcleftHazardFrame(true));
      reverseHazardLatchedOn = false;
      reverseStrobePhase = 3;
      reverseStrobePhaseEnd = now + REVERSE_HAZARD_CLICK_MS;
      break;
    default:  // case 3: OFF-press done -> release and finish
      stopReverseStrobe(true);
      return;
  }
  g_status.reverseStrobeActive = 1;
}

static void serviceRearFogBrakeStrobe(const RuntimeConfig& cfg) {
  if (!canbReady) return;
  // 0x273 rear fog brake strobe output is a fixed non-blocking ON/OFF pulse train.
  if (!cfg.canbEnabled) {
    if (rearFogBrakeStrobeActive || rearFogBrakeStrobeOutputOn) stopRearFogBrakeStrobe(false);
    return;
  }

  if (!cfg.rearFogBrakeStrobeEnabled && !rearFogBrakeStrobeManualTrigger) {
    if (rearFogBrakeStrobeActive || rearFogBrakeStrobeOutputOn) stopRearFogBrakeStrobe(true);
    rearFogLastPedalBrakeActive = false;
    rearFogLastBrakeActive = false;
    return;
  }

  if (!rearFogBrakeStrobeActive) return;

  const uint32_t now = millis();
  if (rearFogBrakeStrobeLastToggleMs != 0 &&
      (now - rearFogBrakeStrobeLastToggleMs) < REAR_FOG_STROBE_INTERVAL_MS) {
    g_status.rearFogBrakeStrobeActive = 1;
    g_status.rearFogBrakeStrobeRemaining = rearFogBrakeStrobePulsesRemaining;
    return;
  }
  rearFogBrakeStrobeLastToggleMs = now;

  if (!rearFogBrakeStrobeOutputOn) {
    canb_send(rearFogFrame(true));
    rearFogBrakeStrobeOutputOn = true;
  } else {
    canb_send(rearFogFrame(false));
    rearFogBrakeStrobeOutputOn = false;
    if (rearFogBrakeStrobePulsesRemaining > 0) rearFogBrakeStrobePulsesRemaining--;
    if (rearFogBrakeStrobePulsesRemaining == 0) {
      stopRearFogBrakeStrobe(false);
      return;
    }
  }

  g_status.rearFogBrakeStrobeActive = 1;
  g_status.rearFogBrakeStrobeRemaining = rearFogBrakeStrobePulsesRemaining;
}

static void handleRearFogPedalBrakeEdge(bool brakeActive, const RuntimeConfig& cfg) {
  (void)cfg;
  const uint32_t now = millis();
  if (brakeActive) {
    rearFogRecentPedalBrakeUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
  }
  rearFogLastPedalBrakeActive = brakeActive;
}

static void handleRearFogBrakeLampState(bool brakeActive, const RuntimeConfig& cfg) {
  (void)cfg;
  const uint32_t now = millis();
  if (brakeActive) {
    rearFogRecentBrakeLampUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
  }
  rearFogLastBrakeActive = brakeActive;
}

static void handleRearFogCanADecelFrame(const can_frame& frame, const RuntimeConfig& cfg) {
  const uint32_t now = millis();

  if (frame.can_id == CANB_ID_VCLEFT_SWITCH && frame.can_dlc >= 4) {
    handleVcleftSwitchFrame(frame, cfg, false);
    return;
  }

  if (frame.can_id == CAN_ID_BRAKE_PEDAL && frame.can_dlc >= 8) {
    uint32_t raw = 0;
    const bool brakeLamp = readBitsLE(frame, 21, 1, raw) && raw != 0;
    const bool driverBrake =
      readBitsLE(frame, 29, 2, raw) && raw == 2;
    const bool brakeApply =
      readBitsLE(frame, 31, 1, raw) && raw != 0;
    const bool brakeTorque =
      readBitsLE(frame, 51, 13, raw) && raw > 0;
    setBrakePedalActive(driverBrake || brakeApply);  // shared with 0x3C2 scroll-gear trigger
    handleRearFogPedalBrakeEdge(driverBrake || brakeApply, cfg);
    if (brakeLamp) rearFogRecentBrakeLampUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    if (brakeTorque) rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    return;
  }

  if ((frame.can_id == CAN_ID_RCM_INERTIAL2_CH || frame.can_id == CAN_ID_RCM_INERTIAL2_ETH) &&
      frame.can_dlc >= 6) {
    int32_t rawAccel = 0;
    if (readSignedBitsLE(frame, 0, 16, rawAccel) && rawAccel != -32768) {
      bool qfOk = true;
      uint32_t qf = 1;
      if (frame.can_id == CAN_ID_RCM_INERTIAL2_CH && frame.can_dlc >= 7) {
        qfOk = readBitsLE(frame, 48, 1, qf) && qf != 0;
      }
      if (qfOk) {
        handleRearFogDecelAccel(static_cast<float>(rawAccel) * 0.00125f, cfg);
      }
    }
    return;
  }

  if (frame.can_id == CAN_ID_DI_SYSTEM_STATUS && frame.can_dlc >= 8) {
    const uint8_t gearRaw = static_cast<uint8_t>((frame.data[2] >> 5) & 0x07);
    g_status.currentGear = gearRaw;
    uint32_t brakeRaw = 0;
    if (readBitsLE(frame, 19, 2, brakeRaw)) setBrakePedalActive(brakeRaw == 2);
    if (cfg.reverseStrobeEnabled && gearRaw == 2 && lastDIGearRaw != 2) {
      startReverseStrobe();
    }
    lastDIGearRaw = gearRaw;

    uint32_t raw = 0;
    if (readBitsLE(frame, 51, 1, raw) && raw != 0) {
      rearFogRecentRegenUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    int32_t accelRaw = 0;
    if (readSignedBitsLE(frame, 52, 12, accelRaw) && accelRaw != -2048) {
      handleRearFogDecelAccel(static_cast<float>(accelRaw) * 0.01f, cfg);
    }
    return;
  }

  if (frame.can_id == CAN_ID_DI_CHASSIS_CONTROL && frame.can_dlc >= 4) {
    uint32_t raw = 0;
    if (readBitsLE(frame, 15, 1, raw) && raw != 0) {
      rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_ESP_BRAKE_TORQUE && frame.can_dlc >= 7) {
    uint32_t qf = 0;
    uint32_t frL = 0, frR = 0, reL = 0, reR = 0;
    if (readBitsLE(frame, 50, 1, qf) && qf != 0 &&
        readBitsLE(frame, 0, 12, frL) &&
        readBitsLE(frame, 12, 12, frR) &&
        readBitsLE(frame, 24, 12, reL) &&
        readBitsLE(frame, 36, 12, reR) &&
        (frL + frR + reL + reR) > 0) {
      rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_DIF_TORQUE && frame.can_dlc >= 8) {
    int32_t torqueActualRaw = 0;
    uint32_t qf = 0;
    if (readSignedBitsLE(frame, 27, 13, torqueActualRaw) &&
        readBitsLE(frame, 56, 2, qf) && qf == 1 &&
        torqueActualRaw < 0) {
      rearFogRecentNegTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_VEHICLE_SPEED && frame.can_dlc >= 4) {
    const uint16_t rawSpeed =
      static_cast<uint16_t>((static_cast<uint16_t>(frame.data[2]) << 4) | (frame.data[1] >> 4));
    float speedKph = static_cast<float>(rawSpeed) * 0.08f - 40.0f;
    if (speedKph < 0.0f) speedKph = 0.0f;
    g_vehicleSpeedKph = speedKph;
    g_vehicleSpeedValid = true;
    g_status.vehicleSpeedKph = static_cast<int>(speedKph + 0.5f);
    if (rearFogLastVehicleSpeedKph >= 0.0f &&
        speedKph + 0.5f < rearFogLastVehicleSpeedKph) {
      rearFogRecentSpeedFallingUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    rearFogLastVehicleSpeedKph = speedKph;
    return;
  }
}

static void handleVcleftSwitchFrame(const can_frame& frame, const RuntimeConfig& cfg, bool cacheHazardFrame) {
  if (frame.can_dlc < 4) return;
  if (cacheHazardFrame) {
    canbLastVcleftSwitchFrame = frame;
    canbHasLastVcleftSwitchFrame = true;
  }

  const uint8_t mux = static_cast<uint8_t>(frame.data[0] & VCLEFT_MUX_MASK);
  if (mux == VCLEFT_MUX_HAZARD) {
    observeLockSleepVcleftSeats(frame);
  }
  if (cacheHazardFrame && mux == VCLEFT_MUX_HAZARD) {
    canbLastVcleftMux0Frame = frame;
    canbHasLastVcleftMux0Frame = true;
  }

  if (mux != VCLEFT_MUX_SCROLL) return;
  if (cacheHazardFrame && frame.can_dlc >= 8) {
    canbLastVcleftMux1Frame = frame;
    canbHasLastVcleftMux1Frame = true;
    canbLastVcleftMux1Ms = millis();
  }

  const uint8_t scrollRaw = static_cast<uint8_t>(frame.data[3] & 0x3F);
  const int8_t scrollTicks = (scrollRaw & 0x20)
    ? static_cast<int8_t>(static_cast<int>(scrollRaw) - 64)
    : static_cast<int8_t>(scrollRaw);
  g_status.rightScrollTicks = scrollTicks;

  const bool scrollEdge = (scrollTicks != 0) && !scrollGearLatched;
  if (scrollTicks == 0) {
    scrollGearLatched = false;
  } else if (scrollEdge && g_brakePedalActive) {
    const uint8_t targetGear = (scrollTicks < 0) ? GEAR_R : GEAR_D;
    const bool sameGearRequest = (g_status.currentGear == targetGear);
    if (sameGearRequest) {
      scrollGearLastBlocked = 4;
      g_status.scrollGearIntent = 0;
      g_status.scrollGearInjectBlocked = scrollGearLastBlocked;
      g_status.scrollGearInjectTarget = targetGear;
    } else {
      requestScrollGearShift(targetGear, cfg);
    }
  }

  if (cfg.reverseStrobeEnabled && g_brakePedalActive && scrollEdge) {
    const uint8_t targetGear = (scrollTicks < 0) ? GEAR_R : GEAR_D;
    const bool sameGearRequest = (g_status.currentGear == targetGear);
    if (scrollTicks < 0 && !sameGearRequest) {
      if (!reverseStrobeActive) startReverseStrobe();
    } else if (scrollTicks > 0 && reverseStrobeActive) {
      stopReverseStrobe(true);
    }
  }
  if (scrollTicks != 0) scrollGearLatched = true;
}

static void handleCanBFrame(const can_frame& frame) {
  // Stage 1: statistics only. No heavy work, no Serial, no JSON, no bridging.
  canbLastId = frame.can_id;
  observeLockSleepFrame(frame);
  serviceLockSleepCandidate(frame);
  if (lockDeepSleepPending) return;

  handleNagKillerContextFrame(frame);
  if (frame.can_id == CAN_ID_NAG_MODE_B_TARGET || frame.can_id == CAN_ID_NAG_MODE_C_TARGET) {
    handleNagKillerTargetFrame(frame, configSnapshot());
  }

  if (frame.can_id == CANB_ID_STW_ACTN_RQ && frame.can_dlc >= 2) {
    canbLastStwActnRqFrame = frame;
    canbHasLastStwActnRqFrame = true;
    highBeamStalkLastCounter = static_cast<uint8_t>(frame.data[1] & 0x0F);

    const RuntimeConfig cfg = configSnapshot();
    const uint8_t stalkStatus = readStalkStatus(frame);
    const bool pullDown = stalkStatus == STALK_STATUS_PULL;
    const uint32_t now = millis();

    if (!cfg.highBeamStrobeEnabled) {
      highBeamPullCount = 0;
    } else if (pullDown && !highBeamLastPullDown && !highBeamStrobeActive) {
      if (highBeamLastPullMs == 0 ||
          (now - highBeamLastPullMs) > HIGH_BEAM_DOUBLE_PULL_WINDOW_MS) {
        highBeamPullCount = 0;
      }
      highBeamLastPullMs = now;
      highBeamPullCount++;
      if (highBeamPullCount >= 2) {
        highBeamPullCount = 0;
        startHighBeamStrobe();
      }
    }
    highBeamLastPullDown = pullDown;
  }

  if (frame.can_id == CANB_ID_SCCM_RIGHT_STALK && frame.can_dlc >= 2) {
    canbLastRightStalkFrame = frame;
    canbHasLastRightStalkFrame = true;
    const uint8_t status = static_cast<uint8_t>((frame.data[1] >> 4) & 0x07);
    const uint8_t counter = static_cast<uint8_t>(frame.data[1] & 0x0F);
    g_status.rightStalkStatus = status;
    g_status.rightStalkCounter = counter;
    // Only re-align our TX counter to the live SCCM counter when we are NOT
    // injecting. During an active scroll-gear burst the counter must free-run
    // strictly +1 (like the real stalk); re-syncing here would let incoming
    // idle frames yank it back and produce duplicate counters in our own TX,
    // which the gear receiver rejects.
    if (!scrollGearShiftActive) rightStalkTxCounter = counter;
  }

  if (frame.can_id == CANB_ID_BODY_LIGHTING && frame.can_dlc >= 8) {
    canbLastBodyLightingFrame = frame;
    // 0x273 uses data[7] bit0 as the brake-lamp source for rear fog strobe.
    canbHasLastBodyLightingFrame = true;

    const RuntimeConfig cfg = configSnapshot();
    const bool brakeActive = (frame.data[7] & 0x01) != 0;
    handleRearFogBrakeLampState(brakeActive, cfg);
  }

  handleBatteryTempDiagFrame(frame, 2);
  handleBatteryPreheatFeedbackFrame(frame, 2);

  if (frame.can_id == CANB_ID_VCLEFT_SWITCH && frame.can_dlc >= 4) {
    handleVcleftSwitchFrame(frame, configSnapshot(), true);
    return;
  }
}

static void drainCanBWithBudget() {
  if (!canbReady) return;

  const bool intAsserted = (digitalRead(MCP2515_INT) == LOW);
#ifdef ENABLE_LIGHT_WEBUI
  const bool recorderActive = recActive;
#else
  const bool recorderActive = false;
#endif
  const uint8_t budget = (intAsserted || recorderActive) ? CANB_RX_SCAN_LIMIT_ACTIVE : CANB_RX_SCAN_LIMIT;
  const uint32_t startUs = micros();
  for (uint8_t i = 0; i < budget; ++i) {
    can_frame frame;
    if (!canb_recv(frame)) break;
    handleCanBFrame(frame);
    if (lockDeepSleepPending) break;
    if ((micros() - startUs) >= CANB_RX_DRAIN_TIME_US) break;
  }
  updateCanBErrorStatus();
}

// VCSEC_serviceDiagnosticRequest (0x339) on the BODY bus / CAN B.
//   enter service mode -> 00 00 00 00 00 80 00 00  (byte5 bit7 = 1)
//   exit  service mode -> 00 00 00 00 00 00 00 00
// Spec: 4 frames at 10ms spacing. Default OFF; only sent after a toggle.
static void setCanBServiceMode(bool enabled) {
  canbServiceModeActive = enabled;
  canbServiceBurstByte5 = enabled ? 0x80 : 0x00;
  canbServiceBurstRemaining = 4;
  canbLastServiceBurstMs = 0;  // fire the first frame on the next tick
}

static void serviceCanBScheduledTx() {
  if (canbServiceBurstRemaining == 0 || !canbReady) return;

  const uint32_t now = millis();
  if (canbLastServiceBurstMs != 0 && (now - canbLastServiceBurstMs) < 10) return;
  canbLastServiceBurstMs = now;

  can_frame f = {};
  f.can_id = 0x339;
  f.can_dlc = 8;
  f.data[5] = canbServiceBurstByte5;
  canb_send(f);
  canbServiceBurstRemaining--;
}

#endif  // ENABLE_CANB_MCP2515

// ============================================================================
// Light WebUI -- optional SoftAP parameter page on a dedicated low-prio task
// ============================================================================
#ifdef ENABLE_LIGHT_WEBUI

#ifndef WEBUI_AP_SSID
#define WEBUI_AP_SSID "T2CAN-FSD"
#endif
#ifndef WEBUI_AP_PASS
#define WEBUI_AP_PASS "12345678"
#endif

static WebServer server(80);
static volatile bool webUiEnabled = true;
static volatile bool webUiShutdownPending = false;
static Preferences prefs;

static void loadConfigFromPrefs();
static void saveConfigToPrefs();
static void setupLightWebUi();
static void webTask(void*);

#include "web_ui_page.h"  // kIndexHtml -- kept out of the .ino prototype scanner

static void handleRoot() {
  server.send_P(200, "text/html", kIndexHtml);
}

static void handleStatus() {
  // Read-only: snapshot the cached config + status, never touch the CAN bus.
  RuntimeConfig c = configSnapshot();
  RuntimeStatus s = g_status;
  const uint32_t now = millis();
  s.batteryPreheatAgeMs = batteryPreheatLastSendMs == 0 ? 0 : (now - batteryPreheatLastSendMs);
  s.batteryPreheatFeedbackAgeMs =
      batteryPreheatFeedbackLastRxMs == 0 ? 0 : (now - batteryPreheatFeedbackLastRxMs);
  s.bmsTempFrameAgeMs = bmsTempLastRxMs == 0 ? 0 : (now - bmsTempLastRxMs);
  s.bmsTempDecodedAgeMs = bms712TempLastRxMs == 0 ? 0 : (now - bms712TempLastRxMs);
  s.lockSleepArmed = c.lockDeepSleepEnabled ? 1 : 0;
  s.lockSleepAgeMs = lockSleepLastSignalMs == 0 ? 0 : (now - lockSleepLastSignalMs);
  s.lockSleep339AgeMs = lockSleep339LastRxMs == 0 ? 0 : (now - lockSleep339LastRxMs);
  s.lockSleepCabinEmpty = lockSleepCabinEmptyReady() ? 1 : 0;
  s.lockSleep339StableAgeMs =
      lockSleep339StableStartMs == 0 ? 0 : (now - lockSleep339StableStartMs);
#ifdef ENABLE_CANB_MCP2515
  s.dndLastTriggerAgeMs = dndLastTriggerMs == 0 ? 0 : (now - dndLastTriggerMs);
  s.dndScrollCacheAgeMs = canbLastVcleftMux1Ms == 0 ? 0 : (now - canbLastVcleftMux1Ms);
#endif
  s.nagKillerMode = normalizeNagKillerMode(c.nagKillerMode);
  s.nagKillerTargetId = nagKillerTargetIdForMode(s.nagKillerMode);
  if (c.nagKillerTest052Enabled && !c.nagKillerTest370Enabled) {
    s.nagKillerTargetId = CAN_ID_NAG_MODE_B_TARGET;
  } else if (c.nagKillerTest370Enabled && !c.nagKillerTest052Enabled) {
    s.nagKillerTargetId = CAN_ID_NAG_MODE_C_TARGET;
  }
  s.nagKillerLastRxAgeMs = nagKillerLastRxMs == 0 ? 0 : (now - nagKillerLastRxMs);
  s.nagKillerLastTxAgeMs = nagKillerLastTxMs == 0 ? 0 : (now - nagKillerLastTxMs);
  s.nagKillerApAgeMs = nagKillerLastApMs == 0 ? 0 : (now - nagKillerLastApMs);
  s.nagKillerSteeringAgeMs = nagKillerLastSteeringMs == 0 ? 0 : (now - nagKillerLastSteeringMs);
  s.nagKillerApState = nagKillerApState;
  s.nagKillerHandsOnState = nagKillerHandsOnState;
  s.nagKillerSteeringDegCx10 = nagKillerLastSteeringMs == 0 ? -32768 : static_cast<int>(nagKillerSteeringAngleDeg * 10.0f);
  if (nagKillerLastTxMs == 0 || (now - nagKillerLastTxMs) > 250UL) {
    s.nagKillerActive = 0;
  }

  String j;
  j.reserve(6600);
  j += '{';
  j += "\"fsdEnabled\":";            j += c.fsdEnabled ? 1 : 0;
  j += ",\"autoSpeedOffsetEnabled\":"; j += c.autoSpeedOffsetEnabled ? 1 : 0;
  j += ",\"cabinCameraDisableEnabled\":"; j += c.cabinCameraDisableEnabled ? 1 : 0;
  j += ",\"cabinCameraTelemetryDisableEnabled\":"; j += c.cabinCameraTelemetryDisableEnabled ? 1 : 0;
  j += ",\"slewPctPerSec\":";        j += c.slewPctPerSec;
  j += ",\"lowSpeedMaxPctRaw\":";    j += c.lowSpeedMaxPctRaw;
  j += ",\"targetBelow60\":";        j += c.targetBelow60;
  j += ",\"target60\":";             j += c.target60;
  j += ",\"target70\":";             j += c.target70;
  j += ",\"target80\":";             j += c.target80;
  j += ",\"target90\":";             j += c.target90;
  j += ",\"target100\":";            j += c.target100;
  j += ",\"target120\":";            j += c.target120;
  j += ",\"canbEnabled\":";          j += c.canbEnabled ? 1 : 0;
  j += ",\"canbServiceModeEnabled\":"; j += c.canbServiceModeEnabled ? 1 : 0;
  j += ",\"canbFilterMode\":";       j += c.canbFilterMode;
  j += ",\"canbFilterEnabled\":";    j += c.canbFilterMode != CANB_FILTER_ALL ? 1 : 0;
#ifdef ENABLE_CANB_MCP2515
  j += ",\"canbReady\":";            j += canbReady ? 1 : 0;
  j += ",\"canbHardwareFilterMode\":"; j += canbHardwareFilterMode;
  j += ",\"canbHardwareFilterEnabled\":"; j += canbHardwareFilterMode != CANB_FILTER_ALL ? 1 : 0;
#else
  j += ",\"canbReady\":0";
  j += ",\"canbHardwareFilterMode\":0";
  j += ",\"canbHardwareFilterEnabled\":0";
#endif
  j += ",\"highBeamStrobeEnabled\":"; j += c.highBeamStrobeEnabled ? 1 : 0;
  j += ",\"rearFogBrakeStrobeEnabled\":"; j += c.rearFogBrakeStrobeEnabled ? 1 : 0;
  j += ",\"reverseStrobeEnabled\":"; j += c.reverseStrobeEnabled ? 1 : 0;
  j += ",\"batteryPreheatEnabled\":"; j += c.batteryPreheatEnabled ? 1 : 0;
  j += ",\"dndEnabled\":"; j += c.dndEnabled ? 1 : 0;
  j += ",\"dndVolumeEnabled\":"; j += c.dndVolumeEnabled ? 1 : 0;
  j += ",\"nagKillerEnabled\":"; j += c.nagKillerEnabled ? 1 : 0;
  j += ",\"nagKillerTest052Enabled\":"; j += c.nagKillerTest052Enabled ? 1 : 0;
  j += ",\"nagKillerTest370Enabled\":"; j += c.nagKillerTest370Enabled ? 1 : 0;
  j += ",\"nagKillerMode\":"; j += normalizeNagKillerMode(c.nagKillerMode);
  j += ",\"nagKillerBurstMs\":"; j += c.nagKillerBurstMs;
  j += ",\"nagKillerPauseMs\":"; j += c.nagKillerPauseMs;
  j += ",\"nagKillerBPos1Nm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerBPos1Cx100), 2);
  j += ",\"nagKillerBPos2Nm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerBPos2Cx100), 2);
  j += ",\"nagKillerBNeg1Nm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerBNeg1Cx100), 2);
  j += ",\"nagKillerBNeg2Nm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerBNeg2Cx100), 2);
  j += ",\"nagKillerCNegNm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerCNegCx100), 2);
  j += ",\"nagKillerCPosNm\":"; j += String(nagKillerTorqueCx100ToNm(c.nagKillerCPosCx100), 2);
  j += ",\"lockDeepSleepEnabled\":"; j += c.lockDeepSleepEnabled ? 1 : 0;
  j += ",\"scrollGearSimEnabled\":"; j += c.scrollGearSimEnabled ? 1 : 0;
  j += ",\"scrollGearInjectEnabled\":"; j += c.scrollGearInjectEnabled ? 1 : 0;
  j += ",\"can1ReceiveOnly\":"; j += c.can1ReceiveOnly ? 1 : 0;
  j += ",\"can1Rx\":";               j += s.can1Rx;
  j += ",\"can1Tx\":";               j += s.can1Tx;
  j += ",\"can1TxFail\":";           j += s.can1TxFail;
  j += ",\"twaiBusOffCount\":";      j += s.twaiBusOffCount;
  j += ",\"twaiState\":";            j += s.twaiState;
  j += ",\"canbRx\":";               j += s.canbRx;
  j += ",\"canbTx\":";               j += s.canbTx;
  j += ",\"canbTxFail\":";           j += s.canbTxFail;
  j += ",\"canbLastId\":";           j += s.canbLastId;
  j += ",\"canbErrorFlags\":";       j += s.canbErrorFlags;
  j += ",\"canbRxOverflowCount\":";  j += s.canbRxOverflowCount;
  j += ",\"highBeamStrobeActive\":"; j += s.highBeamStrobeActive;
  j += ",\"highBeamStrobeRemaining\":"; j += s.highBeamStrobeRemaining;
  j += ",\"rearFogBrakeStrobeActive\":"; j += s.rearFogBrakeStrobeActive;
  j += ",\"rearFogBrakeStrobeRemaining\":"; j += s.rearFogBrakeStrobeRemaining;
  j += ",\"reverseStrobeActive\":"; j += s.reverseStrobeActive;
  j += ",\"reverseStrobeRemaining\":"; j += s.reverseStrobeRemaining;
  j += ",\"batteryPreheatActive\":"; j += s.batteryPreheatActive;
  j += ",\"batteryPreheatTxCount\":"; j += s.batteryPreheatTxCount;
  j += ",\"batteryPreheatAgeMs\":"; j += s.batteryPreheatAgeMs;
  j += ",\"batteryPreheatFeedbackSeen\":"; j += s.batteryPreheatFeedbackSeen;
  j += ",\"batteryPreheatFeedbackBus\":"; j += s.batteryPreheatFeedbackBus;
  j += ",\"batteryPreheatFeedbackAgeMs\":"; j += s.batteryPreheatFeedbackAgeMs;
  j += ",\"batteryPreheatUiTripActive\":"; j += s.batteryPreheatUiTripActive;
  j += ",\"batteryPreheatUiNavToSupercharger\":"; j += s.batteryPreheatUiNavToSupercharger;
  j += ",\"batteryPreheatUiFastChargerType\":"; j += s.batteryPreheatUiFastChargerType;
  j += ",\"batteryPreheatUiState\":"; j += s.batteryPreheatUiState;
  j += ",\"batteryPreheatUiRequestHeat\":"; j += s.batteryPreheatUiRequestHeat;
  j += ",\"batteryPreheatUiPowerW\":"; j += s.batteryPreheatUiPowerW;
  j += ",\"batteryPreheatUiTargetCx100\":"; j += s.batteryPreheatUiTargetCx100;
  j += ",\"batteryPreheatUiAmbientCx100\":"; j += s.batteryPreheatUiAmbientCx100;
  j += ",\"batteryPreheatUiChargeTargetCx10\":"; j += s.batteryPreheatUiChargeTargetCx10;
  j += ",\"batteryPreheatUiEnergyAtDestination\":"; j += s.batteryPreheatUiEnergyAtDestination;
  j += ",\"batteryPreheatFeedbackPayload\":\""; j += s.batteryPreheatFeedbackPayload; j += '"';
  j += ",\"dndHandsOnState\":"; j += s.dndHandsOnState;
  j += ",\"dndWarningActive\":"; j += s.dndWarningActive;
  j += ",\"dndActionActive\":"; j += s.dndActionActive;
  j += ",\"dndActionType\":"; j += s.dndActionType;
  j += ",\"dndBlocked\":"; j += s.dndBlocked;
  j += ",\"dndTxCount\":"; j += s.dndTxCount;
  j += ",\"dndLastTriggerAgeMs\":"; j += s.dndLastTriggerAgeMs;
  j += ",\"dndScrollCacheAgeMs\":"; j += s.dndScrollCacheAgeMs;
  j += ",\"nagKillerMode\":"; j += s.nagKillerMode;
  j += ",\"nagKillerActive\":"; j += s.nagKillerActive;
  j += ",\"nagKillerBlocked\":"; j += s.nagKillerBlocked;
  j += ",\"nagKillerBurstActive\":"; j += s.nagKillerBurstActive;
  j += ",\"nagKillerTargetId\":"; j += s.nagKillerTargetId;
  j += ",\"nagKillerRxCount\":"; j += s.nagKillerRxCount;
  j += ",\"nagKillerTxCount\":"; j += s.nagKillerTxCount;
  j += ",\"nagKillerTxFail\":"; j += s.nagKillerTxFail;
  j += ",\"nagKillerLastRxAgeMs\":"; j += s.nagKillerLastRxAgeMs;
  j += ",\"nagKillerLastTxAgeMs\":"; j += s.nagKillerLastTxAgeMs;
  j += ",\"nagKillerApAgeMs\":"; j += s.nagKillerApAgeMs;
  j += ",\"nagKillerSteeringAgeMs\":"; j += s.nagKillerSteeringAgeMs;
  j += ",\"nagKillerApState\":"; j += s.nagKillerApState;
  j += ",\"nagKillerHandsOnState\":"; j += s.nagKillerHandsOnState;
  j += ",\"nagKillerTargetHandsOn\":"; j += s.nagKillerTargetHandsOn;
  j += ",\"nagKillerSetHandsOn\":"; j += s.nagKillerSetHandsOn;
  j += ",\"nagKillerRealTorqueCx100\":"; j += s.nagKillerRealTorqueCx100;
  j += ",\"nagKillerLastTorqueCx100\":"; j += s.nagKillerLastTorqueCx100;
  j += ",\"nagKillerSteeringDegCx10\":"; j += s.nagKillerSteeringDegCx10;
  j += ",\"bmsTempFrameSeen\":"; j += s.bmsTempFrameSeen;
  j += ",\"bmsTempFrameId\":"; j += s.bmsTempFrameId;
  j += ",\"bmsTempFrameBus\":"; j += s.bmsTempFrameBus;
  j += ",\"bmsTempFrameMux\":"; j += s.bmsTempFrameMux;
  j += ",\"bmsTempFrameAgeMs\":"; j += s.bmsTempFrameAgeMs;
  j += ",\"bmsTempFramePayload\":\""; j += s.bmsTempFramePayload; j += '"';
  j += ",\"bmsTempDecodedSeen\":"; j += s.bmsTempDecodedSeen;
  j += ",\"bmsTempDecodedMux\":"; j += s.bmsTempDecodedMux;
  j += ",\"bmsTempDecodedCount\":"; j += s.bmsTempDecodedCount;
  j += ",\"bmsTempDecodedAgeMs\":"; j += s.bmsTempDecodedAgeMs;
  j += ",\"bmsTempLatest1Cx100\":"; j += s.bmsTempLatest1Cx100;
  j += ",\"bmsTempLatest2Cx100\":"; j += s.bmsTempLatest2Cx100;
  j += ",\"bmsTempLatest3Cx100\":"; j += s.bmsTempLatest3Cx100;
  j += ",\"bmsTempMinCx100\":"; j += s.bmsTempMinCx100;
  j += ",\"bmsTempAvgCx100\":"; j += s.bmsTempAvgCx100;
  j += ",\"bmsTempMaxCx100\":"; j += s.bmsTempMaxCx100;
  j += ",\"lockSleepArmed\":"; j += s.lockSleepArmed;
  j += ",\"lockSleepTriggered\":"; j += s.lockSleepTriggered;
  j += ",\"lockSleepLastId\":"; j += s.lockSleepLastId;
  j += ",\"lockSleepSource\":"; j += s.lockSleepSource;
  j += ",\"lockSleepAgeMs\":"; j += s.lockSleepAgeMs;
  j += ",\"lockSleep339Seen\":"; j += s.lockSleep339Seen;
  j += ",\"lockSleep339SimpleStatus\":"; j += s.lockSleep339SimpleStatus;
  j += ",\"lockSleep339AgeMs\":"; j += s.lockSleep339AgeMs;
  j += ",\"lockSleepCabinEmpty\":"; j += s.lockSleepCabinEmpty;
  j += ",\"lockSleep339StableAgeMs\":"; j += s.lockSleep339StableAgeMs;
  j += ",\"lockSleepBlocked\":"; j += s.lockSleepBlocked;
  j += ",\"rightScrollTicks\":";     j += s.rightScrollTicks;
  j += ",\"rightStalkStatus\":";     j += s.rightStalkStatus;
  j += ",\"rightStalkCounter\":";    j += s.rightStalkCounter;
  j += ",\"currentGear\":";          j += s.currentGear;
  j += ",\"dasAutopilotState\":";    j += s.dasAutopilotState;
  j += ",\"brakeActive\":";          j += s.brakeActive;
  j += ",\"scrollGearIntent\":";     j += s.scrollGearIntent;
  j += ",\"scrollGearDryRun\":";     j += s.scrollGearDryRun;
  j += ",\"scrollGearInjectActive\":"; j += s.scrollGearInjectActive;
  j += ",\"scrollGearInjectTarget\":"; j += s.scrollGearInjectTarget;
  j += ",\"scrollGearInjectOk\":";   j += s.scrollGearInjectOk;
  j += ",\"scrollGearInjectBlocked\":"; j += s.scrollGearInjectBlocked;
  j += ",\"vehicleSpeedKph\":";      j += s.vehicleSpeedKph;
  j += ",\"fusedLimitKph\":";        j += s.fusedLimitKph;
  j += ",\"targetSpeedKph\":";       j += s.targetSpeedKph;
  j += ",\"offsetKph\":";            j += s.offsetKph;
  j += ",\"offsetRaw\":";            j += s.offsetRaw;
  j += ",\"uptime\":";               j += (uint32_t)(millis() / 1000);
  j += '}';
  server.send(200, "application/json", j);
}

static uint16_t argU16(const char* name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  long v = server.arg(name).toInt();
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return static_cast<uint16_t>(v);
}

static bool argBool(const char* name, bool fallback) {
  if (!server.hasArg(name)) return fallback;
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

static uint16_t argPositiveNmCx100(const char* name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  float v = server.arg(name).toFloat();
  if (v < 0.0f) v = 0.0f;
  if (v > 2.8f) v = 2.8f;
  return static_cast<uint16_t>(v * 100.0f + 0.5f);
}

static uint16_t argNegativeNmAbsCx100(const char* name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  float v = server.arg(name).toFloat();
  if (v < 0.0f) v = -v;
  if (v > 2.8f) v = 2.8f;
  return static_cast<uint16_t>(v * 100.0f + 0.5f);
}

// POST /config -- update the live config in RAM only (no Flash write here).
static void handleConfig() {
  RuntimeConfig c = configSnapshot();
  const uint8_t oldCanBFilterMode = c.canbFilterMode;

  c.fsdEnabled              = argBool("fsdEnabled", c.fsdEnabled);
  c.autoSpeedOffsetEnabled  = argBool("autoSpeedOffsetEnabled", c.autoSpeedOffsetEnabled);
  c.cabinCameraDisableEnabled = argBool("cabinCameraDisableEnabled", c.cabinCameraDisableEnabled);
  c.cabinCameraTelemetryDisableEnabled =
      argBool("cabinCameraTelemetryDisableEnabled", c.cabinCameraTelemetryDisableEnabled);
  c.slewPctPerSec           = static_cast<uint8_t>(argU16("slewPctPerSec", c.slewPctPerSec));
  c.lowSpeedMaxPctRaw       = static_cast<uint8_t>(argU16("lowSpeedMaxPctRaw", c.lowSpeedMaxPctRaw));
  c.targetBelow60           = argU16("targetBelow60", c.targetBelow60);
  c.target60                = argU16("target60", c.target60);
  c.target70                = argU16("target70", c.target70);
  c.target80                = argU16("target80", c.target80);
  c.target90                = argU16("target90", c.target90);
  c.target100               = argU16("target100", c.target100);
  c.target120               = argU16("target120", c.target120);
  c.canbEnabled             = argBool("canbEnabled", c.canbEnabled);
  if (server.hasArg("canbFilterMode")) {
    c.canbFilterMode = normalizeCanBFilterMode(static_cast<uint8_t>(argU16("canbFilterMode", c.canbFilterMode)));
  } else {
    c.canbFilterMode = argBool("canbFilterEnabled", c.canbFilterMode != CANB_FILTER_ALL) ? CANB_FILTER_FEATURE : CANB_FILTER_ALL;
  }
  c.highBeamStrobeEnabled   = argBool("highBeamStrobeEnabled", c.highBeamStrobeEnabled);
  c.rearFogBrakeStrobeEnabled = argBool("rearFogBrakeStrobeEnabled", c.rearFogBrakeStrobeEnabled);
  c.reverseStrobeEnabled    = argBool("reverseStrobeEnabled", c.reverseStrobeEnabled);
  c.batteryPreheatEnabled   = argBool("batteryPreheatEnabled", c.batteryPreheatEnabled);
  c.dndEnabled              = argBool("dndEnabled", c.dndEnabled);
  c.dndVolumeEnabled        = argBool("dndVolumeEnabled", c.dndVolumeEnabled);
  c.nagKillerEnabled        = argBool("nagKillerEnabled", c.nagKillerEnabled);
  c.nagKillerTest052Enabled = argBool("nagKillerTest052Enabled", c.nagKillerTest052Enabled);
  c.nagKillerTest370Enabled = argBool("nagKillerTest370Enabled", c.nagKillerTest370Enabled);
  c.nagKillerMode           = normalizeNagKillerMode(static_cast<uint8_t>(argU16("nagKillerMode", c.nagKillerMode)));
  c.nagKillerBurstMs        = clampNagKillerBurstMs(argU16("nagKillerBurstMs", c.nagKillerBurstMs));
  c.nagKillerPauseMs        = clampNagKillerPauseMs(argU16("nagKillerPauseMs", c.nagKillerPauseMs));
  c.nagKillerBPos1Cx100     = argPositiveNmCx100("nagKillerBPos1Nm", c.nagKillerBPos1Cx100);
  c.nagKillerBPos2Cx100     = argPositiveNmCx100("nagKillerBPos2Nm", c.nagKillerBPos2Cx100);
  c.nagKillerBNeg1Cx100     = argNegativeNmAbsCx100("nagKillerBNeg1Nm", c.nagKillerBNeg1Cx100);
  c.nagKillerBNeg2Cx100     = argNegativeNmAbsCx100("nagKillerBNeg2Nm", c.nagKillerBNeg2Cx100);
  c.nagKillerCNegCx100      = argNegativeNmAbsCx100("nagKillerCNegNm", c.nagKillerCNegCx100);
  c.nagKillerCPosCx100      = argPositiveNmCx100("nagKillerCPosNm", c.nagKillerCPosCx100);
  c.lockDeepSleepEnabled    = argBool("lockDeepSleepEnabled", c.lockDeepSleepEnabled);
  c.scrollGearSimEnabled    = argBool("scrollGearSimEnabled", c.scrollGearSimEnabled);
  c.scrollGearInjectEnabled = argBool("scrollGearInjectEnabled", c.scrollGearInjectEnabled);
  c.can1ReceiveOnly        = argBool("can1ReceiveOnly", c.can1ReceiveOnly);

  const bool newServiceMode = argBool("canbServiceModeEnabled", c.canbServiceModeEnabled);
  const bool serviceModeChanged = (newServiceMode != c.canbServiceModeEnabled);
  c.canbServiceModeEnabled  = newServiceMode;
  applyBuildModeGuards(c);

  portENTER_CRITICAL(&g_cfgMux);
  g_config = c;
  portEXIT_CRITICAL(&g_cfgMux);

#ifdef ENABLE_CANB_MCP2515
  if (canbReady && c.canbFilterMode != oldCanBFilterMode) {
    if (!applyCanBFilters(c.canbFilterMode)) {
      c.canbFilterMode = oldCanBFilterMode;
      portENTER_CRITICAL(&g_cfgMux);
      g_config = c;
      portEXIT_CRITICAL(&g_cfgMux);
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"canb_filter\"}");
      return;
    }
  }

  // Toggling service mode queues a 0x339 burst (handled non-blocking in loop()).
  if (serviceModeChanged) setCanBServiceMode(newServiceMode);
#else
  (void)serviceModeChanged;
#endif

  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /save -- persist the current RAM config to Flash (Preferences).
static void handleSave() {
  saveConfigToPrefs();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /web/off -- acknowledge, then shut the WebUI down asynchronously in the
// web task so this handler never blocks. CAN is unaffected.
static void handleWebOff() {
  server.send(200, "application/json", "{\"ok\":true}");
  webUiShutdownPending = true;
}

static void handleCanBTest() {
#ifdef ENABLE_CANB_MCP2515
  RuntimeConfig c = configSnapshot();
  if (!c.canbEnabled) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_disabled\"}");
    return;
  }
  if (!canbReady) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_not_ready\"}");
    return;
  }
  if (!server.hasArg("type")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_type\"}");
    return;
  }

  String type = server.arg("type");
  if (type == "strobe") {
    startHighBeamStrobe(true);
  } else if (type == "fog") {
    startRearFogBrakeStrobe(REAR_FOG_BODY_STROBE_PULSES, REAR_FOG_PRIORITY_BODY, true);
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_type\"}");
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
#else
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_not_compiled\"}");
#endif
}

static uint8_t parseRecIdList(const String& s, uint32_t* out, uint8_t maxCount) {
  uint8_t count = 0;
  const char* p = s.c_str();
  while (*p && count < maxCount) {
    while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    char* end = nullptr;
    uint32_t v = static_cast<uint32_t>(strtoul(p, &end, 16));
    if (end == p) break;
    out[count++] = v & CAN_SFF_MASK;
    p = end;
  }
  return count;
}

static void handleRecStart() {
  if (!recBuf || recCapacity == 0) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"psram_rec_buffer\"}");
    return;
  }

  if (server.hasArg("ids")) {
    recFilterCount = parseRecIdList(server.arg("ids"), recFilterIds, REC_FILTER_MAX);
  } else {
    recFilterCount = 0;
  }
  if (server.hasArg("exclude")) {
    recExcludeCount = parseRecIdList(server.arg("exclude"), recExcludeIds, REC_FILTER_MAX);
  } else {
    recExcludeCount = 0;
  }

  portENTER_CRITICAL(&g_recMux);
  recCount = 0;
  recDropped = 0;
  recBus1Count = 0;
  recBus2Count = 0;
  recStopReason = 0;
  recSaved = false;
  recStartMs = millis();
  recActive = true;
  portEXIT_CRITICAL(&g_recMux);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRecStop() {
  portENTER_CRITICAL(&g_recMux);
  recActive = false;
  recSaved = true;
  recStopReason = 0;
  portEXIT_CRITICAL(&g_recMux);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRecStatus() {
  if (recActive && (millis() - recStartMs >= REC_MAX_DURATION_MS)) {
    portENTER_CRITICAL(&g_recMux);
    recActive = false;
    recSaved = true;
    recStopReason = 2;
    portEXIT_CRITICAL(&g_recMux);
  }
  String j;
  j.reserve(220);
  j += "{\"active\":";
  j += recActive ? "true" : "false";
  j += ",\"count\":";
  j += recCount;
  j += ",\"cap\":";
  j += recCapacity;
  j += ",\"psram\":";
  j += recPsramReady ? "true" : "false";
  j += ",\"bytes\":";
  j += static_cast<unsigned long>(recBufferBytes);
  j += ",\"saved\":";
  j += recSaved ? "true" : "false";
  j += ",\"dropped\":";
  j += recDropped;
  j += ",\"bus1\":";
  j += recBus1Count;
  j += ",\"bus2\":";
  j += recBus2Count;
  j += ",\"stopReason\":";
  j += recStopReason;
  j += ",\"filter\":";
  j += recFilterCount;
  j += ",\"exclude\":";
  j += recExcludeCount;
  j += "}";
  server.send(200, "application/json", j);
}

static void handleRecDownload() {
  if (recActive) {
    server.send(409, "text/plain", "Stop recording before download");
    return;
  }
  if (!recSaved || recCount == 0) {
    server.send(404, "text/plain", "No recording saved yet");
    return;
  }

  const uint32_t n = recCount;
  server.sendHeader("Content-Disposition", "attachment; filename=\"can_recording.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  char chunk[1024];
  size_t used = 0;
  auto flushChunk = [&]() {
    if (used == 0) return;
    server.sendContent(chunk, used);
    used = 0;
    yield();
  };
  auto appendChunk = [&](const char* text, size_t len) {
    while (len > 0) {
      const size_t room = sizeof(chunk) - used;
      if (room == 0) flushChunk();
      const size_t take = std::min(len, sizeof(chunk) - used);
      memcpy(chunk + used, text, take);
      used += take;
      text += take;
      len -= take;
    }
  };

  static const char header[] = "ts_ms,dir,bus,controller,physical,id,dlc,b0,b1,b2,b3,b4,b5,b6,b7\n";
  appendChunk(header, sizeof(header) - 1);
  char line[128];
  for (uint32_t i = 0; i < n; ++i) {
    const RecFrame& r = recBuf[i];
    const char* controller = (r.bus == 1) ? "TWAI" : ((r.bus == 2) ? "MCP2515" : "unknown");
    const char* physical = (r.bus == 1) ? "CANB" : ((r.bus == 2) ? "CANA" : "unknown");
    snprintf(line, sizeof(line),
             "%lu,%c,%u,%s,%s,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
             static_cast<unsigned long>(r.ts),
             r.dir ? r.dir : 'R',
             static_cast<unsigned>(r.bus),
             controller,
             physical,
             static_cast<unsigned long>(r.id),
             static_cast<unsigned>(r.dlc),
             static_cast<unsigned>(r.data[0]),
             static_cast<unsigned>(r.data[1]),
             static_cast<unsigned>(r.data[2]),
             static_cast<unsigned>(r.data[3]),
             static_cast<unsigned>(r.data[4]),
             static_cast<unsigned>(r.data[5]),
             static_cast<unsigned>(r.data[6]),
             static_cast<unsigned>(r.data[7]));
    appendChunk(line, strlen(line));
    if ((i & 0x3F) == 0) flushChunk();
  }
  flushChunk();
  server.sendContent("");
}

static void loadConfigFromPrefs() {
  prefs.begin("t2can", true);
  RuntimeConfig c;  // defaults
  c.fsdEnabled             = prefs.getBool("fsdEnabled", c.fsdEnabled);
  c.autoSpeedOffsetEnabled = prefs.getBool("autoOffset", c.autoSpeedOffsetEnabled);
  c.cabinCameraDisableEnabled = prefs.getBool("cabCamOff", c.cabinCameraDisableEnabled);
  c.cabinCameraTelemetryDisableEnabled =
      prefs.getBool("cabCamTelOff", c.cabinCameraTelemetryDisableEnabled);
  c.slewPctPerSec          = prefs.getUChar("slewPct", c.slewPctPerSec);
  c.lowSpeedMaxPctRaw      = prefs.getUChar("lowRaw", c.lowSpeedMaxPctRaw);
  c.targetBelow60          = prefs.getUShort("tB60", c.targetBelow60);
  c.target60               = prefs.getUShort("t60", c.target60);
  c.target70               = prefs.getUShort("t70", c.target70);
  c.target80               = prefs.getUShort("t80", c.target80);
  c.target90               = prefs.getUShort("t90", c.target90);
  c.target100              = prefs.getUShort("t100", c.target100);
  c.target120              = prefs.getUShort("t120", c.target120);
  c.canbEnabled            = prefs.getBool("canbEn", c.canbEnabled);
  c.canbServiceModeEnabled = prefs.getBool("canbSvc", c.canbServiceModeEnabled);
  c.canbFilterMode         = prefs.getUChar("canbFiltMode",
                                  prefs.getBool("canbFilt", false) ? CANB_FILTER_FEATURE : c.canbFilterMode);
  c.canbFilterMode         = normalizeCanBFilterMode(c.canbFilterMode);
  c.highBeamStrobeEnabled  = prefs.getBool("hbStrobe", c.highBeamStrobeEnabled);
  c.rearFogBrakeStrobeEnabled = prefs.getBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  c.reverseStrobeEnabled   = prefs.getBool("revStrobe", c.reverseStrobeEnabled);
  c.batteryPreheatEnabled  = prefs.getBool("batHeat", c.batteryPreheatEnabled) ||
                             prefs.getBool("bat082Test", false);
  c.dndEnabled             = prefs.getBool("dndEn", c.dndEnabled);
  c.dndVolumeEnabled       = prefs.getBool("dndVol", c.dndVolumeEnabled);
  c.nagKillerEnabled       = prefs.getBool("nagEn", c.nagKillerEnabled);
  c.nagKillerTest052Enabled = prefs.getBool("nagT052", c.nagKillerTest052Enabled);
  c.nagKillerTest370Enabled = prefs.getBool("nagT370", c.nagKillerTest370Enabled);
  c.nagKillerMode          = normalizeNagKillerMode(prefs.getUChar("nagMode", c.nagKillerMode));
  c.nagKillerBurstMs       = clampNagKillerBurstMs(prefs.getUShort("nagBurst", c.nagKillerBurstMs));
  c.nagKillerPauseMs       = clampNagKillerPauseMs(prefs.getUShort("nagPause", c.nagKillerPauseMs));
  c.nagKillerBPos1Cx100    = clampNagKillerTorqueCx100(prefs.getUShort("nagBPos1", c.nagKillerBPos1Cx100));
  c.nagKillerBPos2Cx100    = clampNagKillerTorqueCx100(prefs.getUShort("nagBPos2", c.nagKillerBPos2Cx100));
  c.nagKillerBNeg1Cx100    = clampNagKillerTorqueCx100(prefs.getUShort("nagBNeg1", c.nagKillerBNeg1Cx100));
  c.nagKillerBNeg2Cx100    = clampNagKillerTorqueCx100(prefs.getUShort("nagBNeg2", c.nagKillerBNeg2Cx100));
  c.nagKillerCNegCx100     = clampNagKillerTorqueCx100(prefs.getUShort("nagCNeg", c.nagKillerCNegCx100));
  c.nagKillerCPosCx100     = clampNagKillerTorqueCx100(prefs.getUShort("nagCPos", c.nagKillerCPosCx100));
  c.lockDeepSleepEnabled   = prefs.getBool("lockSleep", c.lockDeepSleepEnabled);
  c.scrollGearSimEnabled   = prefs.getBool("gearSim", c.scrollGearSimEnabled);
  c.scrollGearInjectEnabled = prefs.getBool("gearInject", c.scrollGearInjectEnabled);
  c.can1ReceiveOnly        = prefs.getBool("can1RxOnly", c.can1ReceiveOnly);
  prefs.end();
  applyBuildModeGuards(c);

  portENTER_CRITICAL(&g_cfgMux);
  g_config = c;
  portEXIT_CRITICAL(&g_cfgMux);
}

static void saveConfigToPrefs() {
  RuntimeConfig c = configSnapshot();
  prefs.begin("t2can", false);
  prefs.putBool("fsdEnabled", c.fsdEnabled);
  prefs.putBool("autoOffset", c.autoSpeedOffsetEnabled);
  prefs.putBool("cabCamOff", c.cabinCameraDisableEnabled);
  prefs.putBool("cabCamTelOff", c.cabinCameraTelemetryDisableEnabled);
  prefs.putUChar("slewPct", c.slewPctPerSec);
  prefs.putUChar("lowRaw", c.lowSpeedMaxPctRaw);
  prefs.putUShort("tB60", c.targetBelow60);
  prefs.putUShort("t60", c.target60);
  prefs.putUShort("t70", c.target70);
  prefs.putUShort("t80", c.target80);
  prefs.putUShort("t90", c.target90);
  prefs.putUShort("t100", c.target100);
  prefs.putUShort("t120", c.target120);
  prefs.putBool("canbEn", c.canbEnabled);
  prefs.putBool("canbSvc", c.canbServiceModeEnabled);
  prefs.putUChar("canbFiltMode", c.canbFilterMode);
  prefs.putBool("canbFilt", c.canbFilterMode != CANB_FILTER_ALL);
  prefs.putBool("hbStrobe", c.highBeamStrobeEnabled);
  prefs.putBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  prefs.putBool("revStrobe", c.reverseStrobeEnabled);
  prefs.putBool("batHeat", c.batteryPreheatEnabled);
  prefs.putBool("dndEn", c.dndEnabled);
  prefs.putBool("dndVol", c.dndVolumeEnabled);
  prefs.putBool("nagEn", c.nagKillerEnabled);
  prefs.putBool("nagT052", c.nagKillerTest052Enabled);
  prefs.putBool("nagT370", c.nagKillerTest370Enabled);
  prefs.putUChar("nagMode", normalizeNagKillerMode(c.nagKillerMode));
  prefs.putUShort("nagBurst", c.nagKillerBurstMs);
  prefs.putUShort("nagPause", c.nagKillerPauseMs);
  prefs.putUShort("nagBPos1", clampNagKillerTorqueCx100(c.nagKillerBPos1Cx100));
  prefs.putUShort("nagBPos2", clampNagKillerTorqueCx100(c.nagKillerBPos2Cx100));
  prefs.putUShort("nagBNeg1", clampNagKillerTorqueCx100(c.nagKillerBNeg1Cx100));
  prefs.putUShort("nagBNeg2", clampNagKillerTorqueCx100(c.nagKillerBNeg2Cx100));
  prefs.putUShort("nagCNeg", clampNagKillerTorqueCx100(c.nagKillerCNegCx100));
  prefs.putUShort("nagCPos", clampNagKillerTorqueCx100(c.nagKillerCPosCx100));
  prefs.putBool("lockSleep", c.lockDeepSleepEnabled);
  prefs.putBool("gearSim", c.scrollGearSimEnabled);
  prefs.putBool("gearInject", c.scrollGearInjectEnabled);
  prefs.putBool("can1RxOnly", c.can1ReceiveOnly);
  prefs.end();
}

static void setupLightWebUi() {
  setupRecorderBuffer();
  WiFi.mode(WIFI_AP);
  // SoftAP IP / gateway = 100.100.1.1 (subnet 255.255.255.0). Must precede softAP().
  WiFi.softAPConfig(IPAddress(100, 100, 1, 1), IPAddress(100, 100, 1, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(WEBUI_AP_SSID, WEBUI_AP_PASS);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/test", HTTP_POST, handleCanBTest);
  server.on("/rec_start", HTTP_POST, handleRecStart);
  server.on("/rec_stop", HTTP_POST, handleRecStop);
  server.on("/rec_status", HTTP_GET, handleRecStatus);
  server.on("/rec_download", HTTP_GET, handleRecDownload);
  server.on("/web/off", HTTP_POST, handleWebOff);
  server.begin();
  webUiEnabled = true;
}

// Dedicated WebUI task (pinned to core 0, low priority). The CAN main loop on
// core 1 never waits on this; HTTP is only serviced while the WebUI is enabled.
static void webTask(void*) {
  for (;;) {
    if (webUiShutdownPending) {
      delay(50);
      server.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      webUiEnabled = false;
      webUiShutdownPending = false;
    }
    if (webUiEnabled) {
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#endif  // ENABLE_LIGHT_WEBUI

// ---- Main ----

void setup() {
  pinMode(PIN_LED, OUTPUT);

  delay(500);

#ifdef ENABLE_LIGHT_WEBUI
  loadConfigFromPrefs();
#endif

  // Configure TWAI (CAN) peripheral at 500 kbps
  twai_general_config_t g_config_twai = TWAI_GENERAL_CONFIG_DEFAULT(
    static_cast<gpio_num_t>(TWAI_TX_PIN),
    static_cast<gpio_num_t>(TWAI_RX_PIN),
    TWAI_MODE_NORMAL);
  g_config_twai.rx_queue_len = TWAI_RX_QUEUE_LEN;
  g_config_twai.tx_queue_len = TWAI_TX_QUEUE_LEN;
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = { CAN_ACCEPT_CODE, CAN_ACCEPT_MASK, true };

  twai_driver_install(&g_config_twai, &t_config, &f_config);
  twai_start();
  twai_reconfigure_alerts(TWAI_ALERT_MASK, nullptr);

#ifdef ENABLE_CANB_MCP2515
  setupCanB();
#endif

#ifdef ENABLE_LIGHT_WEBUI
  setupLightWebUi();
  xTaskCreatePinnedToCore(webTask, "web", 4096, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
  serviceLockDeepSleep();
  serviceTwaiAlerts();

  bool didWork = false;

  can_frame frame;
  if (twai_recv(frame)) {
    didWork = true;
    digitalWrite(PIN_LED, LOW);
    recordCanFrame(frame, 'R', 1);
    handleBatteryTempDiagFrame(frame, 1);
    handleBatteryPreheatFeedbackFrame(frame, 1);
    RuntimeConfig cfg = configSnapshot();
    speedLimitMonitor.update(frame);
    handleNagKillerContextFrame(frame);
#ifndef ENABLE_CANB_MCP2515
    handleNagKillerTargetFrame(frame, cfg);
#endif
#ifdef ENABLE_CANB_MCP2515
    handleDndHandsOnFrame(frame);
    observeLockSleepOccupancyFrame(frame);
#endif
    handler.refreshUnifiedSpeedCompensation(cfg);
    handler.handelMessage(frame, cfg);
  }

#ifdef ENABLE_CANB_MCP2515
  RuntimeConfig canbCfg = configSnapshot();
  if (canbCfg.canbEnabled) {
    drainCanBWithBudget();
    serviceCanBScheduledTx();
    serviceHighBeamStrobe(canbCfg);
    serviceReverseStrobe(canbCfg);
    serviceRearFogBrakeStrobe(canbCfg);
    serviceScrollGearShift(canbCfg);
    serviceDndVolumeAuto(canbCfg);
    serviceDndScrollAction(canbCfg);
  } else {
    serviceHighBeamStrobe(canbCfg);
    serviceReverseStrobe(canbCfg);
    serviceRearFogBrakeStrobe(canbCfg);
    serviceScrollGearShift(canbCfg);
    serviceDndVolumeAuto(canbCfg);
    serviceDndScrollAction(canbCfg);
  }
#endif

  if (lockDeepSleepPending) {
    serviceLockDeepSleep();
    return;
  }

  RuntimeConfig preheatCfg = configSnapshot();
  serviceBatteryPreheat(preheatCfg);

  if (!didWork) {
    digitalWrite(PIN_LED, HIGH);
  }
}

