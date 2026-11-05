'use strict';
// ブラウザとホスト試験で共用する ZIP・照合・モデル転送処理。
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.ModelFlasherCore = api;
})(globalThis, function () {
  const KEYS = ['SAAN_DEC_HEAD', 'SAAN_G_EXCITE', 'SAAN_PHRASE_RULE_SBV2', 'SAAN_TAIL_PAUSE', 'SAAN_INT8_ACT'];
  const WIRE_VERSION = 3;   // firmware/main/wire_format.h の NK_MODEL_WIRE_VERSION と同じ（MODEL JSON の短縮）
  const OP = {BEGIN:1, DATA:2, END:3, ABORT:4, REBOOT:5, QUERY:6, PARAMS:7, PROBE:8};
  const TARGET = {model:0, dict:1};
  const DICT_MAGIC = [0x4b, 0x31, 0x44, 0x31];   // "K1D1"
  const DICT_VERSIONS = [1, 2];
  const TIMEOUT = {
    model:{begin:60000, end:30000, probe:30000},
    // 辞書は 3 MB 弱。64 KB ずつ消す時間と、Flash を 2 回読み直す時間が model より長い（実機では未測定。余裕を取る）。
    dict:{begin:240000, end:120000, probe:60000},
  };
  const STATE = {IDLE:0, ERASING:1, RECEIVING:2, VERIFYING:3, VERIFIED:4, ABORTED:5, FAILED:6, RESTARTING:7};
  const ERRORS = ['正常', 'コマンドの形が不正です', '重みの大きさが不正です', '端末の音声処理または書き込みが使用中です',
    '転送の番号が一致しません', '受信位置が飛んでいます', '端末の Flash の処理に失敗しました', '端末の SHA-256 が一致しません',
    '重みが最後まで届いていません', '受信待ちが時間切れになりました', '接続が切れました', '中止しました', '照合が成功していないため再起動できません',
    '書き込み先の領域が無い、またはその領域に使えない操作です（辞書は先頭が K1D1 の辞書だけ書けます）'];
  const MAX_BYTES = 16 * 1024 * 1024;
  const numberFlag = value => {
    const text = String(value);
    return /^\d+$/.test(text) && Number.isSafeInteger(Number(text)) ? String(Number(text)) : null;
  };
  function compareFlags(device, model) {
    return KEYS.flatMap(key => {
      const a = numberFlag(device?.[key]), b = numberFlag(model?.[key]);
      return a !== null && b !== null && a === b ? [] : [`${key}：端末=${a ?? '不明'}、重み=${b ?? '不明'}`];
    });
  }
  const DEFAULT_VNOISE = 0.03, DEFAULT_TAU_MS = 75, K_MAX = 3;
  // build-flags.json の数値は number か数値の文字列（loHz / hiHz は文字列で来る）。それ以外は NaN。
  const toNumber = value => typeof value === 'number' ? value :
    typeof value === 'string' && /^[+-]?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value.trim()) ? Number(value) : NaN;
  // 声ごとの値。f0Shape が無ければ k=1（整形なし）。励起つきモデルで voice.vnoise が無ければ拒否する。
  function validateVoice(value) {
    const excite = numberFlag(value.model?.SAAN_G_EXCITE) === '1';
    // 検証済みの manifest（voice に k がある）を再び検証しても同じ値になるようにする。
    const done = value.voice && value.voice.k !== undefined ? value.voice : null;
    const shape = value.f0Shape ?? (done ? {k:done.k, centerHz:done.centerHz, loHz:done.loHz, hiHz:done.hiHz, tauMs:done.tauMs} : undefined);
    let k = 1, centerHz = 0, loHz = 0, hiHz = 0, tauMs = DEFAULT_TAU_MS;
    if (shape !== undefined && shape !== null) {
      if (typeof shape !== 'object') throw new Error('build-flags.json の f0Shape が不正です');
      k = toNumber(shape.k); centerHz = toNumber(shape.centerHz); loHz = toNumber(shape.loHz); hiHz = toNumber(shape.hiHz);
      tauMs = shape.tauMs === undefined ? DEFAULT_TAU_MS : toNumber(shape.tauMs);
      if (!(k >= 0 && k <= K_MAX)) throw new Error('build-flags.json の f0Shape.k が範囲外です（0〜3）');
      if (k !== 1 && !(centerHz > 0 && loHz > 0 && loHz < centerHz && hiHz > centerHz && tauMs > 0))
        throw new Error('build-flags.json の f0Shape の中心・範囲・時定数が不正です');
      if (k === 1) { if (!(centerHz >= 0)) centerHz = 0; if (!(loHz >= 0)) loHz = 0; if (!(hiHz >= 0)) hiHz = 0; if (!(tauMs > 0)) tauMs = DEFAULT_TAU_MS; }
    }
    let vnoise = DEFAULT_VNOISE;
    const raw = value.voice?.vnoise;
    if (raw === undefined || raw === null) {
      if (excite) throw new Error('励起つきモデルですが build-flags.json に voice.vnoise がありません。bundle を作り直してください');
    } else {
      vnoise = toNumber(raw);
      if (!(vnoise > 0 && vnoise <= 1)) throw new Error('build-flags.json の voice.vnoise が不正です（0 より大きく 1 以下）');
    }
    return {k, centerHz, loHz, hiHz, tauMs, vnoise: Math.fround(vnoise)};
  }
  // 端末のファームの版と、この画面の版が合っているか。合わなければ止める（黙って別の領域へ書かない）。
  function protocolProblem(device) {
    if (!device || device.protocol === WIRE_VERSION) return null;
    if (Number.isInteger(device.protocol) && device.protocol < WIRE_VERSION)
      return `端末のファームが古い（書き込みの版 ${device.protocol}）ため、この画面（版 ${WIRE_VERSION}）では書けません。ファームを更新してください`;
    return `端末の書き込みの版（${device.protocol ?? '不明'}）が、この画面の版（${WIRE_VERSION}）と違うため書けません。画面を更新してください`;
  }
  // 版 3 の短い JSON を画面・転送処理で使う名前に戻す。別の版は版番号をそのまま検査する。
  function decodeModelInfo(wire) {
    if (!wire || wire.protocol !== WIRE_VERSION) return wire;
    const [source, kSource, k, centerHz, loHz, hiHz, tauMs, vnoise] = wire.p || [];
    const [available, partitionBytes, dirty] = wire.d || [];
    const [request, target, bytes, sha256] = wire.q || [];
    const [state, session, hash, hashBytes, writeTarget] = wire.w || [];
    return {protocol:wire.protocol, available:wire.a === 1 || wire.a === true, partitionBytes:wire.c, maxData:wire.m,
      windowChunks:8, voiceParams:1, paramsBytes:4096, bootReady:wire.b === 1 || wire.b === true, voiceReady:wire.v === 1 || wire.v === true,
      model:Object.fromEntries(KEYS.map((key, i) => [key, wire.f?.[i]])),
      params:{source:source ? 'record' : 'default', kSource:kSource ? 'nvs' : 'record', k, centerHz, loHz, hiHz, tauMs, vnoise},
      state, session, sha256:hash, hashBytes, target:writeTarget,
      dict:{available:available === 1 || available === true, partitionBytes, dirty:dirty === 1 || dirty === true}, probe:{request, target, bytes, sha256}};
  }
  // 辞書として正しい形か。端末が jdict_open で最初に見る先頭の "K1D1" と版だけを確かめる（深い検査は端末が起動時にする）。
  function dictFormatProblem(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 8) return '辞書として小さすぎます';
    if (DICT_MAGIC.some((b, i) => bytes[i] !== b)) return '辞書ではありません（先頭が K1D1 ではありません）';
    const version = bytes[4] | (bytes[5] << 8);
    if (!DICT_VERSIONS.includes(version)) return `辞書の版（${version}）にこの画面は対応していません`;
    return null;
  }
  async function prepareDict(input, name) {
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (!bytes.length || bytes.length > MAX_BYTES) throw new Error('辞書の大きさが不正です');
    const problem = dictFormatProblem(bytes);
    if (problem) throw new Error(problem);
    return {kind:'dict', bytes, name, manifest:null, sha:await sha256(bytes)};
  }
  // 端末の dict 領域に収まるか。収まらない辞書は書かない。
  function dictProblem(device, dict) {
    if (!device?.dict?.available) return '端末のファームに dict 領域がありません';
    if (!Number.isInteger(device.dict.partitionBytes) || dict.bytes.length > device.dict.partitionBytes)
      return `辞書が端末の dict 領域に収まりません（辞書 ${dict.bytes.length.toLocaleString()} B / 領域 ${Number(device.dict.partitionBytes).toLocaleString()} B）`;
    return null;
  }
  // 端末が今使っている声の値が、これから書く bundle の値と同じか（モデル本体が同じでも、値が違えば書き直す）。
  function sameVoiceValues(device, voice) {
    const p = device?.params;
    if (!p || p.source !== 'record' || !voice) return false;
    const near = (a, b, tol) => Number.isFinite(a) && Math.abs(a - b) <= tol;
    // 版 3 は値を十分な有効桁数で返す。既存の比較許容差を保つ。
    return near(p.k, voice.k, 6e-5) && near(p.centerHz, voice.centerHz, 6e-5) && near(p.loHz, voice.loHz, 6e-5) &&
      near(p.hiHz, voice.hiHz, 6e-5) && near(p.tauMs, voice.tauMs, 6e-3) && near(p.vnoise, voice.vnoise, 6e-5);
  }
  function validateManifest(value) {
    if (!value || !Number.isSafeInteger(value.blobBytes) || value.blobBytes <= 0 || value.blobBytes > MAX_BYTES)
      throw new Error('build-flags.json の blobBytes が不正です');
    if (typeof value.blobSha256 !== 'string' || !/^[0-9a-f]{64}$/i.test(value.blobSha256))
      throw new Error('build-flags.json の blobSha256 が不正です');
    if (KEYS.some(key => numberFlag(value.model?.[key]) === null))
      throw new Error('build-flags.json の model フラグが不足または不正です');
    return {blobBytes:value.blobBytes, blobSha256:value.blobSha256.toLowerCase(),
      model:Object.fromEntries(KEYS.map(key => [key, numberFlag(value.model[key])])), voice:validateVoice(value)};
  }
  // 端末が声の値の記録に対応していないのに、bundle が k≠1 か vnoise≠0.03 を持つなら書かない（黙って値を落とさない）。
  function voiceProblem(device, manifest) {
    const voice = manifest?.voice;
    if (!voice || device?.voiceParams === 1) return null;
    const dropped = [];
    if (voice.k !== 1) dropped.push(`k=${voice.k}`);
    if (Math.abs(voice.vnoise - DEFAULT_VNOISE) > 1e-6) dropped.push(`vnoise=${Number(voice.vnoise.toPrecision(6))}`);
    return dropped.length ? `端末のファームが声の値の記録に対応していないため、この声の値（${dropped.join('、')}）が反映されません。ファームを更新してから書いてください` : null;
  }
  function paramsTail(shaHex, voice) {
    const tail = new Uint8Array(76), view = new DataView(tail.buffer);
    for (let i = 0; i < 32; i++) tail[i] = parseInt(shaHex.slice(i*2, i*2+2), 16);
    [voice.k, voice.centerHz, voice.loHz, voice.hiHz, voice.tauMs].forEach((n, i) => view.setFloat64(32 + 8*i, n, true));
    view.setFloat32(72, voice.vnoise, true);
    return tail;
  }
  async function sha256(bytes) {
    if (!globalThis.crypto?.subtle) throw new Error('SHA-256 が使えません。localhost の画面を Chrome / Edge で開いてください');
    const digest = new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes));
    return Array.from(digest, b => b.toString(16).padStart(2, '0')).join('');
  }
  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return (crc ^ 0xffffffff) >>> 0;
  }
  async function inflateRaw(bytes, expected) {
    let stream;
    try { stream = new DecompressionStream('deflate-raw'); }
    catch (_) { throw new Error('このブラウザでは ZIP を展開できません。Chrome / Edge を更新するか、ZIP の展開に対応したブラウザを使ってください'); }
    const reader = new Blob([bytes]).stream().pipeThrough(stream).getReader();
    const parts = []; let total = 0;
    try {
      for (;;) {
        const {value, done} = await reader.read();
        if (done) break;
        total += value.length;
        if (total > expected || total > MAX_BYTES) { await reader.cancel(); throw new Error('ZIP の展開後の大きさが不正です'); }
        parts.push(value);
      }
    } catch (_) { throw new Error('ZIP の圧縮データを展開できません'); }
    const out = new Uint8Array(total); let offset = 0;
    for (const part of parts) { out.set(part, offset); offset += part.length; }
    return out;
  }
  async function readBundle(input) {
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (bytes.length > MAX_BYTES + 1024 * 1024) throw new Error('ZIP が大きすぎます');
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const range = (offset, size) => {
      if (offset < 0 || size < 0 || offset + size > bytes.length) throw new Error('ZIP が途中で切れています');
    };
    const u16 = offset => { range(offset, 2); return view.getUint16(offset, true); };
    const u32 = offset => { range(offset, 4); return view.getUint32(offset, true); };
    let end = -1;
    for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
      if (u32(i) === 0x06054b50 && i + 22 + u16(i + 20) === bytes.length) { end = i; break; }
    }
    if (end < 0) throw new Error('ZIP の目次がありません');
    const count = u16(end + 10), directory = u32(end + 16), directorySize = u32(end + 12);
    if (u16(end + 4) || u16(end + 6) || u16(end + 8) !== count || count > 64 ||
        directory + directorySize !== end) throw new Error('分割 ZIP・ZIP64 または不正な目次には対応していません');
    const entries = new Map(); let cursor = directory;
    for (let i = 0; i < count; i++) {
      range(cursor, 46);
      if (u32(cursor) !== 0x02014b50) throw new Error('ZIP の目次が不正です');
      const flags = u16(cursor + 8), method = u16(cursor + 10), crc = u32(cursor + 16);
      const compressed = u32(cursor + 20), size = u32(cursor + 24), local = u32(cursor + 42);
      const nameLength = u16(cursor + 28), extra = u16(cursor + 30), comment = u16(cursor + 32);
      range(cursor + 46, nameLength + extra + comment);
      const name = new TextDecoder('utf-8', {fatal:true}).decode(bytes.subarray(cursor + 46, cursor + 46 + nameLength));
      if (entries.has(name)) throw new Error('ZIP に同じ名前のファイルが複数あります');
      if (flags & 1 || ![0, 8].includes(method) || size > MAX_BYTES || compressed > MAX_BYTES || u16(cursor + 34))
        throw new Error('暗号化・未対応の圧縮方式または大きすぎる ZIP です');
      entries.set(name, {flags, method, crc, compressed, size, local});
      cursor += 46 + nameLength + extra + comment;
    }
    if (cursor !== end) throw new Error('ZIP の目次の大きさが不正です');
    async function extract(name, limit) {
      const entry = entries.get(name);
      if (!entry) throw new Error(`ZIP に ${name} がありません`);
      const {local, method, flags, size, compressed, crc} = entry;
      if (size > limit) throw new Error(`${name} が大きすぎます`);
      range(local, 30);
      if (u32(local) !== 0x04034b50 || u16(local + 8) !== method || u16(local + 6) !== flags)
        throw new Error('ZIP の本文と目次が一致しません');
      const nameLength = u16(local + 26), extra = u16(local + 28), start = local + 30 + nameLength + extra;
      range(local + 30, nameLength + extra);
      const localName = new TextDecoder('utf-8', {fatal:true}).decode(bytes.subarray(local + 30, local + 30 + nameLength));
      if (localName !== name || start + compressed > directory) throw new Error('ZIP の本文の位置が不正です');
      const payload = bytes.subarray(start, start + compressed);
      const out = method === 0 ? payload.slice() : await inflateRaw(payload, size);
      if (out.length !== size || crc32(out) !== crc) throw new Error(`${name} の大きさまたは CRC が一致しません`);
      return out;
    }
    let manifest;
    try { manifest = validateManifest(JSON.parse(new TextDecoder('utf-8', {fatal:true}).decode(await extract('build-flags.json', 65536)))); }
    catch (error) { throw new Error(`bundle の情報を読めません：${error.message}`); }
    return {bytes:await extract('model-int8.bin', MAX_BYTES), manifest};
  }
  async function prepareModel(input, name, manifest = null) {
    let bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (/\.zip$/i.test(name)) ({bytes, manifest} = await readBundle(bytes));
    else if (/\.bin$/i.test(name)) {
      if (!manifest) throw new Error('フラグを照合できないので書けません。model-builder の bundle（.zip）を選んでください');
    } else throw new Error('model-builder の bundle（.zip）を選んでください');
    if (!bytes.length || bytes.length > MAX_BYTES) throw new Error('重みの大きさが不正です');
    if (manifest) manifest = validateManifest(manifest);
    const sha = await sha256(bytes);
    if (manifest && (manifest.blobBytes !== bytes.length || manifest.blobSha256 !== sha))
      throw new Error('重みの大きさまたは SHA-256 が build-flags.json と一致しません');
    return {kind:'model', bytes, name, manifest, sha};
  }
  function packet(op, session, tail = new Uint8Array()) {
    const bytes = new Uint8Array(5 + tail.length); bytes[0] = op;
    new DataView(bytes.buffer).setUint32(1, session, true); bytes.set(tail, 5); return bytes;
  }
  function parseStatus(input) {
    const v = input instanceof DataView ? input : new DataView(input.buffer, input.byteOffset, input.byteLength);
    if (v.byteLength !== 20 || v.getUint8(0) !== WIRE_VERSION || v.getUint8(1) > 7 || v.getUint8(2) >= ERRORS.length)
      throw new Error('端末のモデル状態を読めません。対応するファームが必要です（書き込みの版 ' + WIRE_VERSION + '）');
    return {state:v.getUint8(1), error:v.getUint8(2), op:v.getUint8(3), session:v.getUint32(4,true),
      next:v.getUint32(8,true), size:v.getUint32(12,true), request:v.getUint32(16,true)};
  }
  class Transfer {
    constructor(transport, options = {}) {
      this.transport = transport; this.options = options;
      this.sleep = options.sleep || (ms => new Promise(resolve => setTimeout(resolve, ms)));
      this.now = options.now || (() => performance.now());
      this.cancelled = false; this.lost = false; this.last = null; this.begun = false;
      const random = new Uint32Array(1); globalThis.crypto.getRandomValues(random); this.session = random[0] || 1;
    }
    acceptStatus(input) { this.last = parseStatus(input); this.options.onStatus?.(this.last); }
    cancel() { this.cancelled = true; }
    connectionLost() { this.lost = true; }
    check(ignoreCancel = false) {
      if (this.lost) throw new Error('接続が切れました。再接続後、最初から書き直してください');
      if (!ignoreCancel && this.cancelled) throw new Error('書き込みを中止しました');
    }
    async wait(predicate, timeout, ignoreCancel = false) {
      const until = this.now() + timeout; let nextRead = this.now() + 500;
      for (;;) {
        this.check(ignoreCancel);
        if (this.last && predicate(this.last)) return this.last;
        if (this.now() >= until) throw new Error('端末の応答が時間切れになりました');
        // 通知の欠落を read で回復する。DATA の送信とは重ねない。
        if (this.now() >= nextRead) {
          this.acceptStatus(await this.transport.readStatus()); nextRead = this.now() + 500;
        }
        await this.sleep(25);
      }
    }
    async control(bytes) {
      for (let attempt = 0; ; attempt++) {
        this.check(bytes[0] === OP.ABORT);
        try { await this.transport.write(bytes, true); return; }
        catch (error) { if (attempt >= 2 || this.lost) throw error; await this.sleep(100); }
      }
    }
    async abort() {
      if (!this.begun || this.lost) return false;
      try {
        await this.control(packet(OP.ABORT, this.session));
        await this.wait(s => s.session === this.session && [STATE.ABORTED, STATE.FAILED].includes(s.state), 5000, true);
        return true;
      } catch (_) { return false; }
    }
    // 書き込み先の先頭 bytes の SHA-256 を端末に計算させて返す（書く前の照合）。
    async probe(target, bytes, timeout) {
      const random = new Uint32Array(1); globalThis.crypto.getRandomValues(random);
      const request = random[0] || 1;
      const tail = new Uint8Array(5); tail[0] = target; new DataView(tail.buffer).setUint32(1, bytes, true);
      await this.control(packet(OP.PROBE, request, tail));
      const ack = await this.wait(s => s.op === OP.PROBE && s.request === request, timeout);
      if (ack.error) throw new Error('端末の中身を照合できません：' + (ERRORS[ack.error] || ack.error));
      const info = await this.transport.readModelInfo();
      const p = info?.probe;
      if (!p || p.request !== request || p.target !== target || p.bytes !== bytes || !/^[0-9a-f]{64}$/.test(p.sha256 || ''))
        throw new Error('端末の照合結果を読めません。接続し直してください');
      return {sha256:p.sha256, info};
    }
    // 端末にすでに同じものが入っているか。辞書は先頭からの SHA-256 が同じなら同じ。
    // モデルは SHA-256 が同じで、かつ声の値の記録も bundle と同じときだけ同じ（値が違えば書き直す）。
    async alreadyInstalled(item, device) {
      const dict = item.kind === 'dict', target = dict ? TARGET.dict : TARGET.model;
      const {sha256: deviceSha, info} = await this.probe(target, item.bytes.length, (dict ? TIMEOUT.dict : TIMEOUT.model).probe);
      const bytesSame = deviceSha === item.sha;
      const valuesSame = dict || !bytesSame || sameVoiceValues(info, item.manifest?.voice);
      return {same:bytesSame && valuesSame, bytesSame, valuesSame, deviceSha};
    }
    async run(item, device, options = {}) {
      const dict = item?.kind === 'dict', timeout = dict ? TIMEOUT.dict : TIMEOUT.model;
      if (!dict && !item?.manifest) throw new Error('フラグを照合できないので書けません。model-builder の bundle（.zip）を選んでください');
      const versionProblem = protocolProblem(device);
      if (versionProblem) throw new Error(versionProblem);
      if (device?.protocol !== WIRE_VERSION || !device.available || device.bootReady === false || !Number.isInteger(device.maxData) ||
          device.maxData < 32 || device.maxData > 235 || !Number.isInteger(device.windowChunks) || device.windowChunks < 1)
        throw new Error('モデル書き込みに対応するファームと十分な MTU が必要です。接続し直してください');
      let manifest = null;
      if (dict) {
        const problem = dictProblem(device, item) || dictFormatProblem(item.bytes);
        if (problem) throw new Error(problem);
        if (await sha256(item.bytes) !== item.sha) throw new Error('転送前の SHA-256 が一致しません');
      } else {
        if (!Number.isInteger(device.partitionBytes) || !item.bytes.length || item.bytes.length > device.partitionBytes)
          throw new Error('重みが端末の model パーティションに収まりません');
        manifest = validateManifest(item.manifest);
        const differences = compareFlags(device.model, manifest.model);
        const problem = voiceProblem(device, manifest);
        if (problem) throw new Error(problem);
        if (differences.length) throw new Error('モデルのフラグが一致しません：' + differences.join('、'));
        if (manifest.blobBytes !== item.bytes.length || manifest.blobSha256 !== item.sha)
          throw new Error('bundle の大きさ・SHA-256 が一致しません');
        if (await sha256(item.bytes) !== item.sha) throw new Error('転送前の SHA-256 が一致しません');
      }
      // 書く前に端末の中身と照合する。同じなら書かない（Flash を消さず、発話も止めない）。
      if (options.checkFirst !== false) {
        this.options.onPhase?.('端末の中身と照合しています');
        const installed = await this.alreadyInstalled(item, device);
        if (installed.same) return {skipped:true, target:dict ? 'dict' : 'model', sha:item.sha, bytes:item.bytes.length, deviceSha:installed.deviceSha};
        if (installed.bytesSame && !installed.valuesSame) this.options.onLog?.('重みは同じですが、声の値が端末の記録と違うため書き直します');
        else this.options.onLog?.('端末の中身が違うため書き込みます');
      }
      const start = new Uint8Array(37); const startView = new DataView(start.buffer);
      startView.setUint32(0, item.bytes.length, true);
      for (let i = 0; i < 32; i++) start[4+i] = parseInt(item.sha.slice(i*2, i*2+2),16);
      start[36] = dict ? TARGET.dict : TARGET.model;
      try {
        this.check(); this.options.onPhase?.(dict ? '端末の辞書領域を消去しています（発話と聞き取りを止めます）' : '端末のモデル領域を消去しています');
        this.begun = true;
        await this.control(packet(OP.BEGIN, this.session, start));
        const ready = await this.wait(s => s.op === OP.BEGIN &&
          (s.error !== 0 || s.session === this.session && s.state !== STATE.ERASING), timeout.begin);
        if (ready.error || ready.state !== STATE.RECEIVING) {
          if (ready.session !== this.session) this.begun = false;
          throw new Error(ERRORS[ready.error] || '受信を開始できません');
        }
        this.options.onPhase?.(dict ? '辞書を書き込んでいます' : 'モデルを書き込んでいます');
        const started = this.now(); let offset = 0, request = 0, stalls = 0;
        const window = Math.min(8, device.windowChunks), chunk = device.maxData;
        while (offset < item.bytes.length) {
          const before = offset; let sent = offset;
          for (let i = 0; i < window && sent < item.bytes.length; i++) {
            this.check();
            const count = Math.min(chunk, item.bytes.length - sent), tail = new Uint8Array(4 + count);
            new DataView(tail.buffer).setUint32(0, sent, true); tail.set(item.bytes.subarray(sent, sent + count), 4);
            await this.transport.write(packet(OP.DATA, this.session, tail), false); sent += count;
          }
          const tail = new Uint8Array(4); new DataView(tail.buffer).setUint32(0, ++request, true);
          await this.control(packet(OP.QUERY, this.session, tail));
          const ack = await this.wait(s => s.session === this.session && (s.state === STATE.FAILED || s.state === STATE.ABORTED ||
            s.op === OP.QUERY && s.request === request), 5000);
          if (ack.error || ack.state !== STATE.RECEIVING) throw new Error(ERRORS[ack.error] || '端末が受信を終了しました');
          if (ack.next < before || ack.next > sent || ack.size !== item.bytes.length) throw new Error('端末の受信位置が不正です');
          offset = ack.next;
          if (offset === before) { if (++stalls >= 5) throw new Error('重みが届きません。接続し直してください'); }
          else stalls = 0;
          if (offset < sent) this.options.onLog?.(`${offset.toLocaleString()} B から欠けたデータを再送します`);
          const elapsed = Math.max(1, this.now() - started) / 1000;
          this.options.onProgress?.({bytes:offset, total:item.bytes.length, elapsed, speed:offset/elapsed});
        }
        this.options.onPhase?.('端末の Flash を読み直して SHA-256 を照合しています');
        await this.control(packet(OP.END, this.session));
        const done = await this.wait(s => s.session === this.session &&
          [STATE.VERIFIED, STATE.FAILED, STATE.ABORTED].includes(s.state), timeout.end);
        const info = await this.transport.readModelInfo();
        if (done.error || done.state !== STATE.VERIFIED) {
          const error = new Error(ERRORS[done.error] || '端末の照合に失敗しました');
          error.deviceSha = info.sha256; error.browserSha = item.sha; error.hashBytes = info.hashBytes;
          throw error;
        }
        if (info.state !== STATE.VERIFIED || info.session !== this.session || info.hashBytes !== item.bytes.length || info.sha256 !== item.sha ||
            info.target !== (dict ? TARGET.dict : TARGET.model))
          throw new Error('端末が返した SHA-256 とブラウザの SHA-256 が一致しません');
        this.check();
        if (!dict && device.voiceParams === 1) {
          this.options.onPhase?.('声の値（高さの倍率と有声雑音比）を端末に記録しています');
          await this.control(packet(OP.PARAMS, this.session, paramsTail(item.sha, manifest.voice)));
          const stored = await this.wait(s => s.session === this.session && s.op === OP.PARAMS, 10000);
          if (stored.error) throw new Error('声の値を端末に記録できません：' + (ERRORS[stored.error] || stored.error));
        }
        return {target:dict ? 'dict' : 'model', session:this.session, sha:item.sha, bytes:item.bytes.length, elapsed:(this.now()-started)/1000, voice:manifest?.voice};
      } catch (error) {
        const aborted = await this.abort();
        if (this.begun && !this.lost && !aborted)
          error.message += '。中止の応答を確認できません。切断すると端末の書き込みを終了します';
        if (dict && this.begun) error.message += '。辞書の書き込みが完了していないため、端末は電源を入れ直すまで発話を止めたままです。もう一度書くか、USB を一度抜いて差し直し、端末の電源を入れ直してください';
        throw error;
      }
    }
  }
  return {KEYS, OP, STATE, TARGET, WIRE_VERSION, ERRORS, compareFlags, validateManifest, voiceProblem, protocolProblem, decodeModelInfo, dictFormatProblem, dictProblem,
    sameVoiceValues, paramsTail, sha256, crc32, readBundle, prepareModel, prepareDict, packet, parseStatus, Transfer};
});
