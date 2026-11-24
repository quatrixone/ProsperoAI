// Prospero Music Studio controller mounted in the Workspace Audio panel.
(function () {
  'use strict';
  var AUDIO_DIR = '/download0/ProsperoAI/work/audio/';
  var $ = function (id) { return document.getElementById(id); };

  // raw-body upload (needs the /api/music/uploadbin route; 404 = old build)
  function uploadBinary(name, buf) {
    var opts = {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-Upload-Name': name },
      body: buf
    };
    return window.ProsperoWorkspaceApi('/api/music/uploadbin', opts).then(function (r) {
      return r.json();
    }).catch(function (e) {
      if (/\b404\b/.test(e.message)) e.missing = true;
      throw e;
    });
  }

  function api(path, body, timeoutMs, method) {
    var opts = {};
    if (body !== undefined) {
      opts.method = method || 'POST';
      opts.headers = { 'Content-Type': 'application/json' };
      opts.body = JSON.stringify(body);
    }
    var ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    var timer = ctl && timeoutMs ? setTimeout(function () { ctl.abort(); }, timeoutMs) : null;
    if (ctl) opts.signal = ctl.signal;
    return window.ProsperoWorkspaceApi(path, opts).then(function (r) {
      return r.json();
    }).finally(function () {
      if (timer) clearTimeout(timer);
    });
  }

  // Fire an async job and poll for its result: the console answers
  // /api/generate immediately with a job_id, the generation runs on the
  // server, and every poll is a short request a flaky phone connection can
  // survive. Poll failures are retried until the deadline — only then does
  // the caller see an error. Resolves with the same JSON a sync generate
  // would have returned (j.response / j.error).
  function generateAndWait(model, prompt, maxMs, _retry) {
    var deadline = Date.now() + (maxMs || 2700000);
    var startedAt = Date.now();
    return api('/api/generate', { model: model, prompt: prompt, async: true }, 30000)
      .then(function (j) {
        if (!j.job_id) throw new Error(i18n.t('web.server_has_no_async_jobs_old_build',"server has no async jobs (old build?)"));
        var sleep = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
        var poll = function () {
          return api('/api/jobs?id=' + j.job_id, undefined, 20000)
            .then(function (s) {
              if (s && s.state === 'running') {
                if (Date.now() > deadline)
                  throw new Error(i18n.t('web.mu_job_still_running', "job {0} still running after {1} min", j.job_id, Math.round((maxMs || 2700000) / 60000)));
                setStatus(i18n.t('web.mu_generating_elapsed', "Generating — {0}s elapsed; the job finishes even if you close this page.", Math.round((Date.now() - startedAt) / 1000)), 'busy');
                return sleep(5000).then(poll);
              }
              if (!s || !s.result) return { error: 'job ' + j.job_id + ' vanished' };
              return s.result;
            })
            .catch(function (e) {
              // 404 means the app restarted while the job was running. Jobs
              // are process-local, so the user must check the library before
              // deciding whether to submit the work again.
              if (e && /404|no such job|job lost/.test(e.message)) {
                throw new Error(i18n.t('web.the_job_is_no_longer_available_check',"The job is no longer available. Check the library before starting another generation."));
              }
              // a dropped poll is not a failure: retry until the deadline
              if (e && /vanished|old build|still running/.test(e.message)) throw e;
              if (Date.now() > deadline) throw e;
              return sleep(8000).then(poll);
            });
        };
        return sleep(2500).then(poll);
      });
  }


  function setStatus(msg, cls) {
    var el = $('gen-status');
    el.textContent = msg || '';
    el.className = cls || '';
  }  function waitForStems(base, onReady, onFail) {
    var probe = AUDIO_DIR + encodeURIComponent(base + '-vocals.wav') + '?p=';
    var tries = 0;
    var iv = setInterval(function () {
      tries++;
      fetch(probe + Date.now(), { headers: { Range: 'bytes=0-0' } })
        .then(function (r) {
          if (r.ok || r.status === 206) { clearInterval(iv); onReady(); }
          else if (tries > 36) { clearInterval(iv); onFail(i18n.t('web.mu_stems_never_appeared', "the stems files never appeared (3 min)")); }
        })
        .catch(function () { if (tries > 36) { clearInterval(iv); onFail(i18n.t('web.mu_conn_lost_stems', "connection lost and the stems files never appeared")); } });
    }, 5000);
  }

  function runningJobBanner() {
    api('/api/jobs', undefined, 15000).then(function (d) {
      var run = (d && d.jobs || []).filter(function (j) { return j.state === 'running'; });
      if (run.length) {
        var j = run[run.length - 1];
        setStatus(i18n.t('web.mu_console_busy', "Console is busy: {0} job running for {1}s — it finishes even if you close this page.", j.model, j.elapsed_s), 'busy');
      }
    }).catch(function () {});
  }

  function setAbcStatus(msg, cls) {
    var el = $('abc-status'); if (!el) return;
    el.hidden = !msg; el.textContent = msg || '';
    el.setAttribute('data-cls', cls || '');
  }


  // ---- models ----
  var musicModels = [];
  function syncAudioDurationVisibility() {
    var model = musicModels.find(function (item) { return item.id === $('model').value; });
    $('duration-control').hidden = !window.ProsperoWorkspace.audioDurationAvailable(model);
  }

  function loadModels() {
    return api('/v1/models').then(function (j) {
      var sel = $('model');
      var previous = sel.value;
      sel.innerHTML = '';
      var available = (j.data || []).filter(function (m) {
        return typeof m.id === 'string' && m.purpose === 'text-to-audio';
      });
      musicModels = available;
      var placeholder = document.createElement('option');
      placeholder.value = '';
      placeholder.textContent = available.length ? i18n.t('web.choose_a_music_model',"Choose a music model…") : i18n.t('web.no_installed_music_models',"No installed music models");
      sel.appendChild(placeholder);
      available.forEach(function (m) {
        var o = document.createElement('option');
        o.value = m.id;
        o.textContent = m.name || m.id;
        sel.appendChild(o);
      });
      if (available.some(function (m) { return m.id === previous; })) sel.value = previous;
      syncAudioDurationVisibility();
      $('status-dot').className = available.length ? 'dot on' : 'dot';
    }).catch(function (e) { $('status-dot').className = 'dot'; setStatus(i18n.t('web.mu_cannot_load_models', "Cannot load installed models: {0}", e.message), 'err'); });
  }

  // ---- runtime settings ----
  function loadSettings() {
    return api('/api/settings/runtime').then(function (s) {
      if (typeof s.audio_lyrics === 'string') $('lyrics').value = s.audio_lyrics;
      if (s.audio_duration_sec) { $('duration').value = s.audio_duration_sec; }
      if (s.audio_seed) $('seed').value = s.audio_seed;
      $('dur-val').textContent = $('duration').value;
      var opts = String(s.audio_options || '');
      opts.split('\n').forEach(function (line) {
        var kv = line.split('=');
        if (kv.length < 2) return;
        var key = kv[0].trim(), val = kv.slice(1).join('=').trim();
        if (key === 'cot') $('cot').value = val;
        else if (key === 'abc_max_tokens') $('abc-max').value = val;
        else if (key === 'abc_file') {
          var name = val.replace(AUDIO_DIR, '');
          $('abc-name').value = name;
          fetch(AUDIO_DIR + name).then(function (r) { return r.ok ? r.text() : ''; })
            .then(function (t) { if (t) $('abc').value = t; }).catch(function () {});
        }
      });
    }).catch(function () {});
  }

  function buildOptions(abcName) {
    var lines = [];
    if (!abcName) return '';
    var cot = $('cot').value;
    if (cot) lines.push('cot=' + cot);
    lines.push('abc_file=' + AUDIO_DIR + abcName);
    if ($('abc-max').value) lines.push('abc_max_tokens=' + $('abc-max').value);
    return lines.join('\n');
  }

  // ---- custom audio player (big seekbar under play, ⋮ menu with WAV/MP3 download) ----
  var activePlayer = null;
  var playerAudios = [];
  document.addEventListener('music-vol', function (e) {
    playerAudios.forEach(function (audio) { audio.volume = e.detail / 100; });
  });
  function fmtTime(s) {
    if (!isFinite(s)) return '--:--';
    s = Math.max(0, Math.round(s));
    return Math.floor(s / 60) + ':' + ('0' + (s % 60)).slice(-2);
  }
  function mp3FromUrl(url, name, done, fail) {
    if (!window.lamejs) { fail(new Error(i18n.t('web.mp3_encoder_not_loaded',"MP3 encoder not loaded"))); return; }
    setStatus(i18n.t('web.converting_to_mp3_keep_this_page_ope',"Converting to MP3 — keep this page open..."), 'busy');
    fetch(url).then(function (r) {
      if (!r.ok) throw new Error(i18n.t('web.mu_download_failed_http', "download failed: HTTP {0}", r.status));
      return r.arrayBuffer();
    }).then(function (buf) {
      // Fast path: parse PCM WAV ourselves inside a Web Worker (no tab freeze).
      try {
        var w = new Worker('/mp3-worker.js');
        w.onmessage = function (e) {
          var d = e.data || {};
          if (d.pct !== undefined) { setStatus(i18n.t('web.mu_encoding_mp3', "Encoding MP3... {0}%", d.pct), 'busy'); return; }
          if (d.done) {
            w.terminate();
            var blob = new Blob([d.mp3], { type: 'audio/mpeg' });
            var a2 = document.createElement('a');
            a2.href = URL.createObjectURL(blob);
            a2.download = name.replace(/\.[a-z0-9]+$/i, '') + '.mp3';
            document.body.appendChild(a2); a2.click();
            setTimeout(function () { URL.revokeObjectURL(a2.href); a2.remove(); }, 5000);
            done(blob.size);
            return;
          }
          if (d.error) { w.terminate(); fail(new Error(d.error)); return; }
          if (d.fallback) { w.terminate(); mp3DecodeFallback(buf, name, done, fail); }
        };
        w.onerror = function () { w.terminate(); mp3DecodeFallback(buf, name, done, fail); };
        w.postMessage({ buf: buf }, [buf]);
        return;
      } catch (e) { /* Worker ctor unsupported (very old browser) */ }
      mp3DecodeFallback(buf, name, done, fail);
    }).catch(fail);
  }
  function mp3DecodeFallback(buf, name, done, fail) {
    setStatus(i18n.t('web.converting_to_mp3_compat_mode_page_m',"Converting to MP3 (compat mode, page may pause)..."), 'busy');
    var Ctx = window.AudioContext || window.webkitAudioContext;
    var ctx = new Ctx();
    new Promise(function (res, rej) {
      ctx.decodeAudioData(buf, res, function (e) { rej(new Error(i18n.t('web.mu_decode_failed', "decode failed: {0}", e))); });
    }).then(function (dec) {
      var rate = dec.sampleRate, ch = Math.min(2, dec.numberOfChannels);
      var enc = new lamejs.Mp3Encoder(ch, rate, 320);
      var chans = []; for (var c = 0; c < ch; c++) {
        var f = dec.getChannelData(c), i16 = new Int16Array(f.length);
        for (var i = 0; i < f.length; i++) { var v = Math.max(-1, Math.min(1, f[i])); i16[i] = v < 0 ? v * 32768 : v * 32767; }
        chans.push(i16);
      }
      var BLOCK = 1152, out = [], total = chans[0].length;
      for (var off = 0; off < total; off += BLOCK) {
        var a = chans[0].subarray(off, off + BLOCK);
        var b = ch > 1 ? chans[1].subarray(off, off + BLOCK) : null;
        var chunk = b ? enc.encodeBuffer(a, b) : enc.encodeBuffer(a);
        if (chunk.length) out.push(new Uint8Array(chunk));
      }
      var end = enc.flush(); if (end.length) out.push(new Uint8Array(end));
      try { ctx.close(); } catch (e) {}
      var blob = new Blob(out, { type: 'audio/mpeg' });
      var a2 = document.createElement('a');
      a2.href = URL.createObjectURL(blob);
      a2.download = name.replace(/\.[a-z0-9]+$/i, '') + '.mp3';
      document.body.appendChild(a2); a2.click();
      setTimeout(function () { URL.revokeObjectURL(a2.href); a2.remove(); }, 5000);
      done(blob.size);
    }).catch(fail);
  }
  // ---- shared volume (persisted, applied to every player live) ----
  function getStoredVol() {
    var v = parseInt(localStorage.getItem('prospero-vol'), 10);
    return isNaN(v) ? 80 : Math.max(0, Math.min(100, v));
  }

  // ---- Cover: reload a track's saved generation settings into the form ----
  function applyCover(meta, filename) {
    if (meta.lyrics !== undefined) $('lyrics').value = meta.lyrics;
    if (meta.style !== undefined) $('style').value = meta.style;
    if (meta.seed !== undefined) $('seed').value = String(meta.seed);
    if (meta.duration !== undefined) {
      $('duration').value = String(meta.duration);
      $('dur-val').textContent = $('duration').value;
    }
    if (meta.cot !== undefined) $('cot').value = meta.cot;
    if (meta.abc_name !== undefined) $('abc-name').value = meta.abc_name;
    if (meta.abc_content) { $('abc').value = meta.abc_content; if (meta.abc_name) knownFiles[meta.abc_name] = true; }
    var base = String(filename || '').replace(/\.(wav|mp3)$/i, '').replace(/_seed\d+$/i, '');
    $('track-name').value = (base ? base + ' cover' : 'cover');
    try { formSaveNow(); } catch (e) {}
    $('lyrics').scrollIntoView({ behavior: 'smooth', block: 'start' });
    setStatus(i18n.t('web.mu_cover_settings_loaded', "Cover settings loaded from {0} — press GENERATE", filename), 'ok');
  }

  // ---- Cover + ABC: one-click subtle-variation recipe from a library track.
  // Loads lyrics/style/seed/duration from the track's saved settings AND
  // transcribes the track to ABC on the console (SheetSage2), arming ABC
  // conditioning for the next generation. Memory restarts (when the heap is
  // fragmented) are reported without restarting the title.
  function coverAndAbc(meta, filename) {
    if (meta && Object.keys(meta).length) applyCover(meta, filename);
    var base = String(filename || '').replace(/\.(wav|mp3)$/i, '').replace(/_seed\d+$/i, '');
    if (!meta || !Object.keys(meta).length)
      $('track-name').value = base + ' var';
    var t0 = Date.now();
    var once = function (retry) {
      setStatus(i18n.t('web.mu_cover_abc_transcribing', "Cover+ABC: transcribing {0} with SheetSage2 (1-3 min, you can close the page)...", filename), 'busy');
      if (!$('abc').value.trim())
        setAbcStatus(i18n.t('web.mu_abc_generating_wait', "⏳ Generating ABC score — takes about a minute, it appears here automatically when ready…"), 'busy');
      return api('/api/settings/runtime', { audio_options: 'input_audio=' + AUDIO_DIR + filename }, 30000)
        .then(function () {
          return generateAndWait('sheetsage2-orig', 'transcribe ' + filename, 900000);
        })
        .then(function (j) {
          var resp = String(j.response || j.error || JSON.stringify(j).slice(0, 400));
          if (/NEEDS_RESTART|backend buffer allocation failed|failed to allocate Yue2 NAR/.test(resp)) {
          throw new Error(i18n.t('web.not_enough_usable_memory_wait_for_ot',"Not enough usable memory. Wait for other work to finish, then close and reopen the app before retrying."));
        }
          var m = resp.match(/\/download0\/\S+?\.abc/);
          if (!m) throw new Error(resp.slice(0, 220));
        var path = m[0].replace(/["',).]+$/, '');
        var fname = path.substring(path.lastIndexOf('/') + 1);
        return fetch(AUDIO_DIR + encodeURIComponent(fname)).then(function (r) {
          if (!r.ok) throw new Error(i18n.t('web.mu_fetch_score_http', "fetch score HTTP {0}", r.status));
          return r.text();
        }).then(function (t) {
          $('abc').value = t;
          setAbcStatus(i18n.t('web.mu_score_ready', "✓ Score ready — press GENERATE to use it (or edit it first)."), 'ok');
          var abcName = base.replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 60) + '-var.abc';
          $('abc-name').value = abcName;
          knownFiles[abcName] = true;
          if ($('cot') && !$('cot').value) $('cot').value = 'melody';
          loadAbcSelect();
          try { formSaveNow(); } catch (e) {}
          setStatus(i18n.t('web.mu_cover_abc_done', "Cover+ABC done in {0}s — score armed. Tweak lyrics/style and press GENERATE; memory errors leave the app running.", Math.round((Date.now() - t0) / 1000)), 'ok');
          $('abc').scrollIntoView({ behavior: 'smooth', block: 'start' });
        });
      });
    };
    once(2).catch(function (e) {
      if (!$('abc').value.trim())
        setAbcStatus(i18n.t('web.mu_abc_generation_failed', "ABC generation failed — you can still generate from lyrics, or paste a score manually."), 'err');
      setStatus(i18n.t('web.mu_cover_abc_failed', "Cover+ABC failed: {0}", (e && e.message ? e.message : e)), 'err');
    });
  }

  // ---- Master: explicit-only mastering (peak normalize + limiter), browser-side ----
  function encodeWav16(buffer) {
    var ch = buffer.numberOfChannels, sr = buffer.sampleRate, len = buffer.length;
    var bytes = 44 + len * ch * 2;
    var ab = new ArrayBuffer(bytes), dv = new DataView(ab);
    function str(off, s) { for (var i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); }
    str(0, 'RIFF'); dv.setUint32(4, bytes - 8, true); str(8, 'WAVEfmt ');
    dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, ch, true);
    dv.setUint32(24, sr, true); dv.setUint32(28, sr * ch * 2, true);
    dv.setUint16(32, ch * 2, true); dv.setUint16(34, 16, true);
    str(36, 'data'); dv.setUint32(40, len * ch * 2, true);
    var chans = [], c;
    for (c = 0; c < ch; c++) chans.push(buffer.getChannelData(c));
    var off = 44;
    for (var i = 0; i < len; i++) {
      for (c = 0; c < ch; c++) {
        var v = Math.max(-1, Math.min(1, chans[c][i]));
        dv.setInt16(off, v < 0 ? v * 0x8000 : v * 0x7FFF, true); off += 2;
      }
    }
    return new Blob([ab], { type: 'audio/wav' });
  }
  function masterTrack(url, onProgress) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error(i18n.t('web.mu_fetch_http', "fetch {0}", r.status));
      return r.arrayBuffer();
    }).then(function (buf) {
      var AC = window.AudioContext || window.webkitAudioContext;
      var ac = new AC();
      return ac.decodeAudioData(buf).then(function (decoded) {
        var peak = 0, c, d = decoded.getChannelData(0), i;
        for (c = 0; c < decoded.numberOfChannels; c++) {
          d = decoded.getChannelData(c);
          for (i = 0; i < d.length; i++) { var a = d[i] < 0 ? -d[i] : d[i]; if (a > peak) peak = a; }
        }
        if (peak < 0.001) throw new Error(i18n.t('web.audio_is_silent',"audio is silent"));
        var gain = Math.min(0.89 / peak, 12);
        var oc = new OfflineAudioContext(decoded.numberOfChannels, decoded.length, decoded.sampleRate);
        var src = oc.createBufferSource(); src.buffer = decoded;
        var g = oc.createGain(); g.gain.value = gain;
        var lim = oc.createDynamicsCompressor();
        lim.threshold.value = -6; lim.knee.value = 4; lim.ratio.value = 16;
        lim.attack.value = 0.003; lim.release.value = 0.12;
        src.connect(g); g.connect(lim); lim.connect(oc.destination);
        src.start();
        onProgress(i18n.t('web.mu_mastering_progress', "Mastering (normalize to -1 dBFS + limiter)..."));
        return oc.startRendering();
      }).then(function (rendered) { ac.close(); return rendered; });
    });
  }

  function makePlayer(url, filename, isMp3Source, meta) {
    var wrap = document.createElement('div'); wrap.className = 'pl';
    var audio = new Audio(); audio.preload = 'metadata'; audio.src = url;
    var row = document.createElement('div'); row.className = 'pl-row';
    var play = document.createElement('button'); play.type = 'button'; play.className = 'pl-play'; play.textContent = '▶';
    var tm = document.createElement('span'); tm.className = 'pl-time'; tm.textContent = '0:00 / --:--';
    var vol = document.createElement('input'); vol.type = 'range'; vol.className = 'pl-vol';
    vol.min = '0'; vol.max = '100'; vol.step = '1'; vol.value = String(getStoredVol()); vol.title = i18n.t('web.volume',"Volume");
    audio.volume = getStoredVol() / 100;
    var menuBtn = document.createElement('button'); menuBtn.type = 'button'; menuBtn.className = 'pl-menu'; menuBtn.textContent = '⋮'; menuBtn.title = i18n.t('web.more_actions',"More actions");
    var menu = document.createElement('div'); menu.className = 'pl-menu-box'; menu.style.display = 'none';
    var dlWav = document.createElement('a'); dlWav.href = url; dlWav.download = filename; dlWav.textContent = i18n.t('web.download_wav',"⬇ Download WAV"); dlWav.style.display = isMp3Source ? 'none' : '';
    var dlMp3 = document.createElement('button'); dlMp3.type = 'button'; dlMp3.textContent = isMp3Source ? i18n.t('web.download_mp3',"⬇ Download MP3") : i18n.t('web.download_mp3_320k_converts_in_browse',"⬇ Download MP3 (320k, converts in browser)");
    var coverBtn = document.createElement('button'); coverBtn.type = 'button';
    var hasMeta = meta && Object.keys(meta).length > 0;
    if (hasMeta) {
      coverBtn.textContent = i18n.t('web.cover_load_lyrics_style_seed',"🎤 Cover — load lyrics/style/seed");
      coverBtn.addEventListener('click', function (e) {
        e.stopPropagation(); menu.style.display = 'none';
        applyCover(meta, filename);
      });
    } else {
      coverBtn.textContent = i18n.t('web.cover_no_saved_settings_old_track',"🎤 Cover (no saved settings — old track)");
      coverBtn.disabled = true; coverBtn.className = 'pl-menu-dim';
    }
    var abcCoverBtn = document.createElement('button'); abcCoverBtn.type = 'button';
    if (isMp3Source) {
      abcCoverBtn.textContent = i18n.t('web.cover_abc_wav_only',"🎛 Cover + ABC (WAV only)");
      abcCoverBtn.disabled = true; abcCoverBtn.className = 'pl-menu-dim';
    } else {
      abcCoverBtn.textContent = i18n.t('web.cover_abc_settings_score_subtle_remi',"🎛 Cover + ABC (settings + score, subtle remix)");
      abcCoverBtn.addEventListener('click', function (e) {
        e.stopPropagation(); menu.style.display = 'none';
        coverAndAbc(meta, filename);
      });
    }
    var masterBtn = document.createElement('button'); masterBtn.type = 'button';
    masterBtn.textContent = i18n.t('web.master_normalize_limiter',"🎚 Master (normalize + limiter)");
    var ps5StemsBtn = document.createElement('button'); ps5StemsBtn.type = 'button';
    ps5StemsBtn.textContent = i18n.t('web.stems_ps5_gpu_htdemucs_25_s',"✂ Stems — PS5 GPU (htdemucs, ~25 s)");
    if (isMp3Source) ps5StemsBtn.style.display = 'none';
    menu.appendChild(dlWav); menu.appendChild(dlMp3); menu.appendChild(coverBtn); menu.appendChild(abcCoverBtn); menu.appendChild(masterBtn); menu.appendChild(ps5StemsBtn);
    row.appendChild(play); row.appendChild(tm); row.appendChild(vol); row.appendChild(menuBtn); row.appendChild(menu);
    vol.addEventListener('input', function () {
      try { localStorage.setItem('prospero-vol', vol.value); } catch (e) {}
      document.dispatchEvent(new CustomEvent('music-vol', { detail: parseInt(vol.value, 10) }));
    });
    playerAudios.push(audio);
    var seek = document.createElement('input'); seek.type = 'range'; seek.className = 'pl-seek'; seek.min = '0'; seek.max = '1000'; seek.value = '0';
    wrap.appendChild(row); wrap.appendChild(seek);
    var dragging = false;
    play.addEventListener('click', function () {
      if (audio.paused) {
        if (activePlayer && activePlayer !== api_) activePlayer.pause();
        activePlayer = api_;
        audio.play();
      } else audio.pause();
    });
    audio.addEventListener('play', function () { play.textContent = '❚❚'; });
    audio.addEventListener('pause', function () { play.textContent = '▶'; });
    audio.addEventListener('ended', function () { play.textContent = '▶'; seek.value = '0'; });
    audio.addEventListener('loadedmetadata', function () { tm.textContent = '0:00 / ' + fmtTime(audio.duration); });
    audio.addEventListener('timeupdate', function () {
      if (!dragging && audio.duration) {
        seek.value = String(Math.round(audio.currentTime / audio.duration * 1000));
        tm.textContent = fmtTime(audio.currentTime) + ' / ' + fmtTime(audio.duration);
      }
    });
    seek.addEventListener('input', function () {
      dragging = true;
      if (audio.duration) tm.textContent = fmtTime(seek.value / 1000 * audio.duration) + ' / ' + fmtTime(audio.duration);
    });
    seek.addEventListener('change', function () {
      if (audio.duration) audio.currentTime = seek.value / 1000 * audio.duration;
      dragging = false;
    });
    menuBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      menu.style.display = menu.style.display === 'none' ? '' : 'none';
    });
    document.addEventListener('click', function () { menu.style.display = 'none'; });
    dlMp3.addEventListener('click', function (e) {
      e.stopPropagation(); menu.style.display = 'none';
      if (isMp3Source) { dlWav.href = url; dlWav.download = filename; dlWav.click(); return; }
      mp3FromUrl(url, filename,
        function (bytes) { setStatus(i18n.t('web.mu_mp3_ready', "MP3 ready: {0} — check your Downloads", fmtSize(bytes)), 'ok'); },
        function (err) { setStatus(i18n.t('web.mu_mp3_failed', "MP3 conversion failed: {0}", err.message), 'err'); });
    });
    masterBtn.addEventListener('click', function (e) {
      e.stopPropagation(); menu.style.display = 'none';
      var m0 = Date.now();
      var legacyMaster = function () {
        setStatus(i18n.t('web.mu_mastering_file', "Mastering {0} (decode + normalize + limiter)...", filename), 'busy');
        masterTrack(url, function (msg) { setStatus(msg, 'busy'); }).then(function (rendered) {
          var blob = encodeWav16(rendered);
          var outName = filename.replace(/\.(wav|mp3)$/i, '') + '-mastered.wav';
          var a2 = document.createElement('a');
          a2.href = URL.createObjectURL(blob); a2.download = outName;
          document.body.appendChild(a2); a2.click();
          setTimeout(function () { URL.revokeObjectURL(a2.href); a2.remove(); }, 5000);
          setStatus(i18n.t('web.mu_mastered_download', "Mastered {0} in {1}s ({2}) — check your Downloads", outName, Math.round((Date.now() - m0) / 1000), fmtSize(blob.size)), 'ok');
        }).catch(function (err) {
          setStatus(i18n.t('web.mu_master_failed', "Master failed: {0}", (err && err.message ? err.message : err)), 'err');
        });
      };
      // Console-side mastering (v54): survives a closed page and lands in
      // the library. mp3 has no server decoder -> stays browser-side, and
      // an old app (404 on the route) falls back to the old in-page DSP.
      if (isMp3Source) { legacyMaster(); return; }
      setStatus(i18n.t('web.mastering_on_the_console_normalize_l',"Mastering on the console (normalize + limiter)..."), 'busy');
      api('/api/music/master', { file: filename }, 300000).then(function (j) {
        loadLibrary();
        setStatus(i18n.t('web.mu_mastered_library', "Mastered {0} in {1}s — saved to the library, play it right away", j.out, Math.round((Date.now() - m0) / 1000)), 'ok');
      }).catch(function (err) {
        var msg = err && err.message ? err.message : String(err);
        if (/HTTP 404/.test(msg)) { legacyMaster(); return; }
        setStatus(i18n.t('web.mu_master_failed', "Master failed: {0}", msg.slice(0, 160)), 'err');
      });
    });
    // PS5-side separation: htdemucs runs on the console's GPU through the
    // audio.cpp adapter — the phone only fires the request and may close.
    ps5StemsBtn.addEventListener('click', function (e) {
      e.stopPropagation(); menu.style.display = 'none';
      setStatus(i18n.t('web.stems_ps5_gpu_loading_htdemucs_on_th',"Stems (PS5 GPU): loading htdemucs on the console..."), 'busy');
      var t0 = Date.now();
      api('/api/settings/runtime', { audio_options: 'input_audio=' + AUDIO_DIR + filename }, 30000)
        .then(function () {
          setStatus(i18n.t('web.stems_ps5_gpu_separating_on_vulkan_y',"Stems (PS5 GPU): separating on Vulkan — you can close this page"), 'busy');
          return generateAndWait('htdemucs-q8_0', 'separate ' + filename, 900000);
        })
        .then(function (j) {
          var resp = String(j.response || j.error || JSON.stringify(j).slice(0, 400));
          if (j.error || resp.indexOf('Stems written') < 0) throw new Error(resp.slice(0, 240));
          var secs = Math.round((Date.now() - t0) / 1000);
          var paths = resp.match(/\/download0\/[^\s,]+\.wav/g) || [];
          loadLibrary();
          setStatus(i18n.t('web.mu_stems_done', "Stems done in {0}s — {1} files grouped under the track. Open the ✂ stems group and press ZIP to take them all.", secs, paths.length), 'ok');
        })
        .catch(function (err) {
          var msg = err && err.message ? err.message : String(err);
          var network = err instanceof TypeError || /fetch|network|abort|load failed/i.test(msg);
          if (!network) { setStatus(i18n.t('web.mu_stems_failed', "Stems (PS5 GPU) failed: {0}", msg.slice(0, 160)), 'err'); return; }
          setStatus(i18n.t('web.mu_stems_dropped', "Stems (PS5 GPU): connection dropped ({0}) - the console keeps working, waiting for the files to appear...", msg.slice(0, 60)), 'busy');
          var base = String(filename || '').replace(/\.(wav|mp3)$/i, '').replace(/_seed\d+$/i, '');
          waitForStems(base, function () {
            loadLibrary();
            setStatus(i18n.t('web.stems_ready_the_console_finished_des',"Stems ready - the console finished despite the dropped connection. Open the stems group under the track."), 'ok');
          }, function (why) {
            setStatus(i18n.t('web.mu_stems_failed_why', "Stems (PS5 GPU) failed: {0} ({1})", msg.slice(0, 120), why), 'err');
          });
        });
    });
    var api_ = { pause: function () { audio.pause(); } };
    return wrap;
  }



  // ---- library ----
  function fmtSize(b) { return b > 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB'; }
  function fmtDate(t) { try { return new Date(t * 1000).toLocaleString('en-GB'); } catch (e) { return ''; } }

  var stemAudios = [];
  function makeStemGroup(parentName, stems) {
    var det = document.createElement('details'); det.className = 'track-settings';
    var total = 0; stems.forEach(function (st) { total += st.size; });
    var sum = document.createElement('summary');
    sum.textContent = i18n.t('web.mu_stems_summary', "✂ {0} stems (htdemucs · PS5 GPU) — {1}", stems.length, fmtSize(total));
    det.appendChild(sum);
    var zip = document.createElement('a');
    zip.textContent = i18n.t('web.mu_zip_all_stems', "⬇ ZIP all {0} stems ({1})", stems.length, fmtSize(total));
    zip.href = '/api/music/stems.zip?track=' + encodeURIComponent(parentName);
    zip.setAttribute('download', parentName.replace(/\.(wav|mp3)$/i, '') + '-stems.zip');
    zip.style.cssText = 'display:block;margin:6px 0;padding:8px 10px;border:1px solid #4a5;' +
      'background:#1d3a24;border-radius:8px;color:#b6f0c0;text-decoration:none;font-weight:600;text-align:center';
    det.appendChild(zip);
    stems.forEach(function (st) {
      var r = document.createElement('div');
      r.style.cssText = 'display:flex;align-items:center;gap:8px;margin:4px 0';
      var pb = document.createElement('button'); pb.type = 'button'; pb.className = 'del';
      pb.textContent = '\u25b6';
      var au = null;
      pb.addEventListener('click', function () {
        if (au && !au.paused) { au.pause(); pb.textContent = '\u25b6'; return; }
        stemAudios.forEach(function (x) { x.a.pause(); x.b.textContent = '\u25b6'; });
        if (!au) {
          au = new Audio(AUDIO_DIR + encodeURIComponent(st.name));
          au.preload = 'none'; au.volume = getStoredVol() / 100;
          au.addEventListener('ended', function () { pb.textContent = '\u25b6'; });
        }
        au.play(); pb.textContent = '\u275a\u275a'; stemAudios.push({ a: au, b: pb });
      });
      var nm = document.createElement('span');
      nm.style.cssText = 'flex:1;font-size:12px;word-break:break-all'; nm.textContent = st.name;
      var dl = document.createElement('a'); dl.textContent = '\u2b07';
      dl.href = AUDIO_DIR + encodeURIComponent(st.name); dl.setAttribute('download', st.name);
      dl.style.cssText = 'text-decoration:none;padding:4px 8px;border:1px solid #556;border-radius:6px';
      var del = document.createElement('button'); del.type = 'button'; del.className = 'del';
      del.textContent = '\ud83d\uddd1';
      del.addEventListener('click', function () {
        if (!window.confirm(i18n.t('web.mu_delete_name', "Delete {0}?", st.name))) return;
        api('/api/music/file', { name: st.name }, 15000, 'DELETE').then(loadLibrary)
          .catch(function (e) { setStatus(i18n.t('web.mu_delete_failed', "Delete failed: {0}", e.message), 'err'); });
      });
      r.appendChild(pb); r.appendChild(nm); r.appendChild(dl); r.appendChild(del);
      det.appendChild(r);
    });
    return det;
  }

  function loadLibrary() {
    return api('/api/music/files').then(function (j) {
      trackFiles(j);
      playerAudios.forEach(function (audio) { audio.pause(); });
      playerAudios = [];
      stemAudios.forEach(function (player) { player.a.pause(); });
      stemAudios = [];
      activePlayer = null;
      var box = $('tracks');
      box.innerHTML = '';
      var files = j.files || [];
      if (!files.length) { box.innerHTML = '<div class="empty">No files yet.</div>'; }
      var byName = {}; files.forEach(function (f) { byName[f.name] = f; });
      var stemsBy = {}, tops = [];
      files.forEach(function (f) {
        var parent = f.meta && f.meta.stem_of;
        if (parent && parent !== f.name && byName[parent] && /\.(wav|mp3)$/i.test(f.name))
          (stemsBy[parent] = stemsBy[parent] || []).push(f);
        else tops.push(f);
      });
      tops.forEach(function (f) {
        var d = document.createElement('div'); d.className = 'track';
        var m = document.createElement('div'); m.className = 'meta';
        var n = document.createElement('span'); n.className = 'name'; n.textContent = f.name;
        var s = document.createElement('span');
        s.textContent = fmtSize(f.size) + ' · ' + fmtDate(f.mtime);
        m.appendChild(n); m.appendChild(s);
        d.appendChild(m);
        // settings card: how this track was made (v28 sidecar meta)
        if (f.meta && Object.keys(f.meta).length) {
          var det = document.createElement('details'); det.className = 'track-settings';
          var sum = document.createElement('summary');
          var bits = [];
          if (f.meta.style) bits.push(f.meta.style);
          if (f.meta.seed !== undefined) bits.push(i18n.t('web.mu_meta_seed', "seed {0}", f.meta.seed));
          if (f.meta.duration !== undefined) bits.push(f.meta.duration + 's');
          if (f.meta.cot) bits.push('cot=' + f.meta.cot);
          if (f.meta.abc_name) bits.push(i18n.t('web.mu_meta_abc', "ABC: {0}", f.meta.abc_name));
          sum.textContent = '\u2699 ' + (bits.join(' \u00b7 ') || i18n.t('web.mu_saved_settings', "saved settings"));
          det.appendChild(sum);
          if (f.meta.lyrics) {
            var pre = document.createElement('pre'); pre.className = 'track-lyrics';
            pre.textContent = f.meta.lyrics;
            det.appendChild(pre);
          }
          d.appendChild(det);
        }
        if (/\.(wav|mp3)$/i.test(f.name)) {
          d.appendChild(makePlayer(AUDIO_DIR + encodeURIComponent(f.name), f.name, /\.mp3$/i.test(f.name), f.meta));
        } else {
          var ext = document.createElement('div'); ext.className = 'path';
          ext.textContent = i18n.t('web.mu_abc_use_load', "ABC — use “Load into editor” to edit it");
          d.appendChild(ext);
        }
        var del = document.createElement('button');
        del.type = 'button'; del.className = 'del'; del.textContent = '🗑';
        del.title = i18n.t('web.mu_delete_name_title', "Delete {0}", f.name);
        del.addEventListener('click', function () {
          var kids = (stemsBy[f.name] || []).map(function (k) { return k.name; });
          if (!window.confirm(kids.length ? i18n.t('web.mu_delete_with_stems', "Delete {0} plus its {1} stems?", f.name, kids.length) : i18n.t('web.mu_delete_name', "Delete {0}?", f.name))) return;
          kids.concat([f.name]).reduce(function (pr, nm) {
            return pr.then(function () { return api('/api/music/file', { name: nm }, 15000, 'DELETE'); });
          }, Promise.resolve())
            .then(function () { delete knownFiles[f.name]; loadLibrary(); loadAbcSelect(); })
            .catch(function (e) { setStatus(i18n.t('web.mu_delete_failed', "Delete failed: {0}", e.message), 'err'); });
        });
        s.appendChild(document.createTextNode(' '));
        s.appendChild(del);
        if (stemsBy[f.name]) d.appendChild(makeStemGroup(f.name, stemsBy[f.name]));
        box.appendChild(d);
      });
    }).catch(function (e) { $('tracks').innerHTML = '<div class="empty">' + e.message + '</div>'; });
  }

  // ---- ABC save/load ----
  function loadAbcSelect() {
    return api('/api/music/files').then(function (j) {
      var sel = $('abc-files');
      sel.innerHTML = '';
      (j.files || []).filter(function (f) { return /\.abc$/i.test(f.name); }).forEach(function (f) {
        var o = document.createElement('option'); o.value = f.name; o.textContent = f.name;
        sel.appendChild(o);
      });
    }).catch(function () {});
  }

  $('abc-save').addEventListener('click', function () {
    var name = $('abc-name').value.trim() || 'studio.abc';
    if (!/\.abc$/i.test(name)) name += '.abc';
    setStatus(i18n.t('web.saving_abc',"Saving ABC..."), 'busy');
    api('/api/music/abc', { name: name, content: $('abc').value })
      .then(function () { $('abc-name').value = name; setStatus(i18n.t('web.mu_abc_saved', "ABC saved: {0}", name)); loadAbcSelect(); loadLibrary(); })
      .catch(function (e) { setStatus(i18n.t('web.mu_saving_abc_failed', "Saving ABC failed: {0}", e.message), 'err'); });
  });

  $('abc-open').addEventListener('click', function () { $('abc-disk').click(); });
  $('abc-disk').addEventListener('change', function (ev) {
    var f = ev.target.files && ev.target.files[0];
    if (!f) return;
    var reader = new FileReader();
    reader.onload = function () {
      $('abc').value = reader.result;
      var base = f.name.split(/[\\/]/).pop().replace(/[^A-Za-z0-9._-]/g, '_');
      if (!/\.abc$/i.test(base)) base += '.abc';
      $('abc-name').value = base;
      setStatus(i18n.t('web.mu_file_loaded', "File {0} loaded into editor — press GENERATE (auto-saves)", f.name), 'ok');
    };
    reader.readAsText(f);
    ev.target.value = '';
  });
  $('abc-clear').addEventListener('click', function () {
    $('abc').value = '';
    setStatus(i18n.t('web.editor_cleared_no_score_at_generate',"Editor cleared — no score at GENERATE"), 'ok');
  });

  $('abc-load').addEventListener('click', function () {
    var name = ($('abc-files').value || $('abc-name').value || '').trim();
    if (!name) { setStatus(i18n.t('web.missing_abc_file_name',"Missing ABC file name"), 'err'); return; }
    fetch(AUDIO_DIR + encodeURIComponent(name)).then(function (r) {
      if (!r.ok) throw new Error(i18n.t('web.mu_http_status', "HTTP {0}", r.status));
      return r.text();
    }).then(function (t) { $('abc').value = t; $('abc-name').value = name; setStatus(i18n.t('web.mu_loaded_name', "Loaded: {0}", name)); })
      .catch(function (e) { setStatus(i18n.t('web.mu_load_failed', "Load failed: {0}", e.message), 'err'); });
  });

  // ---- generate ----
  var knownFiles = {};
  function trackFiles(j) { (j.files || []).forEach(function (f) { knownFiles[f.name] = true; }); }

  // single-run and queue-run share this; ov = { seed, status(msg,cls) }
  function generateOnce(ov) {
    ov = ov || {};
    var say = ov.status || setStatus;
    var queuedModelId = Object.prototype.hasOwnProperty.call(ov, 'model') ? ov.model : undefined;
    return window.ProsperoWorkspace.withInstalledMusicModel(
      musicModels, $('model').value, queuedModelId,
      function (model) { return generateOnceWithModel(ov, model.id); }
    ).catch(function (error) {
      if (error && error.code === 'MUSIC_MODEL_UNAVAILABLE') say(error.message, 'err');
      throw error;
    });
  }

  function generateOnceWithModel(ov, modelId) {
    ov = ov || {};
    var say = ov.status || setStatus;
    // Fail fast on inputs the console is known to reject silently after
    // burning two minutes of model load (validated 20.10: empty style ->
    // "generation failed" with zero adapter logs).
    if (modelId === 'sheetsage2-orig') {
      if (!$('abc').value.trim()) { say(i18n.t('web.mu_paste_abc_first', "Paste an ABC score first (or use Cover + ABC)."), 'err'); return Promise.resolve(); }
    } else if (!($('style').value || '').trim()) {
      say(i18n.t('web.mu_write_style_first', "Write a style prompt first (e.g. \"romanian pop, female vocal\")."), 'err'); return Promise.resolve();
    } else if (!($('lyrics').value || '').trim() && !($('abc').value || '').trim()) {
      say(i18n.t('web.mu_write_lyrics_first', "Write lyrics (or load an ABC score) before generating."), 'err'); return Promise.resolve();
    }
    var seed = (ov.seed != null) ? ov.seed : (parseInt($('seed').value, 10) || 42);
    var t0 = Date.now();
    var timer = setInterval(function () {
      say(i18n.t('web.mu_running', "Running... {0}s (model load + generation can take 4-6 min)", Math.round((Date.now() - t0) / 1000)), 'busy');
    }, 1000);
    var abcName = $('abc-name').value.trim();
    if (abcName && !/\.abc$/i.test(abcName)) abcName += '.abc';
    var abcContent = $('abc').value.trim();
    var doGenerate = function (effectiveName) {
      say(i18n.t('web.mu_sending_settings', "Sending settings..."), 'busy');
      var body = {
        audio_lyrics: $('lyrics').value,
        audio_duration_sec: parseInt($('duration').value, 10) || 240,
        audio_seed: seed,
        // always sent: empty = CLEAR inherited options (otherwise an abc_file
        // from an earlier session stays in runtime settings and the engine sees it)
        audio_options: buildOptions(effectiveName)
      };
      return api('/api/settings/runtime', body).then(function () {
        say(i18n.t('web.mu_generating_dots', "Generating..."), 'busy');
        return generateAndWait(modelId, $('style').value, 2400000);
      });
    };
    // ABC: non-empty editor => auto-save and use the file; empty editor =>
    // only names that already exist on the PS5; otherwise no abc_file at all.
    var pre = Promise.resolve('');
    if (abcName && abcContent) {
      say(i18n.t('web.mu_autosaving_abc', "Auto-saving ABC..."), 'busy');
      pre = api('/api/music/abc', { name: abcName, content: $('abc').value })
        .then(function () { knownFiles[abcName] = true; return abcName; });
    } else if (abcName && knownFiles[abcName]) {
      pre = Promise.resolve(abcName);
    } else if (abcName) {
      say(i18n.t('web.mu_abc_missing', "ABC \"{0}\" missing on PS5 and the editor is empty — generating without a score", abcName), 'busy');
    }
    return pre.then(doGenerate).then(function (j) {
      clearInterval(timer);
      var resp = String(j.response || j.error || JSON.stringify(j).slice(0, 300));
      if (/NEEDS_RESTART|backend buffer allocation failed|failed to allocate Yue2 NAR/.test(resp)) {
          throw new Error(i18n.t('web.not_enough_usable_memory_wait_for_ot',"Not enough usable memory. Wait for other work to finish, then close and reopen the app before retrying."));
        }
      var m = resp.match(/\/download0\/\S+?\.(?:wav|mp3)/);
      if (/NEEDS_RESTART|backend buffer allocation failed|failed to allocate Yue2 NAR/.test(resp)) {
          throw new Error(i18n.t('web.not_enough_usable_memory_wait_for_ot',"Not enough usable memory. Wait for other work to finish, then close and reopen the app before retrying."));
        }
      var res = $('result');
      res.innerHTML = '';
      if (m) {
        var url = m[0].replace(/[",).]+$/, '');
        var finish = function (finalUrl) {
          var res = $('result');
          res.innerHTML = '';
          var fname = (finalUrl.match(/[^/]+(\?|$)/) || ['track.wav'])[0];
          // snapshot the exact settings so the library menu can offer Cover later
          var meta = {
            lyrics: $('lyrics').value,
            style: $('style').value,
            seed: seed,
            duration: parseInt($('duration').value, 10) || 240,
            model: modelId,
            cot: $('cot').value,
            abc_name: $('abc-name').value.trim(),
            created: Math.floor(Date.now() / 1000)
          };
          var abcText = $('abc').value;
          if (abcText.trim() && abcText.length <= 60000) meta.abc_content = abcText;
          res.appendChild(makePlayer(finalUrl, fname, /\.mp3$/i.test(finalUrl), meta));
          var p = document.createElement('div'); p.className = 'path'; p.textContent = finalUrl;
          res.appendChild(p);
          say(i18n.t('web.mu_done_in', "Done in {0}s 🎵", Math.round((Date.now() - t0) / 1000)));
          api('/api/music/meta', { name: fname, meta: meta })
            .then(loadLibrary).catch(loadLibrary);
          return { url: finalUrl, fname: fname };
        };
        var nice = $('track-name').value.trim();
        if (nice) {
          var src = url.substring(url.lastIndexOf('/') + 1);
          var ext = src.substring(src.lastIndexOf('.'));
          var safe = nice.toLowerCase().replace(/[ăâîșțşţ]/g, function (c) {
            return { 'ă': 'a', 'â': 'a', 'î': 'i', 'ș': 's', 'ş': 's', 'ț': 't', 'ţ': 't' }[c];
          }).replace(/[^a-z0-9._-]+/g, '-').replace(/^[-.]+|[-.]+$/g, '');
          if (!safe) safe = 'piesa';
          var dst = safe + '_seed' + seed + ext;
          return api('/api/music/rename', { src: src, dst: dst }).then(function () {
            return finish(url.replace(src, dst));
          }).catch(function () {
            setStatus(i18n.t('web.mu_track_ready_name_exists', "Track is ready, but \"{0}\" already exists — kept the original name", dst), 'err');
            return finish(url);
          });
        } else {
          return finish(url);
        }
      } else {
        var res2 = $('result');
        res2.innerHTML = '';
        var p2 = document.createElement('div'); p2.className = 'path'; p2.textContent = resp;
        res2.appendChild(p2);
        say(i18n.t('web.mu_generation_failed', "Generation failed: {0}", (resp.trim() ? resp.trim().slice(0, 160) : i18n.t('web.mu_empty_response', "empty response — console log has the details"))), 'err');
        return { url: '', fname: '' };
      }
    }).catch(function (e) {
      clearInterval(timer);
      say(i18n.t('web.mu_failed', "Failed: {0}", e.message), 'err');
      throw e;
    });
  }

  function runGeneration() {
    var btn = $('generate');
    if (btn.disabled) return;
    btn.disabled = true;
    generateOnce().catch(function () {}).then(function () { btn.disabled = false; });
  }
  $('generate').addEventListener('click', runGeneration);

  // ---- cover (audio -> ABC via SheetSage2) ----
  // The browser decodes any browser-supported audio (MP3/WAV/OGG/M4A) to
  // 16 kHz mono PCM16 with Web Audio, uploads it in <=200 KiB base64 parts
  // (the console's request buffer is 256 KiB), then runs the sheetsage2
  // model through the standard /api/generate path. The score artifact is
  // loaded straight into the ABC editor.
  $('cover-go').addEventListener('click', function () {
    var file = $('cover-file').files && $('cover-file').files[0];
    var btn = $('cover-go');
    if (!file || btn.disabled) return;
    btn.disabled = true;
    var t0 = Date.now();
    var cstatus = function (m, c) {
      var el = $('cover-status');
      el.textContent = m || '';
      el.className = c || '';
    };
    var wav = null, name = '';
    var rate = 16000;
    var AC = window.AudioContext || window.webkitAudioContext;
    var OfflineAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    cstatus(i18n.t('web.mu_decoding', "Decoding {0}...", file.name), 'busy');
    file.arrayBuffer().then(function (buf) {
      if (!AC || !AC.prototype.decodeAudioData)
        throw new Error(i18n.t('web.this_browser_has_no_web_audio_decode',"this browser has no Web Audio decoder"));
      var ctx = new AC();
      var p = ctx.decodeAudioData(buf);
      return p.then(function (ab) { return ab; });
    }).then(function (ab) {
      var frames = Math.max(1, Math.ceil(ab.duration * rate));
      if (frames > 300 * rate) throw new Error(i18n.t('web.audio_longer_than_5_min_trim_it_firs',"audio longer than 5 min — trim it first"));
      if (!OfflineAC) throw new Error(i18n.t('web.this_browser_has_no_offline_web_audi',"this browser has no offline Web Audio renderer"));
      var off = new OfflineAC(1, frames, rate);
      var src = off.createBufferSource();
      src.buffer = ab;
      src.connect(off.destination);
      src.start();
      return off.startRendering();
    }).then(function (rendered) {
      var pcm = rendered.getChannelData(0);
      var view = new DataView(new ArrayBuffer(44 + pcm.length * 2));
      var ws = function (o, s) { for (var i = 0; i < s.length; i++) view.setUint8(o + i, s.charCodeAt(i)); };
      ws(0, 'RIFF'); view.setUint32(4, 36 + pcm.length * 2, true);
      ws(8, 'WAVE'); ws(12, 'fmt ');
      view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
      view.setUint32(24, rate, true); view.setUint32(28, rate * 2, true);
      view.setUint16(32, 2, true); view.setUint16(34, 16, true);
      ws(36, 'data'); view.setUint32(40, pcm.length * 2, true);
      for (var i = 0; i < pcm.length; i++) {
        var v = Math.max(-1, Math.min(1, pcm[i]));
        view.setInt16(44 + i * 2, v < 0 ? v * 32768 : v * 32767, true);
      }
      wav = new Uint8Array(view.buffer);
      var base = file.name.split(/[\\/]/).pop().replace(/\.[^.]+$/, '');
      base = (base.replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 60) || 'cover').toLowerCase();
      name = base + '-src.wav';
      var CHUNK = 140 * 1024;
      var parts = Math.max(1, Math.ceil(wav.length / CHUNK));
      var part = 0;
      var b64 = function (u8) {
        var s = '', CH = 0x8000;
        for (var i = 0; i < u8.length; i += CH)
          s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + CH, u8.length)));
        return btoa(s);
      };
      var next = function () {
        if (part >= parts) return Promise.resolve();
        cstatus(i18n.t('web.mu_uploading_part', "Uploading {0}: part {1}/{2} ({3}%)", name, part + 1, parts, Math.round(100 * part / parts)), 'busy');
        var slice = wav.subarray(part * CHUNK, Math.min((part + 1) * CHUNK, wav.length));
        return api('/api/music/upload', { name: name, part: part, data: b64(slice) }, 120000)
          .then(function () { part++; return next(); });
      };
      return next();
    }).then(function () {
      cstatus(i18n.t('web.mu_transcribing_sheetsage', "Transcribing with SheetSage2 (first run loads the 2.7 GB model — be patient)..."),
        'busy');
      return api('/api/settings/runtime',
        { audio_options: 'input_audio=' + AUDIO_DIR + name }, 30000)
        .then(function () {
          return generateAndWait('sheetsage2-orig', 'transcribe ' + name, 900000);
        });
    }).then(function (j) {
      var resp = String(j.response || j.error || JSON.stringify(j).slice(0, 400));
      var m = resp.match(/\/download0\/\S+?\.abc/);
      if (!m) throw new Error(i18n.t('web.mu_no_abc_artifact', "no ABC artifact in response: {0}", resp.slice(0, 220)));
      var path = m[0].replace(/["',).]+$/, '');
      var fname = path.substring(path.lastIndexOf('/') + 1);
      return fetch(AUDIO_DIR + encodeURIComponent(fname)).then(function (r) {
        if (!r.ok) throw new Error(i18n.t('web.mu_fetch_score_http', "fetch score HTTP {0}", r.status));
        return r.text();
      }).then(function (t) {
        $('abc').value = t;
        $('abc-name').value = name.replace(/-src\.wav$/, '-cover.abc');
        cstatus(i18n.t('web.mu_cover_done', "Cover done in {0}s — score loaded into the editor ✅", Math.round((Date.now() - t0) / 1000)), 'ok');
        setStatus(i18n.t('web.mu_score_loaded', "Score {0} loaded — press GENERATE to make it a song", fname), 'ok');
        loadLibrary(); loadAbcSelect();
      });
    }).catch(function (e) {
      cstatus(i18n.t('web.mu_cover_failed', "Cover failed: {0}", (e && e.message ? e.message : e)), 'err');
    }).then(function () {
      $('cover-file').value = '';
      btn.disabled = false;
    });
  });
  $('regen').addEventListener('click', function () {
    var s = $('seed');
    s.value = String(Math.min(4294967295, (parseInt(s.value, 10) || 42) + 1));
    runGeneration();
  });

  // ---- upload a local audio file straight into the console library ----
  $('lib-save').addEventListener('click', function () {
    var file = $('cover-file').files && $('cover-file').files[0];
    var btn = $('lib-save');
    if (!file || btn.disabled) { cstat(i18n.t('web.mu_pick_file_first', "Pick a file first"), 'err'); return; }
    var ext = (file.name.match(/\.(wav|mp3)$/i) || [''])[0].toLowerCase();
    if (!ext) { cstat(i18n.t('web.mu_upload_wav_mp3', "Library upload accepts plain .wav or .mp3 files"), 'err'); return; }
    if (file.size > 64 * 1024 * 1024) { cstat(i18n.t('web.mu_file_over_cap', "File is over the 64 MiB library cap"), 'err'); return; }
    var safe = file.name.replace(/[^A-Za-z0-9._-]/g, '_').replace(/^[-.]+|[-.]+$/g, '')
      .replace(/\.wav$/i, '.wav').replace(/\.mp3$/i, '.mp3');
    if (safe.length < 5) safe = 'upload_' + Date.now() + ext;
    btn.disabled = true;
    var t0 = Date.now();
    cstat(i18n.t('web.mu_uploading_mb', "Uploading {0} ({1} MB)...", safe, Math.round(file.size / 104857.6) / 10), 'busy');
    file.arrayBuffer()
      .then(function (buf) { return uploadBinary(safe, buf); })
      .then(function () {
        cstat(i18n.t('web.mu_saved_library', "Saved {0} to the library in {1}s ✅", safe, Math.round((Date.now() - t0) / 1000)), 'ok');
        $('cover-file').value = '';
        loadLibrary();
      })
      .catch(function (e) {
        cstat(e && e.missing ? i18n.t('web.mu_no_upload', "This build has no library upload — flash the newest eboot")
          : i18n.t('web.mu_upload_failed', "Upload failed: {0}", (e && e.message ? e.message : e)), 'err');
      })
      .then(function () { btn.disabled = false; });
  });
  function cstat(m, c) {
    var el = $('cover-status');
    el.textContent = m || '';
    el.className = c || '';
  }

  // ---- misc ----
  $('model').addEventListener('change', syncAudioDurationVisibility);
  $('duration').addEventListener('input', function () { $('dur-val').textContent = $('duration').value; });
  $('dice').addEventListener('click', function () {
    $('seed').value = 1 + Math.floor(Math.random() * 4294967294);
  });
  $('refresh').addEventListener('click', function () { loadLibrary(); loadAbcSelect(); });

  // ---- form persistence (survives refresh; lyrics/style/abc/opts all kept) ----
  var FORM_KEY = 'prospero-music-form-v1';
  var FORM_IDS = ['lyrics', 'style', 'abc', 'abc-name', 'cot', 'abc-max', 'duration', 'seed', 'track-name'];
  function restoreForm() {
    try {
      var saved = JSON.parse(localStorage.getItem(FORM_KEY) || '{}');
      FORM_IDS.forEach(function (id) {
        var el = $(id);
        if (el && saved[id] !== undefined && saved[id] !== null) el.value = saved[id];
      });
      $('dur-val').textContent = $('duration').value;
    } catch (e) {}
  }
  var formSaveT = 0;
  function formSaveNow() {
    var o = {};
    FORM_IDS.forEach(function (id) { var el = $(id); if (el) o[id] = el.value; });
    try { localStorage.setItem(FORM_KEY, JSON.stringify(o)); } catch (e) {}
  }
  FORM_IDS.forEach(function (id) {
    var el = $(id);
    if (!el) return;
    el.addEventListener('input', function () {
      clearTimeout(formSaveT); formSaveT = setTimeout(formSaveNow, 300);
    });
  });

  // ---- multi-seed queue (browser-orchestrated; survives reload via localStorage) ----
  var QKEY = 'prospero-music-queue-v1';
  var queue = [], queueRunning = false;
  function qload() {
    try { queue = JSON.parse(localStorage.getItem(QKEY) || '[]'); } catch (e) { queue = []; }
    if (!Array.isArray(queue)) queue = [];
    // The server job survives reload. Never blindly submit the same item twice.
    queue.forEach(function (it) { if (it.status === 'running') { it.status = 'failed'; it.note = i18n.t('web.mu_check_jobs_retry', "Check running jobs and the library before retrying"); } });
  }
  function qsave() { try { localStorage.setItem(QKEY, JSON.stringify(queue)); } catch (e) {} }
  function qparams() {
    return {
      lyrics: $('lyrics').value, style: $('style').value,
      duration: $('duration').value, cot: $('cot').value,
      abc: $('abc').value, abc_name: $('abc-name').value,
      track_name: $('track-name').value, model: $('model').value
    };
  }
  function qapply(p) {
    $('lyrics').value = p.lyrics; $('style').value = p.style;
    $('duration').value = p.duration; $('dur-val').textContent = p.duration;
    $('cot').value = p.cot; $('abc').value = p.abc;
    $('abc-name').value = p.abc_name; $('track-name').value = p.track_name;
    if (p.model && $('model').value !== p.model) {
      for (var i = 0; i < $('model').options.length; i++)
        if ($('model').options[i].value === p.model) { $('model').value = p.model; break; }
    }
    syncAudioDurationVisibility();
  }
  var QICON = { pending: '·', running: '▶', done: '✅', failed: '✕' };
  function renderQueue() {
    var el = $('queue-log');
    if (!queue.length) { el.textContent = ''; return; }
    var done = 0, fail = 0;
    var lines = queue.map(function (it, i) {
      if (it.status === 'done') done++;
      if (it.status === 'failed') fail++;
      return (QICON[it.status] || '?') + ' ' + (i + 1) + '. ' + i18n.t('web.mu_meta_seed', "seed {0}", it.seed) +
        (it.note ? ' — ' + it.note : '');
    });
    el.textContent = i18n.t('web.mu_queue_summary', "Queue {0}✓ {1}✕ / {2}", done, fail, queue.length) +
      (queueRunning ? i18n.t('web.mu_running_tag', " · RUNNING") : '') + '\n' + lines.join('\n');
  }
  function qdelay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  function runQueue() {
    if (queueRunning) return;
    var pend = queue.filter(function (it) { return it.status === 'pending'; });
    if (!pend.length) { setStatus(i18n.t('web.queue_is_empty_add_variants_with_que',"Queue is empty — add variants with \"+ Queue\" first"), 'err'); return; }
    if (queue.some(function (it) { return it.status === 'running'; })) return;
    queueRunning = true;
    $('q-run').disabled = true;
    var idx = 0, total = pend.length;
    var chain = Promise.resolve();
    pend.forEach(function (item) {
      chain = chain.then(function () {
        if (!queueRunning) throw new Error(i18n.t('web.queue_stopped',"queue stopped"));
        idx++;
        var label = i18n.t('web.mu_queue_label', "Queue {0}/{1} · seed {2}", idx, total, item.seed);
        // first item runs on the current session; later ones get a clean
        // title (RADV fragmentation guard), the checkbox decides
        var step = Promise.resolve();
        return step.then(function () {
          item.status = 'running'; item.note = ''; qsave(); renderQueue();
          qapply(item.params);
          return generateOnce({
            model: item.params.model || '',
            seed: item.seed,
            status: function (m, c) { setStatus(label + ' · ' + (m || ''), c); }
          }).then(function (r) { item.status = 'done'; item.note = r.fname || 'ok'; },
                  function (e) { item.status = 'failed'; item.note = String(e && e.message || e).slice(0, 140); });
        });
      });
    });
    chain.catch(function (e) { setStatus(i18n.t('web.mu_queue_stopped_msg', "Queue stopped: {0}", e.message), 'err'); })
      .then(function () {
        queueRunning = false; $('q-run').disabled = false;
        qsave(); renderQueue();
        var ok = queue.filter(function (i) { return i.status === 'done'; }).length;
        setStatus(i18n.t('web.mu_queue_finished', "Queue finished: {0}/{1} tracks in library 🎵", ok, queue.length), 'ok');
      });
  }
  $('q-add').addEventListener('click', function () {
    var n = Math.max(1, Math.min(20, parseInt($('q-count').value, 10) || 3));
    var start = parseInt($('seed').value, 10) || 42;
    for (var i = 0; i < n; i++)
      queue.push({ id: Date.now() + ':' + i, seed: Math.min(4294967295, start + i), params: qparams(), status: 'pending' });
    formSaveNow(); qsave(); renderQueue();
    setStatus(i18n.t('web.mu_queued_variants', "Queued {0} variants (seeds {1}–{2}) — press ▶▶ Run queue", n, start, (start + n - 1)), 'ok');
  });
  $('q-run').addEventListener('click', runQueue);
  $('q-clear').addEventListener('click', function () {
    if (queueRunning) { queueRunning = false; setStatus(i18n.t('web.stopping_queue_after_the_current_tra',"Stopping queue after the current track…"), 'busy'); return; }
    queue = []; qsave(); renderQueue();
  });
  qload(); renderQueue();


  // ---- Enhance + Templates (ported from MLX Serve's rewrite flow) ----
  // Enhance rescrie draft-ul cu chat-modelul local (MiniCPM5) folosind
  // prompt-urile de sistem exacte din mlx-serve (varianta yue2). Templates
  // = catalog built-in (exemplele mlx-serve) + salvare localStorage.
  var stagedChatModel = '';
  function loadChatModels() {
    var sel = $('chat-model');
    if (!sel || sel.dataset.bound) return;
    sel.dataset.bound = '1';
    api('/v1/models', undefined, 15000).then(function (d) {
      var models = (d.data || []).filter(function (model) {
        return typeof model.id === 'string' && model.purpose === 'text-to-text';
      });
      sel.innerHTML = '';
      var placeholder = document.createElement('option');
      placeholder.value = '';
      placeholder.textContent = models.length ? i18n.t('web.choose_a_text_model',"Choose a text model…") : i18n.t('web.no_installed_text_models',"No installed text models");
      sel.appendChild(placeholder);
      models.forEach(function (m) {
        var o = document.createElement('option');
        o.value = m.id;
        o.textContent = m.name || m.id;
        sel.appendChild(o);
      });
      return api('/api/models/selection', undefined, 15000).then(function (s) {
        if (s && models.some(function (model) { return model.id === s.id; }))
          sel.value = stagedChatModel = s.id;
      });
    }).then(function () {
      sel.addEventListener('change', function () {
        stagedChatModel = sel.value;
      });
    }).catch(function () { sel.hidden = true; });
  }
  var ENH_STYLE_SYS = 'You rewrite music style prompts for a text-to-music model. ' +
    'Write ONE line of comma-separated tags like the examples: language or genre first, ' +
    'then instruments, mood and the lead vocal. No sentences, no tempo or key (the score ' +
    'carries those). Keep the user\'s intent; make it more specific and evocative. Reply ' +
    'with ONLY the rewritten prompt, no preamble, no quotes, no markdown.\n\n' +
    'Examples of the expected format:\n\n' +
    'Example 1 \u2014 City pop:\nCity Pop, upbeat, danceable, groovy bass, electric guitar, synth, energetic, joyful, neon city night\n\n' +
    'Example 2 \u2014 Jazz-funk:\nJazz-funk, warm lead vocal, Rhodes piano, electric bass, tight drums\n\n' +
    'Example 3 \u2014 Jazz ballad:\nJazz, expressive lead vocal, piano, tenor saxophone, upright bass, brushed drums, no guitar, spacious modern harmony\n\n' +
    'Example 4 \u2014 Indie pop:\nEnglish, indie pop, bright acoustic guitar, soft drums, warm lead vocal';
  var ENH_LYRICS_SYS = 'You write song lyrics for a text-to-music model. Always write in the SAME language as the user\'s draft. Use section tags ' +
    'on their own lines, exactly like the examples: [Intro] [Verse] [Pre-Chorus] [Chorus] ' +
    '[Interlude] [Bridge] [Outro]. Keep the user\'s theme and any lines they wrote; tighten ' +
    'rhythm and rhyme. Reply with ONLY the lyrics, no title, no preamble, no quotes, no ' +
    'markdown.\n\nExamples of the expected format:\n\n' +
    'Example 1 \u2014 Short song (English):\n[Intro]\n\n[Verse]\nSoft morning light is touching the window\n' +
    'Coffee on the table and the radio low\nI hear your footsteps coming down the hall\n' +
    'Every little moment feels like it is all\n\n[Chorus]\nStay with the rhythm, let it carry us home\n' +
    'We are not alone, we are not alone\nSing it to the sky and it will sing right back\n' +
    'Stay with the rhythm, never look back\n\n[Outro]\nStay with the rhythm, let it carry us home';
  var TPL_BUILTIN = {
    style: [
      { title: i18n.t('web.city_pop',"City pop"), body: 'City Pop, upbeat, danceable, groovy bass, electric guitar, synth, energetic, joyful, neon city night' },
      { title: i18n.t('web.jazz_funk',"Jazz-funk"), body: 'Jazz-funk, warm lead vocal, Rhodes piano, electric bass, tight drums' },
      { title: i18n.t('web.jazz_ballad',"Jazz ballad"), body: 'Jazz, expressive lead vocal, piano, tenor saxophone, upright bass, brushed drums, no guitar, spacious modern harmony' },
      { title: i18n.t('web.indie_pop',"Indie pop"), body: 'English, indie pop, bright acoustic guitar, soft drums, warm lead vocal' },
      { title: i18n.t('web.romanian_pop',"Romanian pop"), body: 'Romanian pop, warm female vocal, synth pads, deep bass, tight drums, sunny summer mood' },
      { title: i18n.t('web.manele_modern',"Manele modern"), body: 'Romanian manele, oriental scales, accordion, synth brass, emotional male vocal, deep bass' },
      { title: i18n.t('web.rock',"Rock"), body: 'Romanian rock, energetic electric guitar, driving drums, gritty male vocal, anthemic chorus' }
    ],
    lyrics: [
      { title: i18n.t('web.song_skeleton',"Song skeleton"), body: '[Intro]\n\n[Verse]\n\n\n[Chorus]\n\n\n[Verse]\n\n\n[Chorus]\n\n\n[Bridge]\n\n\n[Outro]\n' }
    ]
  };
  var TPL_KEY = { style: 'prospero-tpl-style', lyrics: 'prospero-tpl-lyrics' };
  function tplLoad(field) {
    try { return JSON.parse(localStorage.getItem(TPL_KEY[field]) || '[]'); } catch (e) { return []; }
  }
  function tplSave(field, list) {
    try { localStorage.setItem(TPL_KEY[field], JSON.stringify(list)); } catch (e) {}
  }
  function enhRow(field) {
    var anchor = $(field);
    var row = document.createElement('div');
    row.className = 'enh-row';
    row.innerHTML = '<button id="enh-' + field + '" type="button"></button>' +
      '<button id="tpl-' + field + '" type="button"></button>' +
      '<select id="tplmenu-' + field + '" hidden></select>' +
      '<span id="enh-' + field + '-status" class="enh-status"></span>';
    row.querySelector('#enh-' + field).textContent = i18n.t('web.mu_enhance', "✨ Enhance");
    row.querySelector('#tpl-' + field).textContent = i18n.t('web.mu_templates', "📚 Templates");
    row.querySelector('#tpl-' + field).title = i18n.t('web.mu_templates_title', "templates");
    row.querySelector('#tplmenu-' + field).title = i18n.t('web.mu_pick_template_title', "pick a template");
    anchor.parentNode.insertBefore(row, anchor.nextSibling);
    return row;
  }
  function enhStatus(field, msg, busy) {
    var el = $('enh-' + field + '-status');
    el.textContent = msg || '';
    el.style.opacity = busy ? '.9' : '.7';
  }
  function stripWrap(s) {
    return String(s || '').replace(/^\s*```[a-z]*\s*|\s*```\s*$/g, '')
      .replace(/^["'\u201e\u201c]+|["'\u201d]+$/g, '').trim();
  }
  function enhance(field) {
    var el = $(field), draft = el.value.trim();
    if (!draft) { enhStatus(field, 'Write something first.'); return; }
    var btn = $('enh-' + field), tplBtn = $('tpl-' + field);
    btn.disabled = tplBtn.disabled = true;
    var cSel = $('chat-model');
    var chatModel = stagedChatModel || (cSel && cSel.value) || '';
    if (!chatModel) {
      enhStatus(field, 'Choose an installed text model for Enhance.');
      btn.disabled = tplBtn.disabled = false;
      return;
    }
    enhStatus(field, i18n.t('web.mu_rewriting_with', "Rewriting with {0} (first call may load the chat model, ~1 min)…", ((cSel && cSel.selectedIndex > -1) ? cSel.options[cSel.selectedIndex].text : chatModel)), true);
    var sys, user, predict;
    if (field === 'style') {
      sys = ENH_STYLE_SYS;
      user = 'Rewrite this style prompt:\n\n' + draft;
      var lyr = $('lyrics').value.trim();
      user += lyr ? '\n\nIt will sing these lyrics (for mood and language):\n' + lyr
                  : '\n\nThe track is instrumental: no vocals.';
      predict = 150;
    } else {
      sys = ENH_LYRICS_SYS;
      var st = $('style').value.trim();
      var lang = /[\u021b\u0219\u0222\u0218\u0227\u0103\u00ee]/i.test(draft) || /romanian|rom\u00e2n/i.test(st + ' ' + draft)
        ? 'Romanian' : 'the same language they are written in';
      user = 'Rewrite these lyrics in ' + lang + ':\n\n' + draft;
      if (st) user += '\n\nThe music style is:\n' + st;
      predict = 900;
    }
    api('/api/chat', { model: chatModel || undefined, unload_after: true, messages: [
      { role: 'system', content: sys }, { role: 'user', content: user }
    ], num_predict: predict, temperature: 0.2 }, 240000).then(function (j) {
      var out = stripWrap(j && j.message ? j.message.content : '');
      if (j && j.error && !out) throw new Error(String(j.error).slice(0, 120));
      if (!out) throw new Error(i18n.t('web.empty_rewrite_try_again',"empty rewrite, try again"));
      el.value = out;
      el.dispatchEvent(new Event('input'));
      enhStatus(field, i18n.t('web.mu_rewritten_in', "✓ rewritten in {0}", ((j.elapsed_ms || 0) ? (j.elapsed_ms / 1000).toFixed(0) + 's' : i18n.t('web.mu_a_moment', "a moment"))));
    }).catch(function (e) {
      enhStatus(field, '\u2717 ' + (e.name === 'AbortError' ? i18n.t('web.mu_timeout', "timeout") : String(e.message || e).slice(0, 140)));
    }).then(function () { btn.disabled = tplBtn.disabled = false; });
  }
  function refreshTplMenu(field) {
    var sel = $('tplmenu-' + field);
    sel.innerHTML = '';
    var og1 = document.createElement('optgroup'); og1.label = i18n.t('web.built_in',"Built-in");
    TPL_BUILTIN[field].forEach(function (t, i) {
      var o = document.createElement('option');
      o.value = 'b:' + i; o.textContent = t.title; og1.appendChild(o);
    });
    sel.appendChild(og1);
    var saved = tplLoad(field);
    if (saved.length) {
      var og2 = document.createElement('optgroup'); og2.label = i18n.t('web.saved',"Saved");
      saved.forEach(function (t, i) {
        var o = document.createElement('option');
        o.value = 's:' + i; o.textContent = t.title; og2.appendChild(o);
      });
      sel.appendChild(og2);
    }
    var og3 = document.createElement('optgroup'); og3.label = '\u2014';
    var os = document.createElement('option'); os.value = 'save'; os.textContent = i18n.t('web.save_current_as',"💾 Save current as…"); og3.appendChild(os);
    if (saved.length) {
      var od = document.createElement('option'); od.value = 'del'; od.textContent = i18n.t('web.delete_saved',"🗑 Delete saved…"); og3.appendChild(od);
    }
    sel.appendChild(og3);
    var osel = document.createElement('option');
    osel.value = ''; osel.textContent = i18n.t('web.pick_a_template',"Pick a template…"); osel.selected = true;
    sel.insertBefore(osel, sel.firstChild);
  }
  function tplMenuAction(field) {
    var sel = $('tplmenu-' + field), v = sel.value;
    if (!v) return;
    var el = $(field);
    if (v === 'save') {
      var title = (window.prompt(i18n.t('web.template_name',"Template name:"), el.value.trim().split('\n')[0].slice(0, 40)) || '').trim();
      if (title && el.value.trim()) {
        var list = tplLoad(field).filter(function (t) { return t.title !== title; });
        list.unshift({ title: title, body: el.value });
        tplSave(field, list);
        enhStatus(field, i18n.t('web.mu_tpl_saved', "✓ saved “{0}”", title));
      }
    } else if (v === 'del') {
      var saved2 = tplLoad(field);
      var name = (window.prompt(i18n.t('web.delete_which_saved_template_type_its',"Delete which saved template? Type its exact name:"), '') || '').trim();
      var keep = saved2.filter(function (t) { return t.title !== name; });
      if (keep.length !== saved2.length) { tplSave(field, keep); enhStatus(field, i18n.t('web.mu_tpl_deleted', "🗑 deleted “{0}”", name)); }
    } else {
      var parts = v.split(':');
      var arr = parts[0] === 's' ? tplLoad(field) : TPL_BUILTIN[field];
      var item = arr[Number(parts[1])];
      if (item) { el.value = item.body; el.dispatchEvent(new Event('input')); }
    }
    refreshTplMenu(field);
    sel.value = '';
  }
  ['style', 'lyrics'].forEach(function (field) {
    enhRow(field);
    $('enh-' + field).addEventListener('click', function () { enhance(field); });
    $('tpl-' + field).addEventListener('click', function () {
      var sel = $('tplmenu-' + field);
      if (sel.hidden) { refreshTplMenu(field); sel.hidden = false; } else sel.hidden = true;
    });
    $('tplmenu-' + field).addEventListener('change', function () { tplMenuAction(field); });
  });

  loadModels(); loadChatModels(); loadLibrary(); loadAbcSelect();
  // Restore AFTER server settings land so the user's last (possibly unsaved)
  // edits win over the server-persisted snapshot.
  loadSettings().then(restoreForm).then(runningJobBanner);
})();
