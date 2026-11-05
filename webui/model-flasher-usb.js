'use strict';
// 接続と表示だけを担当し、検証・比較・書き込みは段階 A の核に任せる。
const C = ModelFlasherCore, S = ModelFlasherSerial;
const $ = id => document.getElementById(id);
const names = {app:'アプリ',model:'モデル',dict:'辞書',params:'声の値の記録'};
let loader = null, transport = null, device = null, busy = false, checked = false, generation = 0;
let runs = [];
const selected = {};
const size = n => `${Number(n).toLocaleString('ja-JP')} B`;
const supported = Boolean(navigator.serial && window.isSecureContext);
let moduleError = null;
// 画面を開いた時点で取得する。接続ボタンの同期処理では先にポート選択を始める。
const esptoolReady = supported ? import('https://unpkg.com/esptool-js@0.5.5/bundle.js').catch(error => {
  moduleError = error;
  $('support').hidden = false;
  $('support').textContent = '接続モジュールを取得できません：'+(error.message || error);
  update();
  return null;
}) : Promise.resolve(null);
const log = text => { $('log').textContent += text + '\n'; };
function items() { return Object.keys(selected).filter(k => $('use'+k).checked).map(k => selected[k]); }
function update() {
  $('btnConnect').disabled = !supported || Boolean(moduleError) || busy || Boolean(loader);
  $('btnDisconnect').disabled = !transport || busy && Boolean(device);
  $('btnCheck').disabled = !device || busy || !items().length;
  $('btnWrite').disabled = !device || busy || !checked;
  for (const k of Object.keys(names).filter(k => k !== 'params')) {
    $(k+'File').disabled = busy; $('use'+k).disabled = busy;
  }
  for (const id of ['btnApp','btnDict','btnRefreshRuns','btnLoadRun','runs']) $(id).disabled = busy;
}
function invalidate() { checked = false; $('hashResult').textContent = '照合待ち'; update(); }
async function release() {
  ++generation; const old = transport; loader = null; transport = null; device = null; checked = false;
  try { await old?.disconnect(); } finally { update(); }
}
async function connect() {
  if (!supported || moduleError || busy) return;
  busy = true; update(); let timer;
  const token = ++generation;
  try {
    const portRequest = navigator.serial.requestPort({filters:[{usbVendorId:0x303a}]});
    const port = await portRequest;
    const esptool = await esptoolReady;
    if (!esptool) throw moduleError || Error('接続モジュールを取得できません');
    const {ESPLoader,Transport} = esptool;
    if (token !== generation) return;
    transport = new Transport(port,true); update();
    loader = new ESPLoader({transport,baudrate:921600,romBaudrate:115200,
      terminal:{clean(){},write:log,writeLine:log}});
    $('connection').textContent = 'USB Serial/JTAG をリセットして接続しています';
    // esptool-js 自身のリセットと再試行を打ち切らず、切断で中止できる。
    timer = setTimeout(() => { $('connection').textContent = '接続を再試行中です。中止する場合は「切断」'; },20000);
    const current = loader;
    await current.main();
    if (token !== generation) return;
    device = await S.inspect(current);
    if (token !== generation) return;
    $('deviceInfo').textContent = `アプリの版：${device.version}\n` +
      Object.entries(device.partitions).map(([k,p]) => `${k}：0x${p.address.toString(16)} / ${size(p.size)}`).join('\n') + '\n' +
      C.KEYS.map(k => `${k}=${device.flags?.[k] ?? '不明'}`).join('\n');
    $('connection').textContent = '接続しました（115200 → 921600）';
  } catch (error) {
    $('connection').textContent = '接続できません：'+error.message;
    await release();
  } finally { clearTimeout(timer); busy = false; update(); }
}
async function fetchLocal(url) {
  const response = await fetch(url,{cache:'no-store'});
  if (!response.ok) { let detail; try { detail = (await response.json()).error; } catch (_) {} throw Error(detail || `中継サーバの応答：${response.status}`); }
  return response;
}
async function select(kind, read, name, manifest, displayName = S.fileDisplayName(name)) {
  if (busy) return;
  delete selected[kind]; $('use'+kind).checked = false; invalidate(); busy = true; update();
  try {
    const source = await read(), bytes = new Uint8Array(source.bytes ?? source);
    let item;
    if (kind === 'model') item = await C.prepareModel(bytes,name,manifest);
    else if (kind === 'dict') item = await C.prepareDict(bytes,name);
    else { const problem = await S.appProblem(bytes); if (problem) throw Error(problem); item = {kind,bytes,sha:await C.sha256(bytes)}; }
    const state = [`${name}：${size(bytes.length)}`, `SHA-256（書込データ）${item.sha.slice(0,12)}…`];
    if (kind === 'app') {
      const flags = S.readAppFlags(bytes);
      state.push(`版：${source.version ?? S.appVersion(bytes)}`);
      state.push(flags ? `フラグ目印：あり（${C.KEYS.map(k => `${k}=${flags[k]}`).join('、')}）` : 'フラグ目印：なし');
    }
    if (kind === 'model') {
      if (/\.zip$/i.test(name)) state.push(`書込モデル：${size(item.bytes.length)}`);
      state.push(C.KEYS.map(k => `${k}=${item.manifest.model[k]}`).join('\n'));
      state.push('声の値：'+JSON.stringify(item.manifest.voice));
      item.displayName = displayName;
      state.push('端末に書く名前：'+(displayName || '名前なし'));
    }
    selected[kind] = {...item,kind}; $('use'+kind).checked = true;
    $(kind+'State').textContent = state.join('\n');
  } catch (error) { $(kind+'State').textContent = error.message; }
  finally { busy = false; update(); }
}
async function refreshRuns() {
  if (busy) return;
  busy = true; update();
  try {
    const catalog = await (await fetchLocal('/api/runs')).json(); runs = catalog.runs;
    $('runs').textContent = '';
    const empty = document.createElement('option'); empty.value = ''; empty.textContent = 'モデルを選んでください'; $('runs').appendChild(empty);
    runs.forEach((r,i) => {
      const option = document.createElement('option'); option.value = String(i);
      option.textContent = `${r.voice} / ${r.label} / ${Number.isFinite(r.updatedAt) ? new Date(r.updatedAt*1000).toLocaleString('ja-JP') : '日時不明'} / ${size(r.blobBytes)}`;
      $('runs').appendChild(option);
    });
    $('catalogState').textContent = `${runs.length} 件。${(catalog.warnings || []).join('。')}`;
  } catch (error) { $('catalogState').textContent = error.message; }
  finally { busy = false; update(); }
}
async function check() {
  if (!device || busy || !items().length) return;
  checked = false; busy = true; update();
  try {
    const result = await S.check(loader,items());
    $('hashResult').textContent = result.skipped.map(k => `${names[k]}：書き込み不要`).concat(result.changed.map(p => `${names[p.target]}：違います（書き込み対象）`)).join('\n');
    $('compatibility').textContent = ['形・容量・モデルのフラグを確認しました',...(result.warnings || [])].join('\n'); checked = true;
  } catch (error) { $('compatibility').textContent = '書き込めません：'+error.message; }
  finally { busy = false; update(); }
}
async function write() {
  if (!device || busy || !checked) return;
  busy = true; checked = false; update();
  try {
    const result = await S.run(loader,items(),{onProgress:p => {
      $('phase').textContent = names[p.target]+'を書き込み中'; $('progress').value = 100*p.bytes/p.total;
      $('progressText').textContent = `${size(p.bytes)} / ${size(p.total)}（${Math.round(100*p.bytes/p.total)}%）`;
      $('rate').textContent = `${size(Math.round(p.speed))}/秒・残り約 ${Math.ceil(p.remaining)} 秒`;
    }});
    $('hashResult').textContent = result.skipped.map(k => `${names[k]}：${result.recommitted.includes(k) ? '本体の書き込み不要・先頭 4 KB の再確定・全体 MD5 照合成功' : '書き込み不要'}`).concat(result.completed.map(k => `${names[k]}：書き込み・MD5 照合成功`)).join('\n');
    $('compatibility').textContent = (result.warnings || []).join('\n');
    if (!result.ok) throw Error(`${result.phase}：${result.error.message}。未完了：${result.pending.map(k => names[k]).join('、')}`);
    $('result').textContent = '完了しました。端末をリセットしました。新しい中身での起動を確認してください';
    $('phase').textContent = '完了'; await release(); $('connection').textContent = 'リセット後は再接続してください';
  } catch (error) { $('result').textContent = '完了できません：'+error.message; }
  finally { busy = false; update(); }
}
$('btnConnect').addEventListener('click',connect);
$('btnDisconnect').addEventListener('click',async () => { try { await release(); } catch (e) { log(e.message); } $('connection').textContent = '切断しました'; });
$('btnCheck').addEventListener('click',check); $('btnWrite').addEventListener('click',write);
for (const kind of ['app','model','dict']) {
  $('use'+kind).addEventListener('change',invalidate);
  $(kind+'File').addEventListener('change',() => {
    const file = $(kind+'File').files[0];
    if (file) return select(kind,() => file.arrayBuffer(),file.name);
    delete selected[kind]; invalidate();
  });
}
$('btnApp').addEventListener('click',() => select('app',async () => {
  const meta = await (await fetchLocal('/api/app.json')).json();
  const bytes = await (await fetchLocal('/api/app.bin')).arrayBuffer();
  if (bytes.byteLength !== meta.bytes || await C.sha256(new Uint8Array(bytes)) !== meta.sha256) throw Error('アプリの大きさ・SHA-256 が一致しません');
  return {bytes,version:meta.version};
},'app.bin（中継サーバ）'));
$('btnDict').addEventListener('click',() => select('dict',async () => (await fetchLocal('/api/dict.bin')).arrayBuffer(),'dict.bin（中継サーバ）'));
$('btnRefreshRuns').addEventListener('click',refreshRuns);
$('btnLoadRun').addEventListener('click',() => {
  const run = runs[Number($('runs').value)]; if (!$('runs').value || !run) return;
  return select('model',async () => (await fetchLocal(run.downloadUrl)).arrayBuffer(),run.run+'.bin',
    {...run,voice:run.vnoise === undefined ? undefined : {vnoise:run.vnoise}}, S.runDisplayName(run));
});
if (!supported) { $('support').hidden = false; $('support').textContent = 'Web Serial が使えません。PC の Chrome / Edge で http://localhost:8095/ を開いてください（安全な接続が必要です）'; }
update();
