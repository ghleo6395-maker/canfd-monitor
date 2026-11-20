/* Host review controls. Sending still uses the shared C TX FIFO and confirmations. */
let sendChannel = 0, controlGeneration = -1, sequenceDirty = false, sequenceRevision = -1, sequenceChannel = 0;
let recordNameDirty = false, databasePreview = null, databaseSelected = new Set();
const SEQUENCE_STATES = ['已停止','等待设备确认','正在切换通道','循环发送中'];
function controlMessage(text, bad=false) { reviewFeedback(text,bad); workFeedback(text,bad); }
async function controlRequest(path, payload) {
  if(S.dev.power.sid===4&&!S.dev.power.screen&&path!=='/api/database/import'){
    await key(7,true);await key(7,false);
    controlMessage('设备屏幕已唤醒，请再次点击刚才的操作按钮确认。');
    return {failed:true};
  }
  return api(path,{...payload,generation:S.sim.generation});
}
function controlsOpen(tab) { if(tab==='sequence')return openDeviceMenu(0);if(tab==='tx')return editOnDevice(sendChannel,0);if(tab==='record')return openDeviceMenu(3);openTool(tab);$('#workbench').scrollIntoView({behavior:'smooth',block:'start'}); }
function sequenceFor(ch) { return S?.work?.sequence?.[ch]; }
function scopeLabel(mask){return mask===3?'两路':mask===2?'CAN2':'CAN1';}
function sendPageView(d) {
  const mask=d.ui.tx_scope_mask||1,generation=S.sim.generation;
  if(!S?.work?.tx)return '<div class="live-empty">正在读取发送列表…</div>';
  sendChannel=d.ui.tx_channel??sendChannel;
  const scope=S.work.tx_scope||{},plan=S.work.send_plans||{},source=plan.loaded_name||'',loadIndex=d.ui.tx_load_file_index??16;
  const stateFor=ch=>{
    const q=sequenceFor(ch),entries=S.work.tx[ch],running=q?.state===3||entries.some(entry=>entry.active);
    const pending=!running&&([1,2].includes(q?.state)||(!!scope.state&&!!(scope.mask&(1<<ch)))||([2,3].includes(plan.state)&&!!(plan.loaded_mask&(1<<ch))));
    return {q,entries,running,pending,busy:running||pending,label:running?'发送中':pending?'等待确认 / 准备':'已停止'};
  };
  let h='<div class="tx-page tx-unified target-both"><div class="tx-target-bar">';
  for(const ch of [0,1]){
    const state=stateFor(ch);
    h+=`<button type="button" class="tx-channel-toggle ${state.running?'running':state.pending?'pending':''}" data-control="channel-toggle" data-ch="${ch}" data-enabled="${!state.running}" data-send-generation="${generation}" role="switch" aria-checked="${state.running}" aria-label="CAN${ch+1}循环发送${state.running?'已开启':state.pending?'等待确认':'已关闭'}" ${state.pending?'disabled':''}><i></i>CAN${ch+1} · ${state.label}</button>`;
  }
  h+=`<button type="button" class="load-plan ${d.ui.cursor===loadIndex?'sel':''}" data-control="load-plan" data-send-generation="${generation}" title="沿用文件的通道与帧格式；替换文件所含通道的列表，不会自动发送">加载文件</button></div><div class="tx-message-list" data-mask="${mask}" data-generation="${generation}" data-source="${esc(source)}" aria-label="CAN1与CAN2独立发送列表">`;
  for(const bus of [0,1]){
    const {q,entries,running,pending,busy,label}=stateFor(bus),groupIdentity=`data-ch="${bus}" data-send-generation="${generation}"`;
    h+=`<section class="tx-channel-group" data-channel="${bus}"><div class="tx-channel-heading"><b>CAN${bus+1}</b><span class="tx-channel-state ${running?'running':pending?'pending':''}">${label} · ${q?.sent||0} 帧</span><button type="button" data-send-action="group-loop" ${groupIdentity} ${busy?'disabled':''}>本路循环</button><button type="button" data-send-action="group-database" ${groupIdentity} ${busy?'disabled':''}>数据库选帧</button></div>`;
    entries.forEach((entry,index)=>{
      const conf=q?.entries[index],id=entry.id.toString(16).toUpperCase().padStart(entry.flags&1?8:3,'0');
      const format=`${entry.flags&1?'扩展':'标准'} · ${entry.flags&16?'RTR':entry.flags&2?(entry.flags&4?'FD/BRS':'FD'):'CAN'} · ${entry.len}B`;
      const bytes=entry.flags&16?'RTR 无数据':entry.data?entry.data.split(' ').slice(0,8).join(' ')+(entry.len>8?' …':''):'无数据';
      const timing=conf?.enabled?`${conf.repeat_count}次 · 间隔${conf.frame_gap_ms} · 周期${conf.cycle_ms}ms`:'未加入循环 · 时序';
      const fullTiming=`CAN${bus+1} 第${index+1}条：每轮${conf?.repeat_count||1}次，同ID间隔${conf?.frame_gap_ms||0}ms，总周期${conf?.cycle_ms||0}ms；不同ID间隔${q?.id_gap_ms||0}ms`;
      const identity=`data-ch="${bus}" data-index="${index}" data-send-generation="${generation}"`;
      h+=`<div class="tx-message-row ${d.ui.cursor===bus*4+index?'sel':''}"><div class="tx-message-main"><span class="tx-slot">${index+1}</span><strong class="tx-message-id">${id}</strong><span class="tx-message-format">${format}</span><span class="spacer"></span><button type="button" data-send-action="row-single" ${identity} title="只发送CAN${bus+1}第${index+1}条一帧" ${busy?'disabled':''}>单发</button><button type="button" data-send-action="row-edit" ${identity} title="编辑CAN${bus+1}第${index+1}条完整报文" ${busy?'disabled':''}>编辑</button></div><div class="tx-message-values"><code title="${esc(entry.data||'')}">${esc(bytes)}</code><button type="button" class="tx-row-timing" data-send-action="row-timing" ${identity} title="${esc(fullTiming)}" ${busy?'disabled':''}>${esc(timing)} ›</button></div></div>`;
    });
    h+='</section>';
  }
  h+=`</div><div class="tx-unified-footer"><span class="tx-loaded-source" title="${esc(source||'两路分别配置，开关独立启停')}">${esc(source?'已加载：'+source:'两路分别配置，开关独立启停')}</span><button type="button" class="${d.ui.cursor===11?'sel':''}" data-control="save-plan" data-send-generation="${generation}">保存列表</button></div></div>`;
  return h;
}
function txEditorView(ui){
  const button=(index,label,kind)=>{
    const disabled=!ui.items[index]||!!(ui.items[index].f&2);
    return `<button type="button" data-item="${index}" class="${kind} ${ui.cursor===index?'sel':''}" aria-disabled="${disabled}" ${disabled?'disabled':''}>${label}</button>`;
  };
  return `<div class="tx-editor"><div class="tx-editor-toolbar">${button(12,`完整数据 · ${ui.editor.len}B`,'payload')}<span>点字段直接输入</span>${button(10,'保存报文','save')}${button(11,'取消','cancel')}</div>${listView(ui.items,ui.cursor,ui)}</div>`;
}
function liveRecordView(d){
 const r=d.rec,current=S.work.files.find(f=>f.writing),names=['REC / R 短按开始','录制准备中','● 正在录制','正在保存'];
 return `<div class="live-record"><div class="live-record-state"><b class="${r.sid===2?'running':r.sid===1||r.sid===3?'pending':''}">${names[r.sid]}</b><span>${hms(r.elapsed)} · ${r.frames} 帧 · 丢 ${r.drops}</span><span class="file" title="${esc(current?.name||'')}">${esc(r.sid===1||r.sid===2?'REC / R 短按停止':r.sid===3?'请稍候':current?.name||'停止后命名')}</span></div></div>`;
}

function recordPageView(d) {
  const r=d.rec,cur=d.ui.cursor,writing=S?.work?.files.find(f=>f.writing),last=S?.work?.files.filter(f=>f.is_session).at(-1);
  const names=['已停止 / 空闲','正在准备录制','录制中','正在关闭文件'];
  const metric=(label,value,index)=>`<div class="${cur===index?'sel':''}">${label}<b>${value}</b></div>`;
  return `<div class="rec-page"><div class="rec-state"><span class="${r.sid===2?'running':''}">${r.sid===2?'● ':''}${names[r.sid]}</span><small>会话 ${r.session||'—'}</small></div><div class="rec-file" title="${esc((writing||last)?.name||'')}">${esc((writing||last)?.name||'下一次录制将创建新文件')}</div><div class="rec-metrics">${metric('时长',hms(r.elapsed),4)}${metric('记录帧数',r.frames,4)}${metric('分段',r.seg,5)}${metric('丢帧',r.drops,6)}${metric('大小',Math.round((r.bytes||0)/1024)+' KB',5)}</div><p class="rec-key-note">REC / R 短按开始，再短按停止；文件关闭后命名。</p><div class="rec-actions"><button class="${cur===8?'sel':''}" data-control="open-record">默认名前缀</button><button data-control="open-files">本机文件</button></div><div class="rec-config"><button class="${cur===1?'sel':''}" data-item="1" ${r.sid?'disabled':''}>通道：${esc(d.ui.items[1]?.v||'')}</button><button class="${cur===2?'sel':''}" data-item="2" ${r.sid?'disabled':''}>${esc(d.ui.items[2]?.v||'64 MB')} 分段</button><button class="${cur===3?'sel':''}" data-item="3" ${r.sid?'disabled':''}>卡满：${esc(d.ui.items[3]?.v||'停止')}</button></div></div>`;
}
async function controlAction(button) {
  const action=button.dataset.sendAction||button.dataset.control||'';
  if (button.disabled) return;
  if(button.dataset.sendGeneration!==undefined&&(S?.dev.ui.page!=='tx'||S.sim.generation!==Number(button.dataset.sendGeneration)||button.isConnected===false)){
    controlMessage('发送页面已变化，请重新操作。',true);return;
  }
  if(action.startsWith('row-')){
    const ch=Number(button.dataset.ch),index=Number(button.dataset.index),generation=Number(button.dataset.sendGeneration);
    if(S?.dev.ui.page!=='tx'||S.sim.generation!==generation||button.isConnected===false||!Number.isInteger(ch)||ch<0||ch>1||!Number.isInteger(index)||index<0||index>3){controlMessage('报文列表已变化，请重新选择该行。',true);return;}
    if(action==='row-edit')await editOnDevice(ch,index);
    else if(action==='row-timing')await openDeviceMenu(1,ch,index);
    else if(action==='row-single'){
      const result=await controlRequest('/api/tx/scope/control',{action:'single',mask:1<<ch,index});
      if(!result.failed)controlMessage(result.pending_confirmation?`请确认CAN${ch+1}第${index+1}条单发权限。`:`已提交CAN${ch+1}第${index+1}条单发请求。`);
    }
  } else if(action==='group-loop'||action==='group-database'){
    const ch=Number(button.dataset.ch);
    if(!Number.isInteger(ch)||ch<0||ch>1){controlMessage('通道无效，请重新操作。',true);return;}
    await openDeviceMenu(action==='group-loop'?0:2,ch);
  } else if(action==='channel-toggle'){
    const ch=Number(button.dataset.ch),enabled=button.dataset.enabled==='true';
    if(!Number.isInteger(ch)||ch<0||ch>1||!['true','false'].includes(button.dataset.enabled)){controlMessage('发送开关已变化，请重新操作。',true);return;}
    const result=await controlRequest('/api/tx/channel',{ch,enabled});
    if(!result.failed)controlMessage(enabled?(result.pending_confirmation?`请在本机确认CAN${ch+1}发送权限。`:`已提交CAN${ch+1}开启请求；实际发送后开关才点亮。`):`已提交CAN${ch+1}停止请求。`);
  } else if (action==='scope') {
    const r=await controlRequest('/api/tx/scope',{mask:+button.dataset.mask});if(!r.failed)controlMessage('发送范围已选为'+scopeLabel(+button.dataset.mask)+'；编辑列表保持独立。');
  } else if(action==='scope-start'||action==='scope-stop'||action==='scope-single'){
    const r=await controlRequest('/api/tx/scope/control',{action:action==='scope-start'?'start':action==='scope-stop'?'stop':'single',mask:Number(button.dataset.mask??S.dev.ui.tx_scope_mask)||1,index:+(button.dataset.index||0)});
    if(!r.failed)controlMessage(action==='scope-stop'?'已停止所选发送范围。':r.pending_confirmation?'请在本机确认所选通道的发送权限。':'已提交所选通道发送请求。');
  } else if(action==='record-settings')await uiAction('navigate',{page:'rec'});
  else if(action==='record-files')await uiAction('navigate',{page:'files'});
  else if (action==='channel') {
    const result=await controlRequest('/api/tx/select',{ch:+button.dataset.ch,index:0});
    if(!result.failed) {sendChannel=+button.dataset.ch;controlMessage(`已选择 CAN${sendChannel+1}，两路发送配置独立。`);}
  } else if (action==='single') {
    const r=await controlRequest('/api/tx/control',{action:'single',ch:sendChannel,index:+button.dataset.index});
    if(!r.failed)controlMessage(`已提交 CAN${sendChannel+1} 单发请求，请查看设备确认和发送结果。`);
  } else if (action==='edit') {
    await editOnDevice(sendChannel,+button.dataset.index);
  } else if (action==='loop-start') await startSequence(sendChannel);
  else if (action==='send-stop') await stopChannel(sendChannel);
  else if (action==='open-sequence') await openDeviceMenu(0,Number(button.dataset.ch??sendChannel));
  else if (action==='open-database') await openDeviceMenu(2,Number(button.dataset.ch??sendChannel));
  else if (action==='load-plan') await openPlan(0);
  else if (action==='open-record') await openDeviceMenu(3);
  else if (action==='save-plan') await openPlan(2,Number(S.dev.ui.tx_channel??sendChannel));
  else if (action==='open-files') await openDeviceMenu(5);
  else if (action==='stop-all') {const r=await controlRequest('/api/tx/control',{action:'stop_all',ch:0});if(!r.failed)controlMessage('两路发送已停止。');}
  else if (action.startsWith('record-')) await recordAction(action.slice(7));
}
// Native clicks occur after touch panning is cancelled, so list scrolling cannot
// dispatch a row's send/edit action on pointerdown.
$('#scr').addEventListener('click',event=>{
  const button=event.target.closest('[data-send-action]');
  if(button&&!button.disabled)controlAction(button);
});
async function recordAction(action) {
  const r=await controlRequest('/api/record',{action});
  if(!r.failed)controlMessage(action==='start'?'新录制已开始准备。':action==='stop'?'正在结束录制，请按屏幕提示命名保存。':'已提交打标记。');
}
function loadSequence(ch=sequenceChannel) {
  const q=sequenceFor(ch); if(!q)return;
  sequenceChannel=ch;sequenceRevision=q.revision;sequenceDirty=false;
  $('#sequenceCh').value=String(ch);$('#sequenceIdGap').value=q.id_gap_ms;
  $('#sequenceRows').innerHTML=q.entries.map((e,i)=>`<tr><td><label><input type="checkbox" data-seq-enabled="${i}" ${e.enabled?'checked':''}> 第 ${i+1} 条 <span data-seq-id="${i}"></span></label></td><td><input aria-label="第${i+1}条每轮次数" data-seq-count="${i}" type="number" min="1" max="10000" value="${e.repeat_count}" required></td><td><input aria-label="第${i+1}条同ID间隔" data-seq-gap="${i}" type="number" min="0" max="3600000" value="${e.frame_gap_ms}" required></td><td><input aria-label="第${i+1}条单ID总周期" data-seq-cycle="${i}" type="number" min="1" max="3600000" value="${e.cycle_ms}" required></td></tr>`).join('');
  updateSequenceExplanation();
}
function readSequenceDraft() {
  return Array.from({length:4},(_,i)=>({enabled:document.querySelector(`[data-seq-enabled="${i}"]`).checked,repeat_count:+document.querySelector(`[data-seq-count="${i}"]`).value,frame_gap_ms:+document.querySelector(`[data-seq-gap="${i}"]`).value,cycle_ms:+document.querySelector(`[data-seq-cycle="${i}"]`).value}));
}
function updateSequenceExplanation() {
  if(!$('#sequenceRows').children.length)return;
  const rows=readSequenceDraft(), first=rows.findIndex(e=>e.enabled), e=rows[first];
  const stale=sequenceFor(sequenceChannel)?.revision!==sequenceRevision;
  if(!e){$('#sequenceExplanation').textContent='至少勾选一个发送槽后，才能开始循环。';return;}
  const span=(e.repeat_count-1)*e.frame_gap_ms;
  $('#sequenceExplanation').textContent=`${sequenceDirty?'未保存草稿 · ':''}${stale?'设备配置已变化，请读取后重试 · ':''}第 ${first+1} 条：从 0 ms 起发 ${e.repeat_count} 次，间隔 ${e.frame_gap_ms} ms，末帧计划在 ${span} ms；本组总周期 ${e.cycle_ms} ms。周期必须至少 ${span+1} ms。不同 ID 切换另加 ${$('#sequenceIdGap').value} ms。`;
}
async function startSequence(ch) {
  const q=sequenceFor(ch);if(!q)return;
  const r=await controlRequest('/api/sequence',{action:'start',ch,revision:q.revision});
  if(!r.failed)controlMessage(r.pending_confirmation?'请在设备屏幕确认该通道工作模式与发送权限。':`CAN${ch+1} 分组循环已启动。`);
}
async function stopChannel(ch) {
  const r=await controlRequest('/api/tx/control',{action:'stop',ch});
  if(!r.failed)controlMessage(`CAN${ch+1} 的分组循环与独立周期已停止，另一通道不受影响。`);
}
function renderControls(s) {
  renderPlanFiles(s);
  if(controlGeneration!==s.sim.generation){controlGeneration=s.sim.generation;sequenceDirty=false;sequenceRevision=-1;recordNameDirty=false;databasePreview=null;databaseSelected.clear();$('#databasePreview').hidden=true;}
  const q=sequenceFor(sequenceChannel);
  if(q&&!sequenceDirty&&q.revision!==sequenceRevision)loadSequence(sequenceChannel);
  if(q){
    $('#sequenceStatus').textContent=`CAN${sequenceChannel+1} · ${SEQUENCE_STATES[q.state]} · 已入队 ${q.sent} 帧 · 跳过 ${q.missed} 次 · 完整列表 ${q.rounds} 轮${q.stop_reason&&q.stop_reason!=='未停止'?' · '+q.stop_reason:''}`;
    $('#sequenceStart').disabled=!!q.state;$('#sequenceStop').disabled=false;
    document.querySelectorAll('[data-seq-id]').forEach(n=>{const e=s.work.tx[sequenceChannel][+n.dataset.seqId];n.textContent='0x'+e.id.toString(16).toUpperCase();});
  }
  if(!recordNameDirty)$('#recordPrefix').value=s.work.rec_name||'';
  const r=s.dev.rec, current=s.work.files.find(f=>f.writing), latest=s.work.files.filter(f=>f.is_session).at(-1);
  $('#recordCurrentName').textContent=`${r.sid===2?'正在录制':r.sid===3?'正在保存':r.sid===1?'正在准备':'已停止 / 空闲'} · 会话 ${r.session||'—'} · ${r.frames} 帧${(current||latest)?' · '+(current||latest).name:''}`;
  $('#recordStart').disabled=r.sid!==0;$('#recordStop').disabled=![1,2].includes(r.sid);$('#recordMark').hidden=true;
  const recKey=document.querySelector('.k.rec'),naming=s.dev.ui.page==='local_record_save';
  if(recKey){recKey.classList.toggle('recording',r.sid===2);recKey.classList.toggle('preparing',r.sid===1);recKey.classList.toggle('saving',r.sid===3);recKey.setAttribute('aria-pressed',String(r.sid===2));recKey.setAttribute('aria-label',r.sid===2?'REC 录制中，短按停止':r.sid===1?'REC 正在准备，短按停止':r.sid===3?'REC 正在保存':naming?'REC 命名中，请先保存文件名或保留默认名':'REC 已停止，短按开始');recKey.title=r.sid===2?'录制中 · 短按停止后命名':r.sid===1?'准备中 · 短按停止':r.sid===3?'正在保存，请稍候':naming?'请先保存文件名或保留默认名':'短按开始录制';recKey.textContent=r.sid===2?'REC ●':r.sid===3?'保存中':r.sid===1?'准备中':naming?'命名中':'REC';}
  $('#recordPrefix').disabled=r.sid!==0;
  for(const f of s.work.files){
    const meta=document.querySelector(`[data-file-meta="${f.index}"]`);if(!meta)continue;
    const card=meta.closest('.filecard'), link=card.querySelector('a');
    if(link){link.href=`/api/log/export?index=${f.index}&token=${f.token}&generation=${s.sim.generation}`;link.download=f.name;}
    if(f.renamable&&!card.querySelector('.rename-form')){
      const form=document.createElement('form');form.className='rename-form';form.dataset.renameIndex=f.index;form.dataset.renameToken=f.token;
      form.innerHTML=`<input aria-label="${esc(f.name)} 的新文件名" value="${esc(f.name)}" placeholder="自定义名称.log" required><button type="submit">重命名</button>`;card.append(form);
    }
  }
}
$('#sequenceForm').addEventListener('input',()=>{sequenceDirty=true;updateSequenceExplanation();});
$('#sequenceCh').onchange=()=>{const target=+$('#sequenceCh').value;if(sequenceDirty){$('#sequenceCh').value=String(sequenceChannel);controlMessage('当前循环草稿尚未保存。请先保存，或点击读取当前设置后切换通道。',true);return;}loadSequence(target);};
$('#sequenceReload').onclick=()=>{loadSequence();controlMessage('已读取本路当前循环配置。');};
$('#sequenceForm').onsubmit=async ev=>{ev.preventDefault();const rows=readSequenceDraft();for(const [i,e] of rows.entries()){if(e.cycle_ms<(e.repeat_count-1)*e.frame_gap_ms+1||(e.repeat_count>1&&e.frame_gap_ms<1)){controlMessage(`第${i+1}条时间不合法：总周期应覆盖全部次数，且同ID间隔至少1 ms。`,true);return;}}const r=await controlRequest('/api/sequence',{action:'configure',ch:sequenceChannel,revision:sequenceRevision,id_gap_ms:+$('#sequenceIdGap').value,entries:rows});if(!r.failed){sequenceDirty=false;loadSequence();controlMessage('循环时序已保存，尚未启动发送。');}};
async function resetSequenceTimes(all) {if(sequenceDirty){controlMessage('请先保存或读取当前循环配置，再恢复默认时间。',true);return;}const r=await controlRequest('/api/sequence',all?{action:'defaults_all',revisions:S.work.sequence.map(q=>q.revision)}:{action:'defaults',ch:sequenceChannel,revision:sequenceRevision});if(!r.failed){loadSequence();controlMessage(`${all?'两路所有':'本路'}时间已恢复默认；ID、数据、次数和启用选择保留。`);}}
$('#sequenceDefaults').onclick=()=>resetSequenceTimes(false);$('#sequenceDefaultsAll').onclick=()=>resetSequenceTimes(true);
$('#sequenceStart').onclick=()=>startSequence(sequenceChannel);$('#sequenceStop').onclick=()=>stopChannel(sequenceChannel);
$('#recordPrefix').oninput=()=>recordNameDirty=true;
$('#recordNameForm').onsubmit=async ev=>{ev.preventDefault();const r=await controlRequest('/api/record/name',{prefix:$('#recordPrefix').value.trim()});if(!r.failed){recordNameDirty=false;controlMessage('录制名称已保存，将用于下一次新会话。');}};
$('#recordNameDefault').onclick=async()=>{const r=await controlRequest('/api/record/name',{prefix:''});if(!r.failed){recordNameDirty=false;$('#recordPrefix').value='';controlMessage('已恢复系统自动命名。');}};
$('#recordStart').onclick=()=>recordAction('start');$('#recordStop').onclick=()=>recordAction('stop');$('#recordMark').onclick=()=>recordAction('mark');$('#recordFiles').onclick=()=>controlsOpen('files');
$('#fileList').addEventListener('submit',async ev=>{const form=ev.target.closest('.rename-form');if(!form)return;ev.preventDefault();const r=await controlRequest('/api/log/rename',{index:+form.dataset.renameIndex,token:+form.dataset.renameToken,name:form.querySelector('input').value.trim()});if(!r.failed)controlMessage('文件已重命名，下载名称同步更新。');});
function bytesToBase64(bytes) { let text='';for(let n=0;n<bytes.length;n+=16384)text+=String.fromCharCode(...bytes.subarray(n,n+16384));return btoa(text); }
async function importDatabaseFile(file) {
  if(!file){controlMessage('请选择 .dbc 或 CAN 数据库 .dbf 文件。',true);return;}
  if(file.size>2*1024*1024){controlMessage('数据库超过 2 MiB 上限。',true);return;}
  const submit=$('#databaseForm button[type=submit]');submit.disabled=true;
  try {
    const content=bytesToBase64(new Uint8Array(await file.arrayBuffer()));
    const r=await controlRequest('/api/database/import',{name:file.name,content,encoding:$('#databaseEncoding').value});
    if(r.failed)return;
    databasePreview=r;databaseSelected.clear();$('#databasePreview').hidden=false;$('#databaseSource').textContent=file.name;
    $('#databaseSummary').textContent=`${r.name} · ${r.format_label} · ${r.frames.length} 帧 · ${r.encoding}${r.warnings.length?' · '+r.warnings.join('；'):''}`;
    $('#databaseSearch').value='';renderDatabaseFrames();controlMessage('数据库已放入模拟SD。请在设备屏幕进入“数据库选帧”，本地选择和加入发送槽。');
  } catch(error){controlMessage('文件读取失败：'+error.message,true);}
  finally {submit.disabled=false;}
}
function renderDatabaseFrames() {
  if(!databasePreview)return;
  const term=$('#databaseSearch').value.trim().toLowerCase();
  const frames=databasePreview.frames.filter(f=>!term||[f.name,f.id.toString(16),String(f.id),...f.signals.map(s=>s.name)].some(s=>String(s).toLowerCase().includes(term)));
  $('#databaseFrames').innerHTML=frames.slice(0,100).map(f=>`<article class="database-frame"><div class="database-frame-head"><input type="checkbox" data-database-key="${esc(f.key)}" aria-label="选择 ${esc(f.name)}" ${databaseSelected.has(f.key)?'checked':''}><strong>${esc(f.name)}</strong><code>0x${f.id.toString(16).toUpperCase().padStart(f.extended?8:3,'0')}</code><button type="button" data-database-draft="${esc(f.key)}">填入编辑草稿</button></div><div class="meta">${f.extended?'扩展 29 位':'标准 11 位'} · ${f.fd?'CAN FD':'Classic CAN'}${f.brs?' / BRS':''} · ${f.length} 字节 · 数据库周期 ${f.cycle_ms} ms · 初始数据 00</div><details><summary>${f.signals.length} 个信号</summary>${f.signals.map(s=>`<div class="signal">${esc(s.name)} · bit ${s.start_bit} / ${s.length}位 · ${s.byte_order==='little'?'Intel':'Motorola'}${s.unit?' · '+esc(s.unit):''}</div>`).join('')||'<div>此帧未定义信号</div>'}</details></article>`).join('')||'<div class="worknote">没有匹配的报文。</div>';
  if(frames.length>100)$('#databaseFrames').insertAdjacentHTML('beforeend',`<p class="worknote">显示前100条，共${frames.length}条匹配；请进一步搜索。</p>`);
  updateDatabaseSelection();
}
function updateDatabaseSelection() {
  const count=databaseSelected.size,ch=+$('#databaseCh').value,start=+$('#databaseSlot').value;
  $('#databaseSelection').textContent=`已选择 ${count} 帧 / 本路4个槽`;
  $('#databaseApplyHint').textContent=count?`将替换 CAN${ch+1} 第 ${start+1}～${start+count} 条发送槽的 ID、格式、长度与数据（全00），现有内容会被替换；不会启动发送。循环次数和时间另在“循环时序”设置。`:'勾选帧后，选择 CAN1 或 CAN2 及目标起始发送槽。也可逐帧填入编辑草稿，修改数据后保存。';
  $('#databaseApply').disabled=!count||count>4-start;
}
$('#databaseForm').onsubmit=ev=>{ev.preventDefault();importDatabaseFile($('#databaseUpload').files[0]);};
async function importDatabaseExample(format){const r=await fetch('/api/database/example?format='+format);if(!r.ok){controlMessage('示例读取失败。',true);return;}const b=await r.blob();await importDatabaseFile(new File([b],'review.'+format));}
$('#databaseExample').onclick=()=>importDatabaseExample('dbc');$('#databaseDbfExample').onclick=()=>importDatabaseExample('dbf');
$('#databaseSearch').oninput=renderDatabaseFrames;$('#databaseCh').onchange=updateDatabaseSelection;$('#databaseSlot').onchange=updateDatabaseSelection;
$('#databaseClear').onclick=()=>{databaseSelected.clear();renderDatabaseFrames();};
$('#databaseFrames').addEventListener('change',ev=>{const input=ev.target.closest('[data-database-key]');if(!input)return;if(input.checked&&databaseSelected.size>=4){input.checked=false;controlMessage('本版每路4个发送槽，最多同时选择4帧。',true);return;}if(input.checked)databaseSelected.add(input.dataset.databaseKey);else databaseSelected.delete(input.dataset.databaseKey);updateDatabaseSelection();});
$('#databaseFrames').addEventListener('click',ev=>{const b=ev.target.closest('[data-database-draft]');if(!b||!databasePreview)return;if(workTxDirty){controlMessage('已有未保存报文草稿，请先保存或读取当前条目。',true);return;}const f=databasePreview.frames.find(f=>f.key===b.dataset.databaseDraft);if(!f)return;$('#txCh').value=$('#databaseCh').value;$('#txEntry').value=$('#databaseSlot').value;loadTx();const flags=(f.extended?1:0)|(f.fd?2:0)|(f.brs?4:0);prefillTxFromDetail({valid:true,flags,id:f.id,len:f.length,available_len:f.length,data:Array(f.length).fill(0),ch:+$('#databaseCh').value+1});controlMessage(`${f.name} 已填入发送草稿；数据初值全00，请编辑后保存。未发送。`);});
$('#databaseApply').onclick=async()=>{if(!databasePreview||!databaseSelected.size)return;if(workTxDirty){controlMessage('已有未保存报文草稿，请先保存或读取当前条目。',true);return;}const ch=+$('#databaseCh').value,start=+$('#databaseSlot').value,frames=databasePreview.frames.filter(f=>databaseSelected.has(f.key)).map(f=>f.key);if(frames.length>4-start){controlMessage('所选帧超过剩余发送槽，请减少选择或从第1条开始。',true);return;}const revisions=S.work.tx[ch].slice(start,start+frames.length).map(e=>e.revision);const r=await controlRequest('/api/database/apply',{database:databasePreview.database,ch,start_index:start,frames,revisions});if(!r.failed){databaseSelected.clear();renderDatabaseFrames();loadTx();controlMessage(`已将 ${r.added} 帧加入 CAN${ch+1} 发送列表，数据为00；未启动发送。请设置循环时序并核对数据。`);}};

// Main operations open device-local state-machine pages; no desktop form writes.
async function openDeviceMenu(kind,ch=sendChannel,index) {
  if(index===undefined)index=S.dev.ui.page==='tx'&&S.dev.ui.cursor<8?S.dev.ui.cursor%4:0;
  const r=await controlRequest('/api/local',{kind,ch,index});
  if(!r.failed){$('#workbench').open=false;$('#wrap').scrollIntoView({behavior:'smooth',block:'center'});}
  return r;
}
async function editOnDevice(ch,index){const r=await controlRequest('/api/tx/edit',{ch,index});if(!r.failed){$('#workbench').open=false;$('#wrap').scrollIntoView({behavior:'smooth',block:'center'});}return r;}
function localKeyboardView(ui){
  const k=ui.keyboard,mode=['hex','decimal','payload'].includes(k.mode)?k.mode:'text',typed=mode!=='text';
  const bytes=new TextEncoder().encode(k.value||'').length,columns=Math.max(1,Math.min(10,Number(k.columns)||10));
  const rows=Math.ceil(k.keys.length/columns),valueHeight=mode==='payload'?64:36;
  const keyHeight=typed?Math.min(40,Math.max(18,Math.floor((234-10-valueHeight-18-14-(rows-1)*3)/rows))):19;
  const labels={text:'本机字符键盘',hex:'十六进制输入',decimal:'数字输入',payload:'整帧数据 · 十六进制'};
  const placeholder=typed?'点清空后输入新值':'输入名称，留空使用默认';
  const size=typed?`${bytes} / ${k.max_bytes} 字符`:`${bytes} / ${k.max_bytes} 字节`;
  const identity=typed?` data-keyboard-token="${k.token}" data-keyboard-generation="${S.sim.generation}"`:'';
  return `<div class="local-keyboard ${typed?'typed-keyboard '+mode+'-keyboard':''}" style="--keyboard-columns:${columns};--keyboard-key-height:${keyHeight}px"><div class="local-name-value" role="textbox" aria-label="${esc(ui.title||labels[mode])}" aria-readonly="true" tabindex="0">${esc(k.value)||`<span class="keyboard-placeholder">${placeholder}</span>`}</div><div class="local-key-meta"><span>${labels[mode]}</span><span>${size}</span></div><div class="local-key-grid">${k.keys.map(key=>`<button type="button" data-item="${key.index}"${identity} ${key.label==='退格'?'data-backspace="1" title="长按连续删除"':''} class="${key.index===ui.cursor?'sel ':''}${['保存','确定'].includes(key.label)?'save':key.label==='清空'?'clear':['取消','保留默认名'].includes(key.label)?'cancel':''}" aria-label="${key.kind==='text'?'输入 ':''}${esc(key.label)}">${esc(key.label)}</button>`).join('')}</div>${typed?'<div class="keyboard-help">先清空再输入 · 支持键入/粘贴 · 确定写入草稿</div>':''}</div>`;
}

// Computer typing complements the device keyboard and edits the same C draft.
function typedKeyboardForEvent(event){
  const k=S?.dev.ui.keyboard,target=event.target;
  if(!['hex','decimal','payload'].includes(k?.mode)||!S.dev.power.screen||S.dev.power.locked||S.dev.ui.dlg?.kind)return null;
  if(target?.closest?.('input,select,textarea,[contenteditable="true"]'))return null;
  if(target!==document.body&&target!==document.documentElement&&!target?.closest?.('#scr'))return null;
  return {keyboard:k,generation:S.sim.generation};
}
function appendTypedInput(snapshot,text,fromPaste=false){
  if(typeof stopBackspaceHold==='function')stopBackspaceHold();
  const mode=snapshot.keyboard.mode;
  if(fromPaste)text=text.trim();
  const value=mode==='payload'?text.replace(/\s+/g,' ').toUpperCase():mode==='hex'?text.toUpperCase():text;
  return api('/api/local/input',{action:'append',token:snapshot.keyboard.token,text:value,generation:snapshot.generation});
}
document.addEventListener('keydown',event=>{
  const snapshot=typedKeyboardForEvent(event);if(!snapshot)return;
  if(event.key==='Backspace'||event.isComposing||event.ctrlKey||event.metaKey||event.altKey)return;
  if(event.key==='Enter'||event.key==='Escape'){
    event.preventDefault();event.stopImmediatePropagation();if(event.repeat)return;
    const key=snapshot.keyboard.keys.find(item=>item.label===(event.key==='Enter'?'确定':'取消'));
    if(key){if(typeof stopBackspaceHold==='function')stopBackspaceHold();touch(0,key.index,{keyboard_token:snapshot.keyboard.token,generation:snapshot.generation});}
  }else if(event.key.length===1){
    event.preventDefault();event.stopImmediatePropagation();appendTypedInput(snapshot,event.key);
  }
},true);
document.addEventListener('paste',event=>{
  const snapshot=typedKeyboardForEvent(event);if(!snapshot)return;
  event.preventDefault();event.stopImmediatePropagation();
  appendTypedInput(snapshot,event.clipboardData?.getData('text/plain')||'',true);
},true);


async function openPlan(mode=0,ch=sendChannel) {
  const r=await controlRequest('/api/plan/open',{mode,ch});
  if(!r.failed){$('#workbench').open=false;$('#wrap').scrollIntoView({behavior:'smooth',block:'center'});}
  return r;
}
let planFileKey='';
function renderPlanFiles(s) {
  const files=s.work.send_plans?.files||[],key=JSON.stringify([s.sim.generation,files]);
  if(key===planFileKey)return;planFileKey=key;
  $('#planList').innerHTML=files.length?files.map(f=>`<article class="filecard"><b>${esc(f.name)}</b><small>${f.channel_mask===3?'CAN1 + CAN2':f.channel_mask===1?'CAN1':'CAN2'} · 发送方案</small><a href="/api/plan/export?name=${encodeURIComponent(f.name)}&token=${f.token}&generation=${s.sim.generation}" download="${esc(f.name)}">下载方案</a></article>`).join(''):'<p class="worknote">模拟 SD 中还没有发送方案。可在设备发送页保存这一屏，或导入统一模板。</p>';
}
$('#planUpload').onchange=async()=>{
  const file=$('#planUpload').files[0];if(!file)return;
  if(file.size>131072){controlMessage('发送方案最多128KiB。',true);return;}
  $('#planName').value=file.name;$('#planText').value=await file.text();
};
$('#planForm').onsubmit=async ev=>{
  ev.preventDefault();const r=await api('/api/plan/import',{name:$('#planName').value,text:$('#planText').value,generation:S.sim.generation});
  if(!r.failed)controlMessage(r.note+(r.warnings?.length?'；存在未载入文件：'+r.warnings.join('；'):''));
};
$('#planOpen').onclick=()=>openPlan(0);
$('#planRefresh').onclick=async()=>{const r=await api('/api/plan/refresh',{generation:S.sim.generation});if(!r.failed)controlMessage(`已刷新 ${r.files} 个方案。${r.warnings?.length?'未载入：'+r.warnings.join('；'):''}`);};
