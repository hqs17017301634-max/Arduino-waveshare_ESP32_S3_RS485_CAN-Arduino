# DAS_status / 0x399 表格版分析

# DAS_status / 0x399 Table Analysis

## 1. 报文总览 / Frame Overview

| 项目 | Value | 中文说明 | English |
|---|---|---|---|
| CAN ID | `0x399` | DAS 状态帧 | DAS status frame |
| Decimal ID | `921` | 十进制 ID | Decimal CAN ID |
| Frame name | `DAS_status` | Autopilot/DAS 状态广播 | Autopilot/DAS status broadcast |
| Dataset | `model3_mcu3` | 本地 Explorer 数据源 | Local Explorer dataset |
| Firmware dataset | `2026.2` | 数据来源固件版本 | Source firmware version |
| MCU / SOC | `MCU3 / AMD` | Model 3 MCU3 / AMD 数据 | Model 3 MCU3 / AMD data |
| Explorer bus | `ETH`, bus id `3` | Explorer 中显示为 ETH 总线 | Shown as ETH bus in Explorer |
| Signal count | `26` | 共解析 26 个字段 | 26 decoded signals |
| Enumerated signals | `17` | 17 个字段有状态枚举表 | 17 signals have enum maps |
| Current firmware use | `DAS_fusedSpeedLimit` | 当前固件主要只用融合限速 | Current firmware mainly uses fused speed limit |

## 2. 26 个信号总表 / All 26 Signals

| # | Signal | 中文功能 | English Function | 有枚举 | 当前项目用途 |
|---:|---|---|---|---|---|
| 0 | `DAS_autoLaneChangeState` | 自动变道状态 | Auto lane-change state | Yes | 诊断 |
| 1 | `DAS_autoParked` | 自动泊车已泊入/完成类标志 | Autopark parked/completed flag | No | 暂不用 |
| 2 | `DAS_autoparkActive` | 自动泊车激活标志 | Autopark active flag | No | 暂不用 |
| 3 | `DAS_autoparkReady` | 自动泊车是否可用 | Autopark readiness | Yes | 暂不用 |
| 4 | `DAS_autopilotHandsOnState` | 手扶方向盘提示/升级状态 | Hands-on-wheel request/escalation state | Yes | 诊断 |
| 5 | `DAS_autopilotState` | AP/FSD 主状态 | Main Autopilot/FSD state | Yes | 诊断，适合 WebUI 显示 |
| 6 | `DAS_blindSpotRearLeft` | 左后盲区预警 | Left rear blind-spot warning | Yes | 暂不用 |
| 7 | `DAS_blindSpotRearRight` | 右后盲区预警 | Right rear blind-spot warning | Yes | 暂不用 |
| 8 | `DAS_fleetSpeedState` | 车队/云端速度状态 | Fleet speed state | Yes | 诊断 |
| 9 | `DAS_forwardCollisionWarning` | 前向碰撞预警 | Forward collision warning | Yes | 暂不用 |
| 10 | `DAS_fusedSpeedLimit` | 融合限速 | Fused speed limit | Yes | 核心：当前限速偏移依据 |
| 11 | `DAS_heaterState` | DAS/摄像头/系统加热状态 | DAS/camera/system heater state | Yes | 暂不用 |
| 12 | `DAS_laneDepartureWarning` | 车道偏离预警 | Lane departure warning | Yes | 暂不用 |
| 13 | `DAS_lssState` | 车道安全系统状态 | Lane Safety System state | Yes | 诊断 |
| 14 | `DAS_sideCollisionAvoid` | 侧向碰撞规避状态 | Side collision avoidance state | Yes | 暂不用 |
| 15 | `DAS_sideCollisionInhibit` | 侧向碰撞抑制状态 | Side collision inhibit state | Yes | 暂不用 |
| 16 | `DAS_sideCollisionWarning` | 侧向碰撞预警 | Side collision warning | Yes | 暂不用 |
| 17 | `DAS_statusChecksum` | `0x399` 校验 | `DAS_status` checksum | No | 只监听不改写，无需处理 |
| 18 | `DAS_statusCounter` | `0x399` 滚动计数器 | `DAS_status` rolling counter | No | 只监听不改写，无需处理 |
| 19 | `DAS_summonAvailable` | 召唤功能可用标志 | Summon availability flag | No | 暂不用 |
| 20 | `DAS_summonClearedGate` | 召唤门控/路径条件标志 | Summon cleared-gate condition | No | 暂不用 |
| 21 | `DAS_summonFwdLeashReached` | 召唤前进边界到达标志 | Summon forward leash reached | No | 暂不用 |
| 22 | `DAS_summonObstacle` | 召唤障碍物标志 | Summon obstacle flag | No | 暂不用 |
| 23 | `DAS_summonRvsLeashReached` | 召唤倒车边界到达标志 | Summon reverse leash reached | No | 暂不用 |
| 24 | `DAS_suppressSpeedWarning` | 抑制超速提示 | Suppress speed warning | Yes | 暂不用 |
| 25 | `DAS_visionOnlySpeedLimit` | 纯视觉限速 | Vision-only speed limit | Yes | 可做诊断，不建议直接替代融合限速 |

## 3. 当前固件最相关信号 / Firmware-Relevant Signals

| Signal | 当前固件是否使用 | 读取/处理方式 | 对 FSD/速度的意义 | English |
|---|---|---|---|---|
| `DAS_fusedSpeedLimit` | 使用 | `data[1] & 0x1F`，有效值 `raw * 5 kph` | 计算 PCT4 速度偏移的核心输入 | Main input for PCT4 speed-offset calculation |
| `DAS_visionOnlySpeedLimit` | 未使用 | Explorer 显示同样有 `0/31` 特殊值 | 可做诊断；暂不建议作为 fallback | Useful for diagnostics; not recommended as direct fallback yet |
| `DAS_autopilotState` | 未用于控制 | 可观察 `ACTIVE_FSD = 6` | 适合 WebUI 诊断 FSD 状态，不负责激活 | Good diagnostic signal for FSD state, not activation control |
| `DAS_statusChecksum` | 不处理 | 当前只监听 `0x399`，不改写 | 不需要重算；如果未来改写 `0x399` 才必须处理 | No need unless firmware rewrites `0x399` |
| `DAS_statusCounter` | 不处理 | 当前只监听 `0x399`，不改写 | 不需要维护；如果未来改写 `0x399` 才必须处理 | No need unless firmware rewrites `0x399` |

## 4. `DAS_fusedSpeedLimit` 限速换算表 / Fused Speed Limit Conversion

| Raw | KPH | 状态 | English |
|---:|---:|---|---|
| `0` | invalid | `UNKNOWN_SNA`，无有效融合限速 | Unknown / SNA |
| `1` | 5 | 有效限速 | Valid speed limit |
| `2` | 10 | 有效限速 | Valid speed limit |
| `3` | 15 | 有效限速 | Valid speed limit |
| `4` | 20 | 有效限速 | Valid speed limit |
| `5` | 25 | 有效限速 | Valid speed limit |
| `6` | 30 | 有效限速 | Valid speed limit |
| `7` | 35 | 有效限速 | Valid speed limit |
| `8` | 40 | 有效限速 | Valid speed limit |
| `9` | 45 | 有效限速 | Valid speed limit |
| `10` | 50 | 有效限速 | Valid speed limit |
| `11` | 55 | 有效限速 | Valid speed limit |
| `12` | 60 | 有效限速 | Valid speed limit |
| `13` | 65 | 有效限速 | Valid speed limit |
| `14` | 70 | 有效限速 | Valid speed limit |
| `15` | 75 | 有效限速 | Valid speed limit |
| `16` | 80 | 有效限速 | Valid speed limit |
| `17` | 85 | 有效限速 | Valid speed limit |
| `18` | 90 | 有效限速 | Valid speed limit |
| `19` | 95 | 有效限速 | Valid speed limit |
| `20` | 100 | 有效限速 | Valid speed limit |
| `21` | 105 | 有效限速 | Valid speed limit |
| `22` | 110 | 有效限速 | Valid speed limit |
| `23` | 115 | 有效限速 | Valid speed limit |
| `24` | 120 | 有效限速 | Valid speed limit |
| `25` | 125 | 有效限速 | Valid speed limit |
| `26` | 130 | 有效限速 | Valid speed limit |
| `27` | 135 | 有效限速 | Valid speed limit |
| `28` | 140 | 有效限速 | Valid speed limit |
| `29` | 145 | 有效限速 | Valid speed limit |
| `30` | 150 | 有效限速 | Valid speed limit |
| `31` | invalid | `NONE`，无可用限速 | No speed limit / none |

## 5. 当前速度控制表 / Current Speed Target Table

| 融合限速 / Fused limit | 当前目标/行为 | English |
|---:|---|---|
| `< 50 kph` | 直接写 PCT4 raw `200`，等于 50% | Direct PCT4 raw `200`, equal to 50% |
| `50..59 kph` | 目标 `60 kph` | Target `60 kph` |
| `60..69 kph` | 目标 `80 kph` | Target `80 kph` |
| `70..79 kph` | 目标 `85 kph` | Target `85 kph` |
| `80..89 kph` | 目标 `90 kph` | Target `90 kph` |
| `90..99 kph` | 目标 `100 kph` | Target `100 kph` |
| `100..119 kph` | 目标 `120 kph` | Target `120 kph` |
| `120..139 kph` | 目标 `140 kph` | Target `140 kph` |
| `>= 140 kph` | 不增加偏移 | No extra offset |
| `raw 0 / raw 31` | 保留原车 stock offset | Keep stock offset |

## 6. PCT4 编码表 / PCT4 Encoding

| 项目 | 公式/数值 | 中文说明 | English |
|---|---|---|---|
| 偏移速度 | `offsetKph = targetSpeedKph - fusedSpeedLimitKph` | 目标速度减融合限速 | Target speed minus fused limit |
| 百分比 | `offsetPct = round(offsetKph / fusedSpeedLimitKph * 100)` | 偏移占限速百分比 | Offset as percentage of fused limit |
| 线编码 | `raw = offsetPct * 4` | PCT4：1% = 4 raw | PCT4: 1% = 4 raw |
| 最大百分比 | `50%` | 当前上限 | Current cap |
| 最大 raw | `200` | `50 * 4` | `50 * 4` |
| 缓降 | `5%/s` | 下降方向限速，避免突然降速 | Downward slew limit to reduce abrupt slowdown |

## 7. 关键枚举表 / Key Enum Tables

### 7.1 `DAS_autopilotState`

| Raw | Label | 中文含义 | 当前建议 |
|---:|---|---|---|
| 0 | `DISABLED` | AP/FSD 关闭 | 诊断 |
| 1 | `UNAVAILABLE` | 不可用 | 诊断 |
| 2 | `AVAILABLE` | 可用 | 诊断 |
| 3 | `ACTIVE_NOMINAL` | AP 正常激活 | 诊断 |
| 4 | `ACTIVE_RESTRICTED` | AP 受限激活 | 诊断 |
| 5 | `ACTIVE_NAV` | 导航辅助激活 | 诊断 |
| 6 | `ACTIVE_FSD` | FSD 激活 | 很适合 WebUI 显示 |
| 8 | `ABORTING` | 正在退出 | 诊断 |
| 9 | `ABORTED` | 已退出 | 诊断 |
| 14 | `FAULT` | 故障 | 诊断 |
| 15 | `SNA` | 无效/不可用 | 诊断 |

### 7.2 `DAS_autoLaneChangeState`

| Raw | Label | 中文含义 |
|---:|---|---|
| 0 | `UNAVAILABLE_DISABLED` | 自动变道禁用/不可用 |
| 1 | `UNAVAILABLE_NO_LANES` | 没有车道线，不可用 |
| 5 | `UNAVAILABLE_VEHICLE_SPEED` | 车速条件不满足 |
| 6 | `AVAILABLE_ONLY_L` | 仅左侧可变道 |
| 7 | `AVAILABLE_ONLY_R` | 仅右侧可变道 |
| 8 | `AVAILABLE_BOTH` | 两侧都可变道 |
| 9 | `IN_PROGRESS_L` | 正在向左变道 |
| 10 | `IN_PROGRESS_R` | 正在向右变道 |
| 28 | `WAITING_HANDS_ON` | 等待手扶方向盘 |
| 29 | `ABORT_TIMEOUT` | 超时取消 |
| 30 | `ABORT_MISSION_PLAN_INVALID` | 路径/任务规划无效取消 |
| 31 | `SNA` | 无效/不可用 |

### 7.3 `DAS_autopilotHandsOnState`

| Raw | Label | 中文含义 |
|---:|---|---|
| 0 | `NOT_REQD` | 不需要手扶 |
| 1 | `REQD_DETECTED` | 要求手扶，已检测到 |
| 2 | `REQD_NOT_DETECTED` | 要求手扶，未检测到 |
| 3 | `REQD_VISUAL` | 视觉提示 |
| 4 | `REQD_CHIME_1` | 一级声音提示 |
| 5 | `REQD_CHIME_2` | 二级声音提示 |
| 6 | `REQD_SLOWING` | 因未接管开始减速 |
| 7 | `REQD_STRUCK_OUT` | 驾驶辅助受限/被禁用类状态 |
| 8 | `SUSPENDED` | 暂停 |
| 9 | `REQD_ESCALATED_CHIME_1` | 升级一级声音提示 |
| 10 | `REQD_ESCALATED_CHIME_2` | 升级二级声音提示 |
| 15 | `SNA` | 无效/不可用 |

### 7.4 常见安全/预警枚举 / Common Safety Warning Enums

| Signal | Raw / Label | 中文含义 | English |
|---|---|---|---|
| `DAS_blindSpotRearLeft` | `0 NO_WARNING` / `1 WARNING_LEVEL_1` / `2 WARNING_LEVEL_2` / `3 SNA` | 左后盲区预警等级 | Left rear blind-spot level |
| `DAS_blindSpotRearRight` | `0 NO_WARNING` / `1 WARNING_LEVEL_1` / `2 WARNING_LEVEL_2` / `3 SNA` | 右后盲区预警等级 | Right rear blind-spot level |
| `DAS_forwardCollisionWarning` | `0 NONE` / `1 FORWARD_COLLISION_WARNING` / `3 SNA` | 前向碰撞预警 | Forward collision warning |
| `DAS_laneDepartureWarning` | `0 NONE` / `1 LEFT_WARNING` / `2 RIGHT_WARNING` / `3 LEFT_WARNING_SEVERE` / `4 RIGHT_WARNING_SEVERE` / `5 SNA` | 车道偏离预警 | Lane departure warning |
| `DAS_sideCollisionAvoid` | `0 NONE` / `1 AVOID_LEFT` / `2 AVOID_RIGHT` / `3 SNA` | 侧向碰撞规避 | Side collision avoidance |
| `DAS_sideCollisionInhibit` | `0 NO_INHIBIT` / `1 INHIBIT` | 侧向碰撞功能抑制 | Side collision inhibit |
| `DAS_sideCollisionWarning` | `0 NONE` / `1 WARN_LEFT` / `2 WARN_RIGHT` / `3 WARN_LEFT_RIGHT` | 侧向碰撞预警 | Side collision warning |

### 7.5 其他枚举 / Other Enums

| Signal | Raw / Label | 中文含义 | English |
|---|---|---|---|
| `DAS_autoparkReady` | `0 UNAVAILABLE` / `1 READY` | 自动泊车不可用/可用 | Autopark unavailable/ready |
| `DAS_fleetSpeedState` | `0 UNAVAILABLE` / `1 AVAILABLE` / `2 ACTIVE` / `3 HOLD` | 车队速度状态 | Fleet speed state |
| `DAS_heaterState` | `0 OFF_SNA` / `1 ON` | 加热状态 | Heater state |
| `DAS_lssState` | `0 FAULT` / `1 LDW` / `2 LKA` / `3 ELK` / `4 MONITOR` / `5 BLINDSPOT` / `6 ABORT` / `7 OFF` | 车道安全系统状态 | Lane safety state |
| `DAS_suppressSpeedWarning` | `0 Do_Not_Suppress` / `1 Suppress_Speed_Warning` | 是否抑制超速提示 | Suppress speed warning |
| `DAS_visionOnlySpeedLimit` | `0 UNKNOWN_SNA` / `31 NONE`; `1..30` 可按 `raw * 5 kph` 理解 | 纯视觉限速 | Vision-only speed limit |

## 8. 与 FSD 激活和限速的关系 / Relationship To FSD And Speed

| 功能 | 使用的 CAN 帧 | 是否用 `0x399` | 说明 | English |
|---|---|---|---|---|
| FSD 激活 | `0x3FD / 1021 mux 0` | No | 设置 bit `46`，并写速度 profile | Set bit `46` and speed profile |
| 控制位处理 | `0x3FD / 1021 mux 1` | No | 清 bit `19` | Clear bit `19` |
| 速度偏移写入 | `0x3FD / 1021 mux 2` | Indirect | offset 由 `0x399` 融合限速计算 | Offset is calculated from `0x399` fused limit |
| 融合限速读取 | `0x399 DAS_status` | Yes | 读取 `DAS_fusedSpeedLimit` | Read `DAS_fusedSpeedLimit` |
| FSD 状态诊断 | `0x399 DAS_status` | Optional | 可看 `DAS_autopilotState = 6 ACTIVE_FSD` | Can show `ACTIVE_FSD = 6` |

## 9. 建议表 / Recommendations

| 类型 | 建议 | 原因 | English |
|---|---|---|---|
| 保留 | 继续用 `DAS_fusedSpeedLimit` 做限速偏移依据 | 和参考 HW3 代码一致，逻辑清楚 | Keep `DAS_fusedSpeedLimit` as speed-offset source |
| 保留 | raw `0/31` 继续判无效 | `0 = UNKNOWN_SNA`，`31 = NONE` | Keep treating `0/31` as invalid |
| 可做 | WebUI 显示 `DAS_autopilotState` | 可直观看 FSD 是否 `ACTIVE_FSD = 6` | Useful WebUI diagnostic |
| 可做 | WebUI 显示 `DAS_visionOnlySpeedLimit` | 用于判断融合限速无效时视觉限速是否存在 | Useful for future fallback validation |
| 谨慎 | 不要直接用视觉限速替代融合限速 | 未经实车抓包验证可能误判 | Do not directly replace fused limit with vision-only limit |
| 不建议 | 不改写 `0x399` | 改写需要 checksum/counter，风险高 | Do not rewrite `0x399` |
| 不建议 | 不在 CAN 快路径完整解析 26 个字段 | 会增加处理量，对当前 FSD/速度收益小 | Avoid heavy full-frame decode in fast CAN path |

## 10. 一句话结论 / Short Conclusion

| 中文 | English |
|---|---|
| `DAS_status / 0x399` 是只读状态帧，当前最有价值的是 `DAS_fusedSpeedLimit`，它负责给速度偏移算法提供融合限速；FSD 激活本身仍然由 `0x3FD / 1021` 完成。 | `DAS_status / 0x399` should be treated as a read-only status frame. Its most valuable runtime signal is `DAS_fusedSpeedLimit`, which feeds the speed-offset algorithm; actual FSD activation is still done through `0x3FD / 1021`. |
