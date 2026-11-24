// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
(() => {
  function encodeVoiceSample(decoded) {
    const sourceFrames = Math.min(decoded.length, Math.floor(decoded.sampleRate * 5));
    const frames = Math.floor(sourceFrames * 16000 / decoded.sampleRate);
    if (frames < 16000) throw Error(i18n.t('web.record_at_least_one_second_of_speech', 'Record at least one second of speech.'));
    const pcm = new ArrayBuffer(44 + frames * 2), view = new DataView(pcm);
    const put = (offset, text) => { for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i)); };
    put(0, 'RIFF'); view.setUint32(4, 36 + frames * 2, true); put(8, 'WAVE'); put(12, 'fmt ');
    view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
    view.setUint32(24, 16000, true); view.setUint32(28, 32000, true); view.setUint16(32, 2, true);
    view.setUint16(34, 16, true); put(36, 'data'); view.setUint32(40, frames * 2, true);
    const channels = Array.from({ length: decoded.numberOfChannels }, (_, i) => decoded.getChannelData(i));
    let peak = 0;
    for (let i = 0; i < frames; i++) {
      const pos = i * decoded.sampleRate / 16000, left = Math.floor(pos), fraction = pos - left;
      let sample = 0;
      for (const channel of channels) {
        const a = channel[Math.min(left, sourceFrames - 1)] || 0, b = channel[Math.min(left + 1, sourceFrames - 1)] || 0;
        sample += (a + (b - a) * fraction) / channels.length;
      }
      sample = Math.max(-1, Math.min(1, sample)); peak = Math.max(peak, Math.abs(sample));
      view.setInt16(44 + i * 2, sample < 0 ? sample * 32768 : sample * 32767, true);
    }
    if (peak < 0.001) throw Error(i18n.t('web.no_voice_detected_check_your_microphone_', 'No voice detected. Check your microphone and try again.'));
    return pcm;
  }
  async function convertVoiceSample(bytes) {
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) throw Error(i18n.t('web.audio_conversion_is_unavailable_in_this_', 'Audio conversion is unavailable in this browser.'));
    const context = new AudioContextClass();
    try { return encodeVoiceSample(await context.decodeAudioData(bytes)); }
    finally { await context.close(); }
  }
  function createVoiceRecorder({ onStatus, onReady, onRecording, onProgress }) {
    let token = 0, stream = null, recorder = null, timer = null, progressTimer = null;
    function cleanup() {
      clearTimeout(timer); clearInterval(progressTimer); timer = progressTimer = null;
      if (stream) { for (const track of stream.getTracks()) track.stop(); stream = null; }
      recorder = null;
    }
    function cancel() {
      token++;
      if (recorder && recorder.state !== 'inactive') recorder.stop();
      cleanup(); onRecording(false);
    }
    function stop() {
      clearTimeout(timer); clearInterval(progressTimer);
      if (recorder && recorder.state !== 'inactive') recorder.stop();
      if (stream) for (const track of stream.getTracks()) track.stop();
    }
    async function start() {
      cancel(); const attempt = token;
      onReady(null); onProgress(0); onRecording(true);
      try {
        if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia)
          throw Error(i18n.t('web.live_browser_recording_needs_https_or_lo', 'Live browser recording needs HTTPS or localhost. Record with your device or choose a recording instead.'));
        if (!window.MediaRecorder) throw Error(i18n.t('web.microphone_recording_is_unavailable_in_t', 'Microphone recording is unavailable in this browser. Upload a recorded file.'));
        onStatus(i18n.t('web.allow_microphone_access_to_start_recordi', 'Allow microphone access to start recording.'));
        const acquired = await navigator.mediaDevices.getUserMedia({ audio: true });
        if (attempt !== token) { acquired.getTracks().forEach(track => track.stop()); return; }
        stream = acquired;
        const type = ['audio/webm;codecs=opus', 'audio/mp4'].find(mime => MediaRecorder.isTypeSupported(mime));
        const active = new MediaRecorder(stream, type ? { mimeType: type } : {});
        recorder = active; const chunks = []; let size = 0;
        active.ondataavailable = event => {
          if (attempt !== token) return;
          size += event.data.size;
          if (size > 2 * 1024 * 1024) { cancel(); onStatus(i18n.t('web.the_recording_was_too_large_try_again', 'The recording was too large. Try again.'), 'error'); }
          else if (event.data.size) chunks.push(event.data);
        };
        active.onerror = () => { if (attempt === token) { cancel(); onStatus(i18n.t('web.microphone_recording_failed_try_again', 'Microphone recording failed. Try again.'), 'error'); } };
        active.onstop = async () => {
          if (attempt !== token) return;
          cleanup(); onRecording(false); onStatus(i18n.t('web.preparing_your_voice_sample', 'Preparing your voice sample…'));
          try {
            const pcm = await convertVoiceSample(await new Blob(chunks, { type: active.mimeType }).arrayBuffer());
            if (attempt !== token) return;
            onReady(pcm); onStatus(i18n.t('web.listen_to_your_sample_then_choose_save_a', 'Listen to your sample, then choose Save and use.'));
          } catch (error) { if (attempt === token) onStatus(error.message, 'error'); }
        };
        active.start(); const started = performance.now();
        onStatus(i18n.t('web.recording_speak_clearly', 'Recording — speak clearly.'));
        progressTimer = setInterval(() => onProgress(Math.min(5, (performance.now() - started) / 1000)), 100);
        timer = setTimeout(stop, 5000);
      } catch (error) {
        if (attempt !== token) return;
        cleanup(); onRecording(false);
        onStatus(error.name === 'NotAllowedError' ? i18n.t('web.microphone_access_was_denied_allow_acces', 'Microphone access was denied. Allow access and try again.') : error.message, 'error');
      }
    }
    return { start, stop, cancel };
  }
  window.prosperoVoiceRecording = { encodeVoiceSample, convertVoiceSample, createVoiceRecorder };
})();
