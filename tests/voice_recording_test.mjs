// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import { runInNewContext } from './english_i18n.mjs';
import {test} from 'node:test';
const source=readFileSync(new URL('../assets/web/voice-recording.js',import.meta.url),'utf8');
function fixture(overrides={}) {
  const scope={window:{isSecureContext:true},navigator:{},Blob,performance,setTimeout,clearTimeout,setInterval,clearInterval,...overrides};
  runInNewContext(source,scope);return scope;
}
function audio(rate,seconds,channels=2,silent=false) {
  const data=Array.from({length:channels},()=>Float32Array.from({length:rate*seconds},(_,i)=>silent?0:Math.sin(i*2*Math.PI*440/rate)*0.5));
  return {sampleRate:rate,length:data[0].length,numberOfChannels:channels,getChannelData:i=>data[i]};
}
test('conversion produces a bounded 16 kHz mono PCM16 WAV from stereo audio',()=>{
  const {encodeVoiceSample}=fixture().window.prosperoVoiceRecording;
  const wav=encodeVoiceSample(audio(48000,8)),v=new DataView(wav);
  assert.equal(wav.byteLength,160044);assert.equal(v.getUint32(24,true),16000);
  assert.equal(v.getUint16(22,true),1);assert.equal(v.getUint16(34,true),16);assert.equal(v.getUint32(40,true),160000);
  assert.equal(v.getInt16(44+20*2,true),Math.trunc(Math.sin(20*2*Math.PI*440/16000)*0.5*32767));
  assert.throws(()=>encodeVoiceSample(audio(16000,0.5)),/at least one second/);
  assert.throws(()=>encodeVoiceSample(audio(16000,2,1,true)),/No voice detected/);
});
test('cancelling while microphone permission is pending releases the eventual stream',async()=>{
  let resolve,stopped=0,ready=0;
  const s=fixture({navigator:{mediaDevices:{getUserMedia:()=>new Promise(r=>resolve=r)}}});s.window.MediaRecorder=function(){};
  const recorder=s.window.prosperoVoiceRecording.createVoiceRecorder({onStatus(){},onReady:pcm=>{if(pcm)ready++;},onRecording(){},onProgress(){}});
  const pending=recorder.start();recorder.cancel();resolve({getTracks:()=>[{stop(){stopped++;}}]});await pending;
  assert.equal(stopped,1);assert.equal(ready,0);
});
test('insecure browser recording fails clearly without requesting microphone access',async()=>{
  let requested=0,status='';const s=fixture({window:{isSecureContext:false},navigator:{mediaDevices:{getUserMedia(){requested++;}}}});
  const recorder=s.window.prosperoVoiceRecording.createVoiceRecorder({onStatus:value=>status=value,onReady(){},onRecording(){},onProgress(){}});
  await recorder.start();assert.equal(requested,0);assert.match(status,/HTTPS or localhost/);
});
test('decoder closes its audio context after both success and rejection',async()=>{
  let closed=0,fail=false;const s=fixture();s.window.AudioContext=class{async decodeAudioData(){if(fail)throw Error('bad audio');return audio(16000,2,1);}async close(){closed++;}};
  await s.window.prosperoVoiceRecording.convertVoiceSample(new ArrayBuffer(1));fail=true;
  await assert.rejects(s.window.prosperoVoiceRecording.convertVoiceSample(new ArrayBuffer(1)),/bad audio/);assert.equal(closed,2);
});
