/* Workbench is a client of the same C services used by the device screen. */
let workReady = false, workFileKey = "", workTxDirty = false, workTxRevision = 0, workGeneration = -1, workTxCh = "0", workTxIndex = "0";
function workFeedback(message, bad = false) {
  const el = $("#feedback");
  el.textContent = message;
  el.classList.toggle("bad", bad);
}
function workTab(name) {
  document.querySelectorAll("[data-tab]").forEach(b => b.classList.toggle("on", b.dataset.tab === name));
  document.querySelectorAll("[data-pane]").forEach(p => p.hidden = p.dataset.pane !== name);
}
function selectedTx() {
  return S && S.work && S.work.tx[+$("#txCh").value] && S.work.tx[+$("#txCh").value][+$("#txEntry").value];
}
function txDataState() {
  const raw = $("#txData").value.trim();
  const groups = raw ? raw.split(/\s+/) : [];
  const valid = groups.every(g => /^[0-9a-f]+$/i.test(g) && g.length % 2 === 0);
  return {valid, count:groups.reduce((n,g) => n + Math.floor(g.length / 2), 0), raw};
}
function txValidation() {
  const fd = $("#txFd").checked, rtr = $("#txRtr").checked;
  const length = +$("#txLen").value, expected = rtr ? 0 : length;
  const data = txDataState(), idText = $("#txId").value.trim();
  const id = /^(?:0x)?[0-9a-f]{1,8}$/i.test(idText) ? Number.parseInt(idText.replace(/^0x/i,""),16) : NaN;
  const extended = $("#txFormat").value === "1";
  const periodText = $("#txPeriod").value, period = Number(periodText);
  const current = selectedTx(), reasons = [];
  if (!Number.isFinite(id)) reasons.push("ID 需为完整的十六进制数");
  else if (id > (extended ? 0x1FFFFFFF : 0x7FF)) reasons.push(extended ? "扩展 ID 不能超过 1FFFFFFF" : "标准 ID 不能超过 7FF");
  if (!periodText || !Number.isInteger(period) || period < 0 || period > 65535) reasons.push("周期需为 0～65535 ms 的整数");
  const lens = fd ? [0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64] : [0,1,2,3,4,5,6,7,8];
  if (!lens.includes(length)) reasons.push("请选择该帧协议支持的数据长度");
  if (!rtr && !data.valid) reasons.push("数据需为完整十六进制字节，例如 AA BB");
  else if (!rtr && data.count !== expected) reasons.push(`数据还需匹配 ${expected} 字节（当前 ${data.count}）`);
  if (!current) reasons.push("正在读取发送列表");
  else if (current.active) reasons.push("该条正在周期发送；草稿已保留，先停止周期再保存");
  else if (current.revision !== workTxRevision) reasons.push("设备条目已变化；先读取当前条目，再重新编辑");
  if (S && S.dev && S.dev.power) {
    if (S.dev.power.sid !== 4) reasons.push("请先开机后保存");
    else if (S.dev.power.locked) reasons.push("请先解除设备按键锁");
    else if (S.dev.ui && S.dev.ui.dlg && S.dev.ui.dlg.kind) reasons.push("请先处理设备屏幕的提示");
  }
  return {fd,rtr,length,expected,data,id,idText,extended,period,current,reasons};
}
function updateTxHints() {
  const v = txValidation();
  const hint = $("#txByteHint"), preview = $("#txPreview");
  if (hint) {
    hint.textContent = v.rtr ? `RTR 不携带数据，DLC 请求长度 ${v.length}；网页原数据保留但不会发送。` :
      `已输入 ${v.data.count} / ${v.expected} 字节${v.data.valid ? "" : " · 含未完成或无效字节"}${workTxDirty ? " · 未保存草稿" : " · 与当前条目同步"}`;
    if (v.reasons.length) hint.textContent += " · " + v.reasons[0];
    hint.classList.toggle("bad", !!v.reasons.length);
  }
  if (preview) {
    const ident = Number.isFinite(v.id) ? "0x" + v.id.toString(16).toUpperCase().padStart(v.extended ? 8 : 3,"0") : "ID 待完成";
    preview.textContent = `CAN${+$("#txCh").value + 1} · 第 ${+$("#txEntry").value + 1} 条 · ${v.extended ? "扩展" : "标准"} ${ident} · ${v.fd ? "CAN FD" : "Classic CAN"}${$("#txBrs").checked ? " / BRS" : ""} · ${v.rtr ? "RTR" : v.length + " B"} · ${v.period ? v.period + " ms 周期" : "仅单发"} · 保存不会启动发送`;
  }
  const save = $("#txSave") || document.querySelector('#txForm button[type="submit"]');
  if (save) { save.disabled = !!v.reasons.length; save.title = v.reasons.join("；"); }
  for (const id of ["#txFillZero","#txFillIncrement"]) {
    const b = $(id); if (b) { b.disabled = v.rtr || !v.length; b.title = v.rtr ? "RTR 没有数据载荷" : "只填网页草稿，不保存、不发送"; }
  }
  const device = $("#txDevice");
  if (device) { device.disabled = !!(v.current && v.current.active); device.title = device.disabled ? "先停止该条周期发送，再编辑" : "打开设备编辑器，不直接发送"; }
}
function txLengths(preferred) {
  const fd = $("#txFd").checked, rtr = $("#txRtr").checked;
  const old = preferred === undefined ? +$("#txLen").value : preferred;
  const lens = fd ? [0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64] : [0,1,2,3,4,5,6,7,8];
  $("#txLen").innerHTML = lens.map(n => `<option value="${n}">${n} 字节</option>`).join("");
  $("#txLen").value = lens.includes(old) ? old : 8;
  $("#txBrs").disabled = !fd;
  if (!fd) $("#txBrs").checked = false;
  $("#txRtr").disabled = fd;
  if (fd) $("#txRtr").checked = false;
  $("#txData").disabled = rtr && !fd;
  updateTxHints();
}
function loadTx() {
  const w = S && S.work;
  if (!w) return;
  const e = w.tx[+$("#txCh").value][+$("#txEntry").value];
  $("#txId").value = e.id.toString(16).toUpperCase();
  $("#txFormat").value = e.flags & 1 ? "1" : "0";
  $("#txFd").checked = !!(e.flags & 2);
  $("#txBrs").checked = !!(e.flags & 4);
  $("#txRtr").checked = !!(e.flags & 16);
  txLengths(e.len);
  $("#txData").value = e.data;
  $("#txPeriod").value = e.period;
  workTxRevision = e.revision;
  workTxCh = $("#txCh").value;
  workTxIndex = $("#txEntry").value;
  workTxDirty = false;
  updateTxHints();
}
function loadFilter() {
  const f = S && S.work && S.work.filter;
  if (!f) return;
  $("#filterCh").value = f.channel;
  $("#filterType").value = f.type;
  $("#filterId").value = f.id.toString(16).toUpperCase();
  $("#filterMask").value = f.mask.toString(16).toUpperCase();
  $("#filterDir").value = f.direction;
  $("#filterFormat").value = f.format;
}
function renderWorkbench(s) {
  if (!s.work) return;
  if (workGeneration !== s.sim.generation) {
    workReady = false;
    workGeneration = s.sim.generation;
  }
  if (!workReady) { workReady = true; loadTx(); loadFilter(); }
  const w = s.work;
  const current = w.tx[+$("#txCh").value][+$("#txEntry").value];
  if (!workTxDirty && current.revision !== workTxRevision) loadTx();
  updateTxHints();
  $("#fileLimit").textContent = `导入 .log 使用 can0→CAN1、can1→CAN2。每份最多 ${w.limits.frames} 帧，共 ${w.limits.files} 个日志样例槽；容量用尽会明确提示。录制可继续计数，导出只包含已保存的样例。`;
  const k = JSON.stringify(w.files.map(f => [f.index, f.token, f.name, f.sample_available, f.is_session, f.writing, f.renamable]));
  if (k !== workFileKey) {
    workFileKey = k;
    $("#fileList").innerHTML = w.files.map(f => `<article class="filecard"><strong>${esc(f.name)}</strong><div class="meta" data-file-meta="${f.index}"></div><div class="row">${f.writing ? '<span class="muted">正在写入，停止并命名后可导出样例</span>' : `${f.sample_available ? `<a href="/api/log/export?index=${f.index}" download="capture-${f.index}.log">下载已保存样例 .log</a>` : ""}`}</div></article>`).join("") || '<div class="worknote">模拟 SD 卡上没有文件。</div>';
  }
  for (const f of w.files) {
    const source = f.imported ? "导入日志" : f.is_session ? "录制样例" : "内置演示";
    const sample = f.sample_available ? ` · 可导出 ${f.sample_frames} 帧样例` : " · 无可导出样例";
    const truncated = f.sample_truncated ? ` · 样例已截断，未保留 ${f.sample_dropped} 帧` : "";
    const meta = document.querySelector(`[data-file-meta="${f.index}"]`);
    meta.textContent = `${source} · ${f.is_session&&f.sample_available?"candump 导出样例":f.kind} · 总计 ${f.frames} 帧 · ${f.channels} 路${sample}${truncated}`;
    meta.classList.toggle("err", f.sample_truncated);
  }
}
document.querySelectorAll("[data-tab]").forEach(b => b.onclick = () => { workTab(b.dataset.tab); if (b.dataset.tab === "filter") loadFilter(); });
function changeTxSelection() {
  if (workTxDirty) {
    $("#txCh").value = workTxCh;
    $("#txEntry").value = workTxIndex;
    workFeedback("当前有未保存的报文。请先保存，或点击“读取当前条目”放弃网页草稿。", true);
    return;
  }
  loadTx();
  updateTxHints();
}
$("#txCh").onchange = changeTxSelection;
$("#txEntry").onchange = changeTxSelection;
$("#txForm").addEventListener("input", ev => {
  if (!ev.target.matches("#txCh,#txEntry")) workTxDirty = true;
  updateTxHints();
});
$("#txFd").onchange = () => txLengths();
$("#txRtr").onchange = () => txLengths();
$("#txReload").onclick = () => { loadTx(); workFeedback("已读取当前设备条目；此前未保存的网页草稿已放弃。"); };
$("#txDevice").onclick = async () => {
  if (workTxDirty) { workFeedback("网页草稿尚未保存。请先保存，或读取当前条目放弃草稿后，再打开设备编辑器。", true); return; }
  const r = await api("/api/tx/edit", {ch:+$("#txCh").value, index:+$("#txEntry").value, generation:workGeneration});
  if (!r.failed) workFeedback("已打开设备报文编辑页，方向键调整、OK 选择/保存、BACK 返回。");
};
$("#txForm").onsubmit = async ev => {
  ev.preventDefault();
  const v = txValidation();
  if (v.reasons.length) { updateTxHints(); workFeedback(v.reasons[0], true); return; }
  const flags = +$("#txFormat").value | ($("#txFd").checked ? 2 : 0) | ($("#txBrs").checked ? 4 : 0) | ($("#txRtr").checked ? 16 : 0);
  const result = await api("/api/tx-entry", {ch:+$("#txCh").value, index:+$("#txEntry").value, id:$("#txId").value.trim(), flags, len:+$("#txLen").value, data:$("#txData").disabled ? "" : $("#txData").value, period:+$("#txPeriod").value, revision:workTxRevision, generation:workGeneration});
  if (!result.failed) { workTxDirty = false; loadTx(); workFeedback("报文已保存。可以到设备的“自定义发送”页确认发送。"); }
};
function fillTxData(increment) {
  const len = +$("#txLen").value;
  if ($("#txRtr").checked || !len) return;
  $("#txData").value = Array.from({length:len}, (_,i) => (increment ? i : 0).toString(16).toUpperCase().padStart(2,"0")).join(" ");
  workTxDirty = true; updateTxHints();
  workFeedback(`已在网页草稿${increment ? "填入递增示例" : "填入全 00"}，尚未保存或发送。`);
}
if ($("#txFillZero")) $("#txFillZero").onclick = () => fillTxData(false);
if ($("#txFillIncrement")) $("#txFillIncrement").onclick = () => fillTxData(true);
function revealTxWorkbench() {
  workTab("tx");
  const box = $("#workbench");
  if (!box) return;
  box.hidden = false;
  const disclosure = box.closest("details"); if (disclosure) disclosure.open = true;
  if ("open" in box) box.open = true;
  box.dispatchEvent(new CustomEvent("workbench:open", {bubbles:true, detail:{tab:"tx"}}));
  box.scrollIntoView({behavior:"smooth", block:"nearest"});
}
function prefillTxFromDetail(detail) {
  revealTxWorkbench();
  if (workTxDirty) { workFeedback("当前有未保存的网页草稿。请先保存，或读取当前条目后再复制报文。", true); return false; }
  const flags = detail && Number(detail.flags), len = detail && Number(detail.len), ident = detail && Number(detail.id);
  if (!detail || !Number.isInteger(flags) || flags < 0 || flags > 127 || (flags & 64) || detail.err || detail.error) {
    workFeedback("错误记录不能复制为发送报文。", true); return false;
  }
  const fd = !!(flags & 2), rtr = !!(flags & 16), extended = !!(flags & 1);
  const legal = fd ? [0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64] : [0,1,2,3,4,5,6,7,8];
  const expected = rtr ? 0 : len;
  const complete = Array.isArray(detail.data) && detail.data.length >= expected && detail.data.slice(0,expected).every(n => Number.isInteger(n) && n >= 0 && n <= 255);
  if (detail.valid === false || detail.truncated || detail.data_truncated ||
      (Number.isInteger(detail.available_len) && detail.available_len < expected) || !complete || !legal.includes(len) || (fd && rtr) || (!fd && !!(flags & 12)) ||
      !Number.isInteger(ident) || ident < 0 || ident > (extended ? 0x1FFFFFFF : 0x7FF)) {
    workFeedback("报文数据不完整或格式不可发送，不能复制；不会用补零冒充完整数据。", true); return false;
  }
  if (!selectedTx()) { workFeedback("发送列表尚未准备好，请稍后重试。", true); return false; }
  loadTx(); /* Capture this target entry's current revision, never the RX frame's. */
  $("#txId").value = ident.toString(16).toUpperCase().padStart(extended ? 8 : 3,"0");
  $("#txFormat").value = extended ? "1" : "0";
  $("#txFd").checked = fd; $("#txBrs").checked = fd && !!(flags & 4); $("#txRtr").checked = rtr;
  txLengths(len);
  $("#txData").value = rtr ? "" : detail.data.slice(0,len).map(n => n.toString(16).toUpperCase().padStart(2,"0")).join(" ");
  /* Copy a received frame as a single-shot draft; repeated sending is explicit. */
  $("#txPeriod").value = "0";
  workTxDirty = true; updateTxHints();
  const source = Number.isInteger(detail.ch) && detail.ch >= 1 && detail.ch <= 2 ? `来源 CAN${detail.ch}，` : "";
  workFeedback(`${source}已复制到 CAN${+$("#txCh").value + 1} 第 ${+$("#txEntry").value + 1} 条网页草稿（仅单发）；未保存、未发送${flags & 8 ? "，来源 ESI 状态不作为发送设置" : ""}。`);
  return true;
}
$("#filterForm").onsubmit = async ev => {
  ev.preventDefault();
  const result = await api("/api/filter", {channel:+$("#filterCh").value, type:+$("#filterType").value, id:$("#filterId").value, mask:$("#filterMask").value, direction:+$("#filterDir").value, format:+$("#filterFormat").value});
  if (!result.failed) workFeedback("显示筛选已应用，录制内容不受影响。屏幕上长按 OK 可以继续调整。");
};
$("#filterReset").onclick = async () => {
  const r = await api("/api/filter", {channel:0,type:0,id:"0",mask:"0",direction:0,format:0});
  if (!r.failed) { loadFilter(); workFeedback("已清除所有显示筛选。"); }
};
$("#logUpload").onchange = async ev => {
  const file = ev.target.files[0];
  if (!file) return;
  if (S && S.work && file.size > S.work.limits.text_bytes) { workFeedback("文件超过模拟器导入容量，请选择较短的日志片段。",true); ev.target.value = ""; return; }
  $("#logText").value = await file.text();
  $("#logName").value = file.name.replace(/[^\w.\-\u4e00-\u9fff]/g,"_").slice(0,40);
};
$("#logExample").onclick = () => {
  $("#logName").value = "dual-can-demo.log";
  $("#logText").value = "(0.000000) can0 123#1122334455667788\n(0.500000) can1 200#01020304\n(1.000000) can0 321##1AABBCCDDEEFF001122334455\n(2.000000) can1 18FF50E5#1020304050607080\n(3.500000) can0 123#8877665544332211\n";
};
$("#importForm").onsubmit = async ev => {
  ev.preventDefault();
  const result = await api("/api/log/import", {name:$("#logName").value, text:$("#logText").value});
  if (!result.failed) workFeedback(`已导入 ${result.frames} 帧，可在文件列表中查看或导出。`);
};
txLengths(8);
if (S) renderWorkbench(S);
