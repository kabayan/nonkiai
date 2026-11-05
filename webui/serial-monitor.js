/* Pure protocol functions are shared by browser and host tests. */
(function (root) {
  'use strict';
  const states = ['standby', 'idle', 'capture', 'reply'];
  function parseNkLine(line) {
    if (!line.startsWith('@NK ')) return null;
    try {
      const e = JSON.parse(line.slice(4));
      if (!e || typeof e !== 'object' || Array.isArray(e)) return null;
      switch (e.ev) {
        case 'mode': return [0, 1].includes(e.mode) ? e : null;
        case 'status': return [0, 1].includes(e.mode) && states.includes(e.state) && typeof e.speaking === 'boolean' && typeof e.standby_wanted === 'boolean' ? e : null;
        case 'state': return states.includes(e.state) && typeof e.reason === 'string' ? e : null;
        case 'wake': return Number.isFinite(e.score) ? e : null;
        case 'listen': return [0, 1].includes(e.mode) && typeof e.intent_name === 'string' && ['accepted', 'out_of_set', 'too_short', 'rejected', 'error'].includes(e.result) && (Number.isFinite(e.score) || e.score === null) ? e : null;
        case 'speech': return e.phase === 'done' || (e.phase === 'start' && typeof e.text === 'string') ? e : null;
        default: return null;
      }
    } catch (_) { return null; }
  }
  function initialState() { return { mode: null, state: 'idle', reason: '状態未取得', speaking: false, wake: null, listens: [], speeches: [] }; }
  function applyEvent(previous, e, time) {
    const s = { ...previous };
    if (!e) return s;
    if (e.ev === 'mode' || e.ev === 'status') s.mode = e.mode;
    if (e.ev === 'state' || e.ev === 'status') { s.state = e.state; s.reason = e.reason || ''; }
    if (e.ev === 'status') { s.speaking = e.speaking; s.standby_wanted = e.standby_wanted; if (!e.speaking) s.speeches = s.speeches.map(x => ({ ...x, active: false })); }
    if (e.ev === 'wake') s.wake = { ...e, time };
    if (e.ev === 'listen') s.listens = [{ ...e, time }, ...s.listens].slice(0, 50);
    if (e.ev === 'speech') {
      s.speaking = e.phase === 'start';
      s.speeches = s.speeches.map(x => ({ ...x, active: false }));
      if (e.phase === 'start') s.speeches = [{ text: e.text, time, active: true }, ...s.speeches].slice(0, 50);
    }
    return s;
  }
  function makeLineReader(onLine) {
    let pending = '', discarding = false;
    return chunk => {
      let offset = 0;
      while (offset < chunk.length) {
        const newline = chunk.indexOf('\n', offset), end = newline < 0 ? chunk.length : newline;
        if (!discarding) {
          if (pending.length + end - offset > 65536) { pending = ''; discarding = true; }
          else pending += chunk.slice(offset, end);
        }
        if (newline < 0) break;
        if (!discarding) onLine(pending.replace(/\r$/, ''));
        pending = ''; discarding = false; offset = newline + 1;
      }
    };
  }

  const api = { parseNkLine, applyEvent, initialState, makeLineReader };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (typeof document === 'undefined') return;
  const $ = id => document.getElementById(id);
  let state = initialState(), port = null, reader = null, readTask = null, closing = false;
  let logs = [], retry = null, statusSeen = false, writeChain = Promise.resolve();
  const modes = ['ぬいぐるみ', 'おもちゃ'];
  const reasons = { 'sleep-or-quiz': '睡眠・診断中', speech: '発話中・発話待ち', interaction: '動作中', 'imu-suspended': 'センサー停止中', 'pcm-error': '録音エラー', 'not-ready': '認識の準備前・異常', transition: '切り替え待ち', finished: '完了', wake: '起動語を検出', tap: 'タップ', cue: '合図音', 'stop-failed': '認識を停止できません' };
  const timeText = t => new Date(t).toLocaleTimeString('ja-JP');
  function renderLogs() { $('logs').textContent = logs.filter(x => x.includes($('filter').value)).join('\n'); if ($('scroll').checked) $('logs').scrollTop = $('logs').scrollHeight; }
  function raw(line) { logs.push(line); logs = logs.slice(-500); renderLogs(); }
  function render() {
    const reason = reasons[state.reason] || state.reason;
    $('state').textContent = state.speaking ? '話しています' : ({ standby: '起動語を待っています', capture: '聞き取り中', reply: '返事中' }[state.state] || `待受なし（${reason || '切り替え待ち'}）`);
    $('reason').textContent = reason || '';
    $('mode').textContent = modes[state.mode] || '未取得';
    $('toy').setAttribute('aria-pressed', String(state.mode === 1)); $('plush').setAttribute('aria-pressed', String(state.mode === 0));
    if (state.wake) $('wake').textContent = `起動語: ${state.wake.score.toFixed(3)} / ${timeText(state.wake.time)}`;
    $('listens').replaceChildren(...state.listens.map(e => { const li = document.createElement('li'); li.textContent = `${timeText(e.time)} / ${modes[e.mode]} / ${e.intent_name} / ${e.score === null ? '—' : e.score.toFixed(3)} / ${e.result}`; return li; }));
    $('speeches').replaceChildren(...state.speeches.map(e => { const li = document.createElement('li'); li.textContent = `${timeText(e.time)} / ${e.text}${e.active ? '（発話中）' : ''}`; li.classList.toggle('active', e.active); return li; }));
  }
  function connected(on, message) { $('connection').textContent = message; $('connect').disabled = on; for (const id of ['disconnect', 'toy', 'plush', 'status', 'sendSay']) $(id).disabled = !on; }
  function send(command) {
    const target = port;
    writeChain = writeChain.catch(() => {}).then(async () => {
      if (!target || target !== port || !target.writable || closing) throw new Error('未接続です');
      const writer = target.writable.getWriter();
      try { await writer.write(new TextEncoder().encode(command + '\n')); } finally { writer.releaseLock(); }
    });
    return writeChain;
  }
  function sendUi(command) { send(command).catch(e => raw(`送信エラー: ${e.message}`)); }
  async function cleanup(message) {
    if (closing) return;
    closing = true; clearTimeout(retry);
    const old = port; port = null;
    if (reader) { try { await reader.cancel(); } catch (_) {} }
    if (readTask) { try { await readTask; } catch (_) {} }
    await writeChain.catch(() => {});
    if (old) { try { await old.close(); } catch (_) {} }
    reader = null; readTask = null; closing = false;
    state = applyEvent(state, { ev: 'speech', phase: 'done' }, Date.now());
    state.state = 'idle'; state.reason = '未接続'; render(); connected(false, message);
  }
  $('connect').onclick = async () => {
    if (!navigator.serial) { connected(false, 'Web Serial 対応の PC Chrome を localhost で開いてください'); return; }
    $('connect').disabled = true;
    try {
      const chosen = await navigator.serial.requestPort();
      await chosen.open({ baudRate: 115200 }); port = chosen;
      try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch (e) { raw(`制御線の設定: ${e.message}`); }
      state = initialState(); render(); statusSeen = false;
      const decoder = new TextDecoder();
      const feed = makeLineReader(line => { const e = parseNkLine(line); if (e) { state = applyEvent(state, e, Date.now()); if (e.ev === 'status') statusSeen = true; render(); } else raw(line); });
      reader = port.readable.getReader();
      readTask = (async () => {
        try { for (;;) { const { value, done } = await reader.read(); if (done) break; feed(decoder.decode(value, { stream: true })); } feed(decoder.decode()); }
        finally { reader.releaseLock(); reader = null; }
      })();
      readTask.then(() => { if (!closing) void cleanup('切断されました'); }, e => { raw(`受信エラー: ${e.message}`); if (!closing) void cleanup('接続が切れました'); });
      connected(true, '接続中'); sendUi('status');
      retry = setTimeout(() => { if (!statusSeen && port) sendUi('status'); }, 4000);
    } catch (e) { raw(`接続エラー: ${e.message}`); await cleanup('接続できませんでした'); }
  };
  $('disconnect').onclick = () => void cleanup('切断しました');
  if (navigator.serial) navigator.serial.addEventListener('disconnect', e => { if (e.target === port) void cleanup('USB が切断されました'); });
  $('toy').onclick = () => sendUi('mode toy'); $('plush').onclick = () => sendUi('mode plush'); $('status').onclick = () => sendUi('status');
  $('sayForm').onsubmit = e => { e.preventDefault(); const text = $('say').value.trim(); if (!text) return; if (/[\r\n]/.test(text) || new TextEncoder().encode(text).length > 127) { raw('say は改行なし・UTF-8 127 B までです'); return; } sendUi('say ' + text); };
  $('filter').oninput = renderLogs; $('scroll').onchange = renderLogs; render();
})(typeof globalThis !== 'undefined' ? globalThis : this);
