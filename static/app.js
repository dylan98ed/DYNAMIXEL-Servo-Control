'use strict';
const $ = (id) => document.getElementById(id);
let logCursor = 0;
let connected = false;
let lastDataAt = 0;
let data = {};
let samples = [];
let pollBusy = false;
let toastTimer;

function toast(message, error = false) {
  const element = $('toast');
  element.textContent = message;
  element.className = 'toast visible' + (error ? ' error' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => element.classList.remove('visible'), 4500);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    cache: 'no-store',
    headers: {'Content-Type': 'application/json'},
    ...options,
  });
  const json = await response.json();
  if (!response.ok || json.ok === false) {
    throw new Error(json.error || 'Falló la solicitud.');
  }
  return json;
}

async function getPorts() {
  const select = $('portSelect');
  const selected = select.value;
  try {
    const ports = await api('/api/ports');
    select.replaceChildren();
    if (!ports.length) {
      const opt = new Option('No se detectaron puertos COM', '');
      select.add(opt);
      return;
    }
    ports.forEach((p) => select.add(new Option(`${p.device} · ${p.description}`, p.device)));
    if (selected && ports.some((p) => p.device === selected)) {
      select.value = selected;
    } else {
      const suggested = ports.find((p) => /COM6$/i.test(p.device)) || ports[0];
      select.value = suggested.device;
    }
  } catch (err) {
    toast(err.message, true);
  }
}

async function connect() {
  const port = $('portSelect').value;
  if (!port) return toast('Seleccioná un puerto serie.', true);
  try {
    await api('/api/connect', {method:'POST', body: JSON.stringify({port})});
    toast(`Conectado a ${port}. Esperá la telemetría o enviá INFO.`);
    await pollState();
  } catch (err) {
    toast(err.message + ' Si Arduino tiene abierto el Monitor Serie, cerralo.', true);
  }
}

async function disconnect() {
  try {
    await api('/api/disconnect', {method:'POST', body:'{}'});
    await pollState();
    toast('Puerto desconectado.');
  } catch (err) {
    toast(err.message, true);
  }
}

async function send(command) {
  if (!connected) return toast('Primero conectá la ESP32.', true);
  try {
    await api('/api/command', {method:'POST', body: JSON.stringify({command})});
  } catch (err) {
    toast(err.message, true);
  }
}

function val(id, value, digits=1) {
  $(id).textContent = typeof value === 'number' && Number.isFinite(value)
    ? value.toFixed(digits) : '—';
}

function display(snapshot) {
  connected = !!snapshot.connected;
  const serialStatus = $('serialStatus');
  serialStatus.className = 'status-pill ' + (connected ? 'connected' : 'disconnected');
  serialStatus.innerHTML = `<span class="status-dot"></span>${connected ? 'Conectado · ' + (snapshot.port || '') : 'COM desconectado'}`;
  $('connectBtn').disabled = connected;
  $('disconnectBtn').disabled = !connected;
  $('portSelect').disabled = connected;
  $('refreshPorts').disabled = connected;
  document.querySelectorAll('[data-cmd], [data-send], #manualForm button').forEach(btn => btn.disabled = !connected);

  data = snapshot.latest || {};
  samples = snapshot.samples || [];
  lastDataAt = snapshot.last_sample_at || 0;
  const age = Date.now()/1000 - lastDataAt;
  const fresh = connected && lastDataAt > 0 && age < 4;
  const servoStatus = $('servoStatus');
  servoStatus.className = 'status-pill ' + (fresh ? 'connected' : 'neutral');
  servoStatus.innerHTML = `<span class="status-dot"></span>${fresh ? 'Telemetría recibida' : 'Sin telemetría reciente'}`;
  $('lastUpdate').textContent = lastDataAt ? ('Última muestra: hace ' + Math.max(0, Math.round(age)) + ' s') : 'Sin lecturas';

  val('kpiPos', data.POS, 2);
  val('kpiObj', data.OBJ, 2);
  val('kpiVel', data.VEL, 2);
  val('kpiCurrent', data.I, 1);
  val('kpiVoltage', data.VIN, 1);
  val('kpiTemp', data.T, 0);
  $('kpiTorque').textContent = `${data.TORQ === undefined ? '—' : (data.TORQ === 1 ? 'ON' : 'OFF')} / ${data.MOV === undefined ? '—' : (data.MOV === 1 ? 'MOV' : 'QUIETO')}`;
  $('kpiHw').textContent = data.HW === undefined ? '—' : '0x' + Number(data.HW).toString(16).toUpperCase().padStart(2,'0');
  $('kpiHw').style.color = data.HW ? 'var(--danger)' : '';

  if (snapshot.logs && snapshot.logs.length) appendLogs(snapshot.logs);
  renderChart('positionChart', samples, [
    {key:'POS', color:'#3dd7b5'}, {key:'OBJ', color:'#ffb263'}
  ], '°', 'regular');
  renderChart('dynamicChart', samples, [
    {key:'VEL', color:'#74a4ff'}, {key:'I', color:'#e990bd'}
  ], '', 'dual');
  if (snapshot.error && !window._lastErrorShown) {
    toast(snapshot.error, true);
    window._lastErrorShown = true;
  } else if (!snapshot.error) {
    window._lastErrorShown = false;
  }
}

function appendLogs(logs) {
  const el = $('console');
  const wasNearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 80;
  const placeholder = el.querySelector('.console-placeholder');
  if (placeholder) placeholder.remove();
  for (const entry of logs) {
    if (entry.id <= logCursor) continue;
    const row = document.createElement('div');
    row.className = 'log-row ' + (entry.kind || 'rx');
    const time = document.createElement('span');
    time.className = 'log-time';
    time.textContent = entry.time;
    const msg = document.createElement('span');
    msg.className = 'log-text';
    msg.textContent = entry.text;
    row.append(time, msg);
    el.append(row);
    logCursor = Math.max(logCursor, entry.id);
  }
  while (el.children.length > 280) el.firstElementChild.remove();
  if (wasNearBottom) el.scrollTop = el.scrollHeight;
}

async function pollState() {
  if (pollBusy) return;
  pollBusy = true;
  try {
    const state = await api('/api/state?after=' + logCursor);
    display(state);
  } catch (err) {
    toast('No se puede acceder a Flask: ' + err.message, true);
  } finally {
    pollBusy = false;
  }
}

function renderChart(id, allSamples, series, unit, mode) {
  const canvas = $(id);
  const rect = canvas.getBoundingClientRect();
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(10, Math.round(rect.width * dpr));
  canvas.height = Math.max(10, Math.round(rect.height * dpr));
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr,dpr);
  const w = rect.width, h = rect.height;
  const L=47, T=14, R=mode==='dual'?46:14, B=28;
  const plotW=Math.max(20,w-L-R), plotH=Math.max(20,h-T-B);
  const arr = allSamples.slice(-150);
  const x0 = arr.length ? arr[0].ts : 0;
  const x1 = arr.length ? arr[arr.length-1].ts : 0;
  let values=[];
  if (mode==='dual') values = arr.map(s=>s.VEL).filter(Number.isFinite);
  else values = arr.flatMap(s=>series.map(k=>s[k.key])).filter(Number.isFinite);
  let lo = values.length ? Math.min(...values) : -50;
  let hi = values.length ? Math.max(...values) : 50;
  if (mode==='regular') { lo = Math.min(lo,-10); hi = Math.max(hi,10); }
  if (hi-lo<2) {lo-=1;hi+=1;}
  const pad = (hi-lo)*0.12;
  lo-=pad; hi+=pad;
  const mapX = (ts) => L + ((ts-x0)/Math.max(1,x1-x0))*plotW;
  const mapY = (y, low, high) => T + plotH - ((y-low)/(high-low))*plotH;
  ctx.font='11px system-ui';
  ctx.fillStyle='#8fa5bf';
  ctx.lineWidth=1;
  for(let j=0;j<=4;j++){
    const y=T+j*plotH/4;
    ctx.strokeStyle='#29384d'; ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(L+plotW,y);ctx.stroke();
    ctx.textAlign='right';ctx.fillText((hi-(hi-lo)*j/4).toFixed(1),L-9,y+4);
  }
  ctx.textAlign='center';
  for(let j=0;j<=4;j++){
    const x=L+j*plotW/4;
    ctx.fillText(((x1-x0)*(j/4)).toFixed(1)+'s',x,h-7);
  }
  if (mode==='dual') {
    const current = arr.map(s=>s.I).filter(Number.isFinite);
    let clo=current.length?Math.min(...current):0;
    let chi=current.length?Math.max(...current):100;
    if (chi-clo < 10) { clo-=5;chi+=5; }
    const cp=(chi-clo)*0.15;clo-=cp;chi+=cp;
    ctx.textAlign='left';
    for(let j=0;j<=4;j++){
      const y=T+j*plotH/4;
      ctx.fillStyle='#e1a7c8';ctx.fillText((chi-(chi-clo)*j/4).toFixed(0),L+plotW+7,y+4);
    }
    for (const s of series) drawLine(s, s.key==='I'?clo:lo, s.key==='I'?chi:hi);
  } else {
    for (const s of series) drawLine(s,lo,hi);
  }
  function drawLine(s, low, high) {
    ctx.beginPath();ctx.lineWidth=2.3;ctx.strokeStyle=s.color;
    let begun=false;
    for (const row of arr) {
      if (!Number.isFinite(row[s.key])) {begun=false;continue;}
      const x=mapX(row.ts),y=mapY(row[s.key],low,high);
      if (!begun){ctx.moveTo(x,y);begun=true;}else ctx.lineTo(x,y);
    }
    ctx.stroke();
  }
  if (arr.length < 2) {
    ctx.fillStyle='#7189a5';ctx.textAlign='center';ctx.font='13px system-ui';
    ctx.fillText('Esperando muestras del servomotor…',w/2,h/2);
  }
}

$('refreshPorts').addEventListener('click',getPorts);
$('connectBtn').addEventListener('click',connect);
$('disconnectBtn').addEventListener('click',disconnect);
document.querySelectorAll('[data-cmd]').forEach(btn=>btn.addEventListener('click',()=>send(btn.dataset.cmd)));
document.querySelectorAll('[data-send]').forEach(btn=>btn.addEventListener('click',()=>send(btn.dataset.send+' '+$(btn.dataset.input).value)));
$('manualForm').addEventListener('submit', async e => {
  e.preventDefault();const input=$('manualInput'); const command=input.value.trim();
  if(command){await send(command);input.value='';}
});
$('positionInput').addEventListener('input',()=>{
  const value=Math.max(-180,Math.min(180,Number($('positionInput').value)||0));
  $('posSlider').value=value;
});
$('posSlider').addEventListener('input',()=>{$('positionInput').value=$('posSlider').value;});
$('clearConsole').addEventListener('click',()=>{$('console').replaceChildren();});
window.addEventListener('resize',()=>{
  renderChart('positionChart',samples,[{key:'POS',color:'#3dd7b5'},{key:'OBJ',color:'#ffb263'}],'°','regular');
  renderChart('dynamicChart',samples,[{key:'VEL',color:'#74a4ff'},{key:'I',color:'#e990bd'}],'','dual');
});
getPorts();pollState();setInterval(pollState,750);
