// Light WebUI page served by /. Kept in a separate header so the PlatformIO
// .ino prototype generator never has to parse the JavaScript braces.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN FSD 设置</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;padding:12px;background:#111;color:#eee}
h1{font-size:18px;margin:0 0 12px}h2{font-size:15px;margin:0 0 8px;color:#8cf}h3{font-size:13px;margin:12px 0 4px;color:#6a9;border-top:1px solid #2a2a2a;padding-top:8px}
.card{background:#1c1c1c;border:1px solid #333;border-radius:8px;padding:10px 12px;margin-bottom:12px}
label{display:flex;justify-content:space-between;align-items:center;margin:6px 0;font-size:14px;gap:12px}
input[type=number]{width:90px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:3px}
input[type=text],select{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:6px;margin-top:4px}
.signed{display:flex;align-items:center;gap:6px;color:#eee}.signed b{min-width:10px;text-align:right}.signed em{font-style:normal;color:#aaa;font-size:12px}
button{background:#2a6;color:#fff;border:0;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0;font-size:14px}
button.alt,.linkbtn{background:#37c}button.warn{background:#a33}.linkbtn{display:none;color:#fff;text-decoration:none;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0}
.kv{display:flex;justify-content:space-between;font-size:13px;padding:3px 0;border-bottom:1px solid #262626;gap:12px}.kv span:last-child{color:#9f9;font-variant-numeric:tabular-nums;text-align:right}
.hint{font-size:12px;color:#aaa;margin:2px 0 6px;line-height:1.45}.result{font-size:12px;color:#ffd479;margin:8px 0 0;min-height:18px}.bar{position:sticky;top:0;z-index:5;background:#111;padding:6px 0 4px}
.pageSwitch{display:inline-flex;justify-content:flex-start;align-items:center;gap:8px;margin:6px 0 0;font-size:13px;color:#ddd}.hidden{display:none}
</style></head><body>
<h1>T-2CAN FSD 运行参数</h1>

<div class="card bar">
<button onclick="applyConfig()">应用到内存</button>
<button class="alt" onclick="saveConfig()">保存到 Flash</button>
<button class="warn" onclick="webOff()">关闭 WebUI</button>
<label class="pageSwitch">诊断信息页<input type="checkbox" id="diagPageSwitch" onchange="setDiagPage(this.checked)"></label>
<div class="result" id="testResult"></div>
</div>

<div id="mainPage" class="page">

<div class="card">
<h2>通道定义</h2>
<p class="hint">官方 LILYGO T-2CAN V1.0：物理 CANA = MCP2515/SPI；物理 CANB = ESP32-S3 原生 TWAI。当前固件 CSV：bus=1/TWAI/物理CANB，bus=2/MCP2515/物理CANA。</p>
<p class="hint">车机连接本热点后手动打开 http://100.100.1.1；固件只对 tesla.cn 存活检测做本地应答，不再把未知网页重定向成登录页。</p>
<div class="kv"><span>bus=1</span><span>TWAI / physical CANB / GPIO7,6</span></div>
<div class="kv"><span>bus=2</span><span>MCP2515 / physical CANA / SPI + INT8</span></div>
</div>

<div class="card">
<h2>FSD / 速度</h2>
<label>FSD 启用<input type="checkbox" id="fsdEnabled"></label>
<label>自动速度偏移<input type="checkbox" id="autoSpeedOffsetEnabled"></label>
<label>缓降百分比/秒<input type="number" id="slewPctPerSec" min="0" max="100"></label>
<label>低速最大 raw<input type="number" id="lowSpeedMaxPctRaw" min="0" max="255"></label>
<label>目标速度 &lt;60<input type="number" id="targetBelow60" min="0" max="255"></label>
<label>目标速度 60..69<input type="number" id="target60" min="0" max="255"></label>
<label>目标速度 70..79<input type="number" id="target70" min="0" max="255"></label>
<label>目标速度 80..89<input type="number" id="target80" min="0" max="255"></label>
<label>目标速度 90..99<input type="number" id="target90" min="0" max="255"></label>
<label>目标速度 100..119<input type="number" id="target100" min="0" max="255"></label>
<label>目标速度 120..139<input type="number" id="target120" min="0" max="255"></label>
</div>

<div class="card">
<h2>灯光 / 车身</h2>
<label>高光爆闪启用<input type="checkbox" id="highBeamStrobeEnabled"></label>
<p class="hint">bus=2/MCP2515/物理CANA，0x249，超车灯拨杆下拉两次触发 8 次，75ms ON / 75ms OFF，结束强制 idle。</p>
<label>后雾灯刹车爆闪启用<input type="checkbox" id="rearFogBrakeStrobeEnabled"></label>
<p class="hint">缓减速触发 3 次，急减速或车身刹车灯触发 6 次，输出使用 0x273 后雾灯位。</p>
<label>倒挡双闪雾灯启用<input type="checkbox" id="reverseStrobeEnabled"></label>
<p class="hint">R 档或 踩刹车+右滚轮后滚 触发 hazard + 后雾灯；右滚轮前滚视为 D 意图并取消。</p>
<label>强制自动前大灯<input type="checkbox" id="fsdForceHeadlightEnabled"></label>
<label>强制远光灯<input type="checkbox" id="fsdForceHighBeamEnabled"></label>
<p class="hint">仅在 FSD/AP active 时生效，走 bus=2/MCP2515/物理CANA 的 0x3E9 DAS_bodyControls；复制实时 0x3E9 后只改 headlightRequest / highLowBeamDecision。0x3F5 只做实际灯光反馈诊断。</p>
</div>

<div class="card">
<h2>免打扰</h2>
<label>免打扰总开关<input type="checkbox" id="dndEnabled"></label>
<label>音量免打扰<input type="checkbox" id="dndVolumeEnabled"></label>
<p class="hint">FSD/AP 激活后随机 1-5 秒自动音量加减恢复；每步 50ms。发送复用 bus=2/MCP2515/物理CANA 最新 0x3C2 mux1 滚轮帧，默认关闭。</p>
</div>

<div class="card">
<h2>Nag-Killer 扭矩</h2>
<label>扭矩免打扰总开关<input type="checkbox" id="nagKillerEnabled"></label>
<label>0x052 扭矩发送测试<input type="checkbox" id="nagKillerTest052Enabled"></label>
<label>0x370 扭矩发送测试<input type="checkbox" id="nagKillerTest370Enabled"></label>
<label>座舱摄像头关闭<input type="checkbox" id="cabinCameraDisableEnabled"></label>
<label>座舱摄像头关闭遥测<input type="checkbox" id="cabinCameraTelemetryDisableEnabled"></label>
<p class="hint">两个座舱摄像头开关走 bus=1/TWAI/物理CANB 的 0x3FD mux1；启用时只写对应 bit 为 0，未启用时不修改对应 bit。</p>
<label>方案<select id="nagKillerMode"><option value="1">Mode B：0x052 burst/pause</option><option value="2">Mode C：0x370 hands-on状态机</option><option value="3">Mode D：文档0x370状态机</option></select></label>
<label>Mode B 注入窗口 ms<input type="number" id="nagKillerBurstMs" min="50" max="10000"></label>
<label>Mode B 休息窗口 ms<input type="number" id="nagKillerPauseMs" min="0" max="10000"></label>
<h3>Mode B 扭矩 Nm</h3>
<label>第1正扭矩<span class="signed"><b>+</b><input type="number" id="nagKillerBPos1Nm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<label>第2正扭矩<span class="signed"><b>+</b><input type="number" id="nagKillerBPos2Nm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<label>第1负扭矩<span class="signed"><b>-</b><input type="number" id="nagKillerBNeg1Nm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<label>第2负扭矩<span class="signed"><b>-</b><input type="number" id="nagKillerBNeg2Nm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<h3>Mode C 扭矩 Nm</h3>
<label>负端<span class="signed"><b>-</b><input type="number" id="nagKillerCNegNm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<label>正端<span class="signed"><b>+</b><input type="number" id="nagKillerCPosNm" min="0" max="2.8" step="0.01"><em>Nm</em></span></label>
<p class="hint">默认关闭。T-2CAN 上 Nag-Killer 发送走 bus=2/MCP2515/物理CANA。Mode B 在 AP/FSD active 且 0x399 新鲜时，对 0x052 做 burst/pause 扭矩循环。Mode C 对 0x370：AP/FSD active 后前5秒按 Mode A 固定+1.80Nm/L1，之后进入原状态机。Mode D 对 0x370 做文档状态机：state1保持500ms，state2延迟2秒后0.5..2.0Nm随机，state3/4/5延迟1秒后ramp/hold到2.1Nm。任意模式识别到 0x399 hands-on state 2..6 时，会自动触发 3 次音量滚轮免打扰。测试开关打开后，收到对应 0x052/0x370 原车帧就直接发送当前设置扭矩，不等待 AP、hands-on、转角或 burst/rest 条件。所有扭矩框输入范围 0..2.8Nm，左侧固定符号自动生效。</p>
</div>

<div class="card">
<h2>滚轮换挡 / 预热 / MCP2515</h2>
<label>滚轮换挡启用<input type="checkbox" id="scrollGearInjectEnabled"></label>
<p class="hint">踩刹车 + 右滚轮，bus=2/MCP2515/物理CANA 发送 0x229，默认关闭。</p>
<label>电池预热启用<input type="checkbox" id="batteryPreheatEnabled"></label>
<p class="hint">bus=2/MCP2515/物理CANA 每 500ms 固定发送 0x082：BF 50 A8 80 FF 03 00 80；目标 42.0°C。平均温度到 42°C 连续10秒、最高温到45°C、SOC已知且低于5%、运行15分钟、开始充电或手动关闭时停止ON并补发3帧OFF。</p>
<label>锁车深度休眠<input type="checkbox" id="lockDeepSleepEnabled"></label>
<p class="hint">开启后，需车内无人，且 bus=2/MCP2515/物理CANA 的 0x339 VCSEC 简化锁状态=2 连续稳定 5 秒，才停止所有 CAN 发送并进入 ESP32 deep sleep；状态=1 解锁会重置计时。</p>
<label>bus=1/TWAI/物理CANB 只收不发<input type="checkbox" id="can1ReceiveOnly"></label>
<p class="hint">开启后只屏蔽 TWAI 发送；MCP2515/物理CANA 上的灯光、滚轮和 0x082 不受这个开关阻断。</p>
<h3>MCP2515 / 物理 CANA</h3>
<label>启用<input type="checkbox" id="canbEnabled"></label>
<label>维修模式 0x339<input type="checkbox" id="canbServiceModeEnabled"></label>
<label>硬件过滤模式<select id="canbFilterMode"><option value="0">抓包调试</option><option value="1">当前功能相关ID</option></select></label>
</div>

<div class="card">
<h2>CAN 抓包</h2>
<p class="hint">ID 留空=不做录制层过滤；bus=1/TWAI 仍只记录固件关注的 FSD/底盘 ID，bus=2/MCP2515 是否全量取决于上方硬件过滤模式。多个 ID 用逗号分隔，例如 229,082,273。CSV 会输出 controller 和 physical 两列。</p>
<input type="text" id="recIds" value="">
<div>
<button onclick="startRec()">开始抓包</button>
<button class="warn" onclick="stopRec()">停止</button>
<a class="linkbtn" id="recDownload" href="/rec_download" download="can_recording.csv">下载 CSV</a>
</div>
<div class="kv"><span>状态</span><span id="recState">-</span></div>
<div class="kv"><span>帧数</span><span id="recCount">-</span></div>
<div class="kv"><span>bus=1/TWAI/物理CANB</span><span id="recBus1">-</span></div>
<div class="kv"><span>bus=2/MCP2515/物理CANA</span><span id="recBus2">-</span></div>
<div class="kv"><span>丢帧/停止原因</span><span id="recDrop">-</span></div>
<div class="kv"><span>PSRAM 缓冲</span><span id="recPsram">-</span></div>
</div>

</div>

<div id="diagPage" class="page hidden">

<div class="card">
<h2>诊断信息 <label style="display:inline;font-size:13px">轮询<input type="checkbox" id="poll" onchange="setPolling(this.checked)"></label></h2>
<h3>自动诊断</h3>
<div class="kv"><span>系统健康</span><span id="autoDiagLevel">-</span></div>
<div class="kv"><span>诊断原因</span><span id="autoDiagReasons">-</span></div>
<div class="kv"><span>建议动作</span><span id="autoDiagAdvice">-</span></div>
<h3>CAN 总线</h3>
<h3>开发板诊断</h3>
<p class="hint">CPU%已扣除TWAI等待时间；loop最大耗时超过5ms、TX最大耗时超过2ms、TWAI missed/overrun新增、MCP2515 overflow新增，说明实时性需要重点看。</p>
<div class="kv"><span>窗口ms / CPU MHz</span><span><b id="diagWindowMs">-</b> / <b id="cpuMhz">-</b></span></div>
<div class="kv"><span>loop Hz / CPU%</span><span><b id="loopHz">-</b> / <b id="cpuPct">-</b></span></div>
<div class="kv"><span>loop平均/最大 us</span><span><b id="loopAvgUs">-</b> / <b id="loopMaxUs">-</b></span></div>
<div class="kv"><span>loop等待平均 / 周期最大 us</span><span><b id="loopWaitAvgUs">-</b> / <b id="loopPeriodMaxUs">-</b></span></div>
<div class="kv"><span>WebUI最大 us</span><span id="webTaskMaxUs">-</span></div>
<div class="kv"><span>内存可用% / 历史最低%</span><span><b id="freeHeapPct">-</b> / <b id="minFreeHeapPct">-</b></span></div>
<div class="kv"><span>heap总量/可用/历史最低</span><span><b id="totalHeapBytes">-</b> / <b id="freeHeapBytes">-</b> / <b id="minFreeHeapBytes">-</b></span></div>
<div class="kv"><span>bus=1 RX</span><span id="can1Rx">-</span></div>
<div class="kv"><span>bus=1 TX</span><span id="can1Tx">-</span></div>
<div class="kv"><span>bus=1 TX fail</span><span id="can1TxFail">-</span></div>
<div class="kv"><span>bus=1 RX/TX/fail 每秒</span><span><b id="can1RxRate">-</b> / <b id="can1TxRate">-</b> / <b id="can1TxFailRate">-</b></span></div>
<div class="kv"><span>bus=1 RX最大间隔 / TX最大 us</span><span><b id="can1RxGapMaxUs">-</b> / <b id="can1TxMaxUs">-</b></span></div>
<div class="kv"><span>bus=1 TX慢计数</span><span id="can1TxSlowCount">-</span></div>
<div class="kv"><span>TWAI state / bus-off</span><span><b id="twaiState">-</b> / <b id="twaiBusOffCount">-</b></span></div>
<div class="kv"><span>TWAI RX/TX队列 当前</span><span><b id="twaiRxQueue">-</b> / <b id="twaiTxQueue">-</b></span></div>
<div class="kv"><span>TWAI RX/TX队列 最大</span><span><b id="twaiRxQueueMax">-</b> / <b id="twaiTxQueueMax">-</b></span></div>
<div class="kv"><span>TWAI missed/overrun/busErr</span><span><b id="twaiRxMissed">-</b> / <b id="twaiRxOverrun">-</b> / <b id="twaiBusError">-</b></span></div>
<div class="kv"><span>TWAI txFailed / txErr/rxErr</span><span><b id="twaiTxFailed">-</b> / <b id="twaiTxErr">-</b> / <b id="twaiRxErr">-</b></span></div>
<div class="kv"><span>MCP2515 ready</span><span id="canbReady">-</span></div>
<div class="kv"><span>MCP2515 filter mode</span><span id="canbHardwareFilterMode">-</span></div>
<div class="kv"><span>MCP2515 RX / TX / fail</span><span><b id="canbRx">-</b> / <b id="canbTx">-</b> / <b id="canbTxFail">-</b></span></div>
<div class="kv"><span>MCP2515 RX/TX/fail 每秒</span><span><b id="canbRxRate">-</b> / <b id="canbTxRate">-</b> / <b id="canbTxFailRate">-</b></span></div>
<div class="kv"><span>MCP2515 RX最大间隔 / TX最大 us</span><span><b id="canbRxGapMaxUs">-</b> / <b id="canbTxMaxUs">-</b></span></div>
<div class="kv"><span>MCP2515 TX慢 / drain最大</span><span><b id="canbTxSlowCount">-</b> / <b id="canbDrainMaxUs">-</b></span></div>
<div class="kv"><span>MCP2515 drain最大帧数</span><span id="canbDrainMaxFrames">-</span></div>
<div class="kv"><span>MCP2515 last ID</span><span id="canbLastId">-</span></div>
<div class="kv"><span>MCP2515 EFLG / RX overflow</span><span><b id="canbErrorFlags">-</b> / <b id="canbRxOverflowCount">-</b></span></div>
<h3>速度 / FSD</h3>
<div class="kv"><span>融合限速 kph</span><span id="fusedLimitKph">-</span></div>
<div class="kv"><span>目标速度 kph</span><span id="targetSpeedKph">-</span></div>
<div class="kv"><span>速度偏移 kph/raw</span><span><b id="offsetKph">-</b> / <b id="offsetRaw">-</b></span></div>
<h3>灯光 / 预热</h3>
<div class="kv"><span>高光爆闪 / 剩余</span><span><b id="highBeamStrobeActive">-</b> / <b id="highBeamStrobeRemaining">-</b></span></div>
<div class="kv"><span>后雾灯爆闪 / 剩余</span><span><b id="rearFogBrakeStrobeActive">-</b> / <b id="rearFogBrakeStrobeRemaining">-</b></span></div>
<div class="kv"><span>倒挡爆闪 / 剩余</span><span><b id="reverseStrobeActive">-</b> / <b id="reverseStrobeRemaining">-</b></span></div>
<div class="kv"><span>FSD灯光强制 / 阻断</span><span><b id="fsdLightForceActive">-</b> / <b id="fsdLightForceBlocked">-</b></span></div>
<div class="kv"><span>FSD灯光TX / fail / 距今ms</span><span><b id="fsdLightForceTxCount">-</b> / <b id="fsdLightForceTxFail">-</b> / <b id="fsdLightForceLastTxAgeMs">-</b></span></div>
<div class="kv"><span>灯光条件 开关/CANB/0x399/AP/0x3E9</span><span><b id="fsdLightForceSwitchOn">-</b> / <b id="fsdLightForceCanbOk">-</b> / <b id="fsdLightForce399Fresh">-</b> / <b id="fsdLightForceApActive">-</b> / <b id="fsdLightForce3e9Fresh">-</b></span></div>
<div class="kv"><span>0x399距今 / AP state</span><span><b id="fsdLightForce399AgeMs">-</b> / <b id="fsdLightForceApState">-</b></span></div>
<div class="kv"><span>0x3E9收到/新鲜/DLC/有效</span><span><b id="fsdLightForce3e9Seen">-</b> / <b id="fsdLightForce3e9Fresh">-</b> / <b id="fsdLightForce3e9Dlc">-</b> / <b id="fsdLightForce3e9DlcOk">-</b></span></div>
<div class="kv"><span>0x3E9距今 / 大灯 / 远光 / counter</span><span><b id="fsdLightForceBodyAgeMs">-</b> / <b id="fsdLightForceHeadlightRequest">-</b> / <b id="fsdLightForceHighBeamDecision">-</b> / <b id="fsdLightForceBodyCounter">-</b></span></div>
<div class="kv"><span>0x3F5反馈/距今</span><span><b id="fsdLightForce3f5Seen">-</b> / <b id="fsdLightFeedbackAgeMs">-</b></span></div>
<div class="kv"><span>近光 左/右</span><span><b id="fsdLightLowBeamLeftStatus">-</b> / <b id="fsdLightLowBeamRightStatus">-</b></span></div>
<div class="kv"><span>远光 左/右</span><span><b id="fsdLightHighBeamLeftStatus">-</b> / <b id="fsdLightHighBeamRightStatus">-</b></span></div>
<h3>电池预热 / 温度</h3>
<div class="kv"><span>电池预热发送中</span><span id="batteryPreheatActive">-</span></div>
<div class="kv"><span>电池预热TX / 距今ms</span><span><b id="batteryPreheatTxCount">-</b> / <b id="batteryPreheatAgeMs">-</b></span></div>
<div class="kv"><span>预热运行 / 到温稳定ms</span><span><b id="batteryPreheatRunMs">-</b> / <b id="batteryPreheatStableTempMs">-</b></span></div>
<div class="kv"><span>自动关闭 / 原因 / 充电</span><span><b id="batteryPreheatAutoOffLatched">-</b> / <b id="batteryPreheatAutoOffReason">-</b> / <b id="batteryPreheatChargeDetected">-</b></span></div>
<div class="kv"><span>0x212充电状态 / bus / 距今ms</span><span><b id="batteryPreheatChargeStatus">-</b> / <b id="batteryPreheatChargeStatusBus">-</b> / <b id="batteryPreheatChargeStatusAgeMs">-</b></span></div>
<div class="kv"><span>电池SOC / 距今ms</span><span><b id="batteryPreheatSocPct">-</b> / <b id="batteryPreheatSocAgeMs">-</b></span></div>
<div class="kv"><span>0x082反馈 / bus / 距今ms</span><span><b id="batteryPreheatFeedbackSeen">-</b> / <b id="batteryPreheatFeedbackBus">-</b> / <b id="batteryPreheatFeedbackAgeMs">-</b></span></div>
<div class="kv"><span>预热状态 / 请求加热</span><span><b id="batteryPreheatUiState">-</b> / <b id="batteryPreheatUiRequestHeat">-</b></span></div>
<div class="kv"><span>导航快充 / 类型 / 行程</span><span><b id="batteryPreheatUiNavToSupercharger">-</b> / <b id="batteryPreheatUiFastChargerType">-</b> / <b id="batteryPreheatUiTripActive">-</b></span></div>
<div class="kv"><span>预热功率 / 目标温度</span><span><b id="batteryPreheatUiPowerW">-</b> / <b id="batteryPreheatUiTargetCx100">-</b></span></div>
<div class="kv"><span>环境温度 / 到达能量</span><span><b id="batteryPreheatUiAmbientCx100">-</b> / <b id="batteryPreheatUiEnergyAtDestination">-</b></span></div>
<div class="kv"><span>电池温度 最低/平均/最高</span><span><b id="bmsTempMinCx100">-</b> / <b id="bmsTempAvgCx100">-</b> / <b id="bmsTempMaxCx100">-</b></span></div>
<div class="kv"><span>0x332温度 最低/平均/最高</span><span><b id="batteryPreheatBms332MinCx100">-</b> / <b id="batteryPreheatBms332AvgCx100">-</b> / <b id="batteryPreheatBms332MaxCx100">-</b></span></div>
<div class="kv"><span>0x332收到/mux/距今ms</span><span><b id="batteryPreheatBms332Seen">-</b> / <b id="batteryPreheatBms332Mux">-</b> / <b id="batteryPreheatBms332AgeMs">-</b></span></div>
<div class="kv"><span>最新0x712 mux / T1/T2/T3</span><span><b id="bmsTempDecodedMux">-</b> / <b id="bmsTempLatest1Cx100">-</b> / <b id="bmsTempLatest2Cx100">-</b> / <b id="bmsTempLatest3Cx100">-</b></span></div>
<div class="kv"><span>温度点数 / 距今ms</span><span><b id="bmsTempDecodedCount">-</b> / <b id="bmsTempDecodedAgeMs">-</b></span></div>
<div class="kv"><span>温度帧ID / raw</span><span><b id="bmsTempFrameId">-</b> / <b id="bmsTempFramePayload">-</b></span></div>
<div class="kv"><span>0x321收到/bus/距今ms</span><span><b id="batteryPreheatVcfrontSeen">-</b> / <b id="batteryPreheatVcfrontBus">-</b> / <b id="batteryPreheatVcfrontAgeMs">-</b></span></div>
<div class="kv"><span>0x321电池冷却液 / 电驱冷却液</span><span><b id="batteryPreheatVcfrontCoolantBatInletCx100">-</b> / <b id="batteryPreheatVcfrontCoolantPtInletCx100">-</b></span></div>
<div class="kv"><span>0x321环境 / 过滤环境</span><span><b id="batteryPreheatVcfrontAmbientCx100">-</b> / <b id="batteryPreheatVcfrontAmbientFilteredCx100">-</b></span></div>
<div class="kv"><span>0x321冷却液液位</span><span id="batteryPreheatVcfrontCoolantLevel">-</span></div>
<div class="kv"><span>0x321 raw</span><span id="batteryPreheatVcfrontPayload">-</span></div>
<div class="kv"><span>0x332 raw</span><span id="batteryPreheatBms332Payload">-</span></div>
<div class="kv"><span>0x082 raw</span><span id="batteryPreheatFeedbackPayload">-</span></div>
<h3>免打扰</h3>
<div class="kv"><span>hands-on 0x399 / 警告</span><span><b id="dndHandsOnState">-</b> / <b id="dndWarningActive">-</b></span></div>
<div class="kv"><span>动作 / 类型 / 阻止</span><span><b id="dndActionActive">-</b> / <b id="dndActionType">-</b> / <b id="dndBlocked">-</b></span></div>
<div class="kv"><span>DND TX / 触发距今ms</span><span><b id="dndTxCount">-</b> / <b id="dndLastTriggerAgeMs">-</b></span></div>
<div class="kv"><span>0x3C2滚轮缓存距今ms</span><span id="dndScrollCacheAgeMs">-</span></div>
<h3>Nag-Killer 扭矩</h3>
<div class="kv"><span>模式 / 目标ID / 阻止</span><span><b id="nagKillerModeText">-</b> / <b id="nagKillerTargetId">-</b> / <b id="nagKillerBlocked">-</b></span></div>
<div class="kv"><span>激活 / burst / setHo</span><span><b id="nagKillerActive">-</b> / <b id="nagKillerBurstActive">-</b> / <b id="nagKillerSetHandsOn">-</b></span></div>
<div class="kv"><span>RX / TX / fail</span><span><b id="nagKillerRxCount">-</b> / <b id="nagKillerTxCount">-</b> / <b id="nagKillerTxFail">-</b></span></div>
<div class="kv"><span>RX距今 / TX距今ms</span><span><b id="nagKillerLastRxAgeMs">-</b> / <b id="nagKillerLastTxAgeMs">-</b></span></div>
<div class="kv"><span>AP / hands-on / 目标Ho</span><span><b id="nagKillerApState">-</b> / <b id="nagKillerHandsOnState">-</b> / <b id="nagKillerTargetHandsOn">-</b></span></div>
<div class="kv"><span>实车扭矩 / 注入扭矩</span><span><b id="nagKillerRealTorqueCx100">-</b> / <b id="nagKillerLastTorqueCx100">-</b></span></div>
<div class="kv"><span>方向盘角度 / AP年龄 / 转角年龄</span><span><b id="nagKillerSteeringDegCx10">-</b> / <b id="nagKillerApAgeMs">-</b> / <b id="nagKillerSteeringAgeMs">-</b></span></div>
<div class="kv"><span>NAG滚轮剩余 / 次数 / 距今ms</span><span><b id="nagKillerDndRemaining">-</b> / <b id="nagKillerDndTriggerCount">-</b> / <b id="nagKillerDndLastTriggerAgeMs">-</b></span></div>
<div class="kv"><span>锁车休眠 / 来源 / 距今ms</span><span><b id="lockSleepTriggered">-</b> / <b id="lockSleepSource">-</b> / <b id="lockSleepAgeMs">-</b></span></div>
<div class="kv"><span>锁车休眠最后ID / 已启用</span><span><b id="lockSleepLastId">-</b> / <b id="lockSleepArmed">-</b></span></div>
<div class="kv"><span>0x339 简化锁状态 / 距今ms</span><span><b id="lockSleep339SimpleStatus">-</b> / <b id="lockSleep339AgeMs">-</b></span></div>
<div class="kv"><span>车内无人 / 0x339稳定ms / 阻止</span><span><b id="lockSleepCabinEmpty">-</b> / <b id="lockSleep339StableAgeMs">-</b> / <b id="lockSleepBlocked">-</b></span></div>
<div class="kv"><span>0x339 收到</span><span id="lockSleep339Seen">-</span></div>
<h3>滚轮换挡</h3>
<div class="kv"><span>当前挡位 0x118</span><span id="currentGear">-</span></div>
<div class="kv"><span>DAS AP state 0x399</span><span id="dasAutopilotState">-</span></div>
<div class="kv"><span>刹车 / 车速</span><span><b id="brakeActive">-</b> / <b id="vehicleSpeedKph">-</b></span></div>
<div class="kv"><span>右滚轮 ticks 0x3C2</span><span id="rightScrollTicks">-</span></div>
<div class="kv"><span>右拨杆 status/counter 0x229</span><span><b id="rightStalkStatus">-</b> / <b id="rightStalkCounter">-</b></span></div>
<div class="kv"><span>换挡意图 / 注入中 / 目标</span><span><b id="scrollGearIntent">-</b> / <b id="scrollGearInjectActive">-</b> / <b id="scrollGearInjectTarget">-</b></span></div>
<div class="kv"><span>结果确认 / 阻止原因</span><span><b id="scrollGearInjectOk">-</b> / <b id="scrollGearInjectBlocked">-</b></span></div>
<h3>系统</h3>
<div class="kv"><span>运行时间 秒</span><span id="uptime">-</span></div>
</div>

</div>

<script>
let pollTimer=null,recTimer=null,loaded=false,lastDiag=null;
const cfgIds=["fsdEnabled","autoSpeedOffsetEnabled","cabinCameraDisableEnabled","cabinCameraTelemetryDisableEnabled","slewPctPerSec","lowSpeedMaxPctRaw","targetBelow60","target60","target70","target80","target90","target100","target120","canbEnabled","canbServiceModeEnabled","canbFilterMode","highBeamStrobeEnabled","rearFogBrakeStrobeEnabled","reverseStrobeEnabled","fsdForceHeadlightEnabled","fsdForceHighBeamEnabled","batteryPreheatEnabled","dndEnabled","dndVolumeEnabled","nagKillerEnabled","nagKillerTest052Enabled","nagKillerTest370Enabled","nagKillerMode","nagKillerBurstMs","nagKillerPauseMs","nagKillerBPos1Nm","nagKillerBPos2Nm","nagKillerBNeg1Nm","nagKillerBNeg2Nm","nagKillerCNegNm","nagKillerCPosNm","lockDeepSleepEnabled","scrollGearInjectEnabled","can1ReceiveOnly"];
const stats=["diagWindowMs","cpuMhz","loopHz","cpuPct","loopBusyPct","loopAvgUs","loopMaxUs","loopWaitAvgUs","loopPeriodMaxUs","webTaskMaxUs","totalHeapBytes","freeHeapBytes","minFreeHeapBytes","freeHeapPct","minFreeHeapPct","can1Rx","can1Tx","can1TxFail","can1RxRate","can1TxRate","can1TxFailRate","can1RxGapMaxUs","can1TxMaxUs","can1TxSlowCount","twaiState","twaiBusOffCount","twaiRxQueue","twaiTxQueue","twaiRxQueueMax","twaiTxQueueMax","twaiRxMissed","twaiRxOverrun","twaiBusError","twaiTxFailed","twaiTxErr","twaiRxErr","fusedLimitKph","targetSpeedKph","offsetKph","offsetRaw","canbReady","canbHardwareFilterMode","canbRx","canbTx","canbTxFail","canbRxRate","canbTxRate","canbTxFailRate","canbRxGapMaxUs","canbTxMaxUs","canbTxSlowCount","canbDrainMaxUs","canbDrainMaxFrames","canbLastId","canbErrorFlags","canbRxOverflowCount","highBeamStrobeActive","highBeamStrobeRemaining","rearFogBrakeStrobeActive","rearFogBrakeStrobeRemaining","reverseStrobeActive","reverseStrobeRemaining","fsdLightForceActive","fsdLightForceBlocked","fsdLightForceTxCount","fsdLightForceTxFail","fsdLightForceLastTxAgeMs","fsdLightForceBodyAgeMs","fsdLightForceHeadlightRequest","fsdLightForceHighBeamDecision","fsdLightForceBodyCounter","fsdLightFeedbackAgeMs","fsdLightLowBeamLeftStatus","fsdLightHighBeamLeftStatus","batteryPreheatActive","batteryPreheatTxCount","batteryPreheatAgeMs","batteryPreheatRunMs","batteryPreheatStableTempMs","batteryPreheatAutoOffReason","batteryPreheatAutoOffLatched","batteryPreheatChargeDetected","batteryPreheatSocPct","batteryPreheatSocAgeMs","batteryPreheatFeedbackSeen","batteryPreheatFeedbackBus","batteryPreheatFeedbackAgeMs","batteryPreheatUiTripActive","batteryPreheatUiNavToSupercharger","batteryPreheatUiFastChargerType","batteryPreheatUiState","batteryPreheatUiRequestHeat","batteryPreheatUiPowerW","batteryPreheatUiTargetCx100","batteryPreheatUiAmbientCx100","batteryPreheatUiChargeTargetCx10","batteryPreheatUiEnergyAtDestination","batteryPreheatFeedbackPayload","dndHandsOnState","dndWarningActive","dndActionActive","dndActionType","dndBlocked","dndTxCount","dndLastTriggerAgeMs","dndScrollCacheAgeMs","nagKillerMode","nagKillerActive","nagKillerBlocked","nagKillerBurstActive","nagKillerTargetId","nagKillerRxCount","nagKillerTxCount","nagKillerTxFail","nagKillerLastRxAgeMs","nagKillerLastTxAgeMs","nagKillerApAgeMs","nagKillerSteeringAgeMs","nagKillerApState","nagKillerHandsOnState","nagKillerTargetHandsOn","nagKillerSetHandsOn","nagKillerRealTorqueCx100","nagKillerLastTorqueCx100","nagKillerSteeringDegCx10","nagKillerDndRemaining","nagKillerDndTriggerCount","nagKillerDndLastTriggerAgeMs","bmsTempFrameSeen","bmsTempFrameId","bmsTempFrameBus","bmsTempFrameMux","bmsTempFrameAgeMs","bmsTempFramePayload","bmsTempDecodedSeen","bmsTempDecodedMux","bmsTempDecodedCount","bmsTempDecodedAgeMs","bmsTempLatest1Cx100","bmsTempLatest2Cx100","bmsTempLatest3Cx100","bmsTempMinCx100","bmsTempAvgCx100","bmsTempMaxCx100","lockSleepArmed","lockSleepTriggered","lockSleepLastId","lockSleepSource","lockSleepAgeMs","lockSleep339Seen","lockSleep339SimpleStatus","lockSleep339AgeMs","lockSleepCabinEmpty","lockSleep339StableAgeMs","lockSleepBlocked","currentGear","dasAutopilotState","brakeActive","vehicleSpeedKph","rightScrollTicks","rightStalkStatus","rightStalkCounter","scrollGearIntent","scrollGearInjectActive","scrollGearInjectTarget","scrollGearInjectOk","scrollGearInjectBlocked","uptime"];
const statIds={nagKillerMode:"nagKillerModeText"};
stats.push("batteryPreheatChargeStatusSeen","batteryPreheatChargeStatusBus","batteryPreheatChargeStatus","batteryPreheatChargeStatusAgeMs","batteryPreheatBms332Seen","batteryPreheatBms332Bus","batteryPreheatBms332Mux","batteryPreheatBms332AgeMs","batteryPreheatBms332MinCx100","batteryPreheatBms332AvgCx100","batteryPreheatBms332MaxCx100","batteryPreheatBms332Payload","batteryPreheatVcfrontSeen","batteryPreheatVcfrontBus","batteryPreheatVcfrontAgeMs","batteryPreheatVcfrontCoolantLevel","batteryPreheatVcfrontCoolantBatInletCx100","batteryPreheatVcfrontCoolantPtInletCx100","batteryPreheatVcfrontAmbientCx100","batteryPreheatVcfrontAmbientFilteredCx100","batteryPreheatVcfrontPayload");
function setVal(id,v){const e=document.getElementById(id);if(!e)return;if(e.type==="checkbox")e.checked=!!v;else e.value=v;}
function getVal(e){return e.type==="checkbox"?(e.checked?1:0):e.value}
function showResult(text){const e=document.getElementById("testResult");if(e)e.textContent=text}
function yesNo(v){return v?"是":"否"}
function cx100(v){return v<=-32000?"-":(v/100).toFixed(2)+" ℃"}
function us(v){return (v>>>0)+" us"}
function kb(v){return Math.round((v||0)/1024)+" KB"}
function setDiagPage(on){const main=document.getElementById("mainPage"),diag=document.getElementById("diagPage");if(main)main.classList.toggle("hidden",on);if(diag)diag.classList.toggle("hidden",!on);if(on){const p=document.getElementById("poll");if(p&&!p.checked){p.checked=true;setPolling(true)}window.scrollTo(0,0)}}
function fmtStat(k,v){
if(k==="diagWindowMs")return v+" ms";
if(k==="cpuMhz")return v+" MHz";
if(["cpuPct","loopBusyPct","freeHeapPct","minFreeHeapPct"].includes(k))return v+"%";
if(["loopAvgUs","loopMaxUs","loopWaitAvgUs","loopPeriodMaxUs","webTaskMaxUs","can1RxGapMaxUs","can1TxMaxUs","canbRxGapMaxUs","canbTxMaxUs","canbDrainMaxUs"].includes(k))return us(v);
if(["totalHeapBytes","freeHeapBytes","minFreeHeapBytes"].includes(k))return kb(v);
if(["can1RxRate","can1TxRate","can1TxFailRate","canbRxRate","canbTxRate","canbTxFailRate"].includes(k))return v+"/s";
if(k==="canbLastId"||k==="canbErrorFlags"||k==="bmsTempFrameId"||k==="lockSleepLastId"||k==="nagKillerTargetId")return "0x"+(v>>>0).toString(16).toUpperCase();
if(k==="fsdLightForceActive")return yesNo(v);
if(k==="fsdLightForceBlocked")return ["ok","disabled","canb","stale_0x399","ap_state","no_0x3E9","stale_0x3E9","tx_fail","bad_dlc"][v]||v;
if(["fsdLightForceSwitchOn","fsdLightForceCanbOk","fsdLightForce399Fresh","fsdLightForceApActive","fsdLightForce3e9Seen","fsdLightForce3e9Fresh","fsdLightForce3e9DlcOk","fsdLightForce3f5Seen"].includes(k))return yesNo(v);
if(["fsdLightForceLastTxAgeMs","fsdLightForceBodyAgeMs","fsdLightForce399AgeMs","fsdLightFeedbackAgeMs"].includes(k))return v+" ms";
if(k==="fsdLightForceApState")return ["DISABLED","UNAVAILABLE","AVAILABLE","ACTIVE_NOMINAL","ACTIVE_RESTRICTED","ACTIVE_NAV","ACTIVE_FSD"][v]||v;
if(["fsdLightForceHeadlightRequest","fsdLightForceHighBeamDecision","fsdLightForceBodyCounter"].includes(k))return v===255?"-":v;
if(k==="fsdLightLowBeamLeftStatus"||k==="fsdLightLowBeamRightStatus"||k==="fsdLightHighBeamLeftStatus"||k==="fsdLightHighBeamRightStatus")return v===255?"-":(["OFF","ON","FAULT","SNA"][v]||v);
if(k==="batteryPreheatActive"||k==="batteryPreheatAutoOffLatched"||k==="batteryPreheatChargeDetected"||k==="batteryPreheatChargeStatusSeen"||k==="batteryPreheatFeedbackSeen"||k==="batteryPreheatBms332Seen"||k==="batteryPreheatVcfrontSeen"||k==="bmsTempFrameSeen"||k==="bmsTempDecodedSeen")return yesNo(v);
if(k==="batteryPreheatRunMs"||k==="batteryPreheatStableTempMs"||k==="batteryPreheatSocAgeMs"||k==="batteryPreheatChargeStatusAgeMs"||k==="batteryPreheatBms332AgeMs"||k==="batteryPreheatVcfrontAgeMs")return v+" ms";
if(k==="batteryPreheatAutoOffReason")return ["无","平均温度到42°C","最高温到45°C","开始充电","运行15分钟","手动关闭","电量低于5%"][v]||v;
if(k==="batteryPreheatSocPct")return v<0?"未知":(v+"%");
if(k==="batteryPreheatFeedbackBus"||k==="batteryPreheatChargeStatusBus"||k==="batteryPreheatBms332Bus"||k==="batteryPreheatVcfrontBus")return v===1?"bus=1":(v===2?"bus=2":v);
if(k==="batteryPreheatChargeStatus")return ["0 DISCONNECTED","1 NO_POWER","2 ABOUT_TO_CHARGE","3 CHARGING","4 CHARGE_COMPLETE","5 CHARGE_STOPPED","6 CALIBRATING"][v]||"-";
if(k==="batteryPreheatBms332Mux")return v===0?"0 THERM":(v===1?"1 VOLT":(v===2?"2 END":"-"));
if(k==="batteryPreheatVcfrontCoolantLevel")return v===0?"0 NOT_OK":(v===1?"1 FILLED":(v===255?"-":v));
if(k==="batteryPreheatUiTripActive"||k==="batteryPreheatUiNavToSupercharger"||k==="batteryPreheatUiRequestHeat")return yesNo(v);
if(k==="batteryPreheatUiFastChargerType"){if(v===0)return "0 无";if(v===1)return "1 Low";if(v===2)return "2 V2";if(v===3)return "3 V3";if(v===4)return "4 V4";return v+" 未定义"}
if(k==="batteryPreheatUiState")return ["空闲","主动加热","状态2","状态3"][v]||v;
if(k==="batteryPreheatUiPowerW")return v<=-32000?"-":(v+" W");
if(k==="batteryPreheatUiTargetCx100"||k==="batteryPreheatUiAmbientCx100"||k==="batteryPreheatBms332MinCx100"||k==="batteryPreheatBms332AvgCx100"||k==="batteryPreheatBms332MaxCx100"||k==="batteryPreheatVcfrontCoolantBatInletCx100"||k==="batteryPreheatVcfrontCoolantPtInletCx100"||k==="batteryPreheatVcfrontAmbientCx100"||k==="batteryPreheatVcfrontAmbientFilteredCx100"||k==="bmsTempLatest1Cx100"||k==="bmsTempLatest2Cx100"||k==="bmsTempLatest3Cx100"||k==="bmsTempMinCx100"||k==="bmsTempAvgCx100"||k==="bmsTempMaxCx100")return cx100(v);
if(k==="batteryPreheatUiChargeTargetCx10")return v<=-32000?"-":(v/10).toFixed(1)+" ℃";
if(k==="batteryPreheatUiEnergyAtDestination")return v<=-32000?"-":v;
if(k==="bmsTempDecodedMux")return v===255?"-":("mux "+v);
if(k==="lockSleepSource")return v===1?"0x339":v;
if(k==="lockSleep339SimpleStatus"){if(v===0)return "0 SNA";if(v===1)return "1 解锁";if(v===2)return "2 锁定";if(v===255)return "未解码";return v}
if(k==="lockSleepCabinEmpty")return yesNo(v);
if(k==="lockSleepBlocked")return ["ok","未锁/解锁","车内有人/未确认无人","0x339稳定中"][v]||v;
if(k==="dndActionType")return ["none","volume"][v]||v;
if(k==="dndBlocked")return ["ok","disabled","canb","no_cache"][v]||v;
if(k==="nagKillerMode")return ["","Mode B","Mode C","Mode D"][v]||v;
if(k==="nagKillerBlocked")return ["ok","disabled","bad_mode","ap_state","target_ho","rest","stale_0x399","stale_0x129","steer_angle","hands_state","tx_blocked","bad_dlc"][v]||v;
if(k==="nagKillerSetHandsOn")return v?("L"+v):"否";
if(k==="nagKillerActive"||k==="nagKillerBurstActive")return yesNo(v);
if(k==="nagKillerRealTorqueCx100"||k==="nagKillerLastTorqueCx100")return v<=-32000?"-":(v/100).toFixed(2)+" Nm";
if(k==="nagKillerSteeringDegCx10")return v<=-32000?"-":(v/10).toFixed(1)+"°";
if(k==="nagKillerDndLastTriggerAgeMs")return v+" ms";
if(k==="dasAutopilotState")return ["DISABLED","UNAVAILABLE","AVAILABLE","ACTIVE_NOMINAL","ACTIVE_RESTRICTED","ACTIVE_NAV","ACTIVE_FSD"][v]||v;
if(k==="scrollGearInjectBlocked")return ["ok","bad_target","brake","speed","same_gear","cooldown","ap_state"][v]||v;
return v}
function updateAutoDiag(j){
const items=[];
const val=k=>Number(j[k]||0);
const inc=k=>lastDiag?Math.max(0,val(k)-Number(lastDiag[k]||0)):0;
const add=(sev,msg)=>items.push({sev,msg});
if(val("loopMaxUs")>10000)add(2,"主循环最大耗时超过10ms");
else if(val("loopMaxUs")>5000)add(1,"主循环最大耗时超过5ms");
if(val("loopPeriodMaxUs")>20000)add(2,"主循环周期出现20ms以上间隔");
else if(val("loopPeriodMaxUs")>10000)add(1,"主循环周期出现10ms以上间隔");
const cpuPct=val("cpuPct")||val("loopBusyPct");
const freeHeapPct=val("freeHeapPct");
const freeHeapBytes=val("freeHeapBytes");
if(cpuPct>90)add(2,"CPU占用超过90%");
else if(cpuPct>70)add(1,"CPU占用超过70%");
if((freeHeapPct>0&&freeHeapPct<5)||(freeHeapBytes>0&&freeHeapBytes<40000))add(2,"可用内存低于5%或40KB");
else if((freeHeapPct>0&&freeHeapPct<10)||(freeHeapBytes>0&&freeHeapBytes<80000))add(1,"可用内存低于10%或80KB");
if(val("webTaskMaxUs")>50000)add(1,"WebUI单次处理超过50ms");
if(inc("twaiBusOffCount")>0)add(2,"TWAI bus-off刚增加");
else if(val("twaiBusOffCount")>0)add(1,"TWAI bus-off历史非0");
if(inc("twaiRxMissed")>0)add(2,"TWAI RX missed刚增加");
else if(val("twaiRxMissed")>0)add(1,"TWAI RX missed历史非0");
if(inc("twaiRxOverrun")>0)add(2,"TWAI RX overrun刚增加");
else if(val("twaiRxOverrun")>0)add(1,"TWAI RX overrun历史非0");
if(inc("twaiBusError")>0)add(1,"TWAI bus error刚增加");
if(inc("twaiTxFailed")>0||val("can1TxFailRate")>0)add(1,"bus=1发送失败增加");
if(val("twaiRxQueue")>56)add(2,"TWAI RX当前队列接近满");
else if(val("twaiRxQueue")>40||val("twaiRxQueueMax")>48)add(1,"TWAI RX队列偏高");
else if(val("twaiRxQueueMax")>32)add(1,"TWAI RX队列历史偏高");
if(val("twaiTxQueue")>14)add(2,"TWAI TX当前队列接近满");
else if(val("twaiTxQueue")>8||val("twaiTxQueueMax")>14)add(1,"TWAI TX队列偏高");
else if(val("twaiTxQueueMax")>8)add(1,"TWAI TX队列历史偏高");
if(val("can1TxMaxUs")>5000)add(2,"bus=1 TX耗时超过5ms");
else if(val("can1TxMaxUs")>2000||val("can1TxSlowCount")>0)add(1,"bus=1 TX耗时超过2ms");
if(inc("canbRxOverflowCount")>0)add(2,"MCP2515 RX overflow刚增加");
else if(val("canbRxOverflowCount")>0)add(1,"MCP2515 RX overflow历史非0");
if(val("canbErrorFlags")!==0)add(1,"MCP2515 EFLG非0");
if(val("canbDrainMaxUs")>1500)add(2,"MCP2515 drain耗时超过1.5ms");
else if(val("canbDrainMaxUs")>=900)add(1,"MCP2515 drain触及900us预算");
if(val("canbDrainMaxFrames")>=24)add(1,"MCP2515单轮drain达到活动预算");
if(val("canbTxMaxUs")>5000)add(2,"bus=2 TX耗时超过5ms");
else if(val("canbTxMaxUs")>2000||val("canbTxSlowCount")>0)add(1,"bus=2 TX耗时超过2ms");
if(val("canbTxFailRate")>0)add(1,"bus=2发送失败增加");
items.sort((a,b)=>b.sev-a.sev);
const level=items.length?items[0].sev:0;
const levelText=["正常","警告","严重"][level];
const levelEl=document.getElementById("autoDiagLevel");
const reasonEl=document.getElementById("autoDiagReasons");
const adviceEl=document.getElementById("autoDiagAdvice");
if(levelEl){levelEl.textContent=levelText;levelEl.style.color=level===2?"#f66":(level===1?"#ffd479":"#9f9")}
if(reasonEl)reasonEl.textContent=items.length?items.slice(0,5).map(x=>(x.sev===2?"严重：":"警告：")+x.msg).join("；"):"未发现CPU/内存/CAN负载异常";
if(adviceEl)adviceEl.textContent=level===2?"优先关闭抓包调试或切到功能ID过滤，复测关键功能":(level===1?"观察是否持续；必要时关闭WebUI轮询或减少抓包":"保持当前设置");
lastDiag=j;
}
function updateStats(j){Object.keys(j).forEach(k=>{const e=document.getElementById(statIds[k]||k);if(e&&e.tagName!=="INPUT"&&e.tagName!=="SELECT")e.textContent=fmtStat(k,j[k])})}
function pollStatus(){fetch("/status").then(r=>r.json()).then(j=>{updateStats(j);updateAutoDiag(j);if(!loaded){cfgIds.forEach(k=>{if(k in j)setVal(k,j[k])});loaded=true}}).catch(()=>{})}
function setPolling(on){if(on&&!pollTimer){pollStatus();pollTimer=setInterval(pollStatus,1000)}if(!on&&pollTimer){clearInterval(pollTimer);pollTimer=null}}
function body(){const p=new URLSearchParams();cfgIds.forEach(k=>{const e=document.getElementById(k);p.set(k,getVal(e))});return p}
function applyConfig(){fetch("/config",{method:"POST",body:body()}).then(async r=>{showResult(await r.text());pollStatus()})}
function saveConfig(){fetch("/config",{method:"POST",body:body()}).then(()=>fetch("/save",{method:"POST"})).then(async r=>{showResult(await r.text());pollStatus()})}
function recQuery(){const ids=document.getElementById("recIds").value.trim();return ids?("?ids="+encodeURIComponent(ids)):""}
function stopReason(v){return v===1?"满":(v===2?"超时":"手动/无")}
function setRecUi(j){const active=j&&j.active;document.getElementById("recState").textContent=active?"抓包中":(j&&j.saved?"已保存":"空闲");document.getElementById("recCount").textContent=j?(j.count+" / "+j.cap):"-";document.getElementById("recBus1").textContent=j?j.bus1:"-";document.getElementById("recBus2").textContent=j?j.bus2:"-";document.getElementById("recDrop").textContent=j?(j.dropped+" / "+stopReason(j.stopReason)):"-";document.getElementById("recPsram").textContent=j?((j.psram?"可用":"不可用")+" / "+Math.round((j.bytes||0)/1024)+" KB"):"-";document.getElementById("recDownload").style.display=(!active&&j&&j.saved)?"inline-block":"none"}
function pollRec(){fetch("/rec_status").then(r=>r.json()).then(setRecUi).catch(()=>{})}
function startRec(){fetch("/rec_start"+recQuery(),{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(!recTimer)recTimer=setInterval(pollRec,800)})}
function stopRec(){fetch("/rec_stop",{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(recTimer){clearInterval(recTimer);recTimer=null}})}
function webOff(){setPolling(false);document.getElementById("poll").checked=false;fetch("/web/off",{method:"POST"})}
pollStatus();pollRec();
</script>
</body></html>)HTML";
