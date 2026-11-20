/* Offline regression of the real gesture controller, without a browser. */
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const listeners=new Map(),captures=new Set(),calls=[],details=[];
const screen={addEventListener(type,fn){(listeners.get(type)||listeners.set(type,[]).get(type)).push(fn);},
 setPointerCapture(id){captures.add(id);},hasPointerCapture(id){return captures.has(id);},releasePointerCapture(id){captures.delete(id);}};
const table={querySelector(){return{getBoundingClientRect(){return{height:24};}};}};
const row={dataset:{liveIndex:'2',liveSeq:'777'}};
const target={closest(s){return s==='.live table'?table:s==='[data-live-index]'?row:null;}};
const model={sim:{generation:4},dev:{power:{screen:true,locked:false},ui:{page:'live',dlg:{},live:{channel:1,view:'scroll',cur:0,paused:false}}}};
const sandbox={S:model,document:{querySelector(){return screen;}},window:{addEventListener(){}},
 api(path,body){calls.push({path,...body});return Promise.resolve({});},
 uiAction(action,selection){details.push({action,...selection});}};
vm.runInNewContext(fs.readFileSync('sim/web/scroll.js','utf8'),sandbox);
function emit(type,extra={}){const ev={target,button:0,isPrimary:true,pointerId:1,clientX:60,clientY:100,
 deltaY:0,deltaMode:0,preventDefault(){this.prevented=true;},stopImmediatePropagation(){this.stopped=true;},...extra};
 for(const fn of listeners.get(type)||[])fn(ev);return ev;}
const settle=async()=>{for(let n=0;n<8;n++)await Promise.resolve();};
(async()=>{
 const down=emit('pointerdown');assert.equal(down.stopped,true);assert.equal(details.length,0);
 emit('pointermove',{clientY:172});emit('pointerup',{clientY:172});await settle();
 assert.equal(details.length,0);assert.equal(calls.length,1);assert.equal(calls[0].rows,3);
 assert.equal(calls[0].generation,4);assert.equal(calls[0].channel,1);assert.equal(captures.size,0);
 emit('pointerdown');emit('pointermove',{clientY:102});emit('pointerup',{clientY:102});
 assert.equal(details.length,1);assert.equal(details[0].seq,777);
 emit('pointerdown');emit('pointercancel');assert.equal(details.length,1);
 emit('pointerdown');emit('pointermove',{clientX:110,clientY:100});emit('pointerup');assert.equal(details.length,1);
 emit('pointerdown');model.dev.ui.live.channel=2;emit('pointermove',{clientY:160});emit('pointerup');await settle();assert.equal(calls.length,1);
 emit('wheel',{deltaY:-48});await settle();assert.equal(calls.at(-1).rows,2);assert.equal(calls.at(-1).channel,2);
 model.dev.ui.live.view='byid';emit('wheel',{deltaY:72});await settle();assert.equal(calls.at(-1).rows,3);assert.equal(calls.at(-1).view,1);
 emit('pointerdown');emit('pointermove',{clientY:28});emit('pointerup',{clientY:28});await settle();assert.equal(calls.at(-1).rows,3);
 const before=calls.length;model.dev.ui.page='local_input';emit('wheel',{deltaY:100});emit('pointerdown');emit('pointerup');await settle();assert.equal(calls.length,before);assert.equal(details.length,1);
 model.dev.ui.page='live';model.dev.power.locked=true;emit('wheel',{deltaY:100});await settle();assert.equal(calls.length,before);
 model.dev.power.locked=false;model.dev.ui.dlg={kind:'confirm'};emit('pointerdown');emit('pointerup');assert.equal(details.length,1);
 console.log('Live gesture controller: swipe/wheel direction, tap separation, cancellation and stale-view guards passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
