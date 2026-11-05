// BLE の旧経路（接続・照合・転送・再起動・イベント登録）を保存。USB の画面からは呼ばない。
// 'use strict';
// // 流用元（読み取り専用）：notif の webui/control.js・control.html・control.css。
// // service filter の接続、char の個別取得、JSON read、切断イベント、自動再接続、時刻付きログを写した。
// // wire の正本：firmware/main/wire_format.h（モデル専用 0x0005 / 0x0006）。
// (() => {
//   const C = ModelFlasherCore;
//   const SERVICE = 'a1b20000-5d3f-4e8a-9c21-7f6e5d4c3b2a';
//   const UUID = {info:'a1b20004-5d3f-4e8a-9c21-7f6e5d4c3b2a',
//     model:'a1b20005-5d3f-4e8a-9c21-7f6e5d4c3b2a', state:'a1b20006-5d3f-4e8a-9c21-7f6e5d4c3b2a'};
//   const $ = id => document.getElementById(id);
//   const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
//   const size = bytes => `${Number(bytes).toLocaleString()} B`;
//   const flagsText = flags => C.KEYS.map(key => `${key}=${flags?.[key] ?? '不明'}`).join('\n');
//   const voiceText = v => v ? `k=${v.k}${v.k === 1 ? '（整形なし）' : `（中心 ${v.centerHz} Hz / 範囲 ${v.loHz}〜${v.hiHz} Hz / τ ${v.tauMs} ms）`} / vnoise=${Number(v.vnoise.toPrecision(6))}` : '不明';
//   const deviceVoiceText = m => !m ? '未接続' : m.voiceParams !== 1 ? '非対応のファームです（声の値は反映されません）' :
//     m.params ? `${m.params.source === 'record' ? '書き込み済みの記録を使用' : '記録なし（既定）'}：k=${m.params.k}${m.params.kSource === 'nvs' && m.params.source === 'record' ? '（端末で変更した値）' : ''} / vnoise=${m.params.vnoise}` : '不明';
//   const deviceDictText = m => !m ? '未接続' : !m.dict?.available ? 'dict 領域がありません（辞書は書けません）' :
//     `${size(m.dict.partitionBytes)}${m.dict.dirty ? '（辞書の書き込み後は発話を止めています。USB を一度抜いて差し直し、端末の電源を入れ直してください）' : ''}`;
//   let device = null, chars = {}, info = null, modelInfo = null, selected = null, runs = [];
//   let active = null, lastResult = null, busy = false, loading = false, catalogLoading = false;
//   let userDisconnect = false, connecting = false, reconnecting = false, expectingReboot = false;
//   let connectionToken = 0, selectionToken = 0;
//
//   function log(message, cls = '') {
//     const line = document.createElement('div'); line.className = cls;
//     line.textContent = `[${new Date().toLocaleTimeString('ja-JP')}] ${message}`;
//     $('log').insertBefore(line, $('log').firstChild);
//     while ($('log').children.length > 400) $('log').lastChild.remove();
//   }
//   function connected() { return Boolean(device?.gatt.connected && chars.model && chars.state && modelInfo); }
//   const kindName = item => item?.kind === 'dict' ? '辞書' : 'モデル';
//   function update() {
//     const ready = connected(), dictSel = selected?.kind === 'dict';
//     const differences = !dictSel && selected?.manifest && modelInfo ? C.compareFlags(modelInfo.model, selected.manifest.model) : [];
//     const dictIssue = dictSel && modelInfo ? C.dictProblem(modelInfo, selected) : null;
//     const fits = Boolean(selected && modelInfo && (dictSel ? !dictIssue : selected.bytes.length <= modelInfo.partitionBytes));
//     const voiceIssue = !dictSel && selected?.manifest && modelInfo ? C.voiceProblem(modelInfo, selected.manifest) : null;
//     const versionIssue = modelInfo ? C.protocolProblem(modelInfo) : null;
//     const supported = modelInfo?.protocol === C.WIRE_VERSION && modelInfo?.available && modelInfo?.maxData >= 32 && modelInfo?.bootReady !== false;
//     const idle = !busy && !loading && !connecting && !reconnecting;
//     $('btnConnect').disabled = Boolean(device?.gatt.connected || connecting || reconnecting || busy);
//     $('btnDisconnect').disabled = !device?.gatt.connected;
//     $('btnReadInfo').disabled = !device?.gatt.connected || busy || connecting || reconnecting;
//     $('btnWrite').textContent = dictSel ? 'この辞書を書き込む' : 'このモデルを書き込む';
//     $('btnWrite').disabled = !ready || !supported || !selected || (!dictSel && !selected.manifest) || !fits || differences.length > 0 ||
//       Boolean(voiceIssue) || !idle;
//     $('btnCheck').disabled = !ready || !supported || !selected || (!dictSel && !selected.manifest) || !fits || !idle;
//     $('btnAbort').disabled = !busy || !active;
//     $('btnReboot').disabled = !ready || !lastResult || lastResult.deviceId !== device?.id || busy || loading || expectingReboot || connecting || reconnecting;
//     $('modelFile').disabled = busy || loading;
//     $('dictFile').disabled = busy || loading;
//     $('runs').disabled = busy || loading || catalogLoading;
//     $('btnLoadRun').disabled = busy || loading || catalogLoading || !$('runs').value;
//     $('btnRefreshRuns').disabled = busy || loading || catalogLoading;
//     if (!selected) { $('compatibility').textContent = ''; return; }
//     let text, cls;
//     if (!modelInfo) { text = '端末に接続するとフラグと大きさを照合します'; cls = 'note'; }
//     else if (modelInfo.bootReady === false) { text = '端末の起動を待っています。起動後に端末の情報を読み直してください'; cls = 'note'; }
//     else if (versionIssue) { text = versionIssue; cls = 'ng'; }
//     else if (!supported) { text = 'モデル書き込み用のファームと十分な MTU が必要です。端末の情報を読み直すか再接続してください'; cls = 'ng'; }
//     else if (dictSel && dictIssue) { text = dictIssue; cls = 'ng'; }
//     else if (!fits) { text = '重みが端末の model パーティションに収まりません'; cls = 'ng'; }
//     else if (differences.length) { text = 'フラグが一致しないため書き込めません。\n' + differences.join('\n'); cls = 'ng'; }
//     else if (voiceIssue) { text = voiceIssue; cls = 'ng'; }
//     else if (!dictSel && !selected.manifest) { text = 'フラグを照合できないので書けません。model-builder の bundle（.zip）を選んでください'; cls = 'ng'; }
//     else if (dictSel) { text = '辞書の形（先頭 K1D1）と大きさを確認しました。書く前に端末の中身と照合し、同じなら書きません。書き込み中は発話・聞き取りを止め、書き終えたら USB を抜き差しして辞書を開き直します'; cls = 'ok'; }
//     else { text = '５つのフラグと重みの SHA-256 が一致しました。書き込めます'; cls = 'ok'; }
//     $('compatibility').textContent = text; $('compatibility').className = cls;
//     $('compatibility').style.whiteSpace = 'pre-wrap';
//   }
//   async function readJson(characteristic) {
//     let last;
//     for (let attempt = 0; attempt < 3; attempt++) {
//       try {
//         const value = await characteristic.readValue();
//         if (value.byteLength >= 512) throw new Error('端末の情報が長すぎて切れました。ファームを更新してください');
//         return JSON.parse(new TextDecoder().decode(new Uint8Array(value.buffer, value.byteOffset, value.byteLength)));
//       } catch (error) { last = error; if (attempt < 2) await delay(150); }
//     }
//     throw new Error('端末の情報を読めません：' + last.message);
//   }
//   async function readModelInfo(characteristic) {
//     return C.decodeModelInfo(await readJson(characteristic));
//   }
//   async function readInfo() {
//     info = chars.info ? await readJson(chars.info) : null;
//     modelInfo = chars.model ? await readModelInfo(chars.model) : null;
//     $('deviceName').textContent = device?.name || '名前なし';
//     $('partitionSize').textContent = modelInfo ? size(modelInfo.partitionBytes) : 'モデル書き込みに未対応';
//     $('dictPartitionSize').textContent = deviceDictText(modelInfo);
//     $('voiceReady').textContent = modelInfo ? (modelInfo.bootReady === false ? '端末を起動しています' : modelInfo.voiceReady ? '声の読み込みが完了しています' : '声の読み込みに失敗しています。モデルを書き直せます') : '不明';
//     $('deviceFlags').textContent = flagsText(modelInfo?.model);
//     $('deviceVoice').textContent = deviceVoiceText(modelInfo);
//     $('infoView').textContent = JSON.stringify({INFO:info, model:modelInfo}, null, 2);
//     if (!modelInfo || !chars.state) log('モデル書き込み用のファームが必要です', 'ng');
//     else if (C.protocolProblem(modelInfo)) log(C.protocolProblem(modelInfo), 'ng');
//     if (lastResult && (modelInfo?.session !== lastResult.session || modelInfo?.sha256 !== lastResult.sha)) {
//       lastResult = null;
//     }
//     update();
//   }
//   function onState(event) {
//     try { active?.acceptStatus(event.target.value); }
//     catch (error) { log(error.message, 'ng'); }
//   }
//   async function acquireGattAndChars(token) {
//     const server = await device.gatt.connect();
//     const service = await server.getPrimaryService(SERVICE); const acquired = {};
//     for (const [name, uuid] of Object.entries(UUID)) {
//       try { acquired[name] = await service.getCharacteristic(uuid); log(`${name === 'info' ? '端末情報' : name === 'model' ? 'モデル転送' : 'モデル状態'}の口を取得しました`, 'ok'); }
//       catch (_) { log(`${name === 'info' ? '端末情報' : name === 'model' ? 'モデル転送' : 'モデル状態'}の口がありません`, 'ng'); }
//     }
//     if (token !== connectionToken || userDisconnect) return;
//     chars = acquired;
//     if (chars.state) {
//       chars.state.removeEventListener('characteristicvaluechanged', onState);
//       chars.state.addEventListener('characteristicvaluechanged', onState);
//       await chars.state.startNotifications();
//     }
//     await readInfo();
//     for (let attempt = 0; modelInfo?.bootReady === false && attempt < 30; attempt++) {
//       await delay(500);
//       if (token !== connectionToken || userDisconnect) return;
//       await readInfo();
//     }
//     if (token !== connectionToken || userDisconnect) return;
//     $('connection').textContent = '接続しました'; log('接続し、端末の情報を読みました', 'ok');
//     if (expectingReboot) {
//       expectingReboot = false;
//       $('result').textContent = modelInfo?.bootReady === false ? '再接続しましたが、端末は起動中です。しばらく待って端末の情報を読み直してください' : modelInfo?.voiceReady ? '再起動後に再接続しました。新しい声の読み込みは正常です' : '再起動後に再接続しましたが、声を読み込めません。フラグと重みを確認して書き直してください';
//       log($('result').textContent, modelInfo?.voiceReady ? 'ok' : 'ng');
//     }
//   }
//   async function connect() {
//     if (!navigator.bluetooth || !window.isSecureContext) { log('Chrome / Edge で localhost の画面を開いてください', 'ng'); return; }
//     connecting = true; userDisconnect = false; update();
//     try {
//       if (device) device.removeEventListener('gattserverdisconnected', onDisconnected);
//       device = await navigator.bluetooth.requestDevice({filters:[{services:[SERVICE]}]});
//       device.addEventListener('gattserverdisconnected', onDisconnected);
//       await acquireGattAndChars(++connectionToken);
//     } catch (error) { log('接続できません：' + error.message, 'ng'); }
//     finally { connecting = false; update(); }
//   }
//   async function autoReconnect() {
//     if (reconnecting || userDisconnect || !device) return;
//     reconnecting = true; const token = ++connectionToken; update();
//     try {
//       for (let attempt = 1; attempt <= 10; attempt++) {
//         await delay(Math.min(3000, attempt * 500));
//         if (userDisconnect || token !== connectionToken) return;
//         try { await acquireGattAndChars(token); return; }
//         catch (error) { log(`自動再接続 ${attempt}/10 に失敗しました：${error.message}`, 'ng'); }
//       }
//       expectingReboot = false; $('connection').textContent = '自動再接続できません。端末を探して接続し直してください';
//     } finally { reconnecting = false; update(); }
//   }
//   function onDisconnected() {
//     active?.connectionLost(); chars = {}; modelInfo = null; info = null;
//     $('connection').textContent = expectingReboot ? '再起動を待っています' : '接続が切れました';
//     if (!userDisconnect && !expectingReboot && lastResult && !busy) {
//       $('result').textContent = '接続が切れました。端末の電源を入れ直したら「端末を探して接続」で接続し直してください';
//     }
//     $('partitionSize').textContent = '未接続'; $('dictPartitionSize').textContent = '未接続'; $('voiceReady').textContent = '未接続'; $('deviceFlags').textContent = '未接続'; $('deviceVoice').textContent = '未接続';
//     log('Bluetooth の接続が切れました。未完了の書き込みは最初からやり直します', 'ng');
//     update(); if (!userDisconnect) void autoReconnect();
//   }
//   function disconnect() {
//     userDisconnect = true; ++connectionToken; expectingReboot = false;
//     if (device?.gatt.connected) device.gatt.disconnect();
//     else onDisconnected();
//     update();
//   }
//   async function fetchLocal(url) {
//     let response;
//     try { response = await fetch(url, {cache:'no-store'}); }
//     catch (_) { throw new Error('中継サーバに届きません。python3 tools/model_flasher_server.py を起動してください'); }
//     if (!response.ok) {
//       let message = `中継サーバがエラーを返しました（${response.status}）`;
//       try { message = (await response.json()).error || message; } catch (_) { /* 本文が JSON でないときは上の文言を使う。 */ }
//       throw new Error(message);
//     }
//     return response;
//   }
//   async function refreshRuns() {
//     catalogLoading = true; update(); $('catalogState').textContent = 'モデルの一覧と bundle の情報を取得しています';
//     try {
//       const catalog = await (await fetchLocal('/api/runs')).json(); runs = catalog.runs;
//       $('runs').textContent = '';
//       const empty = document.createElement('option'); empty.value = ''; empty.textContent = 'モデルを選んでください'; $('runs').appendChild(empty);
//       for (let i = 0; i < runs.length; i++) {
//         const run = runs[i], option = document.createElement('option'); option.value = String(i);
//         const date = Number.isFinite(run.updatedAt) ? new Date(run.updatedAt * 1000).toLocaleString('ja-JP') : '日時不明';
//         option.textContent = `${run.voice} / ${run.label} / ${date} / ${size(run.blobBytes)}`; $('runs').appendChild(option);
//       }
//       $('catalogState').textContent = `${runs.length} 件のモデルがあります。${(catalog.warnings || []).join('。')}`;
//       log(`モデルの一覧を取得しました：${runs.length} 件`, 'ok');
//     } catch (error) { $('catalogState').textContent = error.message; log(error.message, 'ng'); }
//     finally { catalogLoading = false; update(); }
//   }
//   function beginSelection(kind = 'model') {
//     selected = null; lastResult = null; loading = true;
//     // 選べるのはモデルか辞書のどちらか 1 つ。選び直した側の表示だけを残す。
//     if (kind === 'dict') {
//       $('dictState').textContent = '辞書を読み込み、SHA-256 を計算しています';
//       $('selectionState').textContent = 'モデルは選ばれていません'; $('modelFlags').textContent = '未取得'; $('voiceValues').textContent = '未取得';
//     } else {
//       $('modelFlags').textContent = '読み込み中'; $('voiceValues').textContent = '読み込み中'; $('selectionState').textContent = '重みを読み込み、SHA-256 を計算しています';
//       $('dictState').textContent = '辞書は選ばれていません';
//     }
//     $('result').textContent = 'まだ書き込んでいません'; $('hashResult').textContent = '照合待ち';
//     update(); return ++selectionToken;
//   }
//   async function finishSelection(token, input, name, manifest) {
//     const prepared = await C.prepareModel(input, name, manifest);
//     if (token !== selectionToken) return;
//     selected = prepared;
//     $('selectionState').textContent = `${name}：${size(selected.bytes.length)}\nSHA-256：${selected.sha}`;
//     $('modelFlags').textContent = flagsText(selected.manifest.model);
//     $('voiceValues').textContent = voiceText(selected.manifest.voice);
//     log(`重みの読み込みと SHA-256 の計算が完了しました：${name}`, 'ok');
//   }
//   async function loadRun() {
//     const run = runs[Number($('runs').value)]; if (!run || !$('runs').value) return;
//     const token = beginSelection();
//     try {
//       const response = await fetchLocal(run.downloadUrl);
//       // 一覧の run の voice は声の名前。build-flags の voice.vnoise は run.vnoise で届くので、manifest の形に戻す。
//       const manifest = {...run, voice:run.vnoise === undefined ? undefined : {vnoise:run.vnoise}};
//       await finishSelection(token, await response.arrayBuffer(), run.run + '.bin', manifest);
//     } catch (error) { $('selectionState').textContent = error.message; log(error.message, 'ng'); }
//     finally { loading = false; update(); }
//   }
//   async function loadFile() {
//     const file = $('modelFile').files[0]; if (!file) return;
//     const token = beginSelection();
//     try { await finishSelection(token, await file.arrayBuffer(), file.name); }
//     catch (error) { $('selectionState').textContent = error.message; log(error.message, 'ng'); }
//     finally { loading = false; update(); }
//   }
//   async function loadDict() {
//     const file = $('dictFile').files[0]; if (!file) return;
//     const token = beginSelection('dict');
//     try {
//       const prepared = await C.prepareDict(await file.arrayBuffer(), file.name);
//       if (token !== selectionToken) return;
//       selected = prepared;
//       $('dictState').textContent = `${file.name}：${size(prepared.bytes.length)}\nSHA-256：${prepared.sha}\n辞書の形：先頭 K1D1・版 ${prepared.bytes[4] | (prepared.bytes[5] << 8)}`;
//       log(`辞書の読み込みと SHA-256 の計算が完了しました：${file.name}`, 'ok');
//     } catch (error) { $('dictState').textContent = error.message; log(error.message, 'ng'); }
//     finally { loading = false; update(); }
//   }
//   function progress(p) {
//     const percent = 100 * p.bytes / p.total, remaining = p.speed ? (p.total-p.bytes)/p.speed : 0;
//     $('progress').value = percent;
//     $('progressText').textContent = `${size(p.bytes)} / ${size(p.total)}（${percent.toFixed(1)}%）`;
//     $('rate').textContent = p.speed ? `${(p.speed/1024).toFixed(1)} KiB/秒・経過 ${Math.round(p.elapsed)} 秒・残り約 ${Math.ceil(remaining)} 秒` : 'データの受信を待っています。残り時間は受信後に表示します';
//   }
//   function newTransfer() {
//     const transferChars = {...chars};
//     return new C.Transfer({
//       async write(bytes, response) {
//         if (response || !transferChars.model.properties.writeWithoutResponse) await transferChars.model.writeValueWithResponse(bytes);
//         else await transferChars.model.writeValueWithoutResponse(bytes);
//       },
//       async readStatus() { return transferChars.state.readValue(); },
//       async readModelInfo() { return readModelInfo(transferChars.model); },
//     }, {onProgress:progress, onPhase:message => { $('phase').textContent = message; log(message); }, onLog:log});
//   }
//   async function checkDevice() {
//     if ($('btnCheck').disabled) return;
//     busy = true; const prepared = selected, metadata = modelInfo; active = newTransfer(); update();
//     $('result').textContent = '端末の中身を照合しています'; $('hashResult').textContent = '照合待ち';
//     try {
//       const r = await active.alreadyInstalled(prepared, metadata);
//       $('phase').textContent = '照合しました';
//       $('hashResult').textContent = `選んだ${kindName(prepared)}：${prepared.sha}\n端末の先頭 ${size(prepared.bytes.length)}：${r.deviceSha}`;
//       $('result').textContent = r.same ? `書き込み不要：端末にはすでに同じ${kindName(prepared)}が入っています`
//         : r.bytesSame ? '重みは同じですが、声の値が端末の記録と違います。書き込むと声の値を更新します'
//           : `端末の中身は選んだ${kindName(prepared)}と違います。書き込めます`;
//       log($('result').textContent, r.same ? 'ok' : '');
//     } catch (error) { $('phase').textContent = '照合できません'; $('result').textContent = '端末の中身を照合できません：' + error.message; log($('result').textContent, 'ng'); }
//     finally { busy = false; active = null; update(); }
//   }
//   async function writeModel() {
//     if ($('btnWrite').disabled) return;
//     busy = true; lastResult = null;
//     const prepared = selected; const metadata = modelInfo; const deviceId = device.id; const name = kindName(prepared);
//     active = newTransfer();
//     $('progress').value = 0; $('progressText').textContent = `0 B / ${size(prepared.bytes.length)}（0%）`;
//     $('result').textContent = '書き込み中'; $('hashResult').textContent = '照合待ち'; update();
//     try {
//       const result = await active.run(prepared, metadata);
//       if (result.skipped) {
//         $('phase').textContent = '書き込み不要でした';
//         $('result').textContent = `書き込み不要：端末にはすでに同じ${name}が入っています（何も書いていません）`;
//         $('hashResult').textContent = `選んだ${name}：${result.sha}\n端末の先頭 ${size(result.bytes)}：${result.deviceSha}\nSHA-256 が一致しました`;
//         log($('result').textContent, 'ok');
//         return;
//       }
//       lastResult = {...result, deviceId};
//       $('phase').textContent = '書き込みと照合が完了しました';
//       $('result').textContent = result.target === 'dict'
//         ? '書き込み成功。USB を一度抜いて差し直し、端末の電源を入れ直してください。新しい辞書を開き直します。差し直したら「端末を探して接続」で接続し直してください'
//         : '書き込み成功。USB を一度抜いて差し直し、端末の電源を入れ直してください。差し直したら「端末を探して接続」で接続し直してください（声の値：' + voiceText(result.voice) + '）';
//       $('hashResult').textContent = `ブラウザ：${result.sha}\n端末　　：${result.sha}\n${size(result.bytes)} の SHA-256 が一致しました`;
//       log('端末とブラウザの SHA-256 が一致しました。USB を一度抜いて差し直し、端末の電源を入れ直してください', 'ok');
//     } catch (error) {
//       $('phase').textContent = '書き込みを終了しました'; $('result').textContent = '書き込みを完了できません：' + error.message;
//       $('hashResult').textContent = error.deviceSha ? `ブラウザ：${error.browserSha}\n端末　　：${error.deviceSha}\n${size(error.hashBytes)} の SHA-256 が一致しません。書き直してください` : '照合成功を確認できていません。再接続後、最初から書き直せます';
//       log('書き込みを完了できません：' + error.message, 'ng');
//     } finally { busy = false; active = null; update(); }
//   }
//   async function reboot() {
//     if (!lastResult || !connected() || lastResult.deviceId !== device.id || busy) return;
//     const result = lastResult; busy = true; update();
//     try {
//       const current = await readModelInfo(chars.model);
//       if (current.state !== C.STATE.VERIFIED || current.session !== result.session || current.sha256 !== result.sha || current.hashBytes !== result.bytes ||
//           current.target !== (result.target === 'dict' ? C.TARGET.dict : C.TARGET.model))
//         throw new Error('照合済みのモデル（辞書）を確認できません。最初から書き直してください');
//       expectingReboot = true;
//       await chars.model.writeValueWithResponse(C.packet(C.OP.REBOOT, result.session));
//       $('result').textContent = '再起動を指示しました。切断と自動再接続を待っています';
//       log('端末へ再起動を指示しました'); lastResult = null;
//     } catch (error) { expectingReboot = false; log(error.message, 'ng'); $('result').textContent = error.message; }
//     finally { busy = false; update(); }
//   }
//   $('btnConnect').addEventListener('click', connect);
//   $('btnDisconnect').addEventListener('click', disconnect);
//   $('btnReadInfo').addEventListener('click', () => readInfo().catch(error => log(error.message, 'ng')));
//   $('btnRefreshRuns').addEventListener('click', refreshRuns);
//   $('runs').addEventListener('change', update);
//   $('btnLoadRun').addEventListener('click', loadRun);
//   $('modelFile').addEventListener('change', loadFile);
//   $('dictFile').addEventListener('change', loadDict);
//   $('btnCheck').addEventListener('click', checkDevice);
//   $('btnWrite').addEventListener('click', writeModel);
//   $('btnAbort').addEventListener('click', () => { active?.cancel(); $('phase').textContent = '端末の中止を待っています'; });
//   $('btnReboot').addEventListener('click', reboot);
//   $('btnClearLog').addEventListener('click', () => { $('log').textContent = ''; });
//   if (!navigator.bluetooth || !window.isSecureContext) {
//     $('support').hidden = false;
//     $('support').textContent = 'Web Bluetooth が使えません。PC の Chrome / Edge で http://localhost:8095/ を開いてください';
//   }
//   update();
// })();
