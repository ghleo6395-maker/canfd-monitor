/* Device-list browsing. Only the live table captures swipes and wheel input;
 * recording, tabs, toolbars and the outer page keep their normal controls. */
(() => {
  const screen = document.querySelector('#scr');
  let gesture = null, queued = 0, queuedContext = null, sending = false;
  let wheelPixels = 0, wheelContext = '';
  const context = () => {
    const d = S?.dev;
    if (!d || d.ui.page !== 'live' || d.ui.dlg?.kind || d.ui.keyboard ||
        !d.power.screen || d.power.locked) return null;
    return {generation:S.sim.generation, channel:d.ui.live.channel, view:d.ui.live.view};
  };
  const key = c => c ? `${c.generation}:${c.channel}:${c.view}` : '';
  const current = c => c && key(c) === key(context());
  const rowHeight = table => Math.max(12, table.querySelector('tbody tr')?.getBoundingClientRect().height || 24);
  function flush() {
    if (sending || !queued || !queuedContext) return;
    if (!current(queuedContext)) { queued=0; queuedContext=null; return; }
    const c=queuedContext, rows=Math.max(-128,Math.min(128,queued));
    queued-=rows; sending=true;
    api('/api/ui',{action:'live_scroll',rows,channel:c.channel,
      view:c.view==='scroll'?0:1,generation:c.generation},{quiet:true}).finally(() => {
      sending=false;
      if (queuedContext && !current(queuedContext)) { queued=0; queuedContext=null; }
      flush();
    });
  }
  function move(rows,c) {
    if (!rows || !current(c)) return;
    if (key(c)!==key(queuedContext)) { queued=0; queuedContext=c; }
    queued=Math.max(-1024,Math.min(1024,queued+rows)); flush();
  }
  function finish(ev,cancelled=false) {
    const g=gesture;
    if (!g || (ev.pointerId!==undefined && ev.pointerId!==g.id)) return;
    gesture=null;
    if (screen.hasPointerCapture(g.id)) screen.releasePointerCapture(g.id);
    if (!cancelled && !g.dragged && current(g.context) && g.selection)
      uiAction('live_detail',g.selection);
  }
  screen.addEventListener('pointerdown',ev => {
    const table=ev.target.closest('.live table'), c=context();
    if (!table || !c || ev.button!==0 || !ev.isPrimary) return;
    ev.preventDefault(); ev.stopImmediatePropagation();
    const row=ev.target.closest('[data-live-index]');
    gesture={id:ev.pointerId,x:ev.clientX,start:ev.clientY,last:ev.clientY,pixels:0,
      height:rowHeight(table),dragged:false,context:c,
      selection:row ? (row.dataset.liveSeq!==undefined ? {seq:Number(row.dataset.liveSeq)} :
        {index:Number(row.dataset.liveIndex)}) : null};
    screen.setPointerCapture(ev.pointerId);
  },true);
  screen.addEventListener('pointermove',ev => {
    const g=gesture;
    if (!g || ev.pointerId!==g.id) return;
    ev.preventDefault(); ev.stopImmediatePropagation();
    if (!current(g.context)) { finish(ev,true); return; }
    if (!g.dragged) {
      if (Math.max(Math.abs(ev.clientY-g.start),Math.abs(ev.clientX-g.x))<8) return;
      g.dragged=true;
    }
    g.pixels+=ev.clientY-g.last; g.last=ev.clientY;
    const steps=Math.trunc(g.pixels/g.height);
    if (steps) { g.pixels-=steps*g.height; move(g.context.view==='scroll'?steps:-steps,g.context); }
  },true);
  screen.addEventListener('pointerup',ev => {
    if (!gesture || ev.pointerId!==gesture.id) return;
    ev.preventDefault(); ev.stopImmediatePropagation(); finish(ev);
  },true);
  screen.addEventListener('pointercancel',ev=>finish(ev,true),true);
  screen.addEventListener('lostpointercapture',ev=>finish(ev,true),true);
  screen.addEventListener('wheel',ev => {
    const table=ev.target.closest('.live table'), c=context();
    if (!table || !c) return;
    ev.preventDefault();
    if (wheelContext!==key(c)) { wheelPixels=0; wheelContext=key(c); }
    const height=rowHeight(table);
    wheelPixels+=ev.deltaY*(ev.deltaMode===1?height:ev.deltaMode===2?height*5:1);
    const steps=Math.trunc(wheelPixels/height);
    if (steps) { wheelPixels-=steps*height; move(c.view==='scroll'?-steps:steps,c); }
  },{passive:false});
  window.addEventListener('blur',()=>{if(gesture)finish({},true);queued=0;queuedContext=null;});
  window.addEventListener('pagehide',()=>{if(gesture)finish({},true);queued=0;queuedContext=null;});
})();

// Keep the send-list element alive throughout a native touch/wheel gesture.
// setScr restores its position once the interaction settles.
let txListPointer=null,txListRedrawUntil=0,txListSuppressClickUntil=0;
function txListRenderBusy(){return txListPointer!==null||performance.now()<txListRedrawUntil;}
(() => {
  const screen=document.querySelector('#scr');
  const hold=()=>{txListRedrawUntil=performance.now()+220;};
  function stop(ev){
    const g=txListPointer;
    if(!g||(ev.pointerId!==undefined&&ev.pointerId!==g.id))return;
    txListPointer=null;hold();
    if(g.dragged)txListSuppressClickUntil=performance.now()+350;
    if(screen.hasPointerCapture(g.id))screen.releasePointerCapture(g.id);
  }
  screen.addEventListener('pointerdown',ev=>{
    const pane=ev.target.closest('.tx-message-list');
    if(!pane||ev.button!==0||!ev.isPrimary)return;
    txListPointer={id:ev.pointerId,pane,y:ev.clientY,x:ev.clientX,top:pane.scrollTop,
      scale:pane.clientHeight?pane.getBoundingClientRect().height/pane.clientHeight:1,
      mouse:ev.pointerType==='mouse',dragged:false};hold();
  },true);
  screen.addEventListener('pointermove',ev=>{
    const g=txListPointer;if(!g||g.id!==ev.pointerId)return;
    if(!g.pane.isConnected){stop(ev);return;}
    if(Math.max(Math.abs(ev.clientY-g.y),Math.abs(ev.clientX-g.x))>8)g.dragged=true;
    if(g.mouse&&g.dragged){
      ev.preventDefault();screen.setPointerCapture(g.id);
      g.pane.scrollTop=g.top+(g.y-ev.clientY)/g.scale;hold();
    }
  },true);
  window.addEventListener('pointerup',stop,true);
  window.addEventListener('pointercancel',stop,true);
  screen.addEventListener('lostpointercapture',stop,true);
  screen.addEventListener('wheel',ev=>{if(ev.target.closest('.tx-message-list'))hold();},{passive:true,capture:true});
  screen.addEventListener('scroll',ev=>{
    if(ev.target.matches?.('.tx-message-list')&&txListRenderBusy())hold();
  },true);
  screen.addEventListener('click',ev=>{
    if(ev.target.closest('.tx-message-list')&&performance.now()<txListSuppressClickUntil){
      ev.preventDefault();ev.stopImmediatePropagation();
    }
  },true);
  window.addEventListener('blur',()=>stop({}));
  window.addEventListener('pagehide',()=>stop({}));
})();
