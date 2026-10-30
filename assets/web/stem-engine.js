/* MDX-Net Voc_FT browser engine — STFT/iSTFT around the ONNX U-Net.
 * Contract mirrors UVR's inference_mdx exactly (validated sample-by-sample
 * against the PyTorch reference, 2026-10-19):
 *   n_fft=7680, hop=1024, dim_f=3072, dim_t=256, compensate=1.021,
 *   periodic Hann, center=False (chunks pre-padded with n_fft/2 context),
 *   plane order [L.real, L.imag, R.real, R.imag], instrumental = mix - vocals.
 * n_fft=7680 is not a power of two: forward/inverse use Bluestein's chirp-z
 * through a radix-2 FFT of M=16384. */
(function (global) {
  'use strict';
  var N = 7680, HOP = 1024, DIM_F = 3072, DIM_T = 256, COMP = 1.021;
  var M = 16384, CHUNK = DIM_T * HOP, PAD = N / 2, FRAME_LEN = CHUNK + N;

  // ---- radix-2 complex FFT (in-place, float64 re/im), precomputed tables ----
  var REV = new Uint16Array(M), LOG2M = 14;
  (function () {
    for (var i = 0; i < M; i++) {
      var r = 0;
      for (var b = 0; b < LOG2M; b++) if (i & (1 << b)) r |= 1 << (LOG2M - 1 - b);
      REV[i] = r;
    }
  })();
  function fft(re, im, inverse) {
    for (var i = 0; i < M; i++) {
      var j = REV[i];
      if (j > i) { var t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (var len = 2; len <= M; len <<= 1) {
      var half = len >> 1, step = (inverse ? 2 : -2) * Math.PI / len;
      var wr = Math.cos(step), wi = Math.sin(step);
      for (var s = 0; s < M; s += len) {
        var cwr = 1, cwi = 0;
        for (var k = 0; k < half; k++) {
          var a = s + k, b = a + half;
          var br = re[b] * cwr - im[b] * cwi, bi = re[b] * cwi + im[b] * cwr;
          var ar = re[a], ai = im[a];
          re[a] = ar + br; im[a] = ai + bi;
          re[b] = ar - br; im[b] = ai - bi;
          var nwr = cwr * wr - cwi * wi; cwi = cwr * wi + cwi * wr; cwr = nwr;
        }
      }
    }
    if (inverse) for (var q = 0; q < M; q++) { re[q] /= M; im[q] /= M; }
  }

  // ---- Bluestein chirp kernel, sign=-1 forward ----
  var CH_R = new Float64Array(N), CH_I = new Float64Array(N), B_R, B_I;
  (function () {
    var i;
    for (i = 0; i < N; i++) {
      var n2 = (i * i) % (4 * N);            // angle = pi*n^2/N, reduced mod 2pi
      var ang = -Math.PI * (n2 / N);
      CH_R[i] = Math.cos(ang); CH_I[i] = Math.sin(ang);
    }
    var bR = new Float64Array(M), bI = new Float64Array(M);
    // convolution kernel b[m] = conj(chirp(m)); b[N] is never indexed, skip it
    for (i = 0; i < N; i++) { bR[i] = CH_R[i]; bI[i] = -CH_I[i]; }
    for (i = 1; i < N; i++) { bR[M - i] = CH_R[i]; bI[M - i] = -CH_I[i]; }
    fft(bR, bI, false);
    B_R = bR; B_I = bI;
  })();

  var _ar = new Float64Array(M), _ai = new Float64Array(M);

  // DFT_N of a complex length-N sequence (chirp-z). Outputs bins 0..N-1.
  function dftN(inR, inI, outR, outI) {
    var i;
    for (i = 0; i < N; i++) {
      _ar[i] = inR[i] * CH_R[i] - inI[i] * CH_I[i];
      _ai[i] = inR[i] * CH_I[i] + inI[i] * CH_R[i];
    }
    for (i = N; i < M; i++) { _ar[i] = 0; _ai[i] = 0; }
    fft(_ar, _ai, false);
    for (i = 0; i < M; i++) {
      var ar = _ar[i], ai = _ai[i];
      _ar[i] = ar * B_R[i] - ai * B_I[i];
      _ai[i] = ar * B_I[i] + ai * B_R[i];
    }
    fft(_ar, _ai, true);
    for (i = 0; i < N; i++) {
      outR[i] = CH_R[i] * _ar[i] - CH_I[i] * _ai[i];
      outI[i] = CH_R[i] * _ai[i] + CH_I[i] * _ar[i];
    }
  }

  var _yr = new Float64Array(M), _yi = new Float64Array(M),
      _dr = new Float64Array(N), _di = new Float64Array(N);

  // IDFT_N: complex spectrum X[0..N-1] -> complex x[0..N-1].
  // x = conj(DFT(conj(X)))/N. Caller keeps X conjugate-symmetric-aware.
  function idftN(xr, xi, outR, outI) {
    var i, k;
    for (i = 0; i < N; i++) { _yr[i] = xr[i]; _yi[i] = -xi[i]; }  // conj(X)
    dftN(_yr, _yi, _dr, _di);
    var inv = 1 / N;
    for (i = 0; i < N; i++) { outR[i] = _dr[i] * inv; outI[i] = -_di[i] * inv; }
  }

  // ---- window + istft envelope (identical formulas to reference.py) ----
  var WIN = new Float64Array(N);
  for (var w = 0; w < N; w++) WIN[w] = 0.5 - 0.5 * Math.cos(2 * Math.PI * w / N);
  var ENV = new Float64Array(FRAME_LEN);
  for (var f0 = 0; f0 < DIM_T; f0++)
    for (var e = 0; e < N; e++) ENV[f0 * HOP + e] += WIN[e] * WIN[e];
  for (var e2 = 0; e2 < FRAME_LEN; e2++) if (ENV[e2] < 1e-8) ENV[e2] = 1e-8;

  var _x = new Float64Array(N), _or = new Float64Array(N), _oi = new Float64Array(N),
      _fr = new Float64Array(N), _fi = new Float64Array(N),
      _ir = new Float64Array(N), _ii = new Float64Array(N),
      _real = new Float64Array(N);

  function Engine() {}
  Engine.CHUNK = CHUNK; Engine.PAD = PAD; Engine.FRAME_LEN = FRAME_LEN;
  Engine.COMP = COMP;

  // chunkL/chunkR: Float32Array length CHUNK+N (context-padded) -> [1,4,3072,256]
  Engine.prototype.stftToTensor = function (chunkL, chunkR) {
    var t = new Float32Array(4 * DIM_F * DIM_T);
    var ch, src, f, i, off, base;
    for (ch = 0; ch < 2; ch++) {
      src = ch === 0 ? chunkL : chunkR;
      for (f = 0; f < DIM_T; f++) {
        off = f * HOP;
        for (i = 0; i < N; i++) _x[i] = src[off + i] * WIN[i];
        dftN(_x, ZERO_N, _or, _oi);
        for (i = 0; i < DIM_F; i++) {
          t[(2 * ch) * DIM_F * DIM_T + i * DIM_T + f] = _or[i];
          t[(2 * ch + 1) * DIM_F * DIM_T + i * DIM_T + f] = _oi[i];
        }
      }
    }
    return t;
  };

  // mask [4,3072,256] (compensate applied) -> OLA into accL/accR (Float64Array FRAME_LEN)
  Engine.prototype.istftAccum = function (mask, accL, accR) {
    var ch, f, i;
    for (ch = 0; ch < 2; ch++) {
      var reB = (2 * ch) * DIM_F * DIM_T, imB = (2 * ch + 1) * DIM_F * DIM_T;
      for (f = 0; f < DIM_T; f++) {
        for (i = DIM_F; i < N; i++) { _fr[i] = 0; _fi[i] = 0; }
        for (i = 0; i < DIM_F; i++) {
          _fr[i] = mask[reB + i * DIM_T + f];
          _fi[i] = mask[imB + i * DIM_T + f];
        }
        // build the conjugate mirror above Nyquist (irfft semantics)
        for (i = 1; i <= DIM_F; i++) { _fr[N - i] = _fr[i]; _fi[N - i] = -_fi[i]; }
        idftN(_fr, _fi, _ir, _ii);
        var acc = ch === 0 ? accL : accR, off = f * HOP;
        for (i = 0; i < N; i++) acc[off + i] += _ir[i] * WIN[i];
      }
    }
  };
  Engine.prototype.normalizeEnv = function (accL, accR) {
    for (var i = 0; i < FRAME_LEN; i++) { accL[i] /= ENV[i]; accR[i] /= ENV[i]; }
  };

  var ZERO_N = new Float64Array(N);

  // test hooks
  global.StemEngine = Engine;
  global._dftN = dftN; global._idftN = idftN; global._N = N; global._DIM_F = DIM_F;
  global._fft = fft; global._M = M; global._bR = B_R; global._bI = B_I; global._chirpR = CH_R; global._chirpI = CH_I;
})(self);
