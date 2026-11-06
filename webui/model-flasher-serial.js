'use strict';
// 接続と画面を持たない USB 書き込み処理。ESPLoader の接続は呼び出し側が行う。
(function (root, factory) {
  const api = factory(typeof module === 'object' && module.exports ? require('./model-flasher-core.js') : root.ModelFlasherCore);
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.ModelFlasherSerial = api;
})(globalThis, function (core) {
  const SECTOR = 4096;
  function bytes(input) {
    if (typeof input === 'string') return Uint8Array.from(input, c => c.charCodeAt(0));
    return input instanceof Uint8Array ? input : new Uint8Array(input);
  }
  function hex(data) { return Array.from(data, b => b.toString(16).padStart(2, '0')).join(''); }
  // RFC 1321。Web Crypto が提供しない MD5 をブラウザでも計算する。
  function md5(input) {
    const src = bytes(input), n = Math.ceil((src.length + 9) / 64) * 64;
    const data = new Uint8Array(n); data.set(src); data[src.length] = 128;
    const view = new DataView(data.buffer);
    view.setUint32(n - 8, (src.length * 8) >>> 0, true);
    view.setUint32(n - 4, Math.floor(src.length / 0x20000000), true);
    const shifts = [[7,12,17,22], [5,9,14,20], [4,11,16,23], [6,10,15,21]];
    let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (let offset = 0; offset < n; offset += 64) {
      let a = a0, b = b0, c = c0, d = d0;
      for (let i = 0; i < 64; i++) {
        const round = i >>> 4;
        const f = round === 0 ? (b & c) | (~b & d) : round === 1 ? (d & b) | (~d & c) : round === 2 ? b ^ c ^ d : c ^ (b | ~d);
        const g = round === 0 ? i : round === 1 ? (5*i+1)%16 : round === 2 ? (3*i+5)%16 : (7*i)%16;
        const sum = (a + f + Math.floor(Math.abs(Math.sin(i+1))*0x100000000) + view.getUint32(offset + 4*g, true)) | 0;
        const shift = shifts[round][i%4], next = (b + ((sum << shift) | (sum >>> (32-shift)))) | 0;
        a = d; d = c; c = b; b = next;
      }
      a0 = (a0+a)|0; b0 = (b0+b)|0; c0 = (c0+c)|0; d0 = (d0+d)|0;
    }
    const out = new Uint8Array(16), v = new DataView(out.buffer);
    [a0,b0,c0,d0].forEach((x,i) => v.setUint32(i*4,x,true)); return hex(out);
  }
  function parsePartitionRows(input) {
    const data = bytes(input);
    if (data.length !== 0xc00) throw new Error('パーティション表の大きさが不正です');
    const v = new DataView(data.buffer, data.byteOffset, data.length), all = [], found = {};
    let ended = false, checked = false;
    for (let offset = 0; offset < data.length; offset += 32) {
      const row = data.subarray(offset, offset+32), magic = v.getUint16(offset,true);
      if (row.every(b => b === 255)) { ended = true; if (!data.subarray(offset).every(b => b === 255)) throw new Error('表の終端が不正です'); break; }
      if (magic === 0xebeb && !checked && all.length && row.subarray(2,16).every(b => b === 255)) {
        if (hex(row.subarray(16)) !== md5(data.subarray(0,offset))) throw new Error('パーティション表の MD5 が一致しません');
        checked = true; continue;
      }
      if (magic !== 0x50aa || checked) throw new Error('パーティション表が壊れています');
      const address = v.getUint32(offset+4,true), size = v.getUint32(offset+8,true);
      const label = row.subarray(12,28), zero = label.indexOf(0);
      const name = new TextDecoder().decode(zero < 0 ? label : label.subarray(0,zero));
      const type = row[2], subtype = row[3], flags = v.getUint32(offset+28,true);
      if (!size || address < 0x9000 || address%SECTOR || size%SECTOR || address+size > 0x100000000 ||
          all.some(p => p.name === name || address < p.address+p.size && p.address < address+size)) throw new Error('パーティションの位置・大きさ・名前が不正です');
      const part = {name,address,size,type,subtype,flags}; all.push(part);
      if (['factory','model','dict'].includes(name)) found[name] = part;
    }
    if (!ended || ['factory','model','dict'].some(name => !found[name])) throw new Error('必要なパーティションまたは終端がありません');
    if (found.factory.type !== 0 || found.factory.subtype !== 0 || found.factory.address%0x10000 ||
        found.model.type !== 1 || found.model.subtype !== 0x40 || found.dict.type !== 1 || found.dict.subtype !== 0x41 || Object.values(found).some(p => p.flags !== 0))
      throw new Error('書き込み先の種類または属性が不正です');
    return all;
  }
  function parsePartitions(input) {
    return Object.fromEntries(parsePartitionRows(input).filter(p => ['factory','model','dict'].includes(p.name)).map(p => [p.name,p]));
  }
  async function appProblem(input) {
    const data = bytes(input);
    if (data.length < 24 || data[0] !== 0xe9) return 'ESP アプリイメージではありません';
    if ((data[12] | data[13]<<8) !== 9) return 'ESP32-S3 のアプリではありません';
    const v = new DataView(data.buffer,data.byteOffset,data.length);
    if (data[23] !== 0 && data[23] !== 1) return 'アプリの hash_appended が不正です';
    const segments = data[1];
    if (!segments || segments > 16) return 'アプリのセグメント数が不正です';
    let offset = 24, checksum = 0xef;
    for (let i = 0; i < segments; i++) {
      if (offset + 8 > data.length) return 'アプリのセグメントヘッダが切り詰められています';
      const length = v.getUint32(offset+4,true);
      offset += 8;
      if (i === 0 && (length < 256 || offset + 256 > data.length || v.getUint32(offset,true) !== 0xabcd5432))
        return 'アプリ記述の magic_word が不正です';
      if (length > data.length - offset) return 'アプリのセグメントが切り詰められています';
      for (const value of data.subarray(offset,offset+length)) checksum ^= value;
      offset += length;
    }
    // ESP イメージの末尾には 16 B 境界までのパディングとチェックサムがある。
    const imageEnd = Math.ceil((offset + 1) / 16) * 16;
    if (data.length < imageEnd + (data[23] === 1 ? 32 : 0)) return 'アプリの末尾が切り詰められています';
    if (data[imageEnd-1] !== checksum) return 'アプリのチェックサムが一致しません';
    if (data[23] === 1 && await core.sha256(data.subarray(0,imageEnd)) !== hex(data.subarray(imageEnd,imageEnd+32)))
      return 'アプリの SHA-256 が一致しません';
    return null;
  }
  function readAppFlags(input) {
    const data = bytes(input).subarray(0,SECTOR), text = new TextDecoder().decode(data);
    const matches = [...text.matchAll(/NKFLAGS1(\d+),(\d+),(\d+),(\d+),(\d+)\x00/g)];
    if (matches.length !== 1) return null;
    const values = matches[0].slice(1).map(Number);
    return values.every(Number.isSafeInteger) ? Object.fromEntries(core.KEYS.map((key,i) => [key,values[i]])) : null;
  }
  function validDisplayName(name) {
    return typeof name === 'string' && /^[\x20-\x7e]{1,31}$/.test(name) ? name : null;
  }
  function runDisplayName(run) {
    return [run.voice,run.label,run.run].map(validDisplayName).find(Boolean) || null;
  }
  function fileDisplayName(name) { return validDisplayName(name.replace(/\.[^.]*$/, '')); }
  function encodeVoiceSector(modelBytes, sha, voice, name) {
    if (!Number.isSafeInteger(modelBytes) || modelBytes <= 0 || modelBytes > 0xffffffff || !/^[a-f0-9]{64}$/i.test(sha) ||
        !voice || ![voice.k,voice.centerHz,voice.loHz,voice.hiHz,voice.tauMs,voice.vnoise].every(Number.isFinite) ||
        !(voice.k >= 0 && voice.k <= 3 && Math.fround(voice.vnoise) > 0 && Math.fround(voice.vnoise) <= 1) ||
        (voice.k !== 1 && !(voice.centerHz > 0 && voice.loHz > 0 && voice.loHz < voice.centerHz && voice.hiHz > voice.centerHz && voice.tauMs > 0)))
      throw new Error('声の値の記録が不正です');
    const out = new Uint8Array(SECTOR).fill(255), v = new DataView(out.buffer); out.fill(0,0,96);
    out.set([78,75,86,80,1]); v.setUint32(8,modelBytes,true);
    out.set(core.paramsTail(sha,voice),16); v.setUint32(92,core.crc32(out.subarray(0,92)),true);
    if (validDisplayName(name)) {
      out.fill(0,96,136); out.set([78,75,86,78,name.length],96);
      out.set(new TextEncoder().encode(name),101);
      v.setUint32(132,core.crc32(out.subarray(96,132)),true);
    }
    return out;
  }
  // 全対象を検証してから返す。モデルの最後のセクタを重みが侵食しない。
  async function createPlan(partitions, items, currentApp) {
    if (!Array.isArray(items) || !items.length || new Set(items.map(i => i.kind)).size !== items.length) throw new Error('書く対象が空または重複しています');
    const app = items.find(i => i.kind === 'app'), plan = [];
    plan.warnings = [];
    if (app && !items.some(i => i.kind === 'model')) {
      const current = readAppFlags(currentApp), next = readAppFlags(app.bytes);
      if (current && next) {
        const differences = core.KEYS.filter(k => current[k] !== next[k]).map(k => `${k}：今=${current[k]}、新=${next[k]}`);
        if (differences.length) throw new Error('アプリのフラグが変わるので、新しいアプリに合うモデルも選んでください：'+differences.join('、'));
      }
      if (!current) plan.warnings.push('今の端末のアプリはフラグの目印を持たないため、端末のモデルとの照合ができません。声がおかしければモデルも書き直してください');
      if (!next) plan.warnings.push('新しいアプリはフラグの目印を持たないため、端末のモデルとの照合ができません。声がおかしければモデルも書き直してください');
    }
    for (const item of [...items].sort((a,b) => ['app','model','dict'].indexOf(a.kind) - ['app','model','dict'].indexOf(b.kind))) {
      if (!['app','model','dict'].includes(item.kind)) throw new Error('書く対象が不正です');
      const data = bytes(item.bytes).slice(), p = partitions[item.kind === 'app' ? 'factory' : item.kind];
      const capacity = p.size - (item.kind === 'model' ? SECTOR : 0);
      if (!data.length || data.length > capacity) throw new Error(`${item.kind} が領域に収まりません`);
      if (item.kind === 'app') { const problem = await appProblem(data); if (problem) throw new Error(problem); }
      if (item.kind === 'dict' && core.dictFormatProblem(data)) throw new Error(core.dictFormatProblem(data));
      plan.push({target:item.kind,address:p.address,bytes:data});
      if (item.kind === 'model') {
        const manifest = core.validateManifest(item.manifest);
        if (manifest.blobBytes !== data.length || manifest.blobSha256 !== await core.sha256(data)) throw new Error('bundle の大きさ・SHA-256 が一致しません');
        const flags = readAppFlags(app ? app.bytes : currentApp);
        if (!flags) throw new Error('アプリのフラグを読めないためモデルを書けません');
        if (core.compareFlags(flags,manifest.model).length) throw new Error('モデルのフラグが一致しません');
        plan.push({target:'params',address:p.address+capacity,bytes:encodeVoiceSector(data.length,manifest.blobSha256,manifest.voice,item.displayName)});
      }
    }
    return plan;
  }
  function appVersion(input) {
    const data = bytes(input);
    if (data.length < 80 || new DataView(data.buffer,data.byteOffset,data.length).getUint32(32,true) !== 0xabcd5432) return '不明';
    return new TextDecoder().decode(data.subarray(48,80)).split('\0')[0];
  }
  async function inspect(loader) {
    const partitions = parsePartitions(await loader.readFlash(0x8000,0xc00));
    const app = bytes(await loader.readFlash(partitions.factory.address,SECTOR));
    return {partitions,app,version:appVersion(app),flags:readAppFlags(app)};
  }
  async function comparePlan(loader, plan) {
    const changed = [], skipped = [];
    for (const part of plan) {
      if (String(await loader.flashMd5sum(part.address,part.bytes.length)).toLowerCase() === md5(part.bytes)) skipped.push(part.target);
      else changed.push(part);
    }
    return {changed,skipped};
  }
  async function check(loader, items) {
    const device = await inspect(loader);
    const plan = await createPlan(device.partitions,items,device.app);
    return {...await comparePlan(loader,plan),warnings:plan.warnings};
  }
  function binaryString(data) {
    let out = ''; for (let i = 0; i < data.length; i += 8192) out += String.fromCharCode(...data.subarray(i,i+8192)); return out;
  }
  function firstSector(data, invalid) {
    const sector = new Uint8Array(SECTOR).fill(255);
    sector.set(data.subarray(0,SECTOR));
    if (invalid) sector.fill(0,0,4);
    return sector;
  }
  // items: [{kind:'app'|'model'|'dict', bytes:Uint8Array, manifest?:検証対象のbundle情報}]
  // model は core.prepareModel の戻り値をそのまま渡せる。未展開の ZIP は受け付けない。
  // options.onProgress({target,bytes,total,elapsed,speed,remaining}) は秒・B/秒。
  // options.reset(loader) を指定しなければ ESPLoader.after('hard_reset') で再起動する。
  // 失敗も {ok:false,error,phase,completed,skipped,recommitted,pending} として返す。
  // params はモデル末尾の記録を示す。pending は照合成功まで終わっていない対象。
  // recommitted は本文を省略し、先頭の再確定と全体照合を終えた skipped の対象。
  async function run(loader, items, options = {}) {
    const completed = [], skipped = [], recommitted = [], warnings = []; let pending = items.map(i => i.kind), phase = '検証';
    try {
      const partitions = parsePartitions(await loader.readFlash(0x8000,0xc00));
      const currentApp = items.some(i => i.kind === 'model' || i.kind === 'app') ?
        await loader.readFlash(partitions.factory.address,SECTOR) : new Uint8Array();
      const plan = await createPlan(partitions,items,currentApp); warnings.push(...plan.warnings); pending = plan.map(p => p.target);
      // 書き込み直前にも比較し直す。
      phase = '比較'; const compared = await comparePlan(loader, plan);
      let changed = compared.changed;
      // アプリを変える間は旧モデルも開けないようにする。同一モデルも確定し直す。
      const guardedModel = changed.some(p => p.target === 'app') ? plan.find(p => p.target === 'model') : null;
      const headerOnlyModel = guardedModel && compared.skipped.includes('model') ? guardedModel : null;
      if (guardedModel) {
        changed = plan.filter(p => compared.changed.includes(p) || p === guardedModel);
      }
      skipped.push(...compared.skipped.filter(t => !changed.some(p => p.target === t)));
      pending = changed.map(p => p.target);
      if (guardedModel) {
        phase = 'model';
        await loader.writeFlash({fileArray:[{address:guardedModel.address,data:binaryString(firstSector(guardedModel.bytes,true))}],
          flashSize:'keep',compress:true,eraseAll:false,flashMode:'keep',flashFreq:'keep',
          calculateMD5Hash: input => md5(bytes(input)),reportProgress: () => {}});
      }
      for (const part of changed) {
        phase = part.target; const started = Date.now();
        const staged = part.target === 'model' || part.target === 'dict';
        const transferTotal = part === headerOnlyModel ? SECTOR*2 : staged ? Math.max(SECTOR,part.bytes.length)+SECTOR : part.bytes.length;
        let transferred = part === guardedModel ? SECTOR : 0;
        async function write(address, data, trackProgress = true) {
          await loader.writeFlash({fileArray:[{address,data:binaryString(data)}],flashSize:'keep',compress:true,
            eraseAll:false, flashMode:'keep', flashFreq:'keep', calculateMD5Hash: input => md5(bytes(input)),
            reportProgress: (_index,written) => {
              if (!trackProgress) return;
              const done = transferred + written;
              const elapsed = Math.max(0.001,(Date.now()-started)/1000);
              options.onProgress?.({target:part.target,bytes:done,total:transferTotal,elapsed,speed:done/elapsed,remaining:(transferTotal-done)/(done/elapsed || 1)});
            }});
          if (trackProgress) transferred += data.length;
        }
        if (staged) {
          const invalid = firstSector(part.bytes,true);
          if (part !== guardedModel) await write(part.address,invalid);
          if (part !== headerOnlyModel && part.bytes.length > SECTOR) await write(part.address+SECTOR,part.bytes.subarray(SECTOR));
          const stagedBytes = part.bytes.slice(); stagedBytes.fill(0,0,4);
          if (String(await loader.flashMd5sum(part.address,part.bytes.length)).toLowerCase() !== md5(stagedBytes))
            throw new Error('先頭を無効にした状態の MD5 が一致しません');
          // writeFlash は対象セクタを消す。先頭の確定も 4 KB 全体を書き直す。
          try {
            await write(part.address,firstSector(part.bytes,false));
            if (String(await loader.flashMd5sum(part.address,part.bytes.length)).toLowerCase() !== md5(part.bytes))
              throw new Error('書き込み後の MD5 が一致しません');
          } catch (error) {
            // 確定中または最終照合で失敗したら、再起動時に開けない先頭へ戻す。
            try { await write(part.address,invalid,false); }
            catch (invalidateError) { throw new Error(`${error.message}。先頭の再無効化も失敗しました：${invalidateError.message}`); }
            throw error;
          }
        } else {
          await write(part.address,part.bytes);
          if (String(await loader.flashMd5sum(part.address,part.bytes.length)).toLowerCase() !== md5(part.bytes)) throw new Error('書き込み後の MD5 が一致しません');
        }
        if (part === headerOnlyModel) { skipped.push(part.target); recommitted.push(part.target); }
        else completed.push(part.target);
        pending = pending.filter(t => t !== part.target);
      }
      phase = 'リセット';
      if (options.reset) await options.reset(loader);
      else await loader.after('hard_reset');
      return {ok:true,warnings,completed,skipped,recommitted,pending:[]};
    } catch (error) { return {ok:false,warnings,error,phase,completed,skipped,recommitted,pending}; }
  }
  // 全体書き込みは端末の既存パーティション表に依存しない。
  async function runFull(loader, image, options = {}) {
    const completed = [], warnings = []; let pending = [], phase = '検証';
    try {
      const capacity = /^(\d+)(KB|MB)$/i.exec(image?.flashSize);
      if (!capacity || !['dio','qio','dout','qout'].includes(image.flashMode) ||
          !['20m','26m','40m','80m'].includes(image.flashFreq)) throw Error('フラッシュ設定が不正です');
      const limit = Number(capacity[1]) * (capacity[2].toUpperCase() === 'MB' ? 1048576 : 1024);
      if (!Number.isSafeInteger(limit) || limit <= 0 || !Array.isArray(image.files)) throw Error('一式が不正です');
      const plan = image.files.map(f => ({target:f.name,address:f.address,bytes:bytes(f.bytes).slice()}))
        .sort((a,b) => a.address-b.address);
      pending = plan.map(p => p.target);
      if (new Set(pending).size !== plan.length || [0,0x8000,0x10000].some(a => !plan.some(p => p.address === a)))
        throw Error('必要なアドレスがないか名前が重複しています');
      let end = 0, total = 0;
      for (const p of plan) {
        if (!Number.isSafeInteger(p.address) || p.address < end || p.address % SECTOR ||
            !p.bytes.length || p.address+p.bytes.length > limit) throw Error('ファイルの位置・重なり・容量が不正です');
        end = p.address + p.bytes.length; total += p.bytes.length;
      }
      if (total > limit) throw Error('一式がフラッシュに収まりません');
      const boot = plan.find(p => p.address === 0);
      if (boot.bytes.length <= 16) throw Error('ブートローダーが短すぎます');
      const partitions = parsePartitionRows(plan.find(p => p.address === 0x8000).bytes);
      if (options.eraseUncovered) {
        const uncovered = partitions.filter(p => p.type === 1 &&
          !plan.some(f => f.address < p.address+p.size && p.address < f.address+f.bytes.length));
        for (const p of uncovered) {
          if (p.address+p.size > limit || plan.some(f => f.target === p.name)) throw Error('消去領域が不正です');
          plan.push({target:p.name,address:p.address,bytes:new Uint8Array(p.size).fill(255)});
        }
      }
      const problem = await appProblem(plan.find(p => p.address === 0x10000).bytes);
      if (problem) throw Error(problem);
      // 古い表で新しい内容が見える期間を短くするため、表は最後に確定する。
      plan.sort((a,b) => Number(a.address === 0x8000)-Number(b.address === 0x8000));
      pending = plan.map(p => p.target);
      for (const p of plan) {
        phase = p.target; const started = Date.now();
        await loader.writeFlash({fileArray:[{address:p.address,data:binaryString(p.bytes)}],
          flashMode:image.flashMode,flashFreq:image.flashFreq,flashSize:image.flashSize,
          compress:true,eraseAll:false,calculateMD5Hash:input => md5(bytes(input)),
          reportProgress:(_index,written) => {
            const elapsed = Math.max(0.001,(Date.now()-started)/1000), speed = written/elapsed;
            options.onProgress?.({target:p.target,bytes:written,total:p.bytes.length,elapsed,speed,
              remaining:(p.bytes.length-written)/(speed || 1)});
          }});
        // esptool-js はブートローダーの先頭設定を書き換える。先頭16 Bを除いて照合する。
        const skip = p.address === 0 ? 16 : 0;
        if (String(await loader.flashMd5sum(p.address+skip,p.bytes.length-skip)).toLowerCase() !== md5(p.bytes.subarray(skip)))
          throw Error('書き込み後の MD5 が一致しません');
        completed.push(p.target); pending = pending.filter(t => t !== p.target);
      }
      phase = 'リセット';
      if (options.reset) await options.reset(loader); else await loader.after('hard_reset');
      return {ok:true,phase,completed,pending,warnings};
    } catch (error) { return {ok:false,error,phase,completed,pending,warnings}; }
  }
  return {validDisplayName,runDisplayName,fileDisplayName,inspect,check,appVersion,md5,parsePartitions,appProblem,readAppFlags,encodeVoiceSector,createPlan,run,runFull};
});
