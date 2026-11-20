/* Exercise the actual send-list gesture controller without a browser. */
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const listeners=new Map(),windowListeners=new Map(),captures=new Set();let now=0;
const pane={scrollTop:0,isConnected:true,matches:s=>s==='.tx-message-list'};
const target={closest:s=>s==='.tx-message-list'?pane:null};
const on=(map,type,fn)=>{if(!map.has(type))map.set(type,[]);map.get(type).push(fn);};
const screen={addEventListener:(type,fn)=>on(listeners,type,fn),setPointerCapture:id=>captures.add(id),
 hasPointerCapture:id=>captures.has(id),releasePointerCapture:id=>captures.delete(id)};
const ctx=vm.createContext({performance:{now:()=>now},document:{querySelector:()=>screen},
 window:{addEventListener:(type,fn)=>on(windowListeners,type,fn)},
 S:{sim:{generation:0},dev:{power:{screen:true},ui:{page:'tx',dlg:{}}}},
 api(){throw Error('A list drag must never transmit');},uiAction(){throw Error('A list drag must not navigate');}});
vm.runInContext(fs.readFileSync('sim/web/scroll.js','utf8'),ctx);
const busy=()=>vm.runInContext('txListRenderBusy()',ctx);
function emit(type,extra={}){const e={target,button:0,isPrimary:true,pointerId:1,pointerType:'mouse',clientX:20,clientY:100,
 preventDefault(){this.prevented=true;},stopImmediatePropagation(){this.stopped=true;},...extra};
 for(const fn of windowListeners.get(type)||[])fn(e);
 for(const fn of listeners.get(type)||[])fn(e);return e;}
emit('pointerdown');assert.equal(busy(),true);
emit('pointermove',{clientY:40});assert.equal(pane.scrollTop,60);assert.equal(captures.has(1),true);
emit('pointerup',{clientY:40});assert.equal(captures.size,0);
const draggedClick=emit('click');assert.equal(draggedClick.prevented,true);assert.equal(draggedClick.stopped,true);
now=400;assert.equal(busy(),false);
emit('pointerdown');emit('pointerup');assert.equal(emit('click').prevented,undefined);
now=800;emit('wheel');assert.equal(busy(),true);now=1100;assert.equal(busy(),false);
emit('pointerdown',{pointerType:'touch'});emit('pointermove',{pointerType:'touch',clientY:30});
assert.equal(pane.scrollTop,60);pane.scrollTop=130;emit('scroll',{target:pane});
emit('pointercancel',{pointerType:'touch'});assert.equal(emit('click').stopped,true);
now=1600;assert.equal(busy(),false);
emit('pointerdown');pane.isConnected=false;emit('pointermove',{clientY:0});now=1900;assert.equal(busy(),false);
console.log('Send-list gestures: mouse drag, native touch/wheel redraw hold, click suppression and release guards passed');
