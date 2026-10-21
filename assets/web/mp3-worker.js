/* mp3-worker.js — lamejs MP3 encoding off the main thread (UI v26).
   Receives {buf: ArrayBuffer} of a WAV file, parses PCM manually (no
   AudioContext inside workers), streams progress back, replies with the
   finished MP3 bytes. Non-PCM-WAV input replies {fallback:true} so the
   caller can decode via AudioContext on the main thread instead. */
'use strict';
importScripts('lame.min.js');

function parseWav(view) {
  if (view.byteLength < 44) return null;
  var tag = function (o) { return String.fromCharCode(view.getUint8(o), view.getUint8(o + 1), view.getUint8(o + 2), view.getUint8(o + 3)); };
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  var fmt = null, dataOff = -1, dataLen = 0, o = 12;
  while (o + 8 <= view.byteLength) {
    var id = tag(o), size = view.getUint32(o + 4, true);
    if (id === 'fmt ') fmt = { f: view.getUint16(o + 8, true), ch: view.getUint16(o + 10, true), rate: view.getUint32(o + 12, true), bits: view.getUint16(o + 22, true) };
    else if (id === 'data') { dataOff = o + 8; dataLen = Math.min(size, view.byteLength - o - 8); }
    else if (size === 0xFFFFFFFF) break;
    o += 8 + size + (size & 1);
  }
  if (!fmt || dataOff < 0) return null;
  return { fmt: fmt, off: dataOff, len: dataLen };
}

self.onmessage = function (e) {
  try {
    var buf = e.data.buf;
    var view = new DataView(buf);
    var w = parseWav(view);
    if (!w || (w.fmt.f === 3 && w.fmt.bits !== 32) || (w.fmt.f === 1 && w.fmt.bits !== 16)) {
      self.postMessage({ fallback: true });
      return;
    }
    var ch = Math.min(2, w.fmt.ch || 1), rate = w.fmt.rate, float = w.fmt.f === 3;
    var bytesPer = float ? 4 : 2;
    var frames = Math.floor(w.len / (w.fmt.ch * bytesPer));
    var chans = [];
    for (var c = 0; c < ch; c++) chans.push(new Int16Array(frames));
    var off = w.off;
    for (var i = 0; i < frames; i++) {
      for (var s = 0; s < w.fmt.ch; s++) {
        var v;
        if (float) { v = view.getFloat32(off, true); off += 4; v = Math.max(-1, Math.min(1, v)); v = v < 0 ? v * 32768 : v * 32767; }
        else { v = view.getInt16(off, true); off += 2; }
        if (s < ch) chans[s][i] = v;
      }
    }
    var enc = new lamejs.Mp3Encoder(ch, rate, 320);
    var BLOCK = 1152, out = [];
    for (var p = 0; p < frames; p += BLOCK) {
      var a = chans[0].subarray(p, p + BLOCK);
      var b = ch > 1 ? chans[1].subarray(p, p + BLOCK) : null;
      var chunk = b ? enc.encodeBuffer(a, b) : enc.encodeBuffer(a);
      if (chunk.length > 0) out.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk));
      if ((p / BLOCK) % 100 === 0) self.postMessage({ pct: Math.min(99, Math.round(p / frames * 100)) });
    }
    var tail = enc.flush();
    if (tail.length > 0) out.push(tail instanceof Uint8Array ? tail : new Uint8Array(tail));
    var total = 0, k;
    for (k = 0; k < out.length; k++) total += out[k].length;
    var mp3 = new Uint8Array(total), pos = 0;
    for (k = 0; k < out.length; k++) { mp3.set(out[k], pos); pos += out[k].length; }
    self.postMessage({ done: true, mp3: mp3 }, [mp3.buffer]);
  } catch (err) {
    self.postMessage({ error: String(err && err.message || err) });
  }
};
