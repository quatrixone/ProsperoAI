// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const code = source.slice(source.indexOf('function voiceStatus('),
                          source.indexOf('const voiceRecorder=window'));
const languageCode = source.slice(source.indexOf('function populateWorkspaceLanguagePicker(){'),
                                  source.indexOf('async function refreshCustomChatterboxVoice('));

function fixture({ secureContext = false, mediaDevices = {}, mediaRecorder = true, sample = false } = {}) {
    const elements = {
        'voice-record-status': { textContent: '', classList: { names: new Set(),
            toggle(name, on) { if (on) this.names.add(name); else this.names.delete(name); } } },
        'voice-record-hint': { textContent: '' },
        'voice-record-start': { textContent: 'Start recording' },
        'voice-record-source': { value: 'browser' },
    };
    const scope = {
        window: { isSecureContext: secureContext },
        navigator: { mediaDevices },
        $: id => elements[id],
        recordedVoicePcm: sample ? new ArrayBuffer(8) : null,
        nativeVoiceReady: false,
    };
    if (mediaRecorder) scope.window.MediaRecorder = function () {};
    runInNewContext(code + '\nthis.api={voiceStatus,updateVoiceSource,browserLiveRecording};', scope);
    return { ...scope.api, elements };
}

test('plain HTTP offers the device recorder instead of live microphone capture', () => {
    const { updateVoiceSource, browserLiveRecording, elements } = fixture({
        secureContext: false, mediaDevices: { getUserMedia() {} } });
    assert.equal(browserLiveRecording(), false);
    updateVoiceSource();
    assert.match(elements['voice-record-hint'].textContent, /plain HTTP/);
    assert.match(elements['voice-record-hint'].textContent, /localhost or HTTPS/);
    assert.equal(elements['voice-record-start'].textContent, 'Record or choose audio');
});

test('a secure context keeps live recording and its permission hint', () => {
    const { updateVoiceSource, browserLiveRecording, elements } = fixture({
        secureContext: true, mediaDevices: { getUserMedia() {} } });
    assert.equal(browserLiveRecording(), true);
    updateVoiceSource();
    assert.match(elements['voice-record-hint'].textContent, /microphone access/);
    assert.equal(elements['voice-record-start'].textContent, 'Start recording');
});

test('the PS5 microphone needs no secure context and keeps its own hint', () => {
    const { updateVoiceSource, elements } = fixture({ secureContext: false });
    elements['voice-record-source'].value = 'ps5';
    updateVoiceSource();
    assert.match(elements['voice-record-hint'].textContent, /DualSense/);
    assert.equal(elements['voice-record-start'].textContent, 'Start recording');
});

test('a prepared sample keeps the Record again label, and errors are marked', () => {
    const { updateVoiceSource, voiceStatus, elements } = fixture({ secureContext: false, sample: true });
    elements['voice-record-start'].textContent = 'Record again';
    updateVoiceSource();
    assert.equal(elements['voice-record-start'].textContent, 'Record again');
    voiceStatus('Something went wrong.', 'error');
    assert.ok(elements['voice-record-status'].classList.names.has('error'));
    voiceStatus('Ready to record.');
    assert.ok(!elements['voice-record-status'].classList.names.has('error'));
});

function languageFixture(settings = {}) {
    const select = { value: '', options: [], children: [],
        replaceChildren() { this.children = []; this.options = []; },
        append(option) { this.children.push(option); this.options.push(option); },
        set selectedIndex(index) { this.value = this.options[index].value; } };
    const scope = {
        window: { prosperoChatterboxLanguages: settings.languages },
        document: { createElement: () => ({ value: '', textContent: '' }) },
        $: () => select,
        savedChatterboxLanguage: settings.saved ?? 'en',
        runtimeSettings: change => { scope.sent = change; },
    };
    runInNewContext(languageCode + '\nthis.populate=populateWorkspaceLanguagePicker;', scope);
    scope.populate();
    return { select, scope };
}

test('the language picker lists console choices and stages a new selection locally', () => {
    const { select, scope } = languageFixture({
        languages: [{ id: 'en', name: 'English' }, { id: 'de', name: 'German' }, { id: 'ko', name: 'Korean' }],
        saved: 'de' });
    assert.deepEqual(select.options.map(option => option.value), ['en', 'de', 'ko']);
    assert.deepEqual(select.options.map(option => option.textContent), ['English', 'German', 'Korean']);
    assert.equal(select.value, 'de');
    select.value = 'ko';
    select.onchange();
    assert.equal(scope.savedChatterboxLanguage, 'ko');
    assert.equal(scope.sent, undefined, 'changing a Workspace option must not update runtime settings');
});

test('a saved language the console no longer offers falls back to the first one', () => {
    const { select } = languageFixture({ languages: [{ id: 'en', name: 'English' }], saved: 'zh' });
    assert.equal(select.value, 'en');
});

test('without a catalog the picker still offers English', () => {
    const { select } = languageFixture({ languages: undefined, saved: 'en' });
    assert.deepEqual(select.options.map(option => option.value), ['en']);
});
