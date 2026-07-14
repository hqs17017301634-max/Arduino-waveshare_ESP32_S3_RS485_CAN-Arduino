# HW3/4-NAG

LILYGO T-2CAN 双 CAN 控制固件，支持 Tesla V12/V13（HW3逻辑）与 V14（HW4逻辑）运行时切换，并通过本地 WebUI 配置功能和查看诊断信息。

> 本项目仅用于开发、研究与封闭环境测试。CAN 注入会改变车辆行为，使用者必须自行确认硬件接线、车辆固件兼容性与安全边界。

## English

LILYGO T-2CAN dual-CAN firmware for ESP32-S3. It supports runtime switching between Tesla V12/V13 (HW3 logic) and V14 (HW4 logic), with local WebUI configuration, automatic NVS saving, and diagnostic pages.

> This project is intended only for development, research, and closed-environment testing. CAN injection can change vehicle behavior. Users are responsible for wiring, vehicle firmware compatibility, legal compliance, and safety validation.

### Hardware

- MCU: ESP32-S3, 240MHz
- Flash: 16MB
- PSRAM: OPI PSRAM
- CAN1: ESP32-S3 native TWAI
- CAN2: MCP2515 with 16MHz crystal
- WebUI: ESP32-S3 WiFi SoftAP

### CAN Mapping

| Firmware bus | Physical channel | Interface | Pins | Main use |
|---|---|---|---|---|
| `bus=1` | physical CANB | TWAI | TX GPIO7 / RX GPIO6 | CH CAN: FSD activation, driving profile, speed offset, DAS status |
| `bus=2` | physical CANA | MCP2515 | SCK12 / MOSI11 / MISO13 / CS10 / INT8 / RST9 | BODY CAN: scroll wheel, stalk, lighting, service mode, battery preheat |

Common vehicle-side wiring assumption:

```text
CH CAN   -> X179 PIN 13 / 14
BODY CAN -> X179 PIN 9 / 10
```

Always verify CAN-H/CAN-L on the actual vehicle harness before wiring.

### WebUI

The device creates a local SoftAP:

```text
SSID: T2CAN-FSD
Password: 12345678
URL: http://100.100.1.1/
```

All WebUI configuration changes are automatically saved to NVS. The UI includes light/dark themes, the main control page, diagnostics, and restart controls.

### Main Features

FSD profile selection:

```text
V12/V13 / HW3
V14 / HW4
```

V12/V13 activation:

```text
0x3FD mux0 bit46 = 1
Driving profile uses the V12/V13 field layout
```

V14 activation:

```text
0x3FD mux0 bit46 = 1
0x3FD mux0 bit60 = 1
Driving profile uses 0x3FD mux2 data[7] bits4..6
```

FSD activation resend:

- Caches and resends only `0x3FD mux0`.
- Default period: `20ms`.
- WebUI range: `1..1000ms`.
- Does not resend mux1, mux2, or speed frames.

Enhanced Autopilot / Smart Summon switch:

```text
V12/V13: 0x3FD mux1 bit19 = 0
V14:     0x3FD mux1 bit19 = 0, bit47 = 1
```

This switch is independent from FSD mux0 activation, speed offset, and driving profile control.

Do Not Disturb:

- Uses `0x399 DAS_autopilotHandsOnState` to trigger scroll-wheel fallback.
- Trigger states: `3..6` and `9..10`.
- One action per trigger cycle.
- Repeat interval: `0.5s`.
- Stops after hands-on state returns to `1` or `2`.

Model-specific DND behavior:

```text
V12/V13: scroll fallback + 0x3FD mux1 bit43 = 0
V14:     scroll fallback only; bit43 is not changed
```

HW4 driving profile:

```text
0 Chill
1 Normal
2 Hurry
3 Max
4 Sloth
```

Follow-distance mapping:

```text
1 -> Max
2 -> Hurry
3 -> Normal
4 -> Chill
5 -> Sloth
```

HW3 speed offset:

- Seven-segment target speed table.
- Low-speed maximum offset.
- Downward slew limiting, default `2%/s`.
- Visible and active only in V12/V13 mode.

HW4 speed offset:

1. Fixed plugin mode: `raw 21%`
2. Fixed maximum offset: `raw 60%`
3. Custom seven-segment target speed table

Custom HW4 offset calculation:

```text
offset percentage = ceil((target speed - fused speed limit) * 100 / fused speed limit)
```

The result is clamped to `0..63%`.

HW4 ISA chime suppression:

```text
CAN ID: 0x399
data[1] |= 0x20
Recalculate data[7] vehicle checksum
```

BODY CAN features:

- Flash-to-pass / high-beam strobe
- Rear fog brake strobe
- Reverse hazard and rear fog action
- Service mode `0x339`
- Battery preheat `0x082`

Battery preheat diagnostics include SOC, charging state, temperatures, timeout protection, original vehicle feedback, command power, and target temperature display.

### CAN TX Control

- `Enable CAN communication`: master CAN TX gate.
- CAN1 receive-only: disables TWAI TX.
- CAN2 enable: controls MCP2515.
- CAN2 filter modes:
  - Capture/debug
  - Current feature-related IDs

CAN2 TX uses a lightweight scheduler for lighting, scroll-wheel, service mode, battery preheat, and related sources.

### Diagnostics

The WebUI diagnostics include:

- CAN1/CAN2 RX, TX, and failure counters
- TWAI state, queue state, and bus-off recovery
- MCP2515 errors and filter state
- CPU0/CPU1 load estimation
- Heap and PSRAM status
- FSD state, fused speed limit, target speed, and offset
- HW4 follow distance, driving profile, target/current offset percentage
- DND trigger state
- Battery preheat and BMS temperature diagnostics
- FSD automatic-shift condition chain

### Build

Requirements:

- PlatformIO Core
- Espressif32 Arduino framework
- `autowp-mcp2515`

Standard build:

```powershell
pio run -e lilygo_t2can_arduino_webui
```

Isolated build:

```powershell
$env:PLATFORMIO_CORE_DIR="$PWD\.pio-core"
pio run -e lilygo_t2can_arduino_webui
```

`platformio.ini` is pinned to pioarduino `54.03.21`, using ESP32 toolchain GCC `14.2.0`.

Upload:

```powershell
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COM23
```

Important flashing parameters:

```text
Flash mode: DIO
Flash frequency: 80MHz
Flash size: 16MB
```

Do not force QIO flashing. A wrong QIO image header may cause ESP32-S3 boot loops around `ets_loader.c 78`.

### Project Files

```text
ESP32S3CAN-FSD/
  ESP32S3CAN-FSD.ino
  web_ui_page.h
huge_app.csv
platformio.ini
README.md
```

## 中文

## 硬件

- 主控：ESP32-S3，240MHz
- Flash：16MB
- PSRAM：OPI PSRAM
- CAN1：ESP32-S3 原生 TWAI
- CAN2：MCP2515，16MHz晶振
- WebUI：ESP32-S3 WiFi SoftAP

### CAN通道

| 固件总线 | 物理通道 | 接口 | 引脚 | 主要用途 |
|---|---|---|---|---|
| bus=1 | 物理 CANB | TWAI | TX GPIO7 / RX GPIO6 | CH CAN：FSD激活、驾驶模式、速度偏移、DAS状态 |
| bus=2 | 物理 CANA | MCP2515 | SCK12 / MOSI11 / MISO13 / CS10 / INT8 / RST9 | BODY CAN：滚轮、拨杆、灯光、维修模式、电池预热 |

项目常用车辆接线理解：

```text
CH CAN   -> X179 PIN 13 / 14
BODY CAN -> X179 PIN 9 / 10
```

接线前必须根据车辆型号和线束实测确认 CAN-H/CAN-L。

## WebUI

设备启动后创建：

```text
SSID：T2CAN-FSD
密码：12345678
地址：http://100.100.1.1/
```

所有 WebUI 配置修改会自动保存到 NVS。页面提供日间/夜间主题、主功能页、诊断页和重启按钮。

## 主要功能

### FSD车型选择

WebUI可选择：

```text
V12/V13 / HW3
V14 / HW4
```

V12/V13激活：

```text
0x3FD mux0 bit46 = 1
驾驶模式使用V12/V13字段
```

V14激活：

```text
0x3FD mux0 bit46 = 1
0x3FD mux0 bit60 = 1
驾驶模式使用0x3FD mux2 data[7] bits4..6
```

### FSD激活帧补发

- 只缓存和补发 `0x3FD mux0`
- 默认周期：20ms
- WebUI范围：1..1000ms
- 不补发 mux1 或 mux2 速度帧

### 增强Autopilot / Smart Summon

独立开关，默认关闭：

```text
V12/V13：0x3FD mux1 bit19 = 0
V14：    0x3FD mux1 bit19 = 0，bit47 = 1
```

该开关与 FSD mux0 激活、速度偏移和驾驶模式相互独立。

### 免打扰

免打扰使用 `0x399 DAS_autopilotHandsOnState` 联动滚轮兜底：

```text
触发状态：3..6、9..10
每次动作：1次
触发间隔：0.5秒
状态回到1或2后停止
```

车型差异：

```text
V12/V13：滚轮兜底 + 0x3FD mux1 bit43 = 0
V14：    只做滚轮兜底，不修改bit43
```

### HW4驾驶模式

V14支持5种模式：

```text
0 Chill
1 Normal
2 Hurry
3 Max
4 Sloth
```

跟车距离映射：

```text
1 -> Max
2 -> Hurry
3 -> Normal
4 -> Chill
5 -> Sloth
```

WebUI可手动选择。手动值保持到下一次真实跟车距离变化。

### HW3速度偏移

- 七段目标速度表
- 低速最大偏移
- 下降缓降，默认2%/秒
- 仅在 V12/V13 模式显示和生效

### HW4速度偏移

三种模式互斥：

1. 固定插件模式：`raw 21%`
2. 固定最大偏移：`raw 60%`
3. 自定义七段目标速度

自定义模式根据融合限速计算：

```text
偏移百分比 = ceil((目标速度 - 融合限速) × 100 / 融合限速)
```

结果限制为 `0..63%`。

HW4缓降：

```text
默认：2%/秒
偏移升高：立即生效
偏移降低：按设定速度下降
0%/秒：立即变化
```

没有有效融合限速时停止自定义覆盖，并缓降回原车值。

### HW4 ISA提示音抑制

```text
CAN ID：0x399
data[1] |= 0x20
重算data[7] vehicle checksum
```

### BODY CAN功能

- 高光/超车灯爆闪
- 后雾灯刹车爆闪
- 倒挡双闪与后雾灯动作
- 维修模式 `0x339`
- 电池预热 `0x082`

电池预热包含SOC、充电状态、温度和运行超时保护，并在诊断页显示原车反馈。

## CAN发送控制

- `开启CAN通讯`：CAN总发送门控
- CAN1只收不发：关闭TWAI发送
- CAN2启用：控制MCP2515
- CAN2过滤：
  - 抓包调试
  - 当前功能相关ID

CAN2发送使用轻量调度器，区分灯光、滚轮、维修模式、电池预热等来源。

## 诊断

WebUI诊断包括：

- CAN1/CAN2 RX、TX、失败计数
- TWAI状态、队列、Bus-Off恢复
- MCP2515错误和过滤状态
- CPU0/CPU1负载估算
- Heap与PSRAM状态
- FSD状态、融合限速、目标速度和偏移
- HW4跟车距离、驾驶模式、目标/当前偏移百分比
- DND触发状态
- 电池预热与BMS温度
- FSD自动换挡条件链

## 构建

需要：

- PlatformIO Core
- Espressif32 Arduino框架
- `autowp-mcp2515`

标准构建：

```powershell
pio run -e lilygo_t2can_arduino_webui
```

隔离构建（避免与其他 PlatformIO 项目共用工具链）：

```powershell
$env:PLATFORMIO_CORE_DIR="$PWD\.pio-core"
pio run -e lilygo_t2can_arduino_webui
```

`platformio.ini` 固定使用 pioarduino `54.03.21`，对应 ESP32 工具链 GCC `14.2.0`。

构建输出：

```text
.pio/build/lilygo_t2can_arduino_webui/bootloader.bin
.pio/build/lilygo_t2can_arduino_webui/partitions.bin
.pio/build/lilygo_t2can_arduino_webui/firmware.bin
```

## 下载

PlatformIO：

```powershell
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COM23
```

重要参数：

```text
Flash mode：DIO
Flash frequency：80MHz
Flash size：16MB
```

禁止手动强制使用 QIO 烧录。错误的 QIO 镜像头会导致 ESP32-S3 在 `ets_loader.c 78` 处循环重启。

## 分区

使用 `huge_app.csv`：

```text
NVS       0x9000
OTA data  0xE000
APP       0x10000 / 3MB
SPIFFS    0x310000 / 896KB
Core dump 0x3F0000 / 64KB
```

## 项目文件

```text
ESP32S3CAN-FSD/
  ESP32S3CAN-FSD.ino
  web_ui_page.h
huge_app.csv
platformio.ini
README.md
```

## 许可与责任

本项目不隶属于任何汽车制造商。功能测试必须在符合当地法律法规、车辆安全要求和封闭测试条件下进行。任何辅助驾驶功能都不能替代驾驶员持续观察道路并随时接管车辆。
