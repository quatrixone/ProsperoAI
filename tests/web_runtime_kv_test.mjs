// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');

function makeElement() {
    return {
        children: [], options: [], disabled: false, value: '', checked: false, textContent: '',
        append(...items) { this.children.push(...items); this.options.push(...items); },
        before(...items) { this.inserted = items; },
        after(...items) { this.inserted = items; },
        remove() { this.removed = true; },
    };
}

test('the web KV selector exposes all cache types and saves its selection', () => {
    const start = source.indexOf('const kvCacheLabel=document.createElement(\'label\');');
    const end = source.indexOf('const audioDurationLabel=', start);
    const bindingsStart = source.indexOf("for(const id of ['runtime-context','runtime-kv'", end);
    const bindingsEnd = source.indexOf('\nconst fallbackKokoroVoices=', bindingsStart);
    assert.notEqual(start, -1, 'the workspace should provide a KV-cache selector');
    assert.notEqual(end, -1);
    assert.notEqual(bindingsStart, -1);
    assert.notEqual(bindingsEnd, -1);

    const feedback = makeElement();
    const contextLabel = makeElement();
    const oldHint = makeElement();
    oldHint.textContent = 'KV cache is selected automatically for the model and context.';
    contextLabel.nextElementSibling = oldHint;
    const elements = {
        'runtime-feedback': feedback,
        'runtime-context': { parentElement: contextLabel },
    };
    const scope = {
        document: { createElement: makeElement },
        $: id => id === 'runtime-kv' ? scope.kvCacheSelect :
            elements[id] || (elements[id] = makeElement()),
        sent: null,
        staged: false,
        stageWorkspaceSettingsChange() { scope.staged = true; },
    };
    runInNewContext(source.slice(start, end) + '\nthis.kvCacheSelect=kvCacheSelect;\n' +
        source.slice(bindingsStart, bindingsEnd) +
        '\nthis.selector=$(\'runtime-kv\');', scope);

    assert.deepEqual(scope.selector.options.map(option => option.value),
        ['auto', 'f16', 'q8_0', 'q4_0']);
    assert.ok(contextLabel.inserted, 'selector should sit beside context length');
    assert.equal(contextLabel.inserted[0].htmlFor, 'runtime-kv');
    assert.equal(oldHint.removed, true);
    assert.equal(feedback.inserted, undefined);
    scope.selector.value = 'q4_0';
    scope.selector.onchange();
    assert.equal(scope.sent, null, 'editing a Workspace option must not call runtime settings');
    assert.equal(scope.staged, true, 'the changed value should be staged for submission');
});

test('Kokoro voice selection stays in the Workspace TTS model flow, not Settings', () => {
    assert.equal(/kokoroVoice\.id\s*=\s*['"]kokoro-voice['"]/.test(source), false,
        'Settings must not create a standalone Kokoro voice control');
    assert.equal(/setting-sound'\)\.prepend\(kokoroLabel/.test(source), false,
        'Settings must not expose a Kokoro voice control');

    const pickerStart = source.indexOf('const fallbackKokoroVoices=');
    const pickerEnd = source.indexOf('\nasync function refreshCustomChatterboxVoice', pickerStart);
    const stagingStart = source.indexOf('function stageWorkspaceSettings(){');
    const stagingEnd = source.indexOf('\nfunction stageWorkspaceSettingsChange()', stagingStart);
    assert.notEqual(pickerStart, -1);
    assert.notEqual(pickerEnd, -1);
    assert.notEqual(stagingStart, -1);
    assert.notEqual(stagingEnd, -1);

    const workspaceVoice = {
        options: [], value: '',
        replaceChildren() { this.options = []; },
        append(option) { this.options.push(option); },
    };
    const elements = {
        'workspace-voice': workspaceVoice,
        'workspace-voice-label': {textContent: ''},
        'workspace-language-label': {hidden: false},
        'workspace-language': {hidden: false},
        'chatterbox-record-controls': {hidden: false},
        'runtime-context': {value: '256k'},
        'length': {value: '0'},
        'runtime-thinking': {checked: false},
        'runtime-idle-unload': {value: '5'},
        'runtime-kv': {value: 'auto'},
    };
    const scope = {
        document: {createElement: () => ({})},
        window: {prosperoRuntimeVoiceCatalog: {kokoro: [
            {id: 'af_heart', name: 'Heart', locale: 'en-US'},
            {id: 'af_bella', name: 'Bella', locale: 'en-US'},
        ]}},
        $: id => elements[id],
        savedKokoroVoice: 'af_heart', savedChatterboxVoice: 'cemo',
        savedChatterboxLanguage: 'en', customChatterboxVoiceAvailable: false,
        models: [{id: 'kokoro-model', purpose: 'text-to-speech'}],
        modelChooser: {value: 'kokoro-model'}, current: null,
        workspaceMode: {setOptions(options) { scope.staged = options; }},
    };
    runInNewContext(source.slice(pickerStart, pickerEnd) +
        '\nthis.populateWorkspaceVoicePicker=populateWorkspaceVoicePicker;' +
        source.slice(stagingStart, stagingEnd) +
        '\nthis.stageWorkspaceSettings=stageWorkspaceSettings;', scope);

    scope.populateWorkspaceVoicePicker('kokoro-model');
    assert.equal(elements['workspace-voice-label'].textContent, 'Kokoro voice');
    assert.deepEqual(workspaceVoice.options.map(option => option.value), ['af_heart', 'af_bella']);
    workspaceVoice.value = 'af_bella';
    workspaceVoice.onchange();
    const submitted = scope.stageWorkspaceSettings();
    assert.equal(submitted.kokoro_voice, 'af_bella');
    assert.equal(scope.staged.kokoro_voice, 'af_bella');
});

test('runtime settings restore the selected KV type and roll back a failed change', async () => {
    const start = source.indexOf('async function applyRuntimeSettings(change) {');
    const end = source.indexOf('\nconst kvCacheLabel=', start);
    assert.notEqual(start, -1);
    assert.notEqual(end, -1);

    const elements = Object.fromEntries(
        ['runtime-context', 'runtime-kv', 'length', 'runtime-thinking',
            'runtime-idle-unload', 'runtime-feedback'].map(id => [id, makeElement()]));
    let response = {
        ok: true,
        async json() {
            return {
                context: '256k', kv: 'q4_0', output_limit: 0, thinking: false,
                idle_unload_minutes: 5, audio_duration_sec: 0, audio_seed: 42,
                audio_options: '', sd_seed: 42, sd_width: 0, sd_height: 0,
                sd_upscale: false, web_search: false, web_search_timeout: 15,
                search_scopes: 511, kokoro_voice: 'af_heart', chatterbox_voice: 'cemo',
                chatterbox_language: 'en', chatterbox_languages: [], kokoro_voices: [],
                chatterbox_voices: [],
            };
        },
    };
    const scope = {
        $: id => elements[id] || null,
        api: async (_path, options) => { scope.lastOptions = options; return response; },
        applySearchScopeMask() {},
        populateWorkspaceVoicePicker() {},
        current: null,
        selectedModel: '',
        window: {},
        savedKvCache: 'auto',
        Number,
    };
    runInNewContext(source.slice(start, end) + '\nthis.apply=applyRuntimeSettings;', scope);

    await scope.apply({ kv: 'q4_0' });
    assert.equal(elements['runtime-kv'].value, 'q4_0');
    assert.equal(scope.savedKvCache, 'q4_0');
    assert.equal(JSON.parse(scope.lastOptions.body).kv, 'q4_0');

    elements['runtime-kv'].value = 'f16';
    response = { ok: false };
    await scope.apply({ kv: 'f16' });
    assert.equal(elements['runtime-kv'].value, 'q4_0');
});
