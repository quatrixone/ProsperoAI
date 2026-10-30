/* Stems separation worker — Sprint 2 (validated 2026-10-19: JS engine matches
 * the PyTorch reference at corr 0.99999997, rel -72 dB = WAV16 noise floor).
 * All binaries live under /download0/ProsperoAI/work/audio/ so no eboot asset
 * route is needed: ort.min.js, ort-wasm-simd-threaded.jsep.wasm,
 * stem-engine.js, stems.onnx (UVR-MDX-NET-Voc_FT, 66.8 MB).
 * Contract: in {url} -> out {done, vocals, inst, sr} (ArrayBuffers, transferred)
 * plus progress {pct} / {stage} messages. */
'use strict';
var BASE = '/download0/ProsperoAI/work/audio'; // msg.baseUrl ('' = same-origin) is prepended in run()

function stage(m) { postMessage({ stage: m }); }
function pct(p) { postMessage({ pct: Math.max(0, Math.min(100, Math.round(p * 100))) }); }
function fail(e) { postMessage({ error: String(e && e.message ? e.message : e) }); }

function loadScript(path) {
  // CSP forbids eval; importScripts enforces JS MIME (the media handler serves
  // octet-stream). fetch-as-text + blob: URL + dynamic import works under
  // CSP script-src blob: with any server MIME.
  return fetch(BASE + '/' + path).then(function (r) {
    if (!r.ok) throw new Error('fetch ' + path + ': HTTP ' + r.status);
    return r.text();
  }).then(function (code) {
    // Module-scope import: a classic bundle's top-level `var` would not leak —
    // export it explicitly so loadScript resolves to the bundle's value.
    var wrapped = code + '\n;export default (typeof ort !== "undefined" ? ort : undefined);';
    return import(URL.createObjectURL(new Blob([wrapped], { type: 'text/javascript' })));
  });
}

// PCM16 WAV reader (our library files are always PCM16; fallback via decoder).
function parseWav(buf) {
  var dv = new DataView(buf);
  if (dv.byteLength < 44 || dv.getUint32(0) !== 0x52494646) return null;
  var off = 12, ch = 0, sr = 0, bits = 16, dataOff = -1, dataLen = 0;
  while (off + 8 <= dv.byteLength) {
    var id = dv.getUint32(off), sz = dv.getUint32(off + 4, true);
    if (id === 0x666d7420) { ch = dv.getUint16(off + 10, true); sr = dv.getUint32(off + 12, true); bits = dv.getUint16(off + 22, true); }
    else if (id === 0x64617461) { dataOff = off + 8; dataLen = Math.min(sz, dv.byteLength - off - 8); }
    off += 8 + sz + (sz & 1);
  }
  if (!ch || !sr || dataOff < 0 || bits !== 16) return null;
  var n = Math.floor(dataLen / (2 * ch));
  var L = new Float32Array(n), R = new Float32Array(n);
  for (var i = 0; i < n; i++) {
    L[i] = dv.getInt16(dataOff + i * 2 * ch, true) / 32768;
    R[i] = ch > 1 ? dv.getInt16(dataOff + i * 2 * ch + 2, true) / 32768 : L[i];
  }
  return { L: L, R: R, sr: sr };
}

function encodeWav(L, R, sr) {
  var n = L.length, bytes = 44 + n * 4;
  var ab = new ArrayBuffer(bytes), dv = new DataView(ab);
  function str(o, s) { for (var i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i)); }
  str(0, 'RIFF'); dv.setUint32(4, bytes - 8, true); str(8, 'WAVEfmt ');
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 2, true);
  dv.setUint32(24, sr, true); dv.setUint32(28, sr * 4, true);
  dv.setUint16(32, 4, true); dv.setUint16(34, 16, true);
  str(36, 'data'); dv.setUint32(40, n * 4, true);
  for (var i = 0; i < n; i++) {
    var l = Math.max(-1, Math.min(1, L[i])), r = Math.max(-1, Math.min(1, R[i]));
    dv.setInt16(44 + i * 4, l < 0 ? l * 32768 : l * 32767, true);
    dv.setInt16(44 + i * 4 + 2, r < 0 ? r * 32768 : r * 32767, true);
  }
  return ab;
}

async function run(msg) {
  BASE = (msg.baseUrl || '') + BASE;
  // 1) engine + ort
  stage('Loading stem engine...');
  await loadScript('stem-engine.js');
  if (!self.StemEngine) throw new Error('stem engine failed to load');
  stage('Loading ONNX runtime...');
  var ortMod = await loadScript('ort.min.js');
  if (ortMod) self.ort = ortMod.default || ortMod;
  if (!self.ort) throw new Error('onnxruntime failed to load');
  if (self.ort.env.wasm) self.ort.env.wasm.proxy = false;

  // PS5's HTTP server serves .mjs as application/octet-stream, which Chrome's
  // dynamic import rejects (strict MIME). Blob-ify the wasm glue module and
  // point ort at it via wasmPaths={mjs,wasm} (we control the blob MIME).
  var glue = await (await fetch(BASE + '/ort-wasm-simd-threaded.jsep.mjs')).text();
  var glueUrl = URL.createObjectURL(new Blob([glue], { type: 'text/javascript' }));
  self.ort.env.wasm.wasmPaths = { mjs: glueUrl, wasm: BASE + '/ort-wasm-simd-threaded.jsep.wasm' };

  // 2) audio
  stage('Downloading track...');
  var buf = await (await fetch(msg.url)).arrayBuffer();
  var pcm = parseWav(buf);
  if (!pcm) {
    stage('Decoding audio...');
    var OC = self.OfflineAudioContext || self.webkitOfflineAudioContext;
    var probe = parseWavHead(buf);
    var oc = new OC(2, 1, probe || 44100);
    var decoded = await new Promise(function (res, rej) {
      oc.decodeAudioData(buf, res, function (e) { rej(new Error('decode failed: ' + e)); });
    });
    pcm = { L: new Float32Array(decoded.getChannelData(0)), R: decoded.numberOfChannels > 1 ? new Float32Array(decoded.getChannelData(1)) : null, sr: decoded.sampleRate };
    if (!pcm.R) pcm.R = pcm.L;
  }
  function parseWavHead(b) {
    try {
      var dv = new DataView(b);
      var off = 12;
      while (off + 8 <= dv.byteLength) {
        var id = dv.getUint32(off), sz = dv.getUint32(off + 4, true);
        if (id === 0x666d7420) return dv.getUint32(off + 12, true);
        off += 8 + sz + (sz & 1);
      }
    } catch (e) {}
    return 0;
  }

  // 3) model
  stage('Loading model (66.8 MB, cached)...');
  var mb = await (await fetch(BASE + '/stems.onnx')).arrayBuffer();
  pct(0.02);
  var t0 = Date.now();
  var sess = await self.ort.InferenceSession.create(mb, {
    graphOptimizationLevel: 'all',
    executionProviders: ['webgpu', 'wasm'],
  });
  var ep = 'wasm';
  try { ep = sess.inputMetadata ? 'wasm' : 'wasm'; } catch (e) {}
  stage('Separating stems (' + (Date.now() - t0 < 1 ? 'starting' : 'running') + ')...');

  // 4) chunk loop — identical math to compare.mjs (validated vs PyTorch)
  var E = self.StemEngine, eng = new E();
  var CHUNK = E.CHUNK, PAD = E.PAD, FL = E.FRAME_LEN, COMP = E.COMP;
  var L = pcm.L, R = pcm.R, n = L.length;
  var vL = new Float32Array(n), vR = new Float32Array(n);
  var nChunks = Math.ceil(n / CHUNK);
  var inName = sess.inputNames[0], outName = sess.outputNames[0];
  for (var j = 0; j < nChunks; j++) {
    var st = j * CHUNK, en = Math.min(st + CHUNK, n);
    var cL = new Float32Array(FL), cR = new Float32Array(FL);
    var s0 = Math.max(0, st - PAD), left = PAD - (st - s0), tail = Math.min(n, en + PAD);
    for (var i2 = s0; i2 < tail; i2++) { cL[left + (i2 - s0)] = L[i2]; cR[left + (i2 - s0)] = R[i2]; }
    var tensor = eng.stftToTensor(cL, cR);
    var feeds = {};
    feeds[inName] = new self.ort.Tensor('float32', tensor, [1, 4, 3072, 256]);
    var out = await sess.run(feeds);
    var mask = out[outName].data;
    var maskC = new Float32Array(mask.length);
    for (var m2 = 0; m2 < mask.length; m2++) maskC[m2] = mask[m2] * COMP;
    var aL = new Float64Array(FL), aR = new Float64Array(FL);
    eng.istftAccum(maskC, aL, aR);
    eng.normalizeEnv(aL, aR);
    for (var w2 = 0; w2 < en - st; w2++) { vL[st + w2] = aL[PAD + w2]; vR[st + w2] = aR[PAD + w2]; }
    pct(0.03 + 0.95 * (j + 1) / nChunks);
    stage('Separating stems — chunk ' + (j + 1) + '/' + nChunks + ' (' + Math.round((Date.now() - t0) / 1000) + 's)');
  }

  // 5) instrumental = mix - vocals (validated choice, saves a model pass)
  var iL = new Float32Array(n), iR = new Float32Array(n);
  for (var k = 0; k < n; k++) { iL[k] = L[k] - vL[k]; iR[k] = R[k] - vR[k]; }

  stage('Encoding WAVs...');
  var wV = encodeWav(vL, vR, pcm.sr), wI = encodeWav(iL, iR, pcm.sr);
  postMessage({ done: true, vocals: wV, inst: wI, sr: pcm.sr, secs: Math.round((Date.now() - t0) / 1000) },
    [wV, wI]);
}

self.onmessage = function (e) {
  run(e.data || {}).catch(function (err) { fail(err); });
};
self.onmessageerror = function () { fail(new Error('message deserialize error')); };
