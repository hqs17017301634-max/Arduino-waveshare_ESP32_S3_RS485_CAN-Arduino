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
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

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
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_netif.h>
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
constexpr uint32_t TWAI_TX_WAIT_MS = 2;
constexpr uint32_t TWAI_RX_WAIT_MS = 1;
constexpr uint32_t TWAI_TX_RETRY_DELAY_MS = 1;
constexpr uint8_t TWAI_TX_RETRY_COUNT = 1;
constexpr uint8_t TWAI_RX_SCAN_LIMIT = 8;
constexpr uint8_t TWAI_RX_DRAIN_FRAME_LIMIT = 4;
constexpr uint32_t TWAI_RX_DRAIN_TIME_US = 500;
constexpr uint32_t DIAG_TX_SLOW_US = 2000;

constexpr uint8_t CANB_FILTER_ALL = 0;
constexpr uint8_t CANB_FILTER_FEATURE = 1;
constexpr uint8_t CANB_TX_SRC_OTHER = 0;
constexpr uint8_t CANB_TX_SRC_NAG = 1;
constexpr uint8_t CANB_TX_SRC_BATTERY = 2;
constexpr uint8_t CANB_TX_SRC_SERVICE = 3;
constexpr uint8_t CANB_TX_SRC_SCROLL_GEAR = 4;
constexpr uint8_t CANB_TX_SRC_DND = 5;
constexpr uint8_t CANB_TX_SRC_LIGHT = 6;
constexpr uint8_t CANB_TX_SRC_REAR_FOG = 7;
constexpr uint8_t CANB_TX_SRC_REVERSE = 8;
constexpr uint8_t CANB_TX_SRC_COUNT = 9;
constexpr uint8_t CANB_TX_PRIO_HIGH = 0;
constexpr uint8_t CANB_TX_PRIO_MED = 1;
constexpr uint8_t CANB_TX_PRIO_LOW = 2;
constexpr uint8_t CANB_TX_QUEUE_CAP = 16;
constexpr uint8_t CANB_TX_SCHED_MAX_PER_LOOP = 2;
constexpr uint16_t CANB_TX_TTL_FAST_MS = 100;
constexpr uint16_t CANB_TX_TTL_SCROLL_MS = 150;
constexpr uint16_t CANB_TX_TTL_DEFAULT_MS = 300;
constexpr uint16_t CANB_TX_TTL_PREHEAT_MS = 600;
constexpr uint8_t NAG_KILLER_MODE_B = 1;
constexpr uint8_t NAG_KILLER_MODE_C = 2;
constexpr uint8_t NAG_KILLER_MODE_DOC = 3;
constexpr uint16_t NAG_KILLER_TORQUE_MAX_CX100 = 280; // 2.80 Nm
constexpr uint16_t NAG_KILLER_TORQUE_RAW_BASE = 2050;
constexpr uint16_t NAG_KILLER_TORQUE_RAW_MIN =
    NAG_KILLER_TORQUE_RAW_BASE - NAG_KILLER_TORQUE_MAX_CX100;
constexpr uint16_t NAG_KILLER_TORQUE_RAW_MAX =
    NAG_KILLER_TORQUE_RAW_BASE + NAG_KILLER_TORQUE_MAX_CX100;
constexpr uint8_t NAG_DND_HANDS_PRIMARY_MIN = 3;
constexpr uint8_t NAG_DND_HANDS_PRIMARY_MAX = 6;
constexpr uint8_t NAG_DND_HANDS_ESCALATED_MIN = 9;
constexpr uint8_t NAG_DND_HANDS_ESCALATED_MAX = 10;
constexpr uint8_t NAG_DND_ACTION_COUNT = 1;
constexpr uint8_t DAS_LC_REASON_DECODE_OK = 0;
constexpr uint8_t DAS_LC_REASON_DECODE_NO_FRAME = 1;
constexpr uint8_t DAS_LC_REASON_DECODE_BAD_DLC = 2;
constexpr uint8_t DAS_LC_REASON_DECODE_NO_LAYOUT = 3;
constexpr uint8_t DAS_LC_REASON_DECODE_INVALID_VALUE = 4;
constexpr uint16_t FSD_ACTIVATION_RESEND_DEFAULT_PERIOD_MS = 100;
constexpr uint16_t FSD_ACTIVATION_RESEND_MIN_PERIOD_MS = 1;
constexpr uint16_t FSD_ACTIVATION_RESEND_MAX_PERIOD_MS = 1000;

// ---- Runtime configuration (WebUI-tunable; defaults match the legacy constants) ----
// CAN A reads this every relevant frame, so updates must stay cheap. The legacy
// firmware used plain constexpr values; the defaults below are byte-for-byte
// equivalent, so behaviour with no WebUI is unchanged.
struct RuntimeConfig {
  bool fsdEnabled = true;
  bool canCommsEnabled = true;       // master TX gate; keeps non-FSD CAN features independent from FSD activation
  bool fsdActivationResendEnabled = false;
  uint16_t fsdActivationResendMs = FSD_ACTIVATION_RESEND_DEFAULT_PERIOD_MS;
  bool autoSpeedOffsetEnabled = true;
  bool cabinCameraDisableEnabled = false; // when enabled, write 0x3FD mux1 bit43 to 0
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
  uint8_t canbFilterMode = CANB_FILTER_FEATURE; // 0=capture/debug all, 1=current feature IDs
  bool highBeamStrobeEnabled = false; // arms double-pull flash-to-pass trigger
  bool overtakeLightAlwaysOnEnabled = false; // unconditional 0x249 flash-to-pass PULL
  bool rearFogBrakeStrobeEnabled = false; // arms brake-triggered 0x273 rear fog burst
  bool reverseStrobeEnabled = false;  // arms reverse-gear hazard + rear-fog burst
  bool batteryPreheatEnabled = false; // sends fixed UI_tripPlanning 0x082 every 500 ms
  bool dndEnabled = false;             // continuous left scroll up/down on 0x3C2
  bool nagKillerEnabled = false;       // experimental steering torque echo
  bool nagKillerDndEnabled = false;    // 0x399 hands-on 3..6/9..10 triggers scroll DND actions
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
  bool scrollGearInjectEnabled = false; // experimental: inject 0x229 right-stalk D/R request
  bool can1ReceiveOnly = false;        // bus=1/TWAI/physical CANB RX-only: gate TWAI TX
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

static uint16_t clampFsdActivationResendMs(uint16_t value) {
  if (value < FSD_ACTIVATION_RESEND_MIN_PERIOD_MS) return FSD_ACTIVATION_RESEND_MIN_PERIOD_MS;
  if (value > FSD_ACTIVATION_RESEND_MAX_PERIOD_MS) return FSD_ACTIVATION_RESEND_MAX_PERIOD_MS;
  return value;
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
  uint32_t twaiRxQueue = 0;
  uint32_t twaiTxQueue = 0;
  uint32_t twaiRxQueueMax = 0;
  uint32_t twaiTxQueueMax = 0;
  uint32_t twaiRxMissed = 0;
  uint32_t twaiRxOverrun = 0;
  uint32_t twaiBusError = 0;
  uint32_t twaiTxFailed = 0;
  uint8_t twaiTxErr = 0;
  uint8_t twaiRxErr = 0;

  uint32_t canbRx = 0;
  uint32_t canbTx = 0;
  uint32_t canbTxFail = 0;
  uint32_t canbLastId = 0;
  uint8_t canbErrorFlags = 0;
  uint32_t canbRxOverflowCount = 0;
  uint32_t diagWindowMs = 0;
  uint32_t loopHz = 0;
  uint32_t loopAvgUs = 0;
  uint32_t loopMaxUs = 0;
  uint32_t loopPeriodMaxUs = 0;
  uint32_t loopBusyPct = 0;
  uint32_t cpuPct = 0;
  uint32_t cpu0Pct = 0;
  uint32_t cpu1Pct = 0;
  uint32_t loopWaitAvgUs = 0;
  uint32_t can1RxRate = 0;
  uint32_t can1TxRate = 0;
  uint32_t can1TxFailRate = 0;
  uint32_t can1RxGapMaxUs = 0;
  uint32_t can1TxMaxUs = 0;
  uint32_t can1TxSlowCount = 0;
  uint32_t canbRxRate = 0;
  uint32_t canbTxRate = 0;
  uint32_t canbTxFailRate = 0;
  uint32_t canbRxGapMaxUs = 0;
  uint32_t canbTxMaxUs = 0;
  uint32_t canbTxSlowCount = 0;
  uint32_t canbDrainMaxUs = 0;
  uint8_t canbDrainMaxFrames = 0;
  uint8_t canbTxLoopLast = 0;
  uint8_t canbTxLoopMax = 0;
  uint8_t canbTxLoopMaxEver = 0;
  uint32_t canbTxSrcOther = 0;
  uint32_t canbTxSrcNag = 0;
  uint32_t canbTxSrcBattery = 0;
  uint32_t canbTxSrcService = 0;
  uint32_t canbTxSrcScrollGear = 0;
  uint32_t canbTxSrcDnd = 0;
  uint32_t canbTxSrcLight = 0;
  uint32_t canbTxSrcRearFog = 0;
  uint32_t canbTxSrcReverse = 0;
  uint8_t canbTxQueueDepth = 0;
  uint8_t canbTxQueueMaxDepth = 0;
  uint32_t canbTxSchedTx = 0;
  uint32_t canbTxSchedDrop = 0;
  uint32_t canbTxSchedExpired = 0;
  uint32_t canbTxSchedFail = 0;
  uint32_t canbTxSchedBudgetHit = 0;
  uint8_t fsdActivationResendActive = 0;
  uint8_t fsdActivationResendCachedMuxMask = 0;
  uint16_t fsdActivationResendPeriodMs = 0;
  uint32_t fsdActivationResendTxCount = 0;
  uint32_t fsdActivationResendLastTxAgeMs = 0;
  uint32_t webTaskMaxUs = 0;
  uint32_t totalHeapBytes = 0;
  uint32_t freeHeapBytes = 0;
  uint32_t minFreeHeapBytes = 0;
  uint32_t freeHeapPct = 0;
  uint32_t minFreeHeapPct = 0;
  uint32_t cpuMhz = 0;
  uint8_t batteryPreheatActive = 0;
  uint32_t batteryPreheatTxCount = 0;
  uint32_t batteryPreheatAgeMs = 0;
  uint32_t batteryPreheatRunMs = 0;
  uint32_t batteryPreheatStableTempMs = 0;
  uint8_t batteryPreheatAutoOffReason = 0;
  uint8_t batteryPreheatAutoOffLatched = 0;
  uint8_t batteryPreheatChargeDetected = 0;
  uint16_t batteryPreheatBlockMask = 0;
  uint8_t batteryPreheatChargeStatusBus = 0;
  uint8_t batteryPreheatChargeStatus = 255;
  uint32_t batteryPreheatChargeStatusAgeMs = 0;
  uint8_t batteryPreheatSocBus = 0;
  int batteryPreheatSocUiDeciPct = -1;
  uint32_t batteryPreheatSocAgeMs = 0;
  uint8_t batteryPreheatFeedbackSeen = 0;
  uint8_t batteryPreheatFeedbackBus = 0;
  uint32_t batteryPreheatFeedbackAgeMs = 0;
  uint8_t batteryPreheatUiTripActive = 0;
  uint8_t batteryPreheatUiNavToSupercharger = 0;
  uint8_t batteryPreheatUiFastChargerType = 0;
  uint8_t batteryPreheatUiState = 0;
  uint8_t batteryPreheatUiRequestHeat = 0;
  uint8_t batteryPreheatHeatingActive = 255; // 255 = unknown until 0x082 feedback is seen
  int batteryPreheatUiPowerW = -32768;
  int batteryPreheatUiTargetCx100 = -32768;
  uint8_t batteryPreheatBms312Seen = 0;
  uint8_t batteryPreheatBms312Bus = 0;
  uint32_t batteryPreheatBms312AgeMs = 0;
  uint8_t batteryPreheatBms3b2Seen = 0;
  uint8_t batteryPreheatBms3b2Bus = 0;
  uint32_t batteryPreheatBms3b2AgeMs = 0;
  uint8_t batteryPreheatBmsHeatStatus = 0;
  uint8_t batteryPreheatVcfrontSeen = 0;
  uint8_t batteryPreheatVcfrontBus = 0;
  uint32_t batteryPreheatVcfrontAgeMs = 0;
  int batteryPreheatVcfrontCoolantBatInletCx100 = -32768;
  int batteryPreheatVcfrontCoolantPtInletCx100 = -32768;
  int batteryPreheatVcfrontAmbientCx100 = -32768;
  int batteryPreheatVcfrontAmbientFilteredCx100 = -32768;
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
  uint8_t nagKillerDndActionCount = NAG_DND_ACTION_COUNT;
  uint8_t nagKillerDndRemaining = 0;
  uint32_t nagKillerDndTriggerCount = 0;
  uint32_t nagKillerDndLastTriggerAgeMs = 0;
  uint8_t dasLcHandsOnReasonSeen = 0;
  uint8_t dasLcHandsOnReasonDlc = 0;
  uint8_t dasLcHandsOnReasonDecode = 1;
  uint8_t dasLcHandsOnReasonPrevious = 255;
  uint8_t dasLcHandsOnReasonLatest = 255;
  uint32_t dasLcHandsOnReasonPreviousAgeMs = 0;
  uint32_t dasLcHandsOnReasonLatestAgeMs = 0;
  uint8_t bmsTempDecodedCount = 0;
  uint32_t bmsTempDecodedAgeMs = 0;
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
  uint8_t scrollGearInjectActive = 0;
  uint8_t scrollGearInjectTarget = 0; // 2=R, 4=D
  uint8_t scrollGearInjectOk = 0;
  uint8_t scrollGearInjectBlocked = 0;
  int vehicleSpeedKph = 0;

  int fusedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t offsetRaw = 0;
};

static RuntimeStatus g_status;

static uint32_t diagWindowStartMs = 0;
static uint32_t diagLoopLastStartUs = 0;
static uint32_t diagLoopCount = 0;
static uint64_t diagLoopBusyUs = 0;
static uint64_t diagLoopWaitUs = 0;
static uint32_t diagLoopMaxUs = 0;
static uint32_t diagLoopPeriodMaxUs = 0;
static uint32_t diagCan1LastRxUs = 0;
static uint32_t diagCan1RxGapMaxUs = 0;
static uint32_t diagCan1TxMaxUs = 0;
static uint32_t diagCan1TxSlowCount = 0;
static uint32_t diagCanbLastRxUs = 0;
static uint32_t diagCanbRxGapMaxUs = 0;
static uint32_t diagCanbTxMaxUs = 0;
static uint32_t diagCanbTxSlowCount = 0;
static uint32_t diagCanbDrainMaxUs = 0;
static uint8_t diagCanbDrainMaxFrames = 0;
static uint8_t diagCanbTxThisLoop = 0;
static uint8_t diagCanbTxLoopMax = 0;
static uint8_t diagCanbTxLoopMaxEver = 0;
static uint32_t diagCanbTxSourceCounts[CANB_TX_SRC_COUNT] = {};
static volatile uint32_t diagWebTaskMaxUs = 0;
static uint32_t diagPrevCan1Rx = 0;
static uint32_t diagPrevCan1Tx = 0;
static uint32_t diagPrevCan1TxFail = 0;
static uint32_t diagPrevCanbRx = 0;
static uint32_t diagPrevCanbTx = 0;
static uint32_t diagPrevCanbTxFail = 0;
static volatile uint32_t diagCpuIdleLoops[2] = {0, 0};
static uint32_t diagPrevCpuIdleLoops[2] = {0, 0};
static uint32_t diagCpuIdleLoopsPerSecMax[2] = {0, 0};

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
  c.dndEnabled = false;
  c.nagKillerEnabled = false;
  c.nagKillerTest052Enabled = false;
  c.nagKillerTest370Enabled = false;
  c.overtakeLightAlwaysOnEnabled = false;
  c.scrollGearInjectEnabled = false;
  c.nagKillerDndEnabled = c.cabinCameraDisableEnabled;
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
static uint8_t recBufferMem = 0; // 0=none, 1=PSRAM, 2=internal RAM
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
  recBufferBytes = static_cast<size_t>(REC_TARGET_CAP) * sizeof(RecFrame);
  if (recPsramReady) {
    recBuf = static_cast<RecFrame*>(
        heap_caps_malloc(recBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (recBuf) recBufferMem = 1;
  }
  if (!recBuf) {
    recBuf = static_cast<RecFrame*>(
        heap_caps_malloc(recBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (recBuf) recBufferMem = 2;
  }
  if (!recBuf) {
    recBufferBytes = 0;
    recCapacity = 0;
    recBufferMem = 0;
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
static uint32_t batteryPreheatStartMs = 0;
static uint32_t batteryPreheatTargetStableStartMs = 0;
static bool batteryPreheatPrevEnabled = false;
static bool batteryPreheatAutoOffLatched = false;
static uint8_t batteryPreheatAutoOffReason = 0;
static bool batteryPreheatChargeDetected = false; // Reserved until the charging-state CAN signal is validated.
static int batteryPreheatSocUiDeciPct = -1; // BMS_socUI, 0.1% units from 0x292.
static uint32_t batteryPreheatSocLastRxMs = 0;
static uint32_t batteryPreheatFeedbackLastRxMs = 0;
static uint32_t batteryPreheatBmsStatusLastRxMs = 0;
static uint32_t batteryPreheatBms312LastRxMs = 0;
static uint32_t batteryPreheatBms3b2LastRxMs = 0;
static uint32_t batteryPreheatVcfrontLastRxMs = 0;
static uint32_t dasCarLogLastRxMs = 0;
static uint32_t dasLcHandsOnReasonPreviousMs = 0;
static uint32_t dasLcHandsOnReasonLatestMs = 0;
static can_frame fsdActivationResendFrames[3] = {};
static bool fsdActivationResendHasFrame[3] = {};
static bool fsdActivationResendPrevEnabled = false;
static uint32_t fsdActivationResendNextMs = 0;
static uint32_t fsdActivationResendLastTxMs = 0;
static uint32_t fsdActivationResendTxCount = 0;
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
static uint8_t nagKillerDndRemainingActions = 0;
static uint32_t nagKillerDndNextActionMs = 0;
static uint32_t nagKillerDndLastTriggerMs = 0;
static uint32_t nagKillerDndTriggerCount = 0;
static bool nagKillerDndHandsRangeActive = false;
static uint32_t bms712TempLastRxMs = 0;
static uint32_t bmsTempDecodedLastRxMs = 0;
static int16_t bms712TempCx100[12] = {};
static uint16_t bms712TempValidMask = 0;

static bool isRelevantCanId(uint32_t canId);
static void serviceTwaiAlerts();
static void serviceFsdActivationResend(const RuntimeConfig& cfg);
static void cacheFsdActivationResendFrame(const can_frame& frame, uint8_t mux, const RuntimeConfig& cfg);
static void serviceBatteryPreheat(const RuntimeConfig& cfg);
static void handleBatteryPreheatFeedbackFrame(const can_frame& frame, uint8_t bus);
static void handleBatteryPreheatBmsDiagFrame(const can_frame& frame, uint8_t bus);
static bool readBitsLE(const can_frame& frame, uint8_t startBit, uint8_t length, uint32_t& value);
#ifdef ENABLE_CANB_MCP2515
static bool canbIsReady();
static bool canb_send(const can_frame& frame, uint8_t source = CANB_TX_SRC_OTHER);
static bool canbScheduleTx(const can_frame& frame, uint8_t source, uint8_t priority,
                           uint16_t ttlMs, bool replaceSameSourceId = false);
#endif

static bool twai_send(const can_frame& frame) {
  if (!g_config.canCommsEnabled) return false;
  if (g_config.can1ReceiveOnly) return false;  // bus=1/TWAI/physical CANB RX-only
  if (frame.can_dlc > 8) return false;
  const uint32_t txStartUs = micros();

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
      const uint32_t txUs = micros() - txStartUs;
      if (txUs > diagCan1TxMaxUs) diagCan1TxMaxUs = txUs;
      if (txUs > DIAG_TX_SLOW_US) diagCan1TxSlowCount++;
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
  // T-2CAN: Nag-Killer lives on bus=2 / MCP2515 / physical CANA.
  // Keep only the newest torque frame per target ID so stale torque never builds up.
  return canbScheduleTx(frame, CANB_TX_SRC_NAG, CANB_TX_PRIO_HIGH,
                        CANB_TX_TTL_FAST_MS, true);
#else
  return twai_send(frame);
#endif
}

static bool twai_recv(can_frame& frame, bool waitFirst) {
  twai_message_t msg;

  for (uint8_t i = 0; i < TWAI_RX_SCAN_LIMIT; ++i) {
    TickType_t waitTicks = (waitFirst && i == 0) ? pdMS_TO_TICKS(TWAI_RX_WAIT_MS) : 0;
    const uint32_t waitStartUs = micros();
    const esp_err_t recvResult = twai_receive(&msg, waitTicks);
    diagLoopWaitUs += micros() - waitStartUs;
    if (recvResult != ESP_OK) return false;
    if (msg.extd || msg.rtr || msg.data_length_code > 8) {
      continue;
    }
    if (!isRelevantCanId(msg.identifier)) continue;

    frame.can_id  = msg.identifier;
    frame.can_dlc = msg.data_length_code;
    memset(frame.data, 0, sizeof(frame.data));
    memcpy(frame.data, msg.data, frame.can_dlc);
    const uint32_t rxUs = micros();
    if (diagCan1LastRxUs != 0) {
      const uint32_t gapUs = rxUs - diagCan1LastRxUs;
      if (gapUs > diagCan1RxGapMaxUs) diagCan1RxGapMaxUs = gapUs;
    }
    diagCan1LastRxUs = rxUs;
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
  g_status.twaiRxQueue = statusInfo.msgs_to_rx;
  g_status.twaiTxQueue = statusInfo.msgs_to_tx;
  if (statusInfo.msgs_to_rx > g_status.twaiRxQueueMax) g_status.twaiRxQueueMax = statusInfo.msgs_to_rx;
  if (statusInfo.msgs_to_tx > g_status.twaiTxQueueMax) g_status.twaiTxQueueMax = statusInfo.msgs_to_tx;
  g_status.twaiRxMissed = statusInfo.rx_missed_count;
  g_status.twaiRxOverrun = statusInfo.rx_overrun_count;
  g_status.twaiBusError = statusInfo.bus_error_count;
  g_status.twaiTxFailed = statusInfo.tx_failed_count;
  g_status.twaiTxErr = statusInfo.tx_error_counter;
  g_status.twaiRxErr = statusInfo.rx_error_counter;
}

// ---- CAN IDs ----

constexpr uint32_t CAN_ID_FOLLOW_DISTANCE = 1016; // 0x3F8 UI_driverAssistControl
constexpr uint32_t CAN_ID_UI_DRIVER_ASSIST_CONTROL = CAN_ID_FOLLOW_DISTANCE;
constexpr uint32_t CAN_ID_AP_CONTROL = 1021;
constexpr uint32_t CAN_ID_UI_TRIP_PLANNING = 0x082;
constexpr uint32_t CAN_ID_NAG_MODE_B_TARGET = 0x052;
constexpr uint32_t CAN_ID_NAG_MODE_C_TARGET = 0x370;
constexpr uint32_t CAN_ID_NAG_STEERING_ANGLE = 0x129;
constexpr uint32_t CAN_ID_EPAS_SYS_STATUS = 0x313;
constexpr uint32_t CAN_ID_DAS_STATUS2 = 0x389;
constexpr uint32_t CAN_ID_BMS_STATUS = 0x212;
constexpr uint32_t CAN_ID_BMS_SOC_STATUS = 0x292;
constexpr uint32_t CAN_ID_BMS_THERMAL_STATUS = 0x312;
constexpr uint32_t CAN_ID_VCFRONT_SENSORS = 0x321;
constexpr uint32_t CAN_ID_BMS_BMB_MIN_MAX = 0x332;
constexpr uint32_t CAN_ID_BMS_LOG1 = 0x374;
constexpr uint32_t CAN_ID_BMS_LOG2 = 0x3B2;
constexpr uint32_t CAN_ID_BMS_PACK_TEMPERATURES = 0x712;
constexpr uint32_t CAN_ID_DAS_STATUS = 0x399;
constexpr uint32_t CAN_ID_DAS_CAR_LOG = 0x5D9;
constexpr uint32_t CAN_ID_UI_VEHICLE_CONTROL2 = 0x3B3;
constexpr uint32_t CAN_ID_VCFRONT_ALERT_MATRIX = 0x340;
constexpr uint32_t CAN_ID_VCFRONT1_ALERT_MATRIX = 0x341;
constexpr uint32_t CAN_ID_VCFRONT2_ALERT_MATRIX = 0x342;
constexpr uint32_t CAN_ID_VCLEFT_ALERT_MATRIX = 0x360;
constexpr uint32_t CAN_ID_USM_ALERT_MATRIX = 0x3BA;
constexpr uint32_t CAN_ID_VCRIGHT_ALERT_MATRIX = 0x3C0;
constexpr uint32_t CAN_ID_EPBL_ALERT_MATRIX = 0x3C8;
constexpr uint32_t CAN_ID_VCBATT0_ALERT_MATRIX = 0x3CD;
constexpr uint32_t CAN_ID_VCBATT1_ALERT_MATRIX = 0x3CE;
constexpr uint32_t CAN_ID_VCBATT2_ALERT_MATRIX = 0x3CF;
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
         canId == CAN_ID_DAS_STATUS2 ||
         canId == CAN_ID_BRAKE_PEDAL ||
         canId == CAN_ID_RCM_INERTIAL2_CH ||
         canId == CAN_ID_RCM_INERTIAL2_ETH ||
         canId == CAN_ID_DI_SYSTEM_STATUS ||
         canId == CAN_ID_DI_CHASSIS_CONTROL ||
         canId == CAN_ID_ESP_BRAKE_TORQUE ||
         canId == CAN_ID_DIF_TORQUE ||
         canId == CAN_ID_VEHICLE_SPEED ||
         canId == CAN_ID_BMS_STATUS ||
         canId == CAN_ID_BMS_SOC_STATUS ||
         canId == CAN_ID_BMS_THERMAL_STATUS ||
         canId == CAN_ID_VCFRONT_SENSORS ||
         canId == CAN_ID_BMS_BMB_MIN_MAX ||
         canId == CAN_ID_BMS_LOG1 ||
         canId == CAN_ID_BMS_LOG2 ||
         canId == CAN_ID_BMS_PACK_TEMPERATURES ||
         canId == CAN_ID_DAS_STATUS ||
         canId == CAN_ID_DAS_CAR_LOG ||
         canId == CAN_ID_UI_VEHICLE_CONTROL2 ||
         canId == CAN_ID_VCFRONT_ALERT_MATRIX ||
         canId == CAN_ID_USM_ALERT_MATRIX ||
         canId == CAN_ID_FOLLOW_DISTANCE ||
         canId == CAN_ID_AP_CONTROL;
}

// Battery-preheat 0x082 UI_tripPlanning. This is the fixed payload proven on
// the vehicle; the old dynamic-template and companion-frame replay paths were
// removed so this switch has exactly one CAN behavior.
static const uint8_t BATTERY_PREHEAT_ON[8] = {0xAF, 0x50, 0xA8, 0x80, 0xFF, 0x03, 0x00, 0x80};
static const uint8_t BATTERY_PREHEAT_OFF[8] = {0x01, 0x50, 0xA8, 0x80, 0xFF, 0x03, 0x00, 0x80};
constexpr uint32_t BATTERY_PREHEAT_PERIOD_MS = 500UL;
constexpr int BATTERY_PREHEAT_TARGET_CX100 = 4200;
constexpr int BATTERY_PREHEAT_MAX_CX100 = 4500;
constexpr uint32_t BATTERY_PREHEAT_TARGET_STABLE_MS = 10000UL;
constexpr uint32_t BATTERY_PREHEAT_MAX_RUN_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t BATTERY_PREHEAT_TEMP_FRESH_MS = 30000UL;
constexpr uint32_t BATTERY_PREHEAT_SOC_FRESH_MS = 30000UL;
constexpr int BATTERY_PREHEAT_MIN_SOC_PCT = 5;
constexpr int BATTERY_PREHEAT_MIN_SOC_DECI_PCT = BATTERY_PREHEAT_MIN_SOC_PCT * 10;
constexpr uint8_t BATTERY_PREHEAT_OFF_NONE = 0;
constexpr uint8_t BATTERY_PREHEAT_OFF_AVG_TEMP = 1;
constexpr uint8_t BATTERY_PREHEAT_OFF_MAX_TEMP = 2;
constexpr uint8_t BATTERY_PREHEAT_OFF_CHARGING = 3;
constexpr uint8_t BATTERY_PREHEAT_OFF_TIMEOUT = 4;
constexpr uint8_t BATTERY_PREHEAT_OFF_USER = 5;
constexpr uint8_t BATTERY_PREHEAT_OFF_LOW_SOC = 6;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_DISABLED = 1U << 0;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_CANB_DISABLED = 1U << 1;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_CANB_NOT_READY = 1U << 2;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_LOW_SOC = 1U << 3;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_CHARGING = 1U << 4;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_MAX_TEMP = 1U << 5;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_AVG_TEMP = 1U << 6;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_TIMEOUT = 1U << 7;
constexpr uint16_t BATTERY_PREHEAT_BLOCK_CAN_COMMS_DISABLED = 1U << 8;
constexpr int TEMPERATURE_SNA_CX100 = -32768;
static uint8_t batteryPreheatOffFramesLeft = 0;

static int8_t signedByte(uint8_t raw) {
  return raw >= 0x80 ? static_cast<int8_t>(static_cast<int>(raw) - 256) : static_cast<int8_t>(raw);
}

static bool decodeOffsetHalfDegCx100(uint8_t raw, int& out) {
  if (raw == 0 || raw == 0xFF) return false;
  out = static_cast<int>(raw) * 50 - 4000;
  return out > -5000 && out < 9000;
}

static int decodeVcfrontCoolantCx100(uint32_t raw, uint32_t sna) {
  if (raw == sna) return TEMPERATURE_SNA_CX100;
  return (static_cast<int>(raw) * 125 + 5) / 10 - 4000;
}

static int decodeVcfrontAmbientCx100(uint32_t raw) {
  if (raw == 0) return TEMPERATURE_SNA_CX100;
  return static_cast<int>(raw) * 50 - 4000;
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
}

static void handleBatteryPreheatBmsDiagFrame(const can_frame& frame, uint8_t bus) {
  if (frame.can_id == CAN_ID_BMS_STATUS && frame.can_dlc >= 5) {
    const uint8_t chargeStatus = static_cast<uint8_t>(frame.data[4] & 0x07);
    batteryPreheatBmsStatusLastRxMs = millis();
    g_status.batteryPreheatChargeStatusBus = bus;
    g_status.batteryPreheatChargeStatus = chargeStatus;
    batteryPreheatChargeDetected = (chargeStatus == 3 || chargeStatus == 4);
    return;
  }

  if (frame.can_id == CAN_ID_BMS_SOC_STATUS && frame.can_dlc >= 3) {
    uint32_t raw = 0;
    const uint32_t now = millis();
    batteryPreheatSocLastRxMs = now;
    g_status.batteryPreheatSocBus = bus;
    if (readBitsLE(frame, 10, 10, raw) && raw != 1023 && raw <= 1000) {
      batteryPreheatSocUiDeciPct = static_cast<int>(raw);
    } else {
      batteryPreheatSocUiDeciPct = -1;
    }
    g_status.batteryPreheatSocUiDeciPct = batteryPreheatSocUiDeciPct;
    return;
  }

  if (frame.can_id == CAN_ID_BMS_BMB_MIN_MAX && frame.can_dlc >= 5) {
    const uint8_t mux = static_cast<uint8_t>(frame.data[0] & 0x03);
    if (mux == 0) {
      int tempMax = TEMPERATURE_SNA_CX100;
      int tempMin = TEMPERATURE_SNA_CX100;
      int tempAvg = TEMPERATURE_SNA_CX100;
      const bool maxOk = decodeOffsetHalfDegCx100(frame.data[2], tempMax);
      const bool minOk = decodeOffsetHalfDegCx100(frame.data[3], tempMin);
      const bool avgOk = decodeOffsetHalfDegCx100(frame.data[4], tempAvg);
      if (maxOk && minOk) {
        if (!avgOk) tempAvg = (tempMin + tempMax) / 2;
        g_status.bmsTempDecodedCount = avgOk ? 3 : 2;
        g_status.bmsTempMinCx100 = tempMin;
        g_status.bmsTempAvgCx100 = tempAvg;
        g_status.bmsTempMaxCx100 = tempMax;
        bmsTempDecodedLastRxMs = millis();
      }
    }
    return;
  }

  if (frame.can_id == CAN_ID_VCFRONT_SENSORS && frame.can_dlc >= 8) {
    batteryPreheatVcfrontLastRxMs = millis();
    g_status.batteryPreheatVcfrontSeen = 1;
    g_status.batteryPreheatVcfrontBus = bus;
    uint32_t raw = 0;
    if (readBitsLE(frame, 0, 10, raw)) {
      g_status.batteryPreheatVcfrontCoolantBatInletCx100 = decodeVcfrontCoolantCx100(raw, 1023);
    }
    if (readBitsLE(frame, 10, 11, raw)) {
      g_status.batteryPreheatVcfrontCoolantPtInletCx100 = decodeVcfrontCoolantCx100(raw, 2047);
    }
    if (readBitsLE(frame, 24, 8, raw)) {
      g_status.batteryPreheatVcfrontAmbientCx100 = decodeVcfrontAmbientCx100(raw);
    }
    if (readBitsLE(frame, 40, 8, raw)) {
      g_status.batteryPreheatVcfrontAmbientFilteredCx100 = decodeVcfrontAmbientCx100(raw);
    }
  }
}

static void sendBatteryPreheatFrame(const uint8_t payload[8]) {
  can_frame f = {};
  f.can_id = CAN_ID_UI_TRIP_PLANNING;
  f.can_dlc = 8;
  memcpy(f.data, payload, 8);
#ifdef ENABLE_CANB_MCP2515
  canbScheduleTx(f, CANB_TX_SRC_BATTERY, CANB_TX_PRIO_LOW, CANB_TX_TTL_PREHEAT_MS);
#else
  twai_send(f);
#endif
}

static bool batteryPreheatTemperatureFresh(uint32_t now) {
  return bmsTempDecodedLastRxMs != 0 &&
         (now - bmsTempDecodedLastRxMs) <= BATTERY_PREHEAT_TEMP_FRESH_MS &&
         g_status.bmsTempDecodedCount > 0;
}

static bool batteryPreheatSocFresh(uint32_t now) {
  return batteryPreheatSocLastRxMs != 0 &&
         (now - batteryPreheatSocLastRxMs) <= BATTERY_PREHEAT_SOC_FRESH_MS &&
         batteryPreheatSocUiDeciPct >= 0;
}

static bool batteryPreheatSocTooLow(uint32_t now) {
  return batteryPreheatSocFresh(now) &&
         batteryPreheatSocUiDeciPct < BATTERY_PREHEAT_MIN_SOC_DECI_PCT;
}

static uint16_t batteryPreheatComputeBlockMask(const RuntimeConfig& cfg, uint32_t now);

static void updateBatteryPreheatControlStatus(const RuntimeConfig& cfg, uint32_t now) {
  g_status.batteryPreheatRunMs =
      batteryPreheatStartMs == 0 ? 0 : (now - batteryPreheatStartMs);
  g_status.batteryPreheatStableTempMs =
      batteryPreheatTargetStableStartMs == 0 ? 0 : (now - batteryPreheatTargetStableStartMs);
  g_status.batteryPreheatAutoOffReason = batteryPreheatAutoOffReason;
  g_status.batteryPreheatAutoOffLatched = batteryPreheatAutoOffLatched ? 1 : 0;
  g_status.batteryPreheatChargeDetected = batteryPreheatChargeDetected ? 1 : 0;
  g_status.batteryPreheatBlockMask = batteryPreheatComputeBlockMask(cfg, now);
  g_status.batteryPreheatChargeStatusAgeMs =
      batteryPreheatBmsStatusLastRxMs == 0 ? 0 : (now - batteryPreheatBmsStatusLastRxMs);
  g_status.batteryPreheatSocUiDeciPct = batteryPreheatSocUiDeciPct;
  g_status.batteryPreheatSocAgeMs =
      batteryPreheatSocLastRxMs == 0 ? 0 : (now - batteryPreheatSocLastRxMs);
}

static void batteryPreheatQueueOffFrames(uint8_t reason, bool latch) {
  batteryPreheatAutoOffReason = reason;
  batteryPreheatAutoOffLatched = latch;
  batteryPreheatOffFramesLeft = 3;
  batteryPreheatLastSendMs = 0;
  g_status.batteryPreheatActive = 0;
}

static void serviceBatteryPreheatOffFrames(uint32_t now) {
  if (batteryPreheatOffFramesLeft == 0) {
    batteryPreheatLastSendMs = 0;
    return;
  }
  if (batteryPreheatLastSendMs != 0 &&
      (now - batteryPreheatLastSendMs) < BATTERY_PREHEAT_PERIOD_MS) {
    return;
  }
  batteryPreheatLastSendMs = now;
  sendBatteryPreheatFrame(BATTERY_PREHEAT_OFF);
  batteryPreheatOffFramesLeft--;
}

static void serviceBatteryPreheat(const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  const bool preheatRequested = cfg.batteryPreheatEnabled;
  g_status.batteryPreheatAgeMs =
      batteryPreheatLastSendMs == 0 ? 0 : (now - batteryPreheatLastSendMs);
  updateBatteryPreheatControlStatus(cfg, now);

#ifdef ENABLE_CANB_MCP2515
  if (!cfg.canCommsEnabled || !cfg.canbEnabled || !canbIsReady()) {
    g_status.batteryPreheatActive = 0;
    batteryPreheatLastSendMs = 0;
    batteryPreheatOffFramesLeft = 0;
    batteryPreheatPrevEnabled = false;
    batteryPreheatStartMs = 0;
    batteryPreheatTargetStableStartMs = 0;
    updateBatteryPreheatControlStatus(cfg, now);
    return;
  }
#else
  if (!cfg.canCommsEnabled || cfg.can1ReceiveOnly) {
    g_status.batteryPreheatActive = 0;
    batteryPreheatLastSendMs = 0;
    batteryPreheatOffFramesLeft = 0;
    batteryPreheatPrevEnabled = false;
    batteryPreheatStartMs = 0;
    batteryPreheatTargetStableStartMs = 0;
    updateBatteryPreheatControlStatus(cfg, now);
    return;
  }
#endif

  if (!preheatRequested) {
    g_status.batteryPreheatActive = 0;
    if (batteryPreheatPrevEnabled) {
      batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_USER, false);
    }
    batteryPreheatPrevEnabled = false;
    batteryPreheatAutoOffLatched = false;
    batteryPreheatStartMs = 0;
    batteryPreheatTargetStableStartMs = 0;
    serviceBatteryPreheatOffFrames(now);
    updateBatteryPreheatControlStatus(cfg, now);
    return;
  }

  if (!batteryPreheatPrevEnabled) {
    batteryPreheatPrevEnabled = true;
    batteryPreheatStartMs = now;
    batteryPreheatTargetStableStartMs = 0;
    batteryPreheatAutoOffLatched = false;
    batteryPreheatAutoOffReason = BATTERY_PREHEAT_OFF_NONE;
    batteryPreheatChargeDetected = false;
  }

  if (!batteryPreheatAutoOffLatched) {
    if (batteryPreheatSocTooLow(now)) {
      batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_LOW_SOC, true);
    } else if (batteryPreheatChargeDetected) {
      batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_CHARGING, true);
    } else if (batteryPreheatStartMs != 0 &&
               (now - batteryPreheatStartMs) >= BATTERY_PREHEAT_MAX_RUN_MS) {
      batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_TIMEOUT, true);
    } else if (batteryPreheatTemperatureFresh(now)) {
      if (g_status.bmsTempMaxCx100 >= BATTERY_PREHEAT_MAX_CX100) {
        batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_MAX_TEMP, true);
      } else if (g_status.bmsTempAvgCx100 >= BATTERY_PREHEAT_TARGET_CX100) {
        if (batteryPreheatTargetStableStartMs == 0) {
          batteryPreheatTargetStableStartMs = now;
        } else if ((now - batteryPreheatTargetStableStartMs) >=
                   BATTERY_PREHEAT_TARGET_STABLE_MS) {
          batteryPreheatQueueOffFrames(BATTERY_PREHEAT_OFF_AVG_TEMP, true);
        }
      } else {
        batteryPreheatTargetStableStartMs = 0;
      }
    } else {
      batteryPreheatTargetStableStartMs = 0;
    }
  }

  if (batteryPreheatAutoOffLatched) {
    g_status.batteryPreheatActive = 0;
    serviceBatteryPreheatOffFrames(now);
    updateBatteryPreheatControlStatus(cfg, now);
    return;
  }

  g_status.batteryPreheatActive = 1;
  batteryPreheatOffFramesLeft = 3;  // arm OFF frames for the next disable edge
  if (batteryPreheatLastSendMs != 0 &&
      (now - batteryPreheatLastSendMs) < BATTERY_PREHEAT_PERIOD_MS) {
    updateBatteryPreheatControlStatus(cfg, now);
    return;
  }
  batteryPreheatLastSendMs = now;
  sendBatteryPreheatFrame(BATTERY_PREHEAT_ON);
  g_status.batteryPreheatTxCount++;
  updateBatteryPreheatControlStatus(cfg, now);
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

static bool nagKillerHandsStateTriggersDnd(uint8_t state) {
  return (state >= NAG_DND_HANDS_PRIMARY_MIN && state <= NAG_DND_HANDS_PRIMARY_MAX) ||
         (state >= NAG_DND_HANDS_ESCALATED_MIN && state <= NAG_DND_HANDS_ESCALATED_MAX);
}

static void triggerNagKillerDndBurst(uint32_t now) {
  nagKillerDndRemainingActions = NAG_DND_ACTION_COUNT;
  nagKillerDndNextActionMs = now;
  nagKillerDndLastTriggerMs = now;
  nagKillerDndTriggerCount++;
  g_status.nagKillerDndRemaining = nagKillerDndRemainingActions;
  g_status.nagKillerDndTriggerCount = nagKillerDndTriggerCount;
}

static void clearNagKillerDndPendingActions() {
  nagKillerDndRemainingActions = 0;
  nagKillerDndNextActionMs = 0;
  g_status.nagKillerDndRemaining = 0;
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

static void handleNagKillerContextFrame(const can_frame& frame, const RuntimeConfig& cfg) {
  if (frame.can_id == CAN_ID_DAS_STATUS && frame.can_dlc >= 6) {
    const uint32_t now = millis();
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

    const bool handsStateTriggersDnd = nagKillerHandsStateTriggersDnd(handsOnState);
    if (!cfg.nagKillerDndEnabled) {
      nagKillerDndHandsRangeActive = false;
      clearNagKillerDndPendingActions();
    } else if (handsStateTriggersDnd) {
      if (!nagKillerDndHandsRangeActive) {
        triggerNagKillerDndBurst(now);
      }
      nagKillerDndHandsRangeActive = true;
    } else if (handsOnState == 1 || handsOnState == 2) {
      nagKillerDndHandsRangeActive = false;
      clearNagKillerDndPendingActions();
    } else {
      nagKillerDndHandsRangeActive = false;
      clearNagKillerDndPendingActions();
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
    const uint32_t now = millis();
    const uint16_t raw14 = static_cast<uint16_t>(((static_cast<uint16_t>(frame.data[3]) << 8) |
                                                  frame.data[2]) & 0x3FFF);
    nagKillerSteeringAngleDeg = static_cast<float>(raw14) * 0.1f - 819.2f;
    nagKillerLastSteeringMs = now;
    g_status.nagKillerSteeringDegCx10 = static_cast<int>(nagKillerSteeringAngleDeg * 10.0f);
  }
}

static bool decodeDasLcHandsOnReason(const can_frame& frame, uint8_t& reason) {
  (void)frame;
  (void)reason;
  // Local Tesla Explorer metadata confirms DAS_LC_handsOnReason is in
  // 0x5D9 DAS_carLog, but the available DBC/layout files do not include its
  // bit-level start/length. Keep the decoder disabled until that layout is
  // confirmed by DBC or a known-good capture.
  return false;
}

static void handleDasCarLogFrame(const can_frame& frame) {
  if (frame.can_id != CAN_ID_DAS_CAR_LOG) return;

  const uint32_t now = millis();
  dasCarLogLastRxMs = now;
  g_status.dasLcHandsOnReasonSeen = 1;
  g_status.dasLcHandsOnReasonDlc = frame.can_dlc;

  if (frame.can_dlc < 1) {
    g_status.dasLcHandsOnReasonDecode = DAS_LC_REASON_DECODE_BAD_DLC;
    return;
  }

  uint8_t reason = 255;
  if (!decodeDasLcHandsOnReason(frame, reason)) {
    g_status.dasLcHandsOnReasonDecode = DAS_LC_REASON_DECODE_NO_LAYOUT;
    return;
  }
  if (reason > 53) {
    g_status.dasLcHandsOnReasonDecode = DAS_LC_REASON_DECODE_INVALID_VALUE;
    return;
  }

  g_status.dasLcHandsOnReasonDecode = DAS_LC_REASON_DECODE_OK;
  if (g_status.dasLcHandsOnReasonLatest != reason) {
    if (g_status.dasLcHandsOnReasonLatest != 255) {
      g_status.dasLcHandsOnReasonPrevious = g_status.dasLcHandsOnReasonLatest;
      dasLcHandsOnReasonPreviousMs = dasLcHandsOnReasonLatestMs;
    }
    g_status.dasLcHandsOnReasonLatest = reason;
  }
  dasLcHandsOnReasonLatestMs = now;
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

static uint8_t fsdActivationResendCachedMuxMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    if (fsdActivationResendHasFrame[i]) mask |= static_cast<uint8_t>(1U << i);
  }
  return mask;
}

static void cacheFsdActivationResendFrame(const can_frame& frame, uint8_t mux, const RuntimeConfig& cfg) {
  if (!cfg.canCommsEnabled || !cfg.fsdEnabled || !cfg.fsdActivationResendEnabled) return;
  if (frame.can_id != CAN_ID_AP_CONTROL || frame.can_dlc < 8 || mux != 0) return;
  fsdActivationResendFrames[mux] = frame;
  fsdActivationResendHasFrame[mux] = true;
}

static void serviceFsdActivationResend(const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  const bool enabled = cfg.canCommsEnabled &&
                       cfg.fsdEnabled &&
                       cfg.fsdActivationResendEnabled &&
                       cfg.fsdActivationResendMs > 0;
  const uint16_t periodMs = clampFsdActivationResendMs(cfg.fsdActivationResendMs);

  if (!enabled) {
    fsdActivationResendPrevEnabled = false;
    fsdActivationResendNextMs = 0;
    g_status.fsdActivationResendActive = 0;
    return;
  }

  if (!fsdActivationResendPrevEnabled) {
    fsdActivationResendNextMs = now;
  }
  fsdActivationResendPrevEnabled = true;

  g_status.fsdActivationResendActive = 1;
  if ((int32_t)(now - fsdActivationResendNextMs) < 0) return;
  fsdActivationResendNextMs = now + periodMs;

  if (fsdActivationResendHasFrame[0]) {
    if (twai_send(fsdActivationResendFrames[0])) {
      fsdActivationResendTxCount++;
      fsdActivationResendLastTxMs = now;
    }
  }
}

inline int8_t cabinCameraBit43Override(const RuntimeConfig& cfg, uint32_t now) {
  (void)now;
  return cfg.cabinCameraDisableEnabled ? 0 : -1;
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
      if (index == 0 && cfg.canCommsEnabled && cfg.fsdEnabled) {
        setBit(frame, 46, true);
        // 0x3FD mux 0 enables the FSD/AP bit and writes the current drive style.
        setSpeedProfileV12V13(frame, speedProfile);
        if (twai_send(frame)) cacheFsdActivationResendFrame(frame, index, cfg);
      }
      if (index == 1 && cfg.canCommsEnabled) {
        setBit(frame, 19, false);
        const int8_t cabinCameraOverride = cabinCameraBit43Override(cfg, millis());
        if (cabinCameraOverride >= 0) setBit(frame, 43, cabinCameraOverride != 0);
        // 0x3FD mux 1 keeps bit 19 clear and can optionally clear the cabin camera bit.
        if (twai_send(frame)) cacheFsdActivationResendFrame(frame, index, cfg);
      }
      if (index == 2 && cfg.canCommsEnabled && cfg.autoSpeedOffsetEnabled) {
        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
          ? unifiedSpeedCompensation.speedOffsetRaw
          : readSpeedOffsetRaw(frame);
        // 0x3FD mux 2 writes speed offset, or preserves stock offset if no valid limit.
        speedOffsetRaw = offsetSlewLimiter.apply(speedOffsetRaw, cfg.slewPctPerSec);
        g_status.offsetRaw = speedOffsetRaw;
        writeSpeedOffsetRaw(frame, speedOffsetRaw);
        if (twai_send(frame)) cacheFsdActivationResendFrame(frame, index, cfg);
      }
    }
  }
};

HW3Handler handler;

// ============================================================================
// CAN B (MCP2515) secondary bus -- basic comms + non-blocking service-mode burst
// ============================================================================
#ifdef ENABLE_CANB_MCP2515

// Pass the configured SPI instance explicitly so the MCP2515 constructor does
// not call a default SPI.begin() before setupCanB() applies the LILYGO pins.
static MCP2515 canb(MCP2515_CS, 10000000, &SPI);
static bool canbReady = false;
static uint8_t canbHardwareFilterMode = CANB_FILTER_ALL;
static uint32_t canbRxCount = 0;
static uint32_t canbTxCount = 0;
static uint32_t canbTxFailCount = 0;
static uint32_t canbLastId = 0;
static uint32_t canbRxOverflowCount = 0;

struct CanBTxQueueItem {
  can_frame frame{};
  uint8_t source = CANB_TX_SRC_OTHER;
  uint8_t priority = CANB_TX_PRIO_LOW;
  uint16_t ttlMs = CANB_TX_TTL_DEFAULT_MS;
  uint32_t queuedMs = 0;
  bool used = false;
};

static CanBTxQueueItem canbTxQueue[CANB_TX_QUEUE_CAP];
static uint8_t canbTxQueueDepth = 0;
static uint8_t canbTxQueueMaxDepth = 0;
static uint32_t canbTxSchedTxCount = 0;
static uint32_t canbTxSchedDropCount = 0;
static uint32_t canbTxSchedExpiredCount = 0;
static uint32_t canbTxSchedFailCount = 0;
static uint32_t canbTxSchedBudgetHitCount = 0;

static bool canbIsReady() {
  return canbReady;
}

static uint16_t batteryPreheatComputeBlockMask(const RuntimeConfig& cfg, uint32_t now) {
  uint16_t mask = 0;
  if (!cfg.batteryPreheatEnabled) mask |= BATTERY_PREHEAT_BLOCK_DISABLED;
  if (!cfg.canCommsEnabled) mask |= BATTERY_PREHEAT_BLOCK_CAN_COMMS_DISABLED;
  if (!cfg.canbEnabled) mask |= BATTERY_PREHEAT_BLOCK_CANB_DISABLED;
  if (!canbReady) mask |= BATTERY_PREHEAT_BLOCK_CANB_NOT_READY;
  if (batteryPreheatSocTooLow(now)) mask |= BATTERY_PREHEAT_BLOCK_LOW_SOC;
  if (batteryPreheatChargeDetected) mask |= BATTERY_PREHEAT_BLOCK_CHARGING;
  if (batteryPreheatStartMs != 0 &&
      (now - batteryPreheatStartMs) >= BATTERY_PREHEAT_MAX_RUN_MS) {
    mask |= BATTERY_PREHEAT_BLOCK_TIMEOUT;
  }
  if (batteryPreheatTemperatureFresh(now)) {
    if (g_status.bmsTempMaxCx100 >= BATTERY_PREHEAT_MAX_CX100) {
      mask |= BATTERY_PREHEAT_BLOCK_MAX_TEMP;
    }
    if (g_status.bmsTempAvgCx100 >= BATTERY_PREHEAT_TARGET_CX100 &&
        batteryPreheatTargetStableStartMs != 0 &&
        (now - batteryPreheatTargetStableStartMs) >= BATTERY_PREHEAT_TARGET_STABLE_MS) {
      mask |= BATTERY_PREHEAT_BLOCK_AVG_TEMP;
    }
  }
  if (batteryPreheatAutoOffLatched) {
    if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_AVG_TEMP) {
      mask |= BATTERY_PREHEAT_BLOCK_AVG_TEMP;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_MAX_TEMP) {
      mask |= BATTERY_PREHEAT_BLOCK_MAX_TEMP;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_CHARGING) {
      mask |= BATTERY_PREHEAT_BLOCK_CHARGING;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_TIMEOUT) {
      mask |= BATTERY_PREHEAT_BLOCK_TIMEOUT;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_LOW_SOC) {
      mask |= BATTERY_PREHEAT_BLOCK_LOW_SOC;
    }
  }
  return mask;
}

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
//   0x273: body lighting template for rear-fog strobe.
constexpr uint32_t CANB_ID_SCCM_RIGHT_STALK = 0x229;
constexpr uint32_t CANB_ID_STW_ACTN_RQ = 0x249;
constexpr uint32_t CANB_ID_BODY_LIGHTING = 0x273;
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
constexpr uint16_t HIGH_BEAM_STROBE_INTERVAL_MS = 45;
constexpr uint16_t HIGH_BEAM_STROBE_RESEND_MS = 45;
constexpr uint16_t OVERTAKE_LIGHT_FORCE_PERIOD_MS = HIGH_BEAM_STROBE_RESEND_MS;
constexpr uint16_t OVERTAKE_LIGHT_ALWAYS_ON_PULL_WINDOW_MS = 3000;
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
constexpr uint16_t DND_SCROLL_STEP_MS = 100;
constexpr uint16_t DND_SCROLL_CACHE_MAX_AGE_MS = 1000;
constexpr float SCROLL_GEAR_MAX_SPEED_KPH = 2.0f;
constexpr float REAR_FOG_MILD_DECEL_THRESHOLD = -0.80f;
constexpr float REAR_FOG_HARD_DECEL_THRESHOLD = -2.50f;
constexpr float REAR_FOG_VERY_HARD_DECEL_THRESHOLD = -3.50f;
constexpr uint16_t HIGH_BEAM_DOUBLE_PULL_WINDOW_MS = 1000;
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
constexpr uint8_t REAR_FOG_MASK = 0x80;
constexpr uint8_t REAR_FOG_OFF = 0x10;
constexpr uint8_t REAR_FOG_ON = 0x90;
static can_frame canbLastStwActnRqFrame{};
static bool canbHasLastStwActnRqFrame = false;
static uint32_t canbLastStwActnRqMs = 0;
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
static volatile bool highBeamStrobeOutputOn = false;
static volatile uint8_t highBeamStrobePulsesRemaining = 0;
static volatile uint32_t highBeamStrobeLastToggleMs = 0;
static volatile uint32_t highBeamStrobeLastSendMs = 0;
static volatile bool overtakeLightForceOutputOn = false;
static volatile uint32_t overtakeLightForceLastTxMs = 0;
static bool overtakeLightAlwaysOnLatched = false;
static uint8_t overtakeLightAlwaysOnPullCount = 0;
static uint32_t overtakeLightAlwaysOnLastPullMs = 0;
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
static volatile bool dndActionRequireSwitches = true;
static uint32_t dndLastTriggerMs = 0;

// CAN B read budget per loop pass -- bounded so it can never starve CAN A.
constexpr uint8_t CANB_RX_SCAN_LIMIT = 4;
constexpr uint8_t CANB_RX_SCAN_LIMIT_ACTIVE = 24;
constexpr uint32_t CANB_RX_DRAIN_TIME_US = 900;

static void setupCanB();
static bool applyCanBFilters(uint8_t mode);
static bool canb_recv(can_frame& frame);
static bool canb_send(const can_frame& frame, uint8_t source);
static bool canbScheduleTx(const can_frame& frame, uint8_t source, uint8_t priority,
                           uint16_t ttlMs, bool replaceSameSourceId);
static void serviceCanBTxScheduler();
static bool canbIntAsserted();
static void drainCanBWithBudget(const RuntimeConfig& cfg);
static void updateCanBErrorStatus();
static void handleCanBFrame(const can_frame& frame, const RuntimeConfig& cfg);
static void setCanBServiceMode(bool enabled);
static void serviceCanBScheduledTx();
static void serviceHighBeamStrobe(const RuntimeConfig& cfg);
static void serviceOvertakeLightAlwaysOn(const RuntimeConfig& cfg);
static void serviceReverseStrobe(const RuntimeConfig& cfg);
static void serviceRearFogBrakeStrobe(const RuntimeConfig& cfg);
static void serviceScrollGearShift(const RuntimeConfig& cfg);
static void handleDndHandsOnFrame(const can_frame& frame);
static void serviceNagKillerDndBurst(const RuntimeConfig& cfg);
static void serviceDndScrollAction(const RuntimeConfig& cfg);
static void handleVcleftSwitchFrame(const can_frame& frame, const RuntimeConfig& cfg, bool cacheHazardFrame);

static void setupCanB() {
  Serial.println("[CANB] setup start");
  canbReady = false;
  Serial.println("[CANB] pinMode INT");
  pinMode(MCP2515_INT, INPUT_PULLUP);

  // Hard reset the MCP2515 via its RST line: high / low / high.
  Serial.println("[CANB] reset pins");
  pinMode(MCP2515_RST, OUTPUT);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);
  digitalWrite(MCP2515_RST, LOW);
  delay(10);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);

  Serial.println("[CANB] SPI.begin");
  SPI.begin(MCP2515_SCK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);
  Serial.println("[CANB] SPI.begin done");

  // reset()/setNormalMode() return types vary across library versions, so we
  // call them as statements and only gate on setBitrate(), which is the
  // meaningful failure point. A failure here just leaves canbReady = false; it
  // must never block or disturb CAN A / the FSD pipeline.
  Serial.println("[CANB] canb.reset");
  canb.reset();
  delay(10);
  Serial.println("[CANB] setBitrate");
  if (canb.setBitrate(CAN_500KBPS, MCP2515_CLOCK) != MCP2515::ERROR_OK) {
    Serial.println("[CANB] setBitrate failed");
    return;
  }
  Serial.println("[CANB] apply filters");
  if (!applyCanBFilters(configSnapshot().canbFilterMode)) {
    Serial.println("[CANB] apply filters failed");
    return;
  }
  canbReady = true;
  Serial.println("[CANB] ready");
}

static void canBInitTask(void*) {
  Serial.println("[CANB] init task begin");
  setupCanB();
  Serial.printf("[CANB] init task done ready=%u\n", canbReady ? 1U : 0U);
  vTaskDelete(nullptr);
}

static bool applyCanBFilters(uint8_t mode) {
  mode = normalizeCanBFilterMode(mode);
  if (mode == CANB_FILTER_FEATURE) {
    // Coarse feature filter for the current CANA feature set. MCP2515 only has
    // six filters, so the masks group nearby IDs while still excluding most
    // unrelated 11-bit traffic.
    if (canb.setFilterMask(MCP2515::MASK0, false, 0x42F) != MCP2515::ERROR_OK) return false;
    // Covers: 0x052, 0x082, 0x212, 0x292, 0x3C2.
    if (canb.setFilter(MCP2515::RXF0, false, 0x002) != MCP2515::ERROR_OK) return false;
    // Covers: 0x129, 0x229, 0x339.
    if (canb.setFilter(MCP2515::RXF1, false, 0x029) != MCP2515::ERROR_OK) return false;

    if (canb.setFilterMask(MCP2515::MASK1, false, 0x60C) != MCP2515::ERROR_OK) return false;
    // Primary feature IDs: 0x229, 0x249, 0x339, 0x399.
    if (canb.setFilter(MCP2515::RXF2, false, 0x209) != MCP2515::ERROR_OK) return false;
    // Covers: 0x273, 0x313, 0x332, 0x3B2.
    if (canb.setFilter(MCP2515::RXF3, false, 0x203) != MCP2515::ERROR_OK) return false;
    // Covers: 0x321, 0x370.
    if (canb.setFilter(MCP2515::RXF4, false, 0x200) != MCP2515::ERROR_OK) return false;
    // Covers telemetry expansion IDs such as 0x3FD and 0x3CD..0x3CF.
    if (canb.setFilter(MCP2515::RXF5, false, 0x20C) != MCP2515::ERROR_OK) return false;
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

  const uint32_t rxUs = micros();
  if (diagCanbLastRxUs != 0) {
    const uint32_t gapUs = rxUs - diagCanbLastRxUs;
    if (gapUs > diagCanbRxGapMaxUs) diagCanbRxGapMaxUs = gapUs;
  }
  diagCanbLastRxUs = rxUs;
  canbRxCount++;
  canbLastId = frame.can_id;
  g_status.canbRx = canbRxCount;
  g_status.canbLastId = canbLastId;
  recordCanFrame(frame, 'R', 2);
  return true;
}

static bool canb_send(const can_frame& frame, uint8_t source) {
  if (source >= CANB_TX_SRC_COUNT) source = CANB_TX_SRC_OTHER;
  if (!g_config.canCommsEnabled) return false;
  if (diagCanbTxThisLoop < UINT8_MAX) diagCanbTxThisLoop++;
  if (diagCanbTxThisLoop > diagCanbTxLoopMax) diagCanbTxLoopMax = diagCanbTxThisLoop;
  if (diagCanbTxThisLoop > diagCanbTxLoopMaxEver) diagCanbTxLoopMaxEver = diagCanbTxThisLoop;
  diagCanbTxSourceCounts[source]++;

  if (!canbReady) return false;
  if (frame.can_dlc > 8) return false;
  const uint32_t txStartUs = micros();
  // One short retry on a busy/failed mailbox; no blocking delay so a stuck
  // CAN B can never stall CAN A.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (canb.sendMessage(&frame) == MCP2515::ERROR_OK) {
      const uint32_t txUs = micros() - txStartUs;
      if (txUs > diagCanbTxMaxUs) diagCanbTxMaxUs = txUs;
      if (txUs > DIAG_TX_SLOW_US) diagCanbTxSlowCount++;
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

static void canbTxSchedulerRefreshStatus() {
  g_status.canbTxQueueDepth = canbTxQueueDepth;
  g_status.canbTxQueueMaxDepth = canbTxQueueMaxDepth;
  g_status.canbTxSchedTx = canbTxSchedTxCount;
  g_status.canbTxSchedDrop = canbTxSchedDropCount;
  g_status.canbTxSchedExpired = canbTxSchedExpiredCount;
  g_status.canbTxSchedFail = canbTxSchedFailCount;
  g_status.canbTxSchedBudgetHit = canbTxSchedBudgetHitCount;
}

static void canbTxQueueRemove(uint8_t index) {
  if (index >= CANB_TX_QUEUE_CAP || !canbTxQueue[index].used) return;
  canbTxQueue[index].used = false;
  if (canbTxQueueDepth > 0) canbTxQueueDepth--;
  canbTxSchedulerRefreshStatus();
}

static bool canbScheduleTx(const can_frame& frame, uint8_t source, uint8_t priority,
                           uint16_t ttlMs, bool replaceSameSourceId) {
  if (!g_config.canCommsEnabled) return false;
  if (!canbReady || frame.can_dlc > 8) return canb_send(frame, source);
  if (source >= CANB_TX_SRC_COUNT) source = CANB_TX_SRC_OTHER;
  if (priority > CANB_TX_PRIO_LOW) priority = CANB_TX_PRIO_LOW;

  const uint32_t now = millis();
  const uint32_t canId = frame.can_id & CAN_SFF_MASK;
  if (replaceSameSourceId) {
    for (uint8_t i = 0; i < CANB_TX_QUEUE_CAP; ++i) {
      CanBTxQueueItem& item = canbTxQueue[i];
      if (!item.used) continue;
      if (item.source == source && ((item.frame.can_id & CAN_SFF_MASK) == canId)) {
        item.frame = frame;
        item.priority = priority;
        item.ttlMs = ttlMs;
        item.queuedMs = now;
        canbTxSchedulerRefreshStatus();
        return true;
      }
    }
  }

  for (uint8_t i = 0; i < CANB_TX_QUEUE_CAP; ++i) {
    CanBTxQueueItem& item = canbTxQueue[i];
    if (item.used) continue;
    item.frame = frame;
    item.source = source;
    item.priority = priority;
    item.ttlMs = ttlMs;
    item.queuedMs = now;
    item.used = true;
    if (canbTxQueueDepth < UINT8_MAX) canbTxQueueDepth++;
    if (canbTxQueueDepth > canbTxQueueMaxDepth) canbTxQueueMaxDepth = canbTxQueueDepth;
    canbTxSchedulerRefreshStatus();
    return true;
  }

  canbTxSchedDropCount++;
  canbTxSchedulerRefreshStatus();
  return false;
}

static int8_t canbTxSchedulerBestIndex(uint32_t now) {
  int8_t best = -1;
  uint8_t bestPriority = UINT8_MAX;
  uint32_t bestQueuedMs = 0;
  for (uint8_t i = 0; i < CANB_TX_QUEUE_CAP; ++i) {
    CanBTxQueueItem& item = canbTxQueue[i];
    if (!item.used) continue;
    if (item.ttlMs != 0 && (now - item.queuedMs) > item.ttlMs) {
      canbTxSchedExpiredCount++;
      canbTxQueueRemove(i);
      continue;
    }
    if (best < 0 || item.priority < bestPriority ||
        (item.priority == bestPriority && (int32_t)(item.queuedMs - bestQueuedMs) < 0)) {
      best = static_cast<int8_t>(i);
      bestPriority = item.priority;
      bestQueuedMs = item.queuedMs;
    }
  }
  canbTxSchedulerRefreshStatus();
  return best;
}

static void serviceCanBTxScheduler() {
  if (!canbReady || canbTxQueueDepth == 0) return;
  uint8_t sentThisLoop = 0;
  while (sentThisLoop < CANB_TX_SCHED_MAX_PER_LOOP) {
    const int8_t index = canbTxSchedulerBestIndex(millis());
    if (index < 0) break;

    const CanBTxQueueItem item = canbTxQueue[index];
    canbTxQueueRemove(static_cast<uint8_t>(index));
    if (canb_send(item.frame, item.source)) {
      canbTxSchedTxCount++;
    } else {
      canbTxSchedFailCount++;
    }
    sentThisLoop++;
  }
  if (canbTxQueueDepth > 0 && sentThisLoop >= CANB_TX_SCHED_MAX_PER_LOOP) {
    canbTxSchedBudgetHitCount++;
  }
  canbTxSchedulerRefreshStatus();
}

static bool canbIntAsserted() {
  return canbReady && digitalRead(MCP2515_INT) == LOW;
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
    canbScheduleTx(rightStalkFrame(RIGHT_STALK_IDLE), CANB_TX_SRC_SCROLL_GEAR,
                   CANB_TX_PRIO_LOW, CANB_TX_TTL_SCROLL_MS);
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

  canbScheduleTx(rightStalkFrame(status), CANB_TX_SRC_SCROLL_GEAR,
                 CANB_TX_PRIO_LOW, CANB_TX_TTL_SCROLL_MS);
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

static bool dndActionAllowed(const RuntimeConfig& cfg, bool requireSwitches) {
  if (!cfg.canCommsEnabled) {
    g_status.dndBlocked = DND_BLOCK_DISABLED;
    return false;
  }
  if (requireSwitches && !cfg.dndEnabled) {
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

static bool sendDndVolumeFrame(uint8_t cmd, bool direct, uint8_t priority) {
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

  const bool sent = direct
      ? canb_send(f, CANB_TX_SRC_DND)
      : canbScheduleTx(f, CANB_TX_SRC_DND, priority, CANB_TX_TTL_SCROLL_MS);
  if (!sent) {
    g_status.dndBlocked = DND_BLOCK_CANB;
    return false;
  }
  g_status.dndTxCount++;
  return true;
}

static bool startDndVolumeAction(const RuntimeConfig& cfg, bool requireSwitches = true) {
  if (dndActionActive || !dndActionAllowed(cfg, requireSwitches)) return false;

  const uint32_t now = millis();
  dndActionActive = true;
  dndActionType = DND_ACTION_VOLUME;
  dndActionStep = 0;
  dndNextStepMs = 0;
  dndActionRequireSwitches = requireSwitches;
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
  g_status.dndWarningActive = nagKillerHandsStateTriggersDnd(handsOnState) ? 1 : 0;

  if (handsOnState <= 2 && !dndActionActive) g_status.dndBlocked = DND_BLOCK_NONE;
}

static void serviceNagKillerDndBurst(const RuntimeConfig& cfg) {
  if (!cfg.nagKillerDndEnabled) {
    nagKillerDndRemainingActions = 0;
    nagKillerDndHandsRangeActive = false;
    g_status.nagKillerDndRemaining = 0;
    return;
  }
  const uint32_t now = millis();
  if (nagKillerDndHandsRangeActive &&
      nagKillerDndRemainingActions == 0 &&
      !dndActionActive &&
      (nagKillerDndNextActionMs == 0 || (int32_t)(now - nagKillerDndNextActionMs) >= 0)) {
    triggerNagKillerDndBurst(now);
  }

  g_status.nagKillerDndRemaining = nagKillerDndRemainingActions;
  if (nagKillerDndRemainingActions == 0) return;
  if (dndActionActive) return;

  if (nagKillerDndNextActionMs != 0 && (int32_t)(now - nagKillerDndNextActionMs) < 0) return;

  if (startDndVolumeAction(cfg, false)) {
    nagKillerDndRemainingActions--;
    nagKillerDndNextActionMs = now + (DND_SCROLL_STEP_MS * 5UL);
    g_status.nagKillerDndRemaining = nagKillerDndRemainingActions;
  } else {
    nagKillerDndNextActionMs = now + DND_SCROLL_STEP_MS;
  }
}

static void serviceDndScrollAction(const RuntimeConfig& cfg) {
  if (!dndActionActive) {
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  const bool switchBlocked = dndActionRequireSwitches && !cfg.dndEnabled;
  if (switchBlocked || !cfg.canbEnabled || !canbReady) {
    dndActionActive = false;
    dndActionRequireSwitches = true;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    g_status.dndBlocked = switchBlocked ? DND_BLOCK_DISABLED : DND_BLOCK_CANB;
    return;
  }

  const uint32_t now = millis();
  if (dndNextStepMs != 0 && (int32_t)(now - dndNextStepMs) < 0) return;

  static const uint8_t sequence[4] = {0x01, 0x00, 0x3F, 0x00};
  if (dndActionStep >= sizeof(sequence)) {
    dndActionActive = false;
    dndActionRequireSwitches = true;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  const bool nagLinkedDnd = !dndActionRequireSwitches;
  const uint8_t priority = nagLinkedDnd ? CANB_TX_PRIO_HIGH : CANB_TX_PRIO_MED;
  if (!sendDndVolumeFrame(sequence[dndActionStep], nagLinkedDnd, priority)) {
    dndActionActive = false;
    dndActionRequireSwitches = true;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
    return;
  }

  dndActionStep++;
  if (dndActionStep >= sizeof(sequence)) {
    dndActionActive = false;
    dndActionRequireSwitches = true;
    g_status.dndActionActive = 0;
    g_status.dndActionType = DND_ACTION_NONE;
  } else {
    dndNextStepMs = now + DND_SCROLL_STEP_MS;
    g_status.dndActionActive = 1;
    g_status.dndActionType = DND_ACTION_VOLUME;
  }
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
    canb_send(highBeamFrame(STALK_STATUS_IDLE), CANB_TX_SRC_LIGHT);
  }
  highBeamStrobeActive = false;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = 0;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
}

static void startHighBeamStrobe() {
  highBeamStrobeActive = true;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = HIGH_BEAM_STROBE_PULSES;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
}

static void stopRearFogBrakeStrobe(bool sendOff) {
  if (sendOff && canbReady) {
    canbScheduleTx(rearFogFrame(false), CANB_TX_SRC_REAR_FOG,
                   CANB_TX_PRIO_LOW, CANB_TX_TTL_DEFAULT_MS);
  }
  rearFogBrakeStrobeActive = false;
  rearFogBrakeStrobeManualTrigger = false;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = 0;
  rearFogBrakeStrobePriority = 0;
  rearFogBrakeStrobeLastToggleMs = 0;
}

static void startRearFogBrakeStrobe(uint8_t pulses, uint8_t priority, bool manualTrigger = false) {
  if (rearFogBrakeStrobeActive && priority < rearFogBrakeStrobePriority) return;
  rearFogBrakeStrobeActive = true;
  rearFogBrakeStrobeManualTrigger = manualTrigger;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = pulses;
  rearFogBrakeStrobePriority = priority;
  rearFogBrakeStrobeLastToggleMs = 0;
}

static void stopReverseStrobe(bool sendOff) {
  if (sendOff && canbReady) {
    canbScheduleTx(vcleftHazardFrame(false), CANB_TX_SRC_REVERSE,
                   CANB_TX_PRIO_LOW, CANB_TX_TTL_DEFAULT_MS);  // release any in-progress hazard button press
    if (reverseHazardLatchedOn) {
      canbScheduleTx(vcleftHazardFrame(true), CANB_TX_SRC_REVERSE,
                     CANB_TX_PRIO_LOW, CANB_TX_TTL_DEFAULT_MS);   // toggle hazards back off if this feature turned them on
      canbScheduleTx(vcleftHazardFrame(false), CANB_TX_SRC_REVERSE,
                     CANB_TX_PRIO_LOW, CANB_TX_TTL_DEFAULT_MS);  // release the OFF click
    }
    canbScheduleTx(rearFogFrame(false), CANB_TX_SRC_REVERSE,
                   CANB_TX_PRIO_LOW, CANB_TX_TTL_DEFAULT_MS);
  }
  reverseStrobeActive = false;
  reverseStrobePhase = 0;
  reverseStrobePhaseEnd = 0;
  reverseHazardLatchedOn = false;
}

static void startReverseStrobe() {
  if (!canbReady) return;
  if (reverseStrobePhase != 0 || reverseStrobeActive) return;
  reverseStrobeActive = true;
  reverseStrobePhase = 1;                                 // ON-press
  reverseStrobePhaseEnd = millis() + REVERSE_HAZARD_CLICK_MS;
  canb_send(vcleftHazardFrame(true), CANB_TX_SRC_REVERSE);  // single press edge -> hazards ON
  reverseHazardLatchedOn = true;
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
  bmsTempDecodedLastRxMs = bms712TempLastRxMs;
  refreshBms712TemperatureSummary();
}

static void handleBatteryTempDiagFrame(const can_frame& frame, uint8_t bus) {
  if (frame.can_id != CAN_ID_BMS_THERMAL_STATUS &&
      frame.can_id != CAN_ID_BMS_PACK_TEMPERATURES &&
      frame.can_id != CAN_ID_BMS_BMB_MIN_MAX &&
      frame.can_id != CAN_ID_BMS_LOG1 &&
      frame.can_id != CAN_ID_BMS_LOG2) {
    return;
  }
  const uint32_t now = millis();
  if (frame.can_id == CAN_ID_BMS_THERMAL_STATUS) {
    batteryPreheatBms312LastRxMs = now;
    g_status.batteryPreheatBms312Seen = 1;
    g_status.batteryPreheatBms312Bus = bus;
  } else if (frame.can_id == CAN_ID_BMS_LOG2) {
    batteryPreheatBms3b2LastRxMs = now;
    g_status.batteryPreheatBms3b2Seen = 1;
    g_status.batteryPreheatBms3b2Bus = bus;
  }
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

  if (!cfg.highBeamStrobeEnabled) {
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
      canbScheduleTx(highBeamFrame(highBeamStrobeOutputOn ? STALK_STATUS_PULL : STALK_STATUS_IDLE),
                     CANB_TX_SRC_LIGHT, CANB_TX_PRIO_MED, CANB_TX_TTL_FAST_MS);
      highBeamStrobeLastSendMs = now;
    }
    return;
  }
  highBeamStrobeLastToggleMs = now;

  if (!highBeamStrobeOutputOn) {
    canbScheduleTx(highBeamFrame(STALK_STATUS_PULL), CANB_TX_SRC_LIGHT,
                   CANB_TX_PRIO_MED, CANB_TX_TTL_FAST_MS);
    highBeamStrobeOutputOn = true;
  } else {
    canbScheduleTx(highBeamFrame(STALK_STATUS_IDLE), CANB_TX_SRC_LIGHT,
                   CANB_TX_PRIO_MED, CANB_TX_TTL_FAST_MS);
    highBeamStrobeOutputOn = false;
    if (highBeamStrobePulsesRemaining > 0) highBeamStrobePulsesRemaining--;
    if (highBeamStrobePulsesRemaining == 0) {
      stopHighBeamStrobe(false);
      return;
    }
  }
  highBeamStrobeLastSendMs = now;
}

static void stopOvertakeLightForce(const RuntimeConfig& cfg, bool sendIdle) {
  if (sendIdle && cfg.canbEnabled && canbReady) {
    canb_send(highBeamFrame(STALK_STATUS_IDLE), CANB_TX_SRC_LIGHT);
  }
  overtakeLightForceOutputOn = false;
  overtakeLightForceLastTxMs = 0;
}

static void serviceOvertakeLightAlwaysOn(const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  const bool canbOk = cfg.canbEnabled && canbReady;
  if (!cfg.overtakeLightAlwaysOnEnabled || !canbOk) {
    if (overtakeLightForceOutputOn) {
      stopOvertakeLightForce(cfg, true);
    }
    overtakeLightAlwaysOnLatched = false;
    overtakeLightAlwaysOnPullCount = 0;
    overtakeLightAlwaysOnLastPullMs = 0;
    return;
  }

  if (!overtakeLightAlwaysOnLatched) {
    if (overtakeLightForceOutputOn) stopOvertakeLightForce(cfg, true);
    return;
  }

  if (highBeamStrobeActive || highBeamStrobeOutputOn) {
    stopHighBeamStrobe(false);
    overtakeLightForceOutputOn = false;
    overtakeLightForceLastTxMs = 0;
  }

  if (overtakeLightForceOutputOn &&
      overtakeLightForceLastTxMs != 0 &&
      (now - overtakeLightForceLastTxMs) < OVERTAKE_LIGHT_FORCE_PERIOD_MS) {
    return;
  }

  if (canbScheduleTx(highBeamFrame(STALK_STATUS_PULL), CANB_TX_SRC_LIGHT,
                     CANB_TX_PRIO_MED, CANB_TX_TTL_FAST_MS, true)) {
    overtakeLightForceOutputOn = true;
    overtakeLightForceLastTxMs = now;
  }
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
    return;  // current phase still running
  }

  switch (reverseStrobePhase) {
    case 1:  // ON-press done -> release button; hold while the car flashes
      canb_send(vcleftHazardFrame(false), CANB_TX_SRC_REVERSE);
      reverseStrobePhase = 2;
      reverseStrobePhaseEnd = now + REVERSE_HAZARD_ON_MS;
      break;
    case 2:  // hold done -> press again to toggle hazards back off
      canb_send(vcleftHazardFrame(true), CANB_TX_SRC_REVERSE);
      reverseHazardLatchedOn = false;
      reverseStrobePhase = 3;
      reverseStrobePhaseEnd = now + REVERSE_HAZARD_CLICK_MS;
      break;
    default:  // case 3: OFF-press done -> release and finish
      stopReverseStrobe(true);
      return;
  }
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
    return;
  }
  rearFogBrakeStrobeLastToggleMs = now;

  if (!rearFogBrakeStrobeOutputOn) {
    canb_send(rearFogFrame(true), CANB_TX_SRC_REAR_FOG);
    rearFogBrakeStrobeOutputOn = true;
  } else {
    canb_send(rearFogFrame(false), CANB_TX_SRC_REAR_FOG);
    rearFogBrakeStrobeOutputOn = false;
    if (rearFogBrakeStrobePulsesRemaining > 0) rearFogBrakeStrobePulsesRemaining--;
    if (rearFogBrakeStrobePulsesRemaining == 0) {
      stopRearFogBrakeStrobe(false);
      return;
    }
  }

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

  if (cfg.reverseStrobeEnabled && cfg.scrollGearInjectEnabled &&
      g_brakePedalActive && scrollEdge) {
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

static void handleCanBFrame(const can_frame& frame, const RuntimeConfig& cfg) {
  // Stage 1: statistics only. No heavy work, no Serial, no JSON, no bridging.
  canbLastId = frame.can_id;

  handleNagKillerContextFrame(frame, cfg);

  if (frame.can_id == CANB_ID_STW_ACTN_RQ && frame.can_dlc >= 2) {
    canbLastStwActnRqFrame = frame;
    canbHasLastStwActnRqFrame = true;
    canbLastStwActnRqMs = millis();
    highBeamStalkLastCounter = static_cast<uint8_t>(frame.data[1] & 0x0F);

    const uint8_t stalkStatus = readStalkStatus(frame);
    const bool pullDown = stalkStatus == STALK_STATUS_PULL;
    const uint32_t now = canbLastStwActnRqMs;
    const bool pullEdge = pullDown && !highBeamLastPullDown;
    bool pullEdgeConsumed = false;

    if (!cfg.highBeamStrobeEnabled) {
      highBeamPullCount = 0;
      highBeamLastPullMs = 0;
    } else if (pullEdge && !highBeamStrobeActive) {
      if (highBeamLastPullMs == 0 ||
          (now - highBeamLastPullMs) > HIGH_BEAM_DOUBLE_PULL_WINDOW_MS) {
        highBeamPullCount = 0;
      }
      highBeamLastPullMs = now;
      highBeamPullCount++;
      if (highBeamPullCount >= 2) {
        highBeamPullCount = 0;
        highBeamLastPullMs = 0;
        overtakeLightAlwaysOnLatched = false;
        overtakeLightAlwaysOnPullCount = 0;
        overtakeLightAlwaysOnLastPullMs = 0;
        stopOvertakeLightForce(cfg, true);
        startHighBeamStrobe();
        pullEdgeConsumed = true;
      }
    }

    if (cfg.overtakeLightAlwaysOnEnabled && cfg.canbEnabled && canbReady) {
      if (overtakeLightAlwaysOnPullCount != 0 &&
          overtakeLightAlwaysOnLastPullMs != 0 &&
          (now - overtakeLightAlwaysOnLastPullMs) > OVERTAKE_LIGHT_ALWAYS_ON_PULL_WINDOW_MS) {
        overtakeLightAlwaysOnPullCount = 0;
      }
      if (!pullEdgeConsumed && pullEdge) {
        if (overtakeLightAlwaysOnLatched) {
          overtakeLightAlwaysOnLatched = false;
          stopOvertakeLightForce(cfg, true);
          overtakeLightAlwaysOnPullCount = 0;
          overtakeLightAlwaysOnLastPullMs = 0;
          pullEdgeConsumed = true;
        } else {
          const uint32_t interval =
              overtakeLightAlwaysOnLastPullMs == 0 ? 0 : (now - overtakeLightAlwaysOnLastPullMs);
          if (overtakeLightAlwaysOnPullCount == 1 &&
              interval > HIGH_BEAM_DOUBLE_PULL_WINDOW_MS &&
              interval <= OVERTAKE_LIGHT_ALWAYS_ON_PULL_WINDOW_MS) {
            overtakeLightAlwaysOnLatched = true;
            overtakeLightAlwaysOnPullCount = 0;
            overtakeLightAlwaysOnLastPullMs = 0;
            overtakeLightForceLastTxMs = 0;
            if (highBeamStrobeActive || highBeamStrobeOutputOn) stopHighBeamStrobe(false);
          } else {
            overtakeLightAlwaysOnPullCount = 1;
            overtakeLightAlwaysOnLastPullMs = now;
          }
          pullEdgeConsumed = true;
        }
      }
    } else {
      overtakeLightAlwaysOnPullCount = 0;
      overtakeLightAlwaysOnLastPullMs = 0;
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

    const bool brakeActive = (frame.data[7] & 0x01) != 0;
    handleRearFogBrakeLampState(brakeActive, cfg);
  }

  handleBatteryTempDiagFrame(frame, 2);
  handleBatteryPreheatBmsDiagFrame(frame, 2);
  handleBatteryPreheatFeedbackFrame(frame, 2);

  if (frame.can_id == CANB_ID_VCLEFT_SWITCH && frame.can_dlc >= 4) {
    handleVcleftSwitchFrame(frame, cfg, true);
    return;
  }
}

static void drainCanBWithBudget(const RuntimeConfig& cfg) {
  if (!canbReady) return;

  const bool intAsserted = canbIntAsserted();
#ifdef ENABLE_LIGHT_WEBUI
  const bool recorderActive = recActive;
#else
  const bool recorderActive = false;
#endif
  const uint8_t budget = (intAsserted || recorderActive) ? CANB_RX_SCAN_LIMIT_ACTIVE : CANB_RX_SCAN_LIMIT;
  const uint32_t startUs = micros();
  uint8_t drained = 0;
  for (uint8_t i = 0; i < budget; ++i) {
    can_frame frame;
    if (!canb_recv(frame)) break;
    drained++;
    handleCanBFrame(frame, cfg);
    if ((micros() - startUs) >= CANB_RX_DRAIN_TIME_US) break;
  }
  const uint32_t drainUs = micros() - startUs;
  if (drainUs > diagCanbDrainMaxUs) diagCanbDrainMaxUs = drainUs;
  if (drained > diagCanbDrainMaxFrames) diagCanbDrainMaxFrames = drained;
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
  canbScheduleTx(f, CANB_TX_SRC_SERVICE, CANB_TX_PRIO_LOW, CANB_TX_TTL_FAST_MS);
  canbServiceBurstRemaining--;
}

#endif  // ENABLE_CANB_MCP2515

#ifndef ENABLE_CANB_MCP2515
static uint16_t batteryPreheatComputeBlockMask(const RuntimeConfig& cfg, uint32_t now) {
  uint16_t mask = 0;
  if (!cfg.batteryPreheatEnabled) mask |= BATTERY_PREHEAT_BLOCK_DISABLED;
  if (!cfg.canCommsEnabled) mask |= BATTERY_PREHEAT_BLOCK_CAN_COMMS_DISABLED;
  if (cfg.can1ReceiveOnly) mask |= BATTERY_PREHEAT_BLOCK_CANB_DISABLED;
  if (batteryPreheatSocTooLow(now)) mask |= BATTERY_PREHEAT_BLOCK_LOW_SOC;
  if (batteryPreheatChargeDetected) mask |= BATTERY_PREHEAT_BLOCK_CHARGING;
  if (batteryPreheatStartMs != 0 &&
      (now - batteryPreheatStartMs) >= BATTERY_PREHEAT_MAX_RUN_MS) {
    mask |= BATTERY_PREHEAT_BLOCK_TIMEOUT;
  }
  if (batteryPreheatTemperatureFresh(now)) {
    if (g_status.bmsTempMaxCx100 >= BATTERY_PREHEAT_MAX_CX100) {
      mask |= BATTERY_PREHEAT_BLOCK_MAX_TEMP;
    }
    if (g_status.bmsTempAvgCx100 >= BATTERY_PREHEAT_TARGET_CX100 &&
        batteryPreheatTargetStableStartMs != 0 &&
        (now - batteryPreheatTargetStableStartMs) >= BATTERY_PREHEAT_TARGET_STABLE_MS) {
      mask |= BATTERY_PREHEAT_BLOCK_AVG_TEMP;
    }
  }
  if (batteryPreheatAutoOffLatched) {
    if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_AVG_TEMP) {
      mask |= BATTERY_PREHEAT_BLOCK_AVG_TEMP;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_MAX_TEMP) {
      mask |= BATTERY_PREHEAT_BLOCK_MAX_TEMP;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_CHARGING) {
      mask |= BATTERY_PREHEAT_BLOCK_CHARGING;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_TIMEOUT) {
      mask |= BATTERY_PREHEAT_BLOCK_TIMEOUT;
    } else if (batteryPreheatAutoOffReason == BATTERY_PREHEAT_OFF_LOW_SOC) {
      mask |= BATTERY_PREHEAT_BLOCK_LOW_SOC;
    }
  }
  return mask;
}
#endif

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
static DNSServer dnsServer;
static volatile bool webUiRebootPending = false;
static Preferences prefs;
static const IPAddress WEBUI_AP_IP(100, 100, 1, 1);
static const IPAddress WEBUI_AP_NETMASK(255, 255, 255, 0);
constexpr uint16_t WEBUI_DNS_PORT = 53;
static constexpr const char* TESLA_CONNMAN_HOST = "connman.vn.cloud.tesla.cn";
static constexpr const char* TESLA_WWW_HOST = "www.tesla.cn";
static constexpr const char* TESLA_ROOT_HOST = "tesla.cn";
static constexpr const char TESLA_CONNMAN_ONLINE_BODY[] =
    "<html>\n<head>\n</head>\n<body>\n</body>\n</html>\n";
static_assert(sizeof(TESLA_CONNMAN_ONLINE_BODY) - 1 == 45,
              "ConnMan online body length must stay 45 bytes");

static void loadConfigFromPrefs();
static void saveConfigToPrefs();
static void setupLightWebUi();
static void webTask(void*);

static String normalizedHttpHost() {
  String host = server.hostHeader();
  host.toLowerCase();
  host.trim();
  const int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  if (host.endsWith(".")) host.remove(host.length() - 1);
  return host;
}

static bool isTeslaConnectivityHost(const String& host) {
  return host == TESLA_CONNMAN_HOST || host == TESLA_WWW_HOST || host == TESLA_ROOT_HOST ||
         host.endsWith(".tesla.cn");
}

static bool isTeslaConnectivityUri(String uri) {
  uri.toLowerCase();
  uri.trim();
  if (!uri.startsWith("http://")) return false;
  uri.remove(0, 7);
  const int slash = uri.indexOf('/');
  if (slash >= 0) uri = uri.substring(0, slash);
  const int colon = uri.indexOf(':');
  if (colon >= 0) uri = uri.substring(0, colon);
  if (uri.endsWith(".")) uri.remove(uri.length() - 1);
  return isTeslaConnectivityHost(uri);
}

static bool isTeslaConnectivityRequest() {
  return isTeslaConnectivityHost(normalizedHttpHost()) || isTeslaConnectivityUri(server.uri());
}

static void sendNoCacheHeader() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
}

static void sendTeslaConnectivityResponse() {
  sendNoCacheHeader();
  server.sendHeader("X-ConnMan-Status", "online");
  server.sendHeader("Server", "CloudFront");
  if (server.method() == HTTP_HEAD) {
    server.setContentLength(sizeof(TESLA_CONNMAN_ONLINE_BODY) - 1);
    server.send(200, "text/html", "");
    return;
  }
  server.send(200, "text/html", TESLA_CONNMAN_ONLINE_BODY);
}

static bool isGenericConnectivityHost(const String& host) {
  return host == "connectivitycheck.gstatic.com" || host == "clients3.google.com" ||
         host == "connectivitycheck.android.com" || host == "captive.apple.com" ||
         host == "www.msftconnecttest.com" || host == "msftconnecttest.com";
}

static bool isWebUiHost(const String& host) {
  return host.length() == 0 || host == WEBUI_AP_IP.toString();
}

static void sendGenericConnectivityResponse() {
  sendNoCacheHeader();
  if (server.method() == HTTP_HEAD) {
    server.setContentLength(0);
    server.send(204, "text/plain", "");
    return;
  }
  server.send(204, "text/plain", "");
}

static void handleConnectivityProbe() {
  if (isTeslaConnectivityRequest()) {
    sendTeslaConnectivityResponse();
    return;
  }
  sendGenericConnectivityResponse();
}

#include "web_ui_page.h"  // kIndexHtml -- kept out of the .ino prototype scanner

static void handleRoot() {
  if (isTeslaConnectivityRequest()) {
    sendTeslaConnectivityResponse();
    return;
  }
  if (isGenericConnectivityHost(normalizedHttpHost())) {
    sendGenericConnectivityResponse();
    return;
  }
  server.send_P(200, "text/html", kIndexHtml);
}

static void handleCaptivePortalOrNotFound() {
  if (isTeslaConnectivityRequest()) {
    sendTeslaConnectivityResponse();
    return;
  }

  if (server.method() == HTTP_GET || server.method() == HTTP_HEAD) {
    const String host = normalizedHttpHost();
    if (isGenericConnectivityHost(host) || isTeslaConnectivityUri(server.uri())) {
      handleConnectivityProbe();
      return;
    }
    if (!isWebUiHost(host)) {
      sendGenericConnectivityResponse();
      return;
    }
    server.sendHeader("Location", String("http://") + WEBUI_AP_IP.toString() + "/");
    server.send(302, "text/plain", "");
    return;
  }

  server.send(404, "text/plain", "Not found");
}

static void configureSoftApDhcpDns() {
  esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!apNetif) return;
  esp_netif_dhcp_status_t dhcpStatus = ESP_NETIF_DHCP_STOPPED;
  const bool wasStarted =
      esp_netif_dhcps_get_status(apNetif, &dhcpStatus) == ESP_OK &&
      dhcpStatus == ESP_NETIF_DHCP_STARTED;
  if (wasStarted) esp_netif_dhcps_stop(apNetif);
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  esp_netif_set_ip4_addr(&dns.ip.u_addr.ip4, 100, 100, 1, 1);
  esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                         &dns.ip.u_addr.ip4.addr, sizeof(dns.ip.u_addr.ip4.addr));
  if (wasStarted) esp_netif_dhcps_start(apNetif);
}

static void handleStatus() {
  // Read-only: snapshot the cached config + status, never touch the CAN bus.
  RuntimeConfig c = configSnapshot();
  RuntimeStatus s = g_status;
  const uint32_t now = millis();
  s.batteryPreheatAgeMs = batteryPreheatLastSendMs == 0 ? 0 : (now - batteryPreheatLastSendMs);
  s.batteryPreheatFeedbackAgeMs =
      batteryPreheatFeedbackLastRxMs == 0 ? 0 : (now - batteryPreheatFeedbackLastRxMs);
  s.batteryPreheatChargeStatusAgeMs =
      batteryPreheatBmsStatusLastRxMs == 0 ? 0 : (now - batteryPreheatBmsStatusLastRxMs);
  s.batteryPreheatBms312AgeMs =
      batteryPreheatBms312LastRxMs == 0 ? 0 : (now - batteryPreheatBms312LastRxMs);
  s.batteryPreheatBms3b2AgeMs =
      batteryPreheatBms3b2LastRxMs == 0 ? 0 : (now - batteryPreheatBms3b2LastRxMs);
  s.batteryPreheatVcfrontAgeMs =
      batteryPreheatVcfrontLastRxMs == 0 ? 0 : (now - batteryPreheatVcfrontLastRxMs);
  s.batteryPreheatBlockMask = batteryPreheatComputeBlockMask(c, now);
  const bool batteryPreheatFeedbackFresh =
      batteryPreheatFeedbackLastRxMs != 0 &&
      (now - batteryPreheatFeedbackLastRxMs) <= BATTERY_PREHEAT_TEMP_FRESH_MS;
  s.batteryPreheatHeatingActive =
      batteryPreheatFeedbackFresh ? (s.batteryPreheatUiState == 1 ? 1 : 0) : 255;
  const bool bms312Fresh =
      batteryPreheatBms312LastRxMs != 0 &&
      (now - batteryPreheatBms312LastRxMs) <= BATTERY_PREHEAT_TEMP_FRESH_MS;
  const bool bms3b2Fresh =
      batteryPreheatBms3b2LastRxMs != 0 &&
      (now - batteryPreheatBms3b2LastRxMs) <= BATTERY_PREHEAT_TEMP_FRESH_MS;
  s.batteryPreheatBmsHeatStatus = (bms312Fresh || bms3b2Fresh) ? 1 : 0;
  s.bmsTempDecodedAgeMs =
      bmsTempDecodedLastRxMs == 0 ? 0 : (now - bmsTempDecodedLastRxMs);
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
  s.nagKillerDndActionCount = NAG_DND_ACTION_COUNT;
  s.nagKillerDndRemaining = nagKillerDndRemainingActions;
  s.nagKillerDndTriggerCount = nagKillerDndTriggerCount;
  s.nagKillerDndLastTriggerAgeMs =
      nagKillerDndLastTriggerMs == 0 ? 0 : (now - nagKillerDndLastTriggerMs);
  s.nagKillerApState = nagKillerApState;
  s.nagKillerHandsOnState = nagKillerHandsOnState;
  s.nagKillerSteeringDegCx10 = nagKillerLastSteeringMs == 0 ? -32768 : static_cast<int>(nagKillerSteeringAngleDeg * 10.0f);
  if (nagKillerLastTxMs == 0 || (now - nagKillerLastTxMs) > 250UL) {
    s.nagKillerActive = 0;
  }
  s.dasLcHandsOnReasonSeen = dasCarLogLastRxMs != 0 ? 1 : 0;
  s.dasLcHandsOnReasonLatestAgeMs =
      dasLcHandsOnReasonLatestMs == 0 ? 0xFFFFFFFFUL : (now - dasLcHandsOnReasonLatestMs);
  s.dasLcHandsOnReasonPreviousAgeMs =
      dasLcHandsOnReasonPreviousMs == 0 ? 0xFFFFFFFFUL : (now - dasLcHandsOnReasonPreviousMs);
  if (!s.dasLcHandsOnReasonSeen) {
    s.dasLcHandsOnReasonDecode = DAS_LC_REASON_DECODE_NO_FRAME;
  }
  s.fsdActivationResendActive =
      (c.fsdEnabled && c.fsdActivationResendEnabled && c.fsdActivationResendMs > 0) ? 1 : 0;
  s.fsdActivationResendCachedMuxMask = fsdActivationResendCachedMuxMask();
  s.fsdActivationResendPeriodMs = clampFsdActivationResendMs(c.fsdActivationResendMs);
  s.fsdActivationResendTxCount = fsdActivationResendTxCount;
  s.fsdActivationResendLastTxAgeMs =
      fsdActivationResendLastTxMs == 0 ? 0 : (now - fsdActivationResendLastTxMs);

  String j;
  j.reserve(11800);
  j += '{';
  j += "\"canCommsEnabled\":";       j += c.canCommsEnabled ? 1 : 0;
  j += ",\"fsdEnabled\":";           j += c.fsdEnabled ? 1 : 0;
  j += ",\"fsdActivationResendEnabled\":"; j += c.fsdActivationResendEnabled ? 1 : 0;
  j += ",\"fsdActivationResendMs\":"; j += c.fsdActivationResendMs;
  j += ",\"autoSpeedOffsetEnabled\":"; j += c.autoSpeedOffsetEnabled ? 1 : 0;
  j += ",\"cabinCameraDisableEnabled\":"; j += c.cabinCameraDisableEnabled ? 1 : 0;
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
  j += ",\"overtakeLightAlwaysOnEnabled\":"; j += c.overtakeLightAlwaysOnEnabled ? 1 : 0;
  j += ",\"rearFogBrakeStrobeEnabled\":"; j += c.rearFogBrakeStrobeEnabled ? 1 : 0;
  j += ",\"reverseStrobeEnabled\":"; j += c.reverseStrobeEnabled ? 1 : 0;
  j += ",\"batteryPreheatEnabled\":"; j += c.batteryPreheatEnabled ? 1 : 0;
  j += ",\"dndEnabled\":"; j += c.dndEnabled ? 1 : 0;
  j += ",\"nagKillerEnabled\":"; j += c.nagKillerEnabled ? 1 : 0;
  j += ",\"nagKillerDndEnabled\":"; j += c.nagKillerDndEnabled ? 1 : 0;
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
  j += ",\"scrollGearInjectEnabled\":"; j += c.scrollGearInjectEnabled ? 1 : 0;
  j += ",\"can1ReceiveOnly\":"; j += c.can1ReceiveOnly ? 1 : 0;
  j += ",\"can1Rx\":";               j += s.can1Rx;
  j += ",\"can1Tx\":";               j += s.can1Tx;
  j += ",\"can1TxFail\":";           j += s.can1TxFail;
  j += ",\"twaiBusOffCount\":";      j += s.twaiBusOffCount;
  j += ",\"twaiState\":";            j += s.twaiState;
  j += ",\"twaiRxQueue\":";          j += s.twaiRxQueue;
  j += ",\"twaiTxQueue\":";          j += s.twaiTxQueue;
  j += ",\"twaiRxQueueMax\":";       j += s.twaiRxQueueMax;
  j += ",\"twaiTxQueueMax\":";       j += s.twaiTxQueueMax;
  j += ",\"twaiRxMissed\":";         j += s.twaiRxMissed;
  j += ",\"twaiRxOverrun\":";        j += s.twaiRxOverrun;
  j += ",\"twaiBusError\":";         j += s.twaiBusError;
  j += ",\"twaiTxFailed\":";         j += s.twaiTxFailed;
  j += ",\"twaiTxErr\":";            j += s.twaiTxErr;
  j += ",\"twaiRxErr\":";            j += s.twaiRxErr;
  j += ",\"canbRx\":";               j += s.canbRx;
  j += ",\"canbTx\":";               j += s.canbTx;
  j += ",\"canbTxFail\":";           j += s.canbTxFail;
  j += ",\"canbLastId\":";           j += s.canbLastId;
  j += ",\"canbErrorFlags\":";       j += s.canbErrorFlags;
  j += ",\"canbRxOverflowCount\":";  j += s.canbRxOverflowCount;
  j += ",\"diagWindowMs\":";         j += s.diagWindowMs;
  j += ",\"loopHz\":";               j += s.loopHz;
  j += ",\"loopAvgUs\":";            j += s.loopAvgUs;
  j += ",\"loopMaxUs\":";            j += s.loopMaxUs;
  j += ",\"loopPeriodMaxUs\":";      j += s.loopPeriodMaxUs;
  j += ",\"loopBusyPct\":";          j += s.loopBusyPct;
  j += ",\"cpuPct\":";               j += s.cpuPct;
  j += ",\"cpu0Pct\":";              j += s.cpu0Pct;
  j += ",\"cpu1Pct\":";              j += s.cpu1Pct;
  j += ",\"loopWaitAvgUs\":";        j += s.loopWaitAvgUs;
  j += ",\"can1RxRate\":";           j += s.can1RxRate;
  j += ",\"can1TxRate\":";           j += s.can1TxRate;
  j += ",\"can1TxFailRate\":";       j += s.can1TxFailRate;
  j += ",\"can1RxGapMaxUs\":";       j += s.can1RxGapMaxUs;
  j += ",\"can1TxMaxUs\":";          j += s.can1TxMaxUs;
  j += ",\"can1TxSlowCount\":";      j += s.can1TxSlowCount;
  j += ",\"canbRxRate\":";           j += s.canbRxRate;
  j += ",\"canbTxRate\":";           j += s.canbTxRate;
  j += ",\"canbTxFailRate\":";       j += s.canbTxFailRate;
  j += ",\"canbRxGapMaxUs\":";       j += s.canbRxGapMaxUs;
  j += ",\"canbTxMaxUs\":";          j += s.canbTxMaxUs;
  j += ",\"canbTxSlowCount\":";      j += s.canbTxSlowCount;
  j += ",\"canbDrainMaxUs\":";       j += s.canbDrainMaxUs;
  j += ",\"canbDrainMaxFrames\":";   j += s.canbDrainMaxFrames;
  j += ",\"canbTxLoopLast\":";       j += s.canbTxLoopLast;
  j += ",\"canbTxLoopMax\":";        j += s.canbTxLoopMax;
  j += ",\"canbTxLoopMaxEver\":";    j += s.canbTxLoopMaxEver;
  j += ",\"canbTxSrcOther\":";       j += s.canbTxSrcOther;
  j += ",\"canbTxSrcNag\":";         j += s.canbTxSrcNag;
  j += ",\"canbTxSrcBattery\":";     j += s.canbTxSrcBattery;
  j += ",\"canbTxSrcService\":";     j += s.canbTxSrcService;
  j += ",\"canbTxSrcScrollGear\":";  j += s.canbTxSrcScrollGear;
  j += ",\"canbTxSrcDnd\":";         j += s.canbTxSrcDnd;
  j += ",\"canbTxSrcLight\":";       j += s.canbTxSrcLight;
  j += ",\"canbTxSrcRearFog\":";     j += s.canbTxSrcRearFog;
  j += ",\"canbTxSrcReverse\":";     j += s.canbTxSrcReverse;
  j += ",\"canbTxQueueDepth\":";     j += s.canbTxQueueDepth;
  j += ",\"canbTxQueueMaxDepth\":";  j += s.canbTxQueueMaxDepth;
  j += ",\"canbTxSchedTx\":";        j += s.canbTxSchedTx;
  j += ",\"canbTxSchedDrop\":";      j += s.canbTxSchedDrop;
  j += ",\"canbTxSchedExpired\":";   j += s.canbTxSchedExpired;
  j += ",\"canbTxSchedFail\":";      j += s.canbTxSchedFail;
  j += ",\"canbTxSchedBudgetHit\":"; j += s.canbTxSchedBudgetHit;
  j += ",\"fsdActivationResendActive\":"; j += s.fsdActivationResendActive;
  j += ",\"fsdActivationResendCachedMuxMask\":"; j += s.fsdActivationResendCachedMuxMask;
  j += ",\"fsdActivationResendPeriodMs\":"; j += s.fsdActivationResendPeriodMs;
  j += ",\"fsdActivationResendTxCount\":"; j += s.fsdActivationResendTxCount;
  j += ",\"fsdActivationResendLastTxAgeMs\":"; j += s.fsdActivationResendLastTxAgeMs;
  j += ",\"webTaskMaxUs\":";         j += s.webTaskMaxUs;
  j += ",\"totalHeapBytes\":";       j += s.totalHeapBytes;
  j += ",\"freeHeapBytes\":";        j += s.freeHeapBytes;
  j += ",\"minFreeHeapBytes\":";     j += s.minFreeHeapBytes;
  j += ",\"freeHeapPct\":";          j += s.freeHeapPct;
  j += ",\"minFreeHeapPct\":";       j += s.minFreeHeapPct;
  j += ",\"cpuMhz\":";               j += s.cpuMhz;
  j += ",\"batteryPreheatActive\":"; j += s.batteryPreheatActive;
  j += ",\"batteryPreheatTxCount\":"; j += s.batteryPreheatTxCount;
  j += ",\"batteryPreheatAgeMs\":"; j += s.batteryPreheatAgeMs;
  j += ",\"batteryPreheatRunMs\":"; j += s.batteryPreheatRunMs;
  j += ",\"batteryPreheatStableTempMs\":"; j += s.batteryPreheatStableTempMs;
  j += ",\"batteryPreheatAutoOffReason\":"; j += s.batteryPreheatAutoOffReason;
  j += ",\"batteryPreheatAutoOffLatched\":"; j += s.batteryPreheatAutoOffLatched;
  j += ",\"batteryPreheatChargeDetected\":"; j += s.batteryPreheatChargeDetected;
  j += ",\"batteryPreheatBlockMask\":"; j += s.batteryPreheatBlockMask;
  j += ",\"batteryPreheatChargeStatusBus\":"; j += s.batteryPreheatChargeStatusBus;
  j += ",\"batteryPreheatChargeStatus\":"; j += s.batteryPreheatChargeStatus;
  j += ",\"batteryPreheatChargeStatusAgeMs\":"; j += s.batteryPreheatChargeStatusAgeMs;
  j += ",\"batteryPreheatSocBus\":"; j += s.batteryPreheatSocBus;
  j += ",\"batteryPreheatSocUiDeciPct\":"; j += s.batteryPreheatSocUiDeciPct;
  j += ",\"batteryPreheatSocAgeMs\":"; j += s.batteryPreheatSocAgeMs;
  j += ",\"batteryPreheatFeedbackSeen\":"; j += s.batteryPreheatFeedbackSeen;
  j += ",\"batteryPreheatFeedbackBus\":"; j += s.batteryPreheatFeedbackBus;
  j += ",\"batteryPreheatFeedbackAgeMs\":"; j += s.batteryPreheatFeedbackAgeMs;
  j += ",\"batteryPreheatUiTripActive\":"; j += s.batteryPreheatUiTripActive;
  j += ",\"batteryPreheatUiNavToSupercharger\":"; j += s.batteryPreheatUiNavToSupercharger;
  j += ",\"batteryPreheatUiFastChargerType\":"; j += s.batteryPreheatUiFastChargerType;
  j += ",\"batteryPreheatUiState\":"; j += s.batteryPreheatUiState;
  j += ",\"batteryPreheatUiRequestHeat\":"; j += s.batteryPreheatUiRequestHeat;
  j += ",\"batteryPreheatHeatingActive\":"; j += s.batteryPreheatHeatingActive;
  j += ",\"batteryPreheatUiPowerW\":"; j += s.batteryPreheatUiPowerW;
  j += ",\"batteryPreheatUiTargetCx100\":"; j += s.batteryPreheatUiTargetCx100;
  j += ",\"batteryPreheatCmdPowerW\":"; j += static_cast<int>(signedByte(BATTERY_PREHEAT_ON[1])) * 125;
  j += ",\"batteryPreheatCmdTargetCx100\":"; j += BATTERY_PREHEAT_TARGET_CX100;
  j += ",\"batteryPreheatBms312Seen\":"; j += s.batteryPreheatBms312Seen;
  j += ",\"batteryPreheatBms312Bus\":"; j += s.batteryPreheatBms312Bus;
  j += ",\"batteryPreheatBms312AgeMs\":"; j += s.batteryPreheatBms312AgeMs;
  j += ",\"batteryPreheatBms3b2Seen\":"; j += s.batteryPreheatBms3b2Seen;
  j += ",\"batteryPreheatBms3b2Bus\":"; j += s.batteryPreheatBms3b2Bus;
  j += ",\"batteryPreheatBms3b2AgeMs\":"; j += s.batteryPreheatBms3b2AgeMs;
  j += ",\"batteryPreheatBmsHeatStatus\":"; j += s.batteryPreheatBmsHeatStatus;
  j += ",\"batteryPreheatVcfrontSeen\":"; j += s.batteryPreheatVcfrontSeen;
  j += ",\"batteryPreheatVcfrontBus\":"; j += s.batteryPreheatVcfrontBus;
  j += ",\"batteryPreheatVcfrontAgeMs\":"; j += s.batteryPreheatVcfrontAgeMs;
  j += ",\"batteryPreheatVcfrontCoolantBatInletCx100\":"; j += s.batteryPreheatVcfrontCoolantBatInletCx100;
  j += ",\"batteryPreheatVcfrontCoolantPtInletCx100\":"; j += s.batteryPreheatVcfrontCoolantPtInletCx100;
  j += ",\"batteryPreheatVcfrontAmbientCx100\":"; j += s.batteryPreheatVcfrontAmbientCx100;
  j += ",\"batteryPreheatVcfrontAmbientFilteredCx100\":"; j += s.batteryPreheatVcfrontAmbientFilteredCx100;
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
  j += ",\"nagKillerDndActionCount\":"; j += s.nagKillerDndActionCount;
  j += ",\"nagKillerDndRemaining\":"; j += s.nagKillerDndRemaining;
  j += ",\"nagKillerDndTriggerCount\":"; j += s.nagKillerDndTriggerCount;
  j += ",\"nagKillerDndLastTriggerAgeMs\":"; j += s.nagKillerDndLastTriggerAgeMs;
  j += ",\"dasLcHandsOnReasonSeen\":"; j += s.dasLcHandsOnReasonSeen;
  j += ",\"dasLcHandsOnReasonDlc\":"; j += s.dasLcHandsOnReasonDlc;
  j += ",\"dasLcHandsOnReasonDecode\":"; j += s.dasLcHandsOnReasonDecode;
  j += ",\"dasLcHandsOnReasonPrevious\":"; j += s.dasLcHandsOnReasonPrevious;
  j += ",\"dasLcHandsOnReasonLatest\":"; j += s.dasLcHandsOnReasonLatest;
  j += ",\"dasLcHandsOnReasonPreviousAgeMs\":"; j += s.dasLcHandsOnReasonPreviousAgeMs;
  j += ",\"dasLcHandsOnReasonLatestAgeMs\":"; j += s.dasLcHandsOnReasonLatestAgeMs;
  j += ",\"bmsTempDecodedAgeMs\":"; j += s.bmsTempDecodedAgeMs;
  j += ",\"bmsTempMinCx100\":"; j += s.bmsTempMinCx100;
  j += ",\"bmsTempAvgCx100\":"; j += s.bmsTempAvgCx100;
  j += ",\"bmsTempMaxCx100\":"; j += s.bmsTempMaxCx100;
  j += ",\"rightScrollTicks\":";     j += s.rightScrollTicks;
  j += ",\"rightStalkStatus\":";     j += s.rightStalkStatus;
  j += ",\"rightStalkCounter\":";    j += s.rightStalkCounter;
  j += ",\"currentGear\":";          j += s.currentGear;
  j += ",\"dasAutopilotState\":";    j += s.dasAutopilotState;
  j += ",\"brakeActive\":";          j += s.brakeActive;
  j += ",\"scrollGearIntent\":";     j += s.scrollGearIntent;
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

  c.canCommsEnabled        = argBool("canCommsEnabled", c.canCommsEnabled);
  c.fsdEnabled              = argBool("fsdEnabled", c.fsdEnabled);
  c.fsdActivationResendEnabled =
      argBool("fsdActivationResendEnabled", c.fsdActivationResendEnabled);
  c.fsdActivationResendMs =
      clampFsdActivationResendMs(argU16("fsdActivationResendMs", c.fsdActivationResendMs));
  c.autoSpeedOffsetEnabled  = argBool("autoSpeedOffsetEnabled", c.autoSpeedOffsetEnabled);
  c.cabinCameraDisableEnabled = argBool("cabinCameraDisableEnabled", c.cabinCameraDisableEnabled);
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
  c.overtakeLightAlwaysOnEnabled =
      argBool("overtakeLightAlwaysOnEnabled", c.overtakeLightAlwaysOnEnabled);
  c.rearFogBrakeStrobeEnabled = argBool("rearFogBrakeStrobeEnabled", c.rearFogBrakeStrobeEnabled);
  c.reverseStrobeEnabled    = argBool("reverseStrobeEnabled", c.reverseStrobeEnabled);
  c.batteryPreheatEnabled   = argBool("batteryPreheatEnabled", c.batteryPreheatEnabled);
  c.dndEnabled              = argBool("dndEnabled", c.dndEnabled);
  c.nagKillerEnabled        = argBool("nagKillerEnabled", c.nagKillerEnabled);
  c.nagKillerDndEnabled     = c.cabinCameraDisableEnabled;
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

// POST /reboot -- acknowledge first, then restart from the WebUI task.
static void handleReboot() {
  server.send(200, "application/json", "{\"ok\":true}");
  webUiRebootPending = true;
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
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"rec_buffer\"}");
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
  j += recBufferMem == 1 ? "true" : "false";
  j += ",\"psramReady\":";
  j += recPsramReady ? "true" : "false";
  j += ",\"mem\":";
  j += recBufferMem;
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
  c.canCommsEnabled       = prefs.getBool("canComms", c.canCommsEnabled);
  c.fsdEnabled             = prefs.getBool("fsdEnabled", c.fsdEnabled);
  c.fsdActivationResendEnabled = prefs.getBool("fsdReOn", c.fsdActivationResendEnabled);
  c.fsdActivationResendMs  = clampFsdActivationResendMs(prefs.getUShort("fsdReMs", c.fsdActivationResendMs));
  c.autoSpeedOffsetEnabled = prefs.getBool("autoOffset", c.autoSpeedOffsetEnabled);
  c.cabinCameraDisableEnabled = prefs.getBool("cabCamOff", c.cabinCameraDisableEnabled);
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
  c.overtakeLightAlwaysOnEnabled = prefs.getBool("passAlways", c.overtakeLightAlwaysOnEnabled);
  c.rearFogBrakeStrobeEnabled = prefs.getBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  c.reverseStrobeEnabled   = prefs.getBool("revStrobe", c.reverseStrobeEnabled);
  c.batteryPreheatEnabled  = prefs.getBool("batHeat", c.batteryPreheatEnabled);
  c.dndEnabled             = prefs.getBool("dndCont", c.dndEnabled);
  c.nagKillerEnabled       = prefs.getBool("nagEn", c.nagKillerEnabled);
  c.nagKillerDndEnabled    = prefs.getBool("nagDnd", c.nagKillerDndEnabled);
  c.cabinCameraDisableEnabled = c.cabinCameraDisableEnabled || c.nagKillerDndEnabled;
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
  prefs.putBool("canComms", c.canCommsEnabled);
  prefs.putBool("fsdEnabled", c.fsdEnabled);
  prefs.putBool("fsdReOn", c.fsdActivationResendEnabled);
  prefs.putUShort("fsdReMs", clampFsdActivationResendMs(c.fsdActivationResendMs));
  prefs.putBool("autoOffset", c.autoSpeedOffsetEnabled);
  prefs.putBool("cabCamOff", c.cabinCameraDisableEnabled);
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
  prefs.putBool("passAlways", c.overtakeLightAlwaysOnEnabled);
  prefs.putBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  prefs.putBool("revStrobe", c.reverseStrobeEnabled);
  prefs.putBool("batHeat", c.batteryPreheatEnabled);
  prefs.putBool("dndCont", c.dndEnabled);
  prefs.putBool("nagEn", c.nagKillerEnabled);
  prefs.putBool("nagDnd", c.nagKillerDndEnabled);
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
  prefs.putBool("gearInject", c.scrollGearInjectEnabled);
  prefs.putBool("can1RxOnly", c.can1ReceiveOnly);
  prefs.end();
}

static void setupLightWebUi() {
  Serial.println("[WEBUI] setup begin");
  setupRecorderBuffer();
  Serial.printf("[WEBUI] recorder cap=%lu mem=%u psram=%u\n",
                static_cast<unsigned long>(recCapacity),
                static_cast<unsigned>(recBufferMem),
                recPsramReady ? 1U : 0U);
  const bool modeOk = WiFi.mode(WIFI_AP);
  Serial.printf("[WEBUI] WiFi.mode(WIFI_AP)=%d\n", modeOk ? 1 : 0);
  // SoftAP IP / gateway = 100.100.1.1 (subnet 255.255.255.0). Must precede softAP().
  const bool configOk = WiFi.softAPConfig(WEBUI_AP_IP, WEBUI_AP_IP, WEBUI_AP_NETMASK);
  Serial.printf("[WEBUI] softAPConfig=%d ip=%s\n", configOk ? 1 : 0, WEBUI_AP_IP.toString().c_str());
  const bool apOk = WiFi.softAP(WEBUI_AP_SSID, WEBUI_AP_PASS);
  Serial.printf("[WEBUI] softAP=%d ssid=%s ip=%s mac=%s\n",
                apOk ? 1 : 0,
                WiFi.softAPSSID().c_str(),
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str());
  configureSoftApDhcpDns();
  dnsServer.start(WEBUI_DNS_PORT, "*", WEBUI_AP_IP);
  server.on("/", HTTP_ANY, handleRoot);
  server.on("/generate_204", HTTP_ANY, handleConnectivityProbe);
  server.on("/gen_204", HTTP_ANY, handleConnectivityProbe);
  server.on("/hotspot-detect.html", HTTP_ANY, handleConnectivityProbe);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rec_start", HTTP_POST, handleRecStart);
  server.on("/rec_stop", HTTP_POST, handleRecStop);
  server.on("/rec_status", HTTP_GET, handleRecStatus);
  server.on("/rec_download", HTTP_GET, handleRecDownload);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound(handleCaptivePortalOrNotFound);
  server.begin();
  Serial.println("[WEBUI] http server begin");
}

// Dedicated WebUI task (pinned to core 0, low priority). The CAN main loop on
// core 1 never waits on this; HTTP is only serviced while the WebUI is enabled.
static void webTask(void*) {
  for (;;) {
    if (webUiRebootPending) {
      delay(150);
      ESP.restart();
    }
    const uint32_t webStartUs = micros();
    dnsServer.processNextRequest();
    server.handleClient();
    const uint32_t webUs = micros() - webStartUs;
    if (webUs > diagWebTaskMaxUs) diagWebTaskMaxUs = webUs;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#endif  // ENABLE_LIGHT_WEBUI

// ---- Main ----

static bool diagCpu0IdleHook() {
  diagCpuIdleLoops[0]++;
  return true;
}

static bool diagCpu1IdleHook() {
  diagCpuIdleLoops[1]++;
  return true;
}

static uint32_t diagCpuBusyPctFromIdleLoops(uint8_t core, uint32_t idleDelta, uint32_t windowMs) {
  if (core >= 2 || windowMs == 0) return 0;
  uint64_t loopsPerSec64 = (static_cast<uint64_t>(idleDelta) * 1000ULL) / windowMs;
  if (loopsPerSec64 > UINT32_MAX) loopsPerSec64 = UINT32_MAX;
  const uint32_t loopsPerSec = static_cast<uint32_t>(loopsPerSec64);
  if (loopsPerSec > diagCpuIdleLoopsPerSecMax[core]) {
    diagCpuIdleLoopsPerSecMax[core] = loopsPerSec;
  }
  const uint32_t idleMax = diagCpuIdleLoopsPerSecMax[core];
  if (idleMax == 0 || loopsPerSec >= idleMax) return 0;
  return ((idleMax - loopsPerSec) * 100UL + idleMax / 2UL) / idleMax;
}

static void updateRuntimeDiagnostics(uint32_t loopElapsedUs) {
  const uint32_t nowMs = millis();
  if (diagWindowStartMs == 0) {
    diagWindowStartMs = nowMs;
    diagPrevCan1Rx = g_status.can1Rx;
    diagPrevCan1Tx = g_status.can1Tx;
    diagPrevCan1TxFail = g_status.can1TxFail;
    diagPrevCanbRx = g_status.canbRx;
    diagPrevCanbTx = g_status.canbTx;
    diagPrevCanbTxFail = g_status.canbTxFail;
    diagPrevCpuIdleLoops[0] = diagCpuIdleLoops[0];
    diagPrevCpuIdleLoops[1] = diagCpuIdleLoops[1];
  }

  diagLoopCount++;
  diagLoopBusyUs += loopElapsedUs;
  if (loopElapsedUs > diagLoopMaxUs) diagLoopMaxUs = loopElapsedUs;

  const uint32_t windowMs = nowMs - diagWindowStartMs;
  if (windowMs < 1000UL) return;

  g_status.diagWindowMs = windowMs;
  g_status.loopHz = (diagLoopCount * 1000UL) / windowMs;
  g_status.loopAvgUs = diagLoopCount == 0 ? 0 : static_cast<uint32_t>(diagLoopBusyUs / diagLoopCount);
  g_status.loopMaxUs = diagLoopMaxUs;
  g_status.loopPeriodMaxUs = diagLoopPeriodMaxUs;
  g_status.loopWaitAvgUs = diagLoopCount == 0 ? 0 : static_cast<uint32_t>(diagLoopWaitUs / diagLoopCount);
  const uint64_t activeUs = diagLoopBusyUs > diagLoopWaitUs ? (diagLoopBusyUs - diagLoopWaitUs) : 0ULL;
  uint64_t cpuPct = (activeUs * 100ULL) / (static_cast<uint64_t>(windowMs) * 1000ULL);
  if (cpuPct > 100ULL) cpuPct = 100ULL;
  g_status.cpuPct = static_cast<uint32_t>(cpuPct);
  g_status.loopBusyPct = g_status.cpuPct;
  const uint32_t cpuIdle0 = diagCpuIdleLoops[0];
  const uint32_t cpuIdle1 = diagCpuIdleLoops[1];
  g_status.cpu0Pct =
      diagCpuBusyPctFromIdleLoops(0, cpuIdle0 - diagPrevCpuIdleLoops[0], windowMs);
  g_status.cpu1Pct =
      diagCpuBusyPctFromIdleLoops(1, cpuIdle1 - diagPrevCpuIdleLoops[1], windowMs);

  g_status.can1RxRate = ((g_status.can1Rx - diagPrevCan1Rx) * 1000UL) / windowMs;
  g_status.can1TxRate = ((g_status.can1Tx - diagPrevCan1Tx) * 1000UL) / windowMs;
  g_status.can1TxFailRate = ((g_status.can1TxFail - diagPrevCan1TxFail) * 1000UL) / windowMs;
  g_status.can1RxGapMaxUs = diagCan1RxGapMaxUs;
  g_status.can1TxMaxUs = diagCan1TxMaxUs;
  g_status.can1TxSlowCount = diagCan1TxSlowCount;

  g_status.canbRxRate = ((g_status.canbRx - diagPrevCanbRx) * 1000UL) / windowMs;
  g_status.canbTxRate = ((g_status.canbTx - diagPrevCanbTx) * 1000UL) / windowMs;
  g_status.canbTxFailRate = ((g_status.canbTxFail - diagPrevCanbTxFail) * 1000UL) / windowMs;
  g_status.canbRxGapMaxUs = diagCanbRxGapMaxUs;
  g_status.canbTxMaxUs = diagCanbTxMaxUs;
  g_status.canbTxSlowCount = diagCanbTxSlowCount;
  g_status.canbDrainMaxUs = diagCanbDrainMaxUs;
  g_status.canbDrainMaxFrames = diagCanbDrainMaxFrames;
  g_status.canbTxLoopMax = diagCanbTxLoopMax;
  g_status.canbTxLoopMaxEver = diagCanbTxLoopMaxEver;
  g_status.canbTxSrcOther = diagCanbTxSourceCounts[CANB_TX_SRC_OTHER];
  g_status.canbTxSrcNag = diagCanbTxSourceCounts[CANB_TX_SRC_NAG];
  g_status.canbTxSrcBattery = diagCanbTxSourceCounts[CANB_TX_SRC_BATTERY];
  g_status.canbTxSrcService = diagCanbTxSourceCounts[CANB_TX_SRC_SERVICE];
  g_status.canbTxSrcScrollGear = diagCanbTxSourceCounts[CANB_TX_SRC_SCROLL_GEAR];
  g_status.canbTxSrcDnd = diagCanbTxSourceCounts[CANB_TX_SRC_DND];
  g_status.canbTxSrcLight = diagCanbTxSourceCounts[CANB_TX_SRC_LIGHT];
  g_status.canbTxSrcRearFog = diagCanbTxSourceCounts[CANB_TX_SRC_REAR_FOG];
  g_status.canbTxSrcReverse = diagCanbTxSourceCounts[CANB_TX_SRC_REVERSE];

#ifdef ENABLE_LIGHT_WEBUI
  g_status.webTaskMaxUs = diagWebTaskMaxUs;
  diagWebTaskMaxUs = 0;
#else
  g_status.webTaskMaxUs = 0;
#endif
  g_status.totalHeapBytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  g_status.freeHeapBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  g_status.minFreeHeapBytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  if (g_status.totalHeapBytes > 0) {
    g_status.freeHeapPct = (g_status.freeHeapBytes * 100UL) / g_status.totalHeapBytes;
    g_status.minFreeHeapPct = (g_status.minFreeHeapBytes * 100UL) / g_status.totalHeapBytes;
  } else {
    g_status.freeHeapPct = 0;
    g_status.minFreeHeapPct = 0;
  }
  g_status.cpuMhz = ESP.getCpuFreqMHz();

  diagPrevCan1Rx = g_status.can1Rx;
  diagPrevCan1Tx = g_status.can1Tx;
  diagPrevCan1TxFail = g_status.can1TxFail;
  diagPrevCanbRx = g_status.canbRx;
  diagPrevCanbTx = g_status.canbTx;
  diagPrevCanbTxFail = g_status.canbTxFail;
  diagPrevCpuIdleLoops[0] = cpuIdle0;
  diagPrevCpuIdleLoops[1] = cpuIdle1;
  diagWindowStartMs = nowMs;
  diagLoopCount = 0;
  diagLoopBusyUs = 0;
  diagLoopWaitUs = 0;
  diagLoopMaxUs = 0;
  diagLoopPeriodMaxUs = 0;
  diagCan1RxGapMaxUs = 0;
  diagCan1TxMaxUs = 0;
  diagCan1TxSlowCount = 0;
  diagCanbRxGapMaxUs = 0;
  diagCanbTxMaxUs = 0;
  diagCanbTxSlowCount = 0;
  diagCanbDrainMaxUs = 0;
  diagCanbDrainMaxFrames = 0;
  diagCanbTxLoopMax = 0;
}

static void handleTwaiFrame(can_frame& frame, const RuntimeConfig& cfg) {
  recordCanFrame(frame, 'R', 1);
  handleBatteryTempDiagFrame(frame, 1);
  handleBatteryPreheatBmsDiagFrame(frame, 1);
  handleBatteryPreheatFeedbackFrame(frame, 1);
  speedLimitMonitor.update(frame);
  handleNagKillerContextFrame(frame, cfg);
  handleDasCarLogFrame(frame);
#ifdef ENABLE_CANB_MCP2515
  handleDndHandsOnFrame(frame);
#endif
  handler.refreshUnifiedSpeedCompensation(cfg);
  handler.handelMessage(frame, cfg);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("[BOOT] T2CAN FSD starting");
  Serial.printf("[BOOT] CPU=%uMHz flash=%u psram=%u\n",
                static_cast<unsigned>(getCpuFrequencyMhz()),
                static_cast<unsigned>(ESP.getFlashChipSize()),
                psramFound() ? 1U : 0U);

  Serial.println("[BOOT] register idle hooks");
  esp_err_t hook0 = esp_register_freertos_idle_hook_for_cpu(diagCpu0IdleHook, 0);
  esp_err_t hook1 = esp_register_freertos_idle_hook_for_cpu(diagCpu1IdleHook, 1);
  Serial.printf("[BOOT] idle hooks result cpu0=%d cpu1=%d\n", static_cast<int>(hook0), static_cast<int>(hook1));

#ifdef ENABLE_LIGHT_WEBUI
  Serial.println("[BOOT] loadConfigFromPrefs begin");
  loadConfigFromPrefs();
  Serial.println("[BOOT] loadConfigFromPrefs done");
#endif

  // Configure TWAI (CAN) peripheral at 500 kbps
  Serial.println("[BOOT] TWAI install begin");
  twai_general_config_t g_config_twai = TWAI_GENERAL_CONFIG_DEFAULT(
    static_cast<gpio_num_t>(TWAI_TX_PIN),
    static_cast<gpio_num_t>(TWAI_RX_PIN),
    TWAI_MODE_NORMAL);
  g_config_twai.rx_queue_len = TWAI_RX_QUEUE_LEN;
  g_config_twai.tx_queue_len = TWAI_TX_QUEUE_LEN;
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = { CAN_ACCEPT_CODE, CAN_ACCEPT_MASK, true };

  esp_err_t twaiInstall = twai_driver_install(&g_config_twai, &t_config, &f_config);
  esp_err_t twaiStart = twai_start();
  esp_err_t twaiAlerts = twai_reconfigure_alerts(TWAI_ALERT_MASK, nullptr);
  Serial.printf("[BOOT] TWAI install=%d start=%d alerts=%d\n",
                static_cast<int>(twaiInstall),
                static_cast<int>(twaiStart),
                static_cast<int>(twaiAlerts));

#ifdef ENABLE_LIGHT_WEBUI
  Serial.println("[BOOT] ENABLE_LIGHT_WEBUI=1");
  setupLightWebUi();
  BaseType_t webTaskOk = xTaskCreatePinnedToCore(webTask, "web", 4096, nullptr, 1, nullptr, 0);
  Serial.printf("[WEBUI] webTask create=%ld\n", static_cast<long>(webTaskOk));
#else
  Serial.println("[BOOT] ENABLE_LIGHT_WEBUI=0");
#endif

#ifdef ENABLE_CANB_MCP2515
  BaseType_t canbTaskOk = xTaskCreatePinnedToCore(canBInitTask, "canbinit", 4096, nullptr, 1, nullptr, 1);
  Serial.printf("[CANB] init task create=%ld\n", static_cast<long>(canbTaskOk));
#endif
}

void loop() {
  const uint32_t loopStartUs = micros();
  diagCanbTxThisLoop = 0;
  if (diagLoopLastStartUs != 0) {
    const uint32_t periodUs = loopStartUs - diagLoopLastStartUs;
    if (periodUs > diagLoopPeriodMaxUs) diagLoopPeriodMaxUs = periodUs;
  }
  diagLoopLastStartUs = loopStartUs;

  serviceTwaiAlerts();

  RuntimeConfig cfg = configSnapshot();

#ifdef ENABLE_CANB_MCP2515
  bool canbDrainedThisLoop = false;
  if (cfg.canbEnabled && canbIntAsserted()) {
    drainCanBWithBudget(cfg);
    canbDrainedThisLoop = true;
  }
#endif

  can_frame frame;
  if (twai_recv(frame, true)) {
    handleTwaiFrame(frame, cfg);

    const uint32_t twaiDrainStartUs = micros();
    uint8_t twaiDrainedFrames = 1;
    while (twaiDrainedFrames < TWAI_RX_DRAIN_FRAME_LIMIT &&
           (micros() - twaiDrainStartUs) < TWAI_RX_DRAIN_TIME_US) {
      if (!twai_recv(frame, false)) break;
      handleTwaiFrame(frame, cfg);
      twaiDrainedFrames++;
    }
  }

  serviceFsdActivationResend(cfg);

#ifdef ENABLE_CANB_MCP2515
  if (cfg.canbEnabled) {
    if (!canbDrainedThisLoop) drainCanBWithBudget(cfg);
    serviceCanBScheduledTx();
  }
  serviceHighBeamStrobe(cfg);
  serviceOvertakeLightAlwaysOn(cfg);
  serviceReverseStrobe(cfg);
  serviceRearFogBrakeStrobe(cfg);
  serviceScrollGearShift(cfg);
  serviceNagKillerDndBurst(cfg);
  serviceDndScrollAction(cfg);
#endif

  serviceBatteryPreheat(cfg);

#ifdef ENABLE_CANB_MCP2515
  if (cfg.canbEnabled) serviceCanBTxScheduler();
#endif

  g_status.canbTxLoopLast = diagCanbTxThisLoop;
  updateRuntimeDiagnostics(micros() - loopStartUs);
}

