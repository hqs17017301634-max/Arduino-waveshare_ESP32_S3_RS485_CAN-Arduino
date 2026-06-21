# CAN ID Vehicle Network Mapping / CAN ID 车辆网络分工

This note records the wiring rule for this branch: firmware `bus=1` / `bus=2`
names are controller names, while each CAN ID/function must be bound to the
vehicle network where it actually takes effect.

本文档记录当前分支的接线规则：固件里的 `bus=1` / `bus=2` 只是开发板控制器编号，
每个 CAN ID / 功能必须绑定到它实际生效的车辆 CAN 网络和 X179 针脚。

## Controller Mapping / 控制器映射

| Firmware bus / 固件通道 | Controller / 控制器 | LILYGO physical port / 物理口 |
|---|---|---|
| `CAN1` / `bus=1` | ESP32-S3 native TWAI / 原生 TWAI | physical CANB |
| `CAN2` / `bus=2` | MCP2515 over SPI / SPI 外挂 MCP2515 | physical CANA |

## Vehicle Network Binding / 车辆网络绑定

| Firmware bus / 固件通道 | Effective vehicle network / 生效车辆网络 | X179 wiring / 接线 | Current function group / 当前功能组 | CAN IDs |
|---|---|---|---|---|
| `CAN1` / TWAI | CH CAN | X179 PIN `13 / 14` | All CAN1 functions / CAN1 所有功能 | Follow the CAN1 feature definitions |
| `CAN2` / MCP2515 | PT CAN | X179 PIN `2 / 3` | Nag-Killer torque targets / Nag-Killer 扭矩目标帧 | `0x052`, `0x370` |
| `CAN2` / MCP2515 | BODY CAN | X179 PIN `9 / 10` | Scroll, stalk, lighting, battery preheat, service mode / 滚轮、拨杆、灯光、电池预热、维修模式 | `0x3C2`, `0x229`, `0x249`, `0x273`, `0x082`, `0x339` |

## Practical Rule / 实际规则

- Bind CAN IDs to the vehicle network first, then decide which development-board
  CAN controller is wired there.
- 先按 CAN ID / 功能确认车辆网络，再决定开发板哪一路 CAN 接到这组针脚。
- `CAN2` / MCP2515 is only one physical CAN channel. If it is connected to PT CAN
  at X179 PIN `2 / 3`, only the PT CAN functions such as `0x052` / `0x370` can
  take effect on that wiring.
- `CAN2` / MCP2515 只有一路物理 CAN。如果它接到 X179 PIN `2 / 3` 的 PT CAN，
  那这次接线下只有 `0x052` / `0x370` 这类 PT CAN 功能会生效。
- If `CAN2` / MCP2515 is connected to BODY CAN at X179 PIN `9 / 10`, BODY CAN
  functions such as `0x3C2`, `0x229`, `0x249`, `0x273`, `0x082`, and `0x339`
  can take effect, while PT CAN torque targets will not.
- 如果 `CAN2` / MCP2515 接到 X179 PIN `9 / 10` 的 BODY CAN，则 `0x3C2`、`0x229`、
  `0x249`、`0x273`、`0x082`、`0x339` 这类 BODY CAN 功能会生效，而 PT CAN 扭矩目标帧不会生效。

## Quick Check / 快速检查

When a feature does not work, first check whether the CAN ID is on the same
vehicle network as the current wiring:

功能无效时，先检查该 CAN ID 是否和当前接线处于同一个车辆网络：

| Symptom / 现象 | First check / 优先检查 |
|---|---|
| `0x052` / `0x370` torque does not work / 扭矩不生效 | Is CAN2 wired to PT CAN X179 PIN `2 / 3`? / CAN2 是否接到 PT CAN X179 PIN `2 / 3` |
| `0x082` preheat does not work / 电池预热不生效 | Is CAN2 wired to BODY CAN X179 PIN `9 / 10`? / CAN2 是否接到 BODY CAN X179 PIN `9 / 10` |
| `0x3C2` scroll or hazard does not work / 滚轮或双闪不生效 | Is CAN2 wired to BODY CAN X179 PIN `9 / 10`? / CAN2 是否接到 BODY CAN X179 PIN `9 / 10` |
| CAN1 function does not work / CAN1 功能不生效 | Is CAN1 wired to CH CAN X179 PIN `13 / 14`? / CAN1 是否接到 CH CAN X179 PIN `13 / 14` |
