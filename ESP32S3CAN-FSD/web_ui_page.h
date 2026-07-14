// Light WebUI page served by /. Kept in a separate header so the PlatformIO
// .ino prototype generator never has to parse the JavaScript braces.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN" data-theme="dark"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN FSD 设置</title>
<style>
html[data-theme="dark"]{--bg:#101113;--panel:#181a1e;--panel2:#121417;--line:#2a2d33;--line2:#23262b;--text:#eef2f6;--muted:#9aa3ad;--accent:#62c7d8;--accent2:#80d39b;--ok:#44d184;--warn:#ffd166;--bad:#ef6b6b;--btn:#238c61;--btn2:#2d6fbb;--bar:rgba(16,17,19,.94);--shadow:0 1px 0 rgba(255,255,255,.03) inset}
html[data-theme="light"]{--bg:#f7f3eb;--panel:#fffdf8;--panel2:#f2ece2;--line:#ded6ca;--line2:#ebe1d4;--text:#17202a;--muted:#687382;--accent:#276d7e;--accent2:#227650;--ok:#168a52;--warn:#a55f00;--bad:#c2403c;--btn:#1f8a62;--btn2:#2f67b1;--bar:rgba(255,253,248,.94);--shadow:0 1px 2px rgba(39,34,25,.05)}
*{box-sizing:border-box}
html{background:var(--bg)}
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;margin:0 auto;padding:14px;background:var(--bg);color:var(--text);-webkit-text-size-adjust:100%;line-height:1.45;max-width:1180px;overflow-x:hidden}
h1{font-size:21px;margin:2px 0 12px;letter-spacing:0;font-weight:780;color:var(--text)}h2{font-size:15px;margin:0 0 10px;color:var(--accent);font-weight:750}h3{font-size:13px;margin:14px 0 6px;color:var(--accent2);border-top:1px solid var(--line2);padding-top:9px;font-weight:700}h2+h3{border-top:0;padding-top:0;margin-top:2px}
.page{display:grid;gap:12px;min-width:0}.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:13px;margin:0;box-shadow:var(--shadow);min-width:0;max-width:100%}.wide{grid-column:1/-1}
label{display:flex;justify-content:space-between;align-items:center;min-height:40px;margin:5px 0;font-size:14px;gap:12px;color:var(--text);min-width:0}
input,select,button{max-width:100%}
input[type=number]{width:98px;background:var(--panel2);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:8px 9px;font-size:14px}
input[type=text],input[type=password],select{width:100%;background:var(--panel2);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:9px;margin-top:4px;font-size:14px}
label input[type=number]{flex:0 0 98px;margin-left:auto}label select{flex:1;min-width:160px}label input[type=checkbox]{margin-left:auto}
input[type=number]:focus,input[type=text]:focus,input[type=password]:focus,select:focus{outline:none;border-color:var(--accent)}
input[type=checkbox]{appearance:none;-webkit-appearance:none;width:48px;height:27px;min-width:48px;border-radius:999px;border:1px solid var(--line);background:var(--panel2);position:relative;vertical-align:middle}
input[type=checkbox]:before{content:"";position:absolute;width:21px;height:21px;left:2px;top:2px;border-radius:50%;background:#dce3ea;transition:left .12s,background .12s;box-shadow:0 1px 3px rgba(0,0,0,.25)}
input[type=checkbox]:checked{background:var(--ok);border-color:var(--ok)}input[type=checkbox]:checked:before{left:23px;background:#fff}
.signed{display:flex;align-items:center;gap:6px;color:var(--text)}.signed b{min-width:10px;text-align:right}.signed em{font-style:normal;color:var(--muted);font-size:12px}
button,.linkbtn{min-height:40px;background:var(--btn);color:#fff;border:1px solid transparent;border-radius:8px;padding:9px 13px;margin:4px 4px 0 0;font-size:14px;font-weight:650;text-decoration:none}button.alt,.linkbtn{background:var(--btn2)}button.warn{background:#a64040}button.ghost{background:transparent;color:var(--accent);border-color:var(--line)}.linkbtn{display:none}
.profileButtons{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:6px;margin:8px 0 10px}.profileButtons button{width:100%;min-width:0;margin:0;padding:8px 4px;background:var(--panel2);color:var(--text);border-color:var(--line)}.profileButtons button.active{background:var(--btn2);color:#fff;border-color:var(--btn2)}
.kv{display:flex;justify-content:space-between;align-items:flex-start;font-size:13px;padding:7px 0;border-bottom:1px solid var(--line2);gap:14px}.kv span{min-width:0}.kv span:first-child{color:var(--muted)}.kv span:last-child{color:var(--ok);font-variant-numeric:tabular-nums;text-align:right;word-break:break-word;overflow-wrap:anywhere;max-width:62%}.kv b{font-weight:700}
.result{display:none;font-size:12px;color:var(--warn);margin:8px 0 0;flex-basis:100%}.result:not(:empty){display:block;min-height:18px}.bar{position:sticky;top:0;z-index:5;background:var(--bar);backdrop-filter:blur(6px);display:flex;flex-wrap:wrap;align-items:center;gap:5px;padding:8px;margin-bottom:12px}
.pageSwitch{display:inline-flex;justify-content:flex-start;align-items:center;gap:8px;margin:4px 0 0 auto;font-size:13px;color:var(--muted);min-height:36px}.pollSwitch{display:inline-flex;gap:8px;align-items:center;min-height:0;margin:0 0 0 10px;color:var(--muted);font-size:13px;font-weight:400}.pollSwitch input{width:40px;height:24px;min-width:40px}.pollSwitch input:before{width:18px;height:18px}.pollSwitch input:checked:before{left:18px}.hidden{display:none!important}
.legalBackdrop{position:fixed;inset:0;z-index:50;background:rgba(0,0,0,.74);display:flex;align-items:center;justify-content:center;padding:14px}.legalModal{width:min(760px,100%);max-height:92vh;overflow:auto;background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:18px;box-shadow:0 14px 45px rgba(0,0,0,.45)}.legalModal h2{font-size:18px;margin:0 0 10px}.legalModal p{margin:10px 0}.legalStrong{font-size:2em;line-height:1.28;color:#ff3b3b;font-weight:850}.legalActions{display:flex;justify-content:flex-end;margin-top:14px}.legalActions button{min-width:180px}
@media(min-width:900px){body{padding:16px 18px}.page{display:block;columns:2 430px;column-gap:12px}.page>.card{break-inside:avoid;margin:0 0 12px}.page>.wide{column-span:all}}
@media(max-width:520px){body{padding:10px}h1{font-size:18px}.card{padding:11px;border-radius:9px}button,.linkbtn{width:100%;margin:4px 0 0}.pageSwitch{width:100%;margin-left:0}.kv{gap:8px;flex-wrap:wrap}.kv span:first-child{flex:1 1 34%}.kv span:last-child{flex:1 1 58%;max-width:100%}label{align-items:center;flex-wrap:wrap;overflow-wrap:anywhere}label select{flex-basis:100%;min-width:0}label input[type=number]{margin-left:auto}}
</style></head><body>
<h1>T-2CAN FSD 运行参数</h1>

<div id="legalNotice" class="legalBackdrop">
<div class="legalModal" role="dialog" aria-modal="true" aria-labelledby="legalTitle">
<h2 id="legalTitle">安全提示与使用声明</h2>
<p>本固件仅供开源学习、研究与测试使用。使用前请确认你已充分理解 CAN 通讯、FSD/AP 相关信号测试和免打扰等功能的作用、适用场景及潜在后果，并自行承担全部责任。</p>
<p class="legalStrong">禁止任何形式的售卖、转售或商业化分发。</p>
<p>相关功能可能带来法律、合规及行车安全风险。请严格遵守当地法律法规，并仅在确认安全的环境中使用。</p>
<p class="legalStrong">驾驶过程中，请始终保持清醒并专注驾驶，目视前方，双手随时准备接管方向盘。任何辅助驾驶功能都不能替代驾驶员对车辆和道路环境的持续观察与控制。</p>
<p>点击确认即表示你已阅读并理解以上提示。</p>
<div class="legalActions"><button onclick="closeLegalNotice()">我已阅读并理解</button></div>
</div>
</div>

<div class="card bar">
<button class="warn" onclick="rebootBoard()">重启</button>
<button class="ghost" id="themeBtn" onclick="toggleTheme()">日间</button>
<label class="pageSwitch">诊断信息页<input type="checkbox" id="diagPageSwitch" onchange="setDiagPage(this.checked)"></label>
<div class="result" id="testResult"></div>
</div>

<div id="mainPage" class="page">

<div class="card wide">
<h2>通道定义</h2>
<div class="kv"><span>bus=1</span><span>TWAI / physical CANB / GPIO7,6</span></div>
<div class="kv"><span>bus=2</span><span>MCP2515 / physical CANA / SPI + INT8</span></div>
</div>

<div class="card wide">
<h2>功能开关</h2>
<label>开启CAN通讯<input type="checkbox" id="canCommsEnabled"></label>
<label>FSD 启用<input type="checkbox" id="fsdEnabled"></label>
<label>FSD 激活帧补发<input type="checkbox" id="fsdActivationResendEnabled"></label>
<label id="hw3AutoSpeedOffsetRow">自动速度偏移<input type="checkbox" id="autoSpeedOffsetEnabled"></label>
<label>HW4 速度偏移 +15<input type="checkbox" id="hw4SpeedOffsetPlus15Enabled"></label>
<label>HW4 最大速度偏移 60%<input type="checkbox" id="hw4SpeedOffset60Enabled"></label>
<label>HW4 自定义速度偏移<input type="checkbox" id="hw4CustomSpeedOffsetEnabled"></label>
<label>HW4 ISA 提示静音<input type="checkbox" id="hw4IsaChimeSuppressEnabled"></label>
<label>增强Autopilot / Smart Summon<input type="checkbox" id="enhancedAutopilotEnabled"></label>
<label>免打扰<input type="checkbox" id="cabinCameraDisableEnabled"></label>
<label>高光爆闪启用<input type="checkbox" id="highBeamStrobeEnabled"></label>
<label>后雾灯刹车爆闪启用<input type="checkbox" id="rearFogBrakeStrobeEnabled"></label>
<label>倒挡双闪雾灯启用<input type="checkbox" id="reverseStrobeEnabled"></label>
<label>电池预热启用<input type="checkbox" id="batteryPreheatEnabled"></label>
<label>维修模式 0x339<input type="checkbox" id="canbServiceModeEnabled"></label>
</div>

<div class="card">
<h2>FSD / 速度参数</h2>
<label>FSD 激活车型<select id="fsdActivationProfile"><option value="0">V12/V13 / HW3</option><option value="1">V14 / HW4</option></select></label>
<div class="kv" id="hw4ProfileRow" style="display:none"><span>跟车距离 / 驾驶模式</span><span><b id="hw4FollowDistanceText">-</b> / <b id="hw4DrivingProfileText">-</b></span></div>
<div id="hw4ProfileControls" style="display:none">
<div class="profileButtons">
<button type="button" data-hw4-profile="3" onclick="selectHw4Profile(3)">1</button>
<button type="button" data-hw4-profile="2" onclick="selectHw4Profile(2)">2</button>
<button type="button" data-hw4-profile="1" onclick="selectHw4Profile(1)">3</button>
<button type="button" data-hw4-profile="0" onclick="selectHw4Profile(0)">4</button>
<button type="button" data-hw4-profile="4" onclick="selectHw4Profile(4)">5</button>
</div>
<label>HW4 缓降 %/秒<input type="number" id="hw4SlewPctPerSec" min="0" max="60" step="1"></label>
<div class="kv"><span>HW4 偏移目标 / 当前</span><span><b id="hw4OffsetTargetPct">-</b> / <b id="hw4OffsetSentPct">-</b></span></div>
<div id="hw4CustomSpeedOffsetFields" style="display:none">
<label>HW4 目标速度 &lt;60<input type="number" id="hw4TargetBelow60" min="0" max="255"></label>
<label>HW4 目标速度 60..69<input type="number" id="hw4Target60" min="0" max="255"></label>
<label>HW4 目标速度 70..79<input type="number" id="hw4Target70" min="0" max="255"></label>
<label>HW4 目标速度 80..89<input type="number" id="hw4Target80" min="0" max="255"></label>
<label>HW4 目标速度 90..99<input type="number" id="hw4Target90" min="0" max="255"></label>
<label>HW4 目标速度 100..119<input type="number" id="hw4Target100" min="0" max="255"></label>
<label>HW4 目标速度 120..139<input type="number" id="hw4Target120" min="0" max="255"></label>
</div>
</div>
<label>补发周期 ms<input type="number" id="fsdActivationResendMs" min="1" max="1000" step="1"></label>
<div class="kv"><span>补发状态 / 周期</span><span><b id="fsdActivationResendActive">-</b> / <b id="fsdActivationResendPeriodMs">-</b></span></div>
<div class="kv"><span>补发次数 / mux0缓存</span><span><b id="fsdActivationResendTxCount">-</b> / <b id="fsdActivationResendCachedMuxMask">-</b></span></div>
<div id="hw3SpeedOffsetFields">
<label>缓降百分比/秒<input type="number" id="slewPctPerSec" min="0" max="100"></label>
<label>低速最大偏移 %<input type="number" id="lowSpeedMaxPctRaw" min="0" max="60" step="0.25"></label>
<label>目标速度 &lt;60<input type="number" id="targetBelow60" min="0" max="255"></label>
<label>目标速度 60..69<input type="number" id="target60" min="0" max="255"></label>
<label>目标速度 70..79<input type="number" id="target70" min="0" max="255"></label>
<label>目标速度 80..89<input type="number" id="target80" min="0" max="255"></label>
<label>目标速度 90..99<input type="number" id="target90" min="0" max="255"></label>
<label>目标速度 100..119<input type="number" id="target100" min="0" max="255"></label>
<label>目标速度 120..139<input type="number" id="target120" min="0" max="255"></label>
</div>
</div>

<div class="card wide">
<h2>CAN 总线控制</h2>
<label>bus=1/TWAI/物理CANB 只收不发<input type="checkbox" id="can1ReceiveOnly"></label>
<h3>MCP2515 / 物理 CANA</h3>
<label>启用<input type="checkbox" id="canbEnabled"></label>
<label>硬件过滤模式<select id="canbFilterMode"><option value="0">抓包调试</option><option value="1">当前功能相关ID</option></select></label>
</div>

<div class="card hidden">
<h2>CAN 抓包</h2>
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

<div class="card wide">
<h2>诊断信息 <label class="pollSwitch">轮询<input type="checkbox" id="poll" onchange="setPolling(this.checked)"></label></h2>
<h3>自动诊断</h3>
<div class="kv"><span>系统健康</span><span id="autoDiagLevel">-</span></div>
<div class="kv"><span>诊断原因</span><span id="autoDiagReasons">-</span></div>
<div class="kv"><span>建议动作</span><span id="autoDiagAdvice">-</span></div>
</div>

<div class="card">
<h2>开发板诊断</h2>
<div class="kv"><span>窗口ms / CPU MHz</span><span><b id="diagWindowMs">-</b> / <b id="cpuMhz">-</b></span></div>
<div class="kv"><span>CPU0 / CPU1 占用估算</span><span><b id="cpu0Pct">-</b> / <b id="cpu1Pct">-</b></span></div>
<div class="kv"><span>loop Hz / CPU%</span><span><b id="loopHz">-</b> / <b id="cpuPct">-</b></span></div>
<div class="kv"><span>loop平均/最大 us</span><span><b id="loopAvgUs">-</b> / <b id="loopMaxUs">-</b></span></div>
<div class="kv"><span>loop等待平均 / 周期最大 us</span><span><b id="loopWaitAvgUs">-</b> / <b id="loopPeriodMaxUs">-</b></span></div>
<div class="kv"><span>WebUI最大 us</span><span id="webTaskMaxUs">-</span></div>
<div class="kv"><span>内存可用% / 历史最低%</span><span><b id="freeHeapPct">-</b> / <b id="minFreeHeapPct">-</b></span></div>
<div class="kv"><span>heap总量/可用/历史最低</span><span><b id="totalHeapBytes">-</b> / <b id="freeHeapBytes">-</b> / <b id="minFreeHeapBytes">-</b></span></div>
</div>

<div class="card">
<h2>CAN 总线</h2>
<h3>bus=1 / TWAI / 物理CANB</h3>
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
<h3>bus=2 / MCP2515 / 物理CANA</h3>
<div class="kv"><span>MCP2515 ready</span><span id="canbReady">-</span></div>
<div class="kv"><span>MCP2515 filter mode</span><span id="canbHardwareFilterMode">-</span></div>
<div class="kv"><span>MCP2515 RX / TX / fail</span><span><b id="canbRx">-</b> / <b id="canbTx">-</b> / <b id="canbTxFail">-</b></span></div>
<div class="kv"><span>MCP2515 RX/TX/fail 每秒</span><span><b id="canbRxRate">-</b> / <b id="canbTxRate">-</b> / <b id="canbTxFailRate">-</b></span></div>
<div class="kv"><span>MCP2515 RX最大间隔 / TX最大 us</span><span><b id="canbRxGapMaxUs">-</b> / <b id="canbTxMaxUs">-</b></span></div>
<div class="kv"><span>MCP2515 TX慢 / drain最大</span><span><b id="canbTxSlowCount">-</b> / <b id="canbDrainMaxUs">-</b></span></div>
<div class="kv"><span>MCP2515 drain最大帧数</span><span id="canbDrainMaxFrames">-</span></div>
<div class="kv"><span>MCP2515 TX调用每轮 上次/窗口最大/历史最大</span><span><b id="canbTxLoopLast">-</b> / <b id="canbTxLoopMax">-</b> / <b id="canbTxLoopMaxEver">-</b></span></div>
<div class="kv"><span>MCP2515 TX调用来源 NAG/预热/灯光</span><span><b id="canbTxSrcNag">-</b> / <b id="canbTxSrcBattery">-</b> / <b id="canbTxSrcLight">-</b></span></div>
<div class="kv"><span>MCP2515 TX调用来源 滚轮/换挡/后雾/倒挡/0x339/其他</span><span><b id="canbTxSrcDnd">-</b> / <b id="canbTxSrcScrollGear">-</b> / <b id="canbTxSrcRearFog">-</b> / <b id="canbTxSrcReverse">-</b> / <b id="canbTxSrcService">-</b> / <b id="canbTxSrcOther">-</b></span></div>
<div class="kv"><span>MCP2515 TX队列 当前/最大</span><span><b id="canbTxQueueDepth">-</b> / <b id="canbTxQueueMaxDepth">-</b></span></div>
<div class="kv"><span>MCP2515 TX调度 发出/丢弃/过期/失败/触顶</span><span><b id="canbTxSchedTx">-</b> / <b id="canbTxSchedDrop">-</b> / <b id="canbTxSchedExpired">-</b> / <b id="canbTxSchedFail">-</b> / <b id="canbTxSchedBudgetHit">-</b></span></div>
<div class="kv"><span>MCP2515 last ID</span><span id="canbLastId">-</span></div>
<div class="kv"><span>MCP2515 EFLG / RX overflow</span><span><b id="canbErrorFlags">-</b> / <b id="canbRxOverflowCount">-</b></span></div>
</div>

<div class="card wide">
<h2>FSD 自动换挡诊断</h2>
<div class="kv"><span>总状态</span><span id="autoShiftDiagSummary">-</span></div>
<div class="kv"><span>Smart Shift 网关/UI</span><span><b id="autoShiftGtwSmartShiftStatus">-</b> / <b id="autoShiftGtwPrimaryGearControlStatus">-</b> / <b id="autoShiftGtwGearStripEnable">-</b> / <b id="autoShiftGtwGearAgeMs">-</b></span></div>
<div class="kv"><span>DI 推荐换挡</span><span><b id="autoShiftDiSmartShiftUnavailableReason">-</b> / <b id="autoShiftDiSmartShiftGear">-</b> / <b id="autoShiftDiFrontBlocked">-</b> / <b id="autoShiftDiRearBlocked">-</b> / <b id="autoShiftDiSuggestedAgeMs">-</b></span></div>
<div class="kv"><span>DAS 换挡方向</span><span><b id="autoShiftDasUpcomingDirection">-</b> / <b id="autoShiftDasTorqueDirection">-</b> / <b id="autoShiftDasBrakeRequired">-</b> / <b id="autoShiftDasSmartShiftAgeMs">-</b></span></div>
<div class="kv"><span>P档/物理状态</span><span><b id="autoShiftDiGear">-</b> / <b id="autoShiftDiBrakePedalState">-</b> / <b id="autoShiftDiSystemState">-</b> / <b id="autoShiftDiVehicleSpeedCx10">-</b></span></div>
<div class="kv"><span>驾驶员/车身</span><span><b id="autoShiftDriverBuckleStatus">-</b> / <b id="autoShiftDriverDoorStatus">-</b> / <b id="autoShiftDriverUnbuckled">-</b> / <b id="autoShiftVcfrontAgeMs">-</b></span></div>
<div class="kv"><span>FSD UI配置</span><span><b id="autoShiftUiEnableFsd">-</b> / <b id="autoShiftUiHasFsd">-</b> / <b id="autoShiftUiFsdSuspended">-</b> / <b id="autoShiftUiStartFromPark">-</b></span></div>
<div class="kv"><span>DAS Ready</span><span><b id="autoShiftDasAutopilotState">-</b> / <b id="autoShiftDasLssState">-</b> / <b id="autoShiftDasFleetSpeedState">-</b> / <b id="autoShiftDasHandsOnState">-</b></span></div>
<div class="kv"><span>DAS状态2</span><span><b id="autoShiftDasPmmSysFaultReason">-</b> / <b id="autoShiftDasCsaState">-</b> / <b id="autoShiftDasRobState">-</b> / <b id="autoShiftDasDriverInteractionLevel">-</b></span></div>
<div class="kv"><span>摄像头/APP Warning</span><span><b id="autoShiftW330CabinCameraFault">-</b> / <b id="autoShiftW372CabinCameraBlocked">-</b> / <b id="autoShiftW391CabinCameraBlockedOnAp">-</b> / <b id="autoShiftW393AttnMntrUnavailable">-</b></span></div>
</div>

<div class="card">
<h2>速度 / FSD</h2>
<div class="kv"><span>融合限速 kph</span><span id="fusedLimitKph">-</span></div>
<div class="kv"><span>目标速度 kph</span><span id="targetSpeedKph">-</span></div>
<div class="kv"><span>速度偏移 kph / 百分比</span><span><b id="offsetKph">-</b> / <b id="offsetRaw">-</b></span></div>
</div>

<div class="card">
<h2>DND 诊断</h2>
<div class="kv"><span>DND hands-on / warning</span><span><b id="dndHandsOnState">-</b> / <b id="dndWarningActive">-</b></span></div>
<div class="kv"><span>DND动作 / 阻止 / TX</span><span><b id="dndActionActive">-</b> / <b id="dndBlocked">-</b> / <b id="dndTxCount">-</b></span></div>
<div class="kv"><span>AP状态 / hands-on / 方向角</span><span><b id="nagKillerApState">-</b> / <b id="nagKillerHandsOnState">-</b> / <b id="nagKillerSteeringDegCx10">-</b></span></div>
<div class="kv"><span>NAG联动滚轮 次数/剩余/累计</span><span><b id="nagKillerDndActionCount">-</b> / <b id="nagKillerDndRemaining">-</b> / <b id="nagKillerDndTriggerCount">-</b></span></div>
</div>

<div class="card">
<h2>电池预热</h2>
<div class="kv"><span>电池预热发送中</span><span id="batteryPreheatActive">-</span></div>
<div class="kv"><span>发送次数 / 最近发送</span><span><b id="batteryPreheatTxCount">-</b> / <b id="batteryPreheatAgeMs">-</b></span></div>
<div class="kv"><span>运行时长 / 到温保持</span><span><b id="batteryPreheatRunMs">-</b> / <b id="batteryPreheatStableTempMs">-</b></span></div>
<div class="kv"><span>自动关闭 / 原因 / 正在充电</span><span><b id="batteryPreheatAutoOffLatched">-</b> / <b id="batteryPreheatAutoOffReason">-</b> / <b id="batteryPreheatChargeDetected">-</b></span></div>
<div class="kv"><span>阻止预热原因</span><span id="batteryPreheatBlockMask">-</span></div>
<div class="kv"><span>车辆充电状态 / 来源 / 更新时间</span><span><b id="batteryPreheatChargeStatus">-</b> / <b id="batteryPreheatChargeStatusBus">-</b> / <b id="batteryPreheatChargeStatusAgeMs">-</b></span></div>
<div class="kv"><span>电量SOC / 来源 / 更新时间</span><span><b id="batteryPreheatSocUiDeciPct">-</b> / <b id="batteryPreheatSocBus">-</b> / <b id="batteryPreheatSocAgeMs">-</b></span></div>
<div class="kv"><span>预热指令反馈 / 来源 / 更新时间</span><span><b id="batteryPreheatFeedbackSeen">-</b> / <b id="batteryPreheatFeedbackBus">-</b> / <b id="batteryPreheatFeedbackAgeMs">-</b></span></div>
<div class="kv"><span>0x082导航预热状态 / 请求加热</span><span><b id="batteryPreheatUiState">-</b> / <b id="batteryPreheatUiRequestHeat">-</b></span></div>
<div class="kv"><span>0x082导航预热主动加热</span><span id="batteryPreheatHeatingActive">-</span></div>
<div class="kv"><span>导航快充 / 快充类型 / 行程规划</span><span><b id="batteryPreheatUiNavToSupercharger">-</b> / <b id="batteryPreheatUiFastChargerType">-</b> / <b id="batteryPreheatUiTripActive">-</b></span></div>
<div class="kv"><span>0x082反馈功率 / 目标温度</span><span><b id="batteryPreheatUiPowerW">-</b> / <b id="batteryPreheatUiTargetCx100">-</b></span></div>
<div class="kv"><span>本机发送功率 / 目标温度</span><span><b id="batteryPreheatCmdPowerW">-</b> / <b id="batteryPreheatCmdTargetCx100">-</b></span></div>
<h3>电池温度 / 冷却</h3>
<div class="kv"><span>电池温度 最低/平均/最高</span><span><b id="bmsTempMinCx100">-</b> / <b id="bmsTempAvgCx100">-</b> / <b id="bmsTempMaxCx100">-</b></span></div>
<div class="kv"><span>电池温度更新时间</span><span id="bmsTempDecodedAgeMs">-</span></div>
<div class="kv"><span>BMS热管理0x312 / 来源 / 更新时间</span><span><b id="batteryPreheatBms312Seen">-</b> / <b id="batteryPreheatBms312Bus">-</b> / <b id="batteryPreheatBms312AgeMs">-</b></span></div>
<div class="kv"><span>BMS目标模式0x3B2 / 来源 / 更新时间</span><span><b id="batteryPreheatBms3b2Seen">-</b> / <b id="batteryPreheatBms3b2Bus">-</b> / <b id="batteryPreheatBms3b2AgeMs">-</b></span></div>
<div class="kv"><span>BMS实际加热判断</span><span id="batteryPreheatBmsHeatStatus">-</span></div>
<div class="kv"><span>冷却环境数据 / 来源 / 更新时间</span><span><b id="batteryPreheatVcfrontSeen">-</b> / <b id="batteryPreheatVcfrontBus">-</b> / <b id="batteryPreheatVcfrontAgeMs">-</b></span></div>
<div class="kv"><span>冷却液温度 电池/电驱</span><span><b id="batteryPreheatVcfrontCoolantBatInletCx100">-</b> / <b id="batteryPreheatVcfrontCoolantPtInletCx100">-</b></span></div>
<div class="kv"><span>环境温度 当前/过滤</span><span><b id="batteryPreheatVcfrontAmbientCx100">-</b> / <b id="batteryPreheatVcfrontAmbientFilteredCx100">-</b></span></div>
</div>

<div class="card">
<h2>系统</h2>
<div class="kv"><span>运行时间 秒</span><span id="uptime">-</span></div>
</div>

</div>

<script>
let pollTimer=null,recTimer=null,loaded=false,lastDiag=null,autoSaveTimer=null,autoSaveBusy=false,autoSaveQueued=false;
function getStoredTheme(){try{return localStorage.getItem("theme")||""}catch(e){return ""}}
function setStoredTheme(t){try{localStorage.setItem("theme",t)}catch(e){}}
function preferredTheme(){const t=getStoredTheme();if(t==="light"||t==="dark")return t;const h=(new Date()).getHours();return h>=7&&h<19?"light":"dark"}
function applyTheme(t){document.documentElement.setAttribute("data-theme",t);const b=document.getElementById("themeBtn");if(b)b.textContent=t==="light"?"夜间":"日间"}
function toggleTheme(){const cur=document.documentElement.getAttribute("data-theme")==="light"?"light":"dark";const next=cur==="light"?"dark":"light";applyTheme(next);setStoredTheme(next)}
applyTheme(preferredTheme());
function closeLegalNotice(){const e=document.getElementById("legalNotice");if(e)e.classList.add("hidden")}
const cfgIds=["canCommsEnabled","fsdEnabled","fsdActivationProfile","fsdActivationResendEnabled","fsdActivationResendMs","autoSpeedOffsetEnabled","hw4SpeedOffsetPlus15Enabled","hw4SpeedOffset60Enabled","hw4CustomSpeedOffsetEnabled","hw4SlewPctPerSec","hw4IsaChimeSuppressEnabled","enhancedAutopilotEnabled","hw4TargetBelow60","hw4Target60","hw4Target70","hw4Target80","hw4Target90","hw4Target100","hw4Target120","cabinCameraDisableEnabled","slewPctPerSec","lowSpeedMaxPctRaw","targetBelow60","target60","target70","target80","target90","target100","target120","canbEnabled","canbServiceModeEnabled","canbFilterMode","highBeamStrobeEnabled","rearFogBrakeStrobeEnabled","reverseStrobeEnabled","batteryPreheatEnabled","can1ReceiveOnly"];
const statIds={nagKillerMode:"nagKillerModeText"};
function setVal(id,v){const e=document.getElementById(id);if(!e)return;if(e.type==="checkbox")e.checked=!!v;else if(id==="lowSpeedMaxPctRaw")e.value=Math.max(0,Math.min(60,Number(v||0)/4)).toFixed(2);else e.value=v;}
function getVal(e){if(e.type==="checkbox")return e.checked?1:0;if(e.id==="lowSpeedMaxPctRaw")return Math.max(0,Math.min(240,Math.round((Number(e.value)||0)*4)));return e.value}
function showResult(text){const e=document.getElementById("testResult");if(e)e.textContent=text}
function yesNo(v){return v?"是":"否"}
function yesNoUnknown(v){return v===255?"-":yesNo(v)}
function dasLcHandsReason(v){const m={
0:"0 无 / NONE",
1:"1 转角饱和提示 / ANGLE_SATURATION",
2:"2 施工区域提示 / CONSTRUCTION",
3:"3 导航视觉融合提示 / FUSION_NAV",
4:"4 模型视觉融合提示 / FUSION_MODEL",
5:"5 未检测到驾驶员 / DRIVER_NOT_PRESENT",
6:"6 长时间超时 / TIMEOUT_LONG_TERM",
7:"7 高速超时 / TIMEOUT_HI_SPEED",
8:"8 中速超时 / TIMEOUT_MED_SPEED",
9:"9 低速超时 / TIMEOUT_LOW_SPEED",
10:"10 施工超时 / TIMEOUT_CONSTRUCTION",
11:"11 前车切出超时 / TIMEOUT_CUTOUT_CIPV",
12:"12 有前车超时 / TIMEOUT_CIPV",
13:"13 无前车超时 / TIMEOUT_NO_CIPV",
14:"14 受限场景超时 / TIMEOUT_RESTRICTED",
15:"15 加速踏板超时 / TIMEOUT_ACCELERATOR",
16:"16 横向加速度提示 / LATERAL_ACCELERATION",
17:"17 自动变道提示 / ALC",
18:"18 车道线不足提示 / NO_LINES",
19:"19 感知测量降级 / DEGRADED_MEASUREMENTS",
20:"20 可行驶空间越界 / DRIVABLE_SPACE_VIOLATION",
21:"21 静止雷达目标 / STATIONARY_RADAR",
22:"22 收费站 / TOLL_BOOTH",
23:"23 ULC请求 / ULC_REQUEST",
24:"24 法规超时 / TIMEOUT_HOMOLOGATION",
25:"25 走走停停 / STOP_AND_GO",
26:"26 天气较差 / POOR_WEATHER",
27:"27 极端天气提示 / EXTREME_WEATHER",
28:"28 交通控制 / TRAFFIC_CONTROLS",
29:"29 检测到规避装置 / DEFEAT_DEVICE_DETECTED",
30:"30 驾驶员分心提示 / DRIVER_DISTRACTED",
31:"31 能见度差 / POOR_VISIBILITY",
32:"32 关键交通控制 / CRITICAL_TRAFFIC_CONTROL",
33:"33 受限退出 / CAPTIVE_EXIT",
34:"34 警示灯场景 / CAUTION_LIGHTS",
35:"35 封闭汇入警告 / SEALED_MERGE_WARNING",
36:"36 有轨电车 / STREET_CAR",
37:"37 横向误差提示 / REFERENCE_LATERAL_ERROR",
38:"38 扭矩偏移未校准超时 / TORQUE_OFFSET_UNCALIBRATED",
39:"39 驾驶员活跃度不足 / DRIVER_NOT_LIVELY",
40:"40 驾驶员状态未知 / DRIVER_STATE_UNKNOWN",
41:"41 AP开启后分心提示 / DISTRACTED_AFTER_AP_ACTIVATION",
42:"42 即将到达交通控制 / UPCOMING_TRAFFIC_CONTROL",
43:"43 AP开启后手离开 / HANDS_OFF_AFTER_AP_ACTIVATION",
44:"44 即将交通控制提示 / UPCOMING_TRAFFIC_CONTROL_CHIME",
45:"45 弱势交通参与者在路径内 / VRU_IN_DRIVE_SPACE",
46:"46 弱势交通参与者保护 / VRU_FAILSAFE",
47:"47 驾驶员注意力丢失 / DRIVER_LOSS_OF_ATTENTION",
48:"48 注意力分低 / DRIVER_LOW_ATTENTION_SCORE",
49:"49 可能分心 / DRIVER_POSSIBLY_DISTRACTED",
50:"50 AP开启后伸手提示 / REACHING_AFTER_AP_ACTIVATION",
51:"51 太阳镜超时 / TIMEOUT_SUNGLASSES",
52:"52 手未准备好 / DRIVER_HANDS_NOT_READY",
53:"53 注意力监测不可用 / ATTN_MONITORING_UNAVAILABLE"};
if(v===255)return "未解码 / Not decoded";return m[v]||((v>>>0)+" 未知 / Unknown")}
function preheatBlock(v){const a=[];if(v&1)a.push("开关关闭");if(v&2)a.push("CANB关闭");if(v&4)a.push("MCP2515未就绪");if(v&8)a.push("SOC低于5%");if(v&16)a.push("检测到充电");if(v&32)a.push("最高温>=45°C");if(v&64)a.push("平均温>=42°C稳定10秒");if(v&128)a.push("运行超过15分钟");if(v&256)a.push("CAN通讯关闭");return a.length?a.join("；"):"无阻止"}
function cx100(v){return v<=-32000?"-":(v/100).toFixed(2)+" ℃"}
function durMs(v){v=Number(v)||0;if(v<1000)return v+" ms";if(v<60000)return (v/1000).toFixed(1)+" 秒";return (v/60000).toFixed(1)+" 分钟"}
function ageMs(v){v=Number(v)||0;return v===0?"刚刚":durMs(v)+"前"}
function canBus(v){return v===1?"bus=1":(v===2?"bus=2":v)}
function us(v){return (v>>>0)+" us"}
function kb(v){return Math.round((v||0)/1024)+" KB"}
function asAge(v){return Number(v)===4294967295?"未收到":ageMs(v)}
function asBool(v){v=Number(v);return v===255?"未收到":(v?"是":"否")}
function asMap(v,m){v=Number(v);return v===255?"未收到":(m[v]||String(v))}
function asSpeed(v){v=Number(v);return v<=-32000?"未收到":(v/10).toFixed(1)+" kph"}
function asOk(v,want){v=Number(v);return v!==255&&v===want}
function asNot(v,want){v=Number(v);return v!==255&&v!==want}
function autoShiftAp(v){return asMap(v,["0 DISABLED","1 UNAVAILABLE","2 AVAILABLE","3 ACTIVE_NOMINAL","4 ACTIVE_RESTRICTED","5 ACTIVE_NAV","6 ACTIVE_FSD"])}
function autoShiftGear(v){return asMap(v,["0 INVALID","1 P","2 R","3 N","4 D",,,,, "7 SNA"])}
function autoShiftDir(v){return asMap(v,["0 NONE","1 REVERSE","2 DRIVE","3 SNA"])}
function setDiagPage(on){const main=document.getElementById("mainPage"),diag=document.getElementById("diagPage");if(main)main.classList.toggle("hidden",on);if(diag)diag.classList.toggle("hidden",!on);if(on){const p=document.getElementById("poll");if(p&&!p.checked){p.checked=true;setPolling(true)}window.scrollTo(0,0)}}
function fmtStat(k,v){
if(k==="diagWindowMs")return v+" ms";
if(k==="cpuMhz")return v+" MHz";
if(["cpuPct","cpu0Pct","cpu1Pct","loopBusyPct","freeHeapPct","minFreeHeapPct"].includes(k))return v+"%";
if(["loopAvgUs","loopMaxUs","loopWaitAvgUs","loopPeriodMaxUs","webTaskMaxUs","can1RxGapMaxUs","can1TxMaxUs","canbRxGapMaxUs","canbTxMaxUs","canbDrainMaxUs"].includes(k))return us(v);
if(["totalHeapBytes","freeHeapBytes","minFreeHeapBytes"].includes(k))return kb(v);
if(["can1RxRate","can1TxRate","can1TxFailRate","canbRxRate","canbTxRate","canbTxFailRate"].includes(k))return v+"/s";
if(k==="canbLastId"||k==="canbErrorFlags"||k==="nagKillerTargetId")return v?("0x"+(v>>>0).toString(16).toUpperCase()):"-";
if(k==="fsdActivationResendActive")return yesNo(v);
if(k==="fsdActivationResendCachedMuxMask")return (v&1)?"已缓存":"-";
if(k==="fsdActivationResendPeriodMs")return v+" ms";
if(k==="fsdActivationResendLastTxAgeMs")return ageMs(v);
if(k.startsWith("autoShift")&&k.endsWith("AgeMs"))return asAge(v);
if(["autoShiftUiEnableFsd","autoShiftUiHasFsd","autoShiftUiFsdSuspended","autoShiftUiMux0Valid","autoShiftUiStartFromPark","autoShiftUiStartFromParkBrakeConfirm","autoShiftUiMux2Valid","autoShiftGtwGearStripEnable","autoShiftDiFrontBlocked","autoShiftDiRearBlocked","autoShiftDasBrakeRequired","autoShiftDiAccelPedalPressed","autoShiftW330CabinCameraFault","autoShiftW372CabinCameraBlocked","autoShiftW391CabinCameraBlockedOnAp","autoShiftW393AttnMntrUnavailable"].includes(k))return asBool(v);
if(k==="autoShiftGtwSmartShiftStatus")return asMap(v,{1:"1 ENABLED"});
if(k==="autoShiftGtwPrimaryGearControlStatus")return asMap(v,{0:"0 NOT_QUALIFIED",1:"1 QUALIFIED"});
if(k==="autoShiftDiSmartShiftUnavailableReason")return asMap(v,{0:"0 SNA",1:"1 NONE",2:"2 UI_DISABLE",5:"5 CHARGE_CABLE",6:"6 CLOSURE_OPEN",8:"8 GTW_NOT_QUALIFIED"});
if(k==="autoShiftDiSmartShiftGear")return asMap(v,{0:"0 NONE",1:"1 DRIVE",2:"2 REVERSE"});
if(k==="autoShiftDasUpcomingDirection")return autoShiftDir(v);
if(k==="autoShiftDasTorqueDirection")return asMap(v,{0:"0 SNA",1:"1 NOT_CONFIDENT",2:"2 REVERSE",3:"3 FORWARD",4:"4 NOT_READY"});
if(k==="autoShiftDiGear")return autoShiftGear(v);
if(k==="autoShiftDiBrakePedalState")return asMap(v,{0:"0 OFF",1:"1 ON",2:"2 INVALID"});
if(k==="autoShiftDiSystemState")return asMap(v,{0:"0 UNAVAILABLE",1:"1 IDLE",2:"2 STANDBY",3:"3 FAULT",4:"4 ABORT",5:"5 ENABLE"});
if(k==="autoShiftDiVehicleSpeedCx10")return asSpeed(v);
if(k==="autoShiftDriverBuckleStatus")return asMap(v,{0:"0 未系",1:"1 已系"});
if(k==="autoShiftDriverDoorStatus")return asMap(v,{0:"0 打开",1:"1 关闭"});
if(k==="autoShiftDriverUnbuckled")return asMap(v,{0:"0 无",1:"1 有人未系",2:"2 SNA"});
if(k==="autoShiftDasAutopilotState")return autoShiftAp(v);
if(k==="autoShiftDasLssState")return asMap(v,{0:"0 FAULT",1:"1 LDW",2:"2 LKA",3:"3 ELK",4:"4 MONITOR",6:"6 ABORT",7:"7 OFF"});
if(k==="autoShiftDasFleetSpeedState")return asMap(v,{0:"0 UNAVAILABLE",1:"1 AVAILABLE",2:"2 ACTIVE",3:"3 HOLD"});
if(k==="autoShiftDasHandsOnState")return asMap(v,{0:"0 NOT_REQD",1:"1 REQD_DETECTED",2:"2 NOT_DETECTED",3:"3 VISUAL",4:"4 CHIME_1",5:"5 CHIME_2",6:"6 SLOWING",7:"7 STRUCK_OUT",15:"15 SNA"});
if(k==="autoShiftDasPmmSysFaultReason")return asMap(v,{0:"0 NONE",1:"1 DAS_DISABLED",2:"2 SPEED",3:"3 DI_FAULT",4:"4 STEER_RATE",5:"5 USER_DISABLED",6:"6 ROAD_TYPE",7:"7 BRAKE_INHIBIT"});
if(k==="autoShiftDasCsaState")return asMap(v,{0:"0 UNAVAILABLE",1:"1 AVAILABLE",2:"2 ENABLE",3:"3 HOLD"});
if(k==="autoShiftDasRobState")return asMap(v,{0:"0 INHIBITED",1:"1 MEASURE",2:"2 ACTIVE",3:"3 MAPLESS"});
if(k==="autoShiftDasDriverInteractionLevel")return asMap(v,{0:"0 DRIVER_INTERACTING",1:"1 NOT_INTERACTING",2:"2 CONTINUED_NOT_INTERACTING"});
if(k==="hw4OffsetTargetPct"||k==="hw4OffsetSentPct")return v+"%";
if(k==="offsetRaw")return (Number(v||0)/4).toFixed(1)+"%";
if(k==="dasLcHandsOnReasonSeen")return yesNo(v);
if(k==="dasLcHandsOnReasonDecode")return ["已解码 / Decoded","未收到0x5D9 / No 0x5D9","DLC不足 / Bad DLC","缺DBC bit定义 / Missing bit layout","值超范围 / Invalid value"][v]||v;
if(k==="dasLcHandsOnReasonPrevious"||k==="dasLcHandsOnReasonLatest")return dasLcHandsReason(v);
if(k==="dasLcHandsOnReasonPreviousAgeMs"||k==="dasLcHandsOnReasonLatestAgeMs")return Number(v)===4294967295?"-":ageMs(v);
if(k==="batteryPreheatActive"||k==="batteryPreheatAutoOffLatched"||k==="batteryPreheatChargeDetected"||k==="batteryPreheatFeedbackSeen"||k==="batteryPreheatBms312Seen"||k==="batteryPreheatBms3b2Seen"||k==="batteryPreheatVcfrontSeen")return yesNo(v);
if(k==="batteryPreheatBlockMask")return preheatBlock(v);
if(k==="batteryPreheatRunMs"||k==="batteryPreheatStableTempMs")return durMs(v);
if(k==="batteryPreheatAgeMs"||k==="batteryPreheatFeedbackAgeMs"||k==="batteryPreheatSocAgeMs"||k==="batteryPreheatChargeStatusAgeMs"||k==="batteryPreheatBms312AgeMs"||k==="batteryPreheatBms3b2AgeMs"||k==="batteryPreheatVcfrontAgeMs"||k==="bmsTempDecodedAgeMs")return ageMs(v);
if(k==="batteryPreheatAutoOffReason")return ["无","平均温度到42°C","最高温到45°C","开始充电","运行15分钟","手动关闭","电量低于5%"][v]||v;
if(k==="batteryPreheatSocUiDeciPct")return v<0?"未知":((v/10).toFixed(1)+"%");
if(k==="batteryPreheatFeedbackBus"||k==="batteryPreheatChargeStatusBus"||k==="batteryPreheatSocBus"||k==="batteryPreheatBms312Bus"||k==="batteryPreheatBms3b2Bus"||k==="batteryPreheatVcfrontBus")return canBus(v);
if(k==="batteryPreheatChargeStatus")return ["0 DISCONNECTED","1 NO_POWER","2 ABOUT_TO_CHARGE","3 CHARGING","4 CHARGE_COMPLETE","5 CHARGE_STOPPED","6 CALIBRATING"][v]||"-";
if(k==="batteryPreheatUiTripActive"||k==="batteryPreheatUiNavToSupercharger"||k==="batteryPreheatUiRequestHeat")return yesNo(v);
if(k==="batteryPreheatHeatingActive")return yesNoUnknown(v);
if(k==="batteryPreheatUiFastChargerType"){if(v===0)return "0 无";if(v===1)return "1 Low";if(v===2)return "2 V2";if(v===3)return "3 V3";if(v===4)return "4 V4";return v+" 未定义"}
if(k==="batteryPreheatUiState")return ["0 被动加热","1 主动加热","2 被动冷却","3 主动冷却"][v]||v;
if(k==="batteryPreheatBmsHeatStatus")return ["未收到新鲜BMS热管理帧","已收到BMS帧，缺DBC位定义，暂不判断"][v]||v;
if(k==="batteryPreheatUiPowerW"||k==="batteryPreheatCmdPowerW")return v<=-32000?"-":(v+" W");
if(k==="batteryPreheatUiTargetCx100"||k==="batteryPreheatCmdTargetCx100"||k==="batteryPreheatVcfrontCoolantBatInletCx100"||k==="batteryPreheatVcfrontCoolantPtInletCx100"||k==="batteryPreheatVcfrontAmbientCx100"||k==="batteryPreheatVcfrontAmbientFilteredCx100"||k==="bmsTempMinCx100"||k==="bmsTempAvgCx100"||k==="bmsTempMaxCx100")return cx100(v);
if(k==="dndActionType")return ["none","volume"][v]||v;
if(k==="dndBlocked")return ["ok","disabled","canb","no_cache"][v]||v;
if(k==="nagKillerSteeringDegCx10")return v<=-32000?"-":(v/10).toFixed(1)+"°";
if(k==="nagKillerDndLastTriggerAgeMs"||k==="dndLastTriggerAgeMs"||k==="dndScrollCacheAgeMs")return ageMs(v);
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
const cpu0Pct=val("cpu0Pct");
const cpu1Pct=val("cpu1Pct");
const freeHeapPct=val("freeHeapPct");
const freeHeapBytes=val("freeHeapBytes");
if(cpuPct>90)add(2,"CPU占用超过90%");
else if(cpuPct>70)add(1,"CPU占用超过70%");
if(cpu0Pct>90)add(2,"CPU0占用超过90%");
else if(cpu0Pct>70)add(1,"CPU0占用超过70%");
if(cpu1Pct>90)add(2,"CPU1占用超过90%");
else if(cpu1Pct>70)add(1,"CPU1占用超过70%");
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
if(inc("canbTxSchedDrop")>0)add(2,"MCP2515 TX调度队列丢弃增加");
if(inc("canbTxSchedExpired")>0)add(1,"MCP2515 TX调度队列过期增加");
if(inc("canbTxSchedFail")>0)add(1,"MCP2515 TX调度发送失败增加");
if(inc("canbTxSchedBudgetHit")>0)add(1,"MCP2515 TX调度单轮预算触顶");
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
function updateAutoShiftDiag(j){
const bad=[],warn=[];
const n=k=>Number(j[k]);
const need=(k,want,label)=>{const v=n(k);if(v===255||Number.isNaN(v))warn.push(label+" 未收到");else if(v!==want)bad.push(label+"="+fmtStat(k,v))};
const oneOf=(k,vals,label)=>{const v=n(k);if(v===255||Number.isNaN(v))warn.push(label+" 未收到");else if(!vals.includes(v))bad.push(label+"="+fmtStat(k,v))};
need("autoShiftGtwSmartShiftStatus",1,"GTW Smart Shift");
need("autoShiftGtwPrimaryGearControlStatus",1,"GTW 换挡资格");
need("autoShiftGtwGearStripEnable",1,"GTW gear strip");
need("autoShiftDiSmartShiftUnavailableReason",1,"DI Smart Shift原因");
oneOf("autoShiftDiSmartShiftGear",[1,2],"DI 推荐挡位");
need("autoShiftDiFrontBlocked",0,"前方阻挡");
need("autoShiftDiRearBlocked",0,"后方阻挡");
oneOf("autoShiftDasUpcomingDirection",[1,2],"DAS即将换挡方向");
oneOf("autoShiftDasTorqueDirection",[2,3],"DAS扭矩方向");
oneOf("autoShiftDiGear",[1,2,4],"DI 当前挡位");
oneOf("autoShiftDiSystemState",[1,2,5],"DI系统状态");
need("autoShiftUiEnableFsd",1,"UI FSD启用");
need("autoShiftUiHasFsd",1,"UI FSD权限");
need("autoShiftUiFsdSuspended",0,"UI FSD暂停");
need("autoShiftUiStartFromPark",1,"UI P档启动FSD");
oneOf("autoShiftDasAutopilotState",[2,3,4,5,6],"DAS AP状态");
oneOf("autoShiftDasHandsOnState",[0,1],"DAS hands-on");
need("autoShiftDasPmmSysFaultReason",0,"DAS PMM故障");
oneOf("autoShiftDasCsaState",[1,2,3],"DAS CSA");
oneOf("autoShiftDasRobState",[1,2,3],"DAS ROB");
oneOf("autoShiftDasDriverInteractionLevel",[0,1],"DAS驾驶员交互");
need("autoShiftW330CabinCameraFault",0,"摄像头故障");
need("autoShiftW372CabinCameraBlocked",0,"摄像头遮挡");
need("autoShiftW391CabinCameraBlockedOnAp",0,"AP摄像头遮挡");
need("autoShiftW393AttnMntrUnavailable",0,"注意力监控不可用");
const e=document.getElementById("autoShiftDiagSummary");if(!e)return;
if(bad.length){e.textContent="不满足："+bad.slice(0,6).join("；")+(bad.length>6?"；…":"");e.style.color="#f66"}
else if(warn.length){e.textContent="未完整："+warn.slice(0,6).join("；")+(warn.length>6?"；…":"");e.style.color="#ffd479"}
else{e.textContent="通过：核心自动换挡链路未发现阻断";e.style.color="#9f9"}
}
function hw4DrivingProfileText(v){return ["0 Chill","1 Normal","2 Hurry","3 Max","4 Sloth"][Number(v)]||"-"}
function updateHw4CustomUi(){const profile=document.getElementById("fsdActivationProfile"),custom=document.getElementById("hw4CustomSpeedOffsetEnabled"),fields=document.getElementById("hw4CustomSpeedOffsetFields");if(fields)fields.style.display=profile&&Number(profile.value)===1&&custom&&custom.checked?"":"none"}
function updateFsdProfileUi(profile){const isV14=Number(profile)===1;["hw4ProfileRow","hw4ProfileControls"].forEach(id=>{const e=document.getElementById(id);if(e)e.style.display=isV14?"":"none"});["hw3AutoSpeedOffsetRow","hw3SpeedOffsetFields"].forEach(id=>{const e=document.getElementById(id);if(e)e.style.display=isV14?"none":""});updateHw4CustomUi()}
function setHw4ProfileButtons(profile){document.querySelectorAll("[data-hw4-profile]").forEach(b=>b.classList.toggle("active",Number(b.dataset.hw4Profile)===Number(profile)))}
function selectHw4Profile(profile){const pf=document.getElementById("hw4DrivingProfileText"),p=new URLSearchParams();if(pf)pf.textContent=hw4DrivingProfileText(profile);setHw4ProfileButtons(profile);showResult("正在自动保存...");p.set("hw4DrivingProfileSetting",profile);fetch("/config",{method:"POST",body:p}).then(r=>{if(!r.ok)throw new Error("config");return fetch("/save",{method:"POST"})}).then(r=>{if(!r.ok)throw new Error("save");showResult("已自动保存");pollStatus()}).catch(()=>{showResult("自动保存失败");pollStatus()})}
function updateHw4Profile(j){updateFsdProfileUi(j.fsdActivationProfile);if(Number(j.fsdActivationProfile)!==1)return;const fd=document.getElementById("hw4FollowDistanceText"),pf=document.getElementById("hw4DrivingProfileText");const d=Number(j.hw4FollowDistance||0),p=Number(j.hw4DrivingProfile);if(fd)fd.textContent=d>0?String(d):"-";if(pf)pf.textContent=hw4DrivingProfileText(p);setHw4ProfileButtons(p)}
function updateStats(j){Object.keys(j).forEach(k=>{const e=document.getElementById(statIds[k]||k);if(e&&e.tagName!=="INPUT"&&e.tagName!=="SELECT")e.textContent=fmtStat(k,j[k])})}
function pollStatus(){fetch("/status").then(r=>r.json()).then(j=>{if(!loaded){cfgIds.forEach(k=>{if(k in j)setVal(k,j[k])});loaded=true}updateStats(j);updateHw4Profile(j);updateAutoShiftDiag(j);updateAutoDiag(j)}).catch(()=>{})}
function setPolling(on){if(on&&!pollTimer){pollStatus();pollTimer=setInterval(pollStatus,1000)}if(!on&&pollTimer){clearInterval(pollTimer);pollTimer=null}}
function body(){const p=new URLSearchParams();cfgIds.forEach(k=>{const e=document.getElementById(k);if(e)p.set(k,getVal(e))});return p}
function scheduleAutoSave(){if(!loaded)return;clearTimeout(autoSaveTimer);showResult("正在自动保存...");autoSaveTimer=setTimeout(saveConfig,700)}
function bindAutoSave(){cfgIds.forEach(k=>{const e=document.getElementById(k);if(!e||e.type==="hidden")return;const ev=(e.type==="checkbox"||e.tagName==="SELECT")?"change":"input";e.addEventListener(ev,scheduleAutoSave);if(ev==="input")e.addEventListener("change",scheduleAutoSave)});const p=document.getElementById("fsdActivationProfile"),s15=document.getElementById("hw4SpeedOffsetPlus15Enabled"),s60=document.getElementById("hw4SpeedOffset60Enabled"),custom=document.getElementById("hw4CustomSpeedOffsetEnabled");if(p)p.addEventListener("change",()=>updateFsdProfileUi(p.value));if(s15)s15.addEventListener("change",()=>{if(s15.checked){if(s60)s60.checked=false;if(custom)custom.checked=false}updateHw4CustomUi()});if(s60)s60.addEventListener("change",()=>{if(s60.checked){if(s15)s15.checked=false;if(custom)custom.checked=false}updateHw4CustomUi()});if(custom)custom.addEventListener("change",()=>{if(custom.checked){if(s15)s15.checked=false;if(s60)s60.checked=false}updateHw4CustomUi()})}
function saveConfig(){if(autoSaveBusy){autoSaveQueued=true;return}autoSaveBusy=true;fetch("/config",{method:"POST",body:body()}).then(r=>{if(!r.ok)throw new Error("config");return fetch("/save",{method:"POST"})}).then(r=>{if(!r.ok)throw new Error("save");showResult("已自动保存");pollStatus()}).catch(()=>showResult("自动保存失败")).finally(()=>{autoSaveBusy=false;if(autoSaveQueued){autoSaveQueued=false;scheduleAutoSave()}})}
function rebootBoard(){setPolling(false);const p=document.getElementById("poll");if(p)p.checked=false;showResult("正在重启...");fetch("/reboot",{method:"POST"}).catch(()=>{})}
function recQuery(){const ids=document.getElementById("recIds").value.trim();return ids?("?ids="+encodeURIComponent(ids)):""}
function stopReason(v){return v===1?"满":(v===2?"超时":"手动/无")}
function recMemText(j){if(!j)return "-";const mem=Number(j.mem||0);const src=mem===1?"PSRAM":(mem===2?"内部RAM":"未分配");const ps=j.psramReady?"PSRAM已识别":"PSRAM未识别";return src+" / "+ps+" / "+Math.round((j.bytes||0)/1024)+" KB"}
function setRecUi(j){const active=j&&j.active;document.getElementById("recState").textContent=active?"抓包中":(j&&j.saved?"已保存":"空闲");document.getElementById("recCount").textContent=j?(j.count+" / "+j.cap):"-";document.getElementById("recBus1").textContent=j?j.bus1:"-";document.getElementById("recBus2").textContent=j?j.bus2:"-";document.getElementById("recDrop").textContent=j?(j.dropped+" / "+stopReason(j.stopReason)):"-";document.getElementById("recPsram").textContent=recMemText(j);document.getElementById("recDownload").style.display=(!active&&j&&j.saved)?"inline-block":"none"}
function pollRec(){fetch("/rec_status").then(r=>r.json()).then(setRecUi).catch(()=>{})}
function startRec(){fetch("/rec_start"+recQuery(),{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(!recTimer)recTimer=setInterval(pollRec,800)})}
function stopRec(){fetch("/rec_stop",{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(recTimer){clearInterval(recTimer);recTimer=null}})}
bindAutoSave();pollStatus();
</script>
</body></html>)HTML";
