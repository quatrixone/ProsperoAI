// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {existsSync, readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {test} from 'node:test';

const modulePath = new URL('../assets/web/workspace-audio.js', import.meta.url);
const source = existsSync(modulePath) ? readFileSync(modulePath, 'utf8') : '';
const appSource = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../assets/web/index.html', import.meta.url), 'utf8');
const serverSource = readFileSync(new URL('../vulkan/http_server.cpp', import.meta.url), 'utf8');
function workspaceApi() {
    const scope = {globalThis: {}};
    runInNewContext(source, scope);
    return scope.globalThis.ProsperoWorkspace;
}

test('Workspace modes filter installed models by their declared purpose', () => {
    assert.ok(source, 'the workspace audio/mode controller module should exist');
    const {filterModels} = workspaceApi();
    const models = [
        {id: 'text-1', purpose: 'text-to-text'},
        {id: 'audio-1', purpose: 'text-to-audio'},
        {id: 'sd-1', purpose: 'text-to-image'},
        {id: 'tts-1', purpose: 'text-to-speech'},
    ];
    assert.deepEqual(filterModels(models, 'text').map(model => model.id), ['text-1']);
    assert.deepEqual(filterModels(models, 'audio').map(model => model.id), ['audio-1']);
    assert.deepEqual(filterModels(models, 'sd').map(model => model.id), ['sd-1']);
    assert.deepEqual(filterModels(models.filter(model => model.id !== 'sd-1'), 'sd'), []);
    assert.deepEqual(filterModels(models, 'tts').map(model => model.id), ['tts-1']);
});

test('mode, model, and options stay staged until submit selects and prepares the model', async () => {
    assert.ok(source, 'the workspace audio/mode controller module should exist');
    const calls = [];
    const workspace = workspaceApi().createWorkspaceMode({
        models: [
            {id: 'text-1', purpose: 'text-to-text'},
            {id: 'audio-1', purpose: 'text-to-audio'},
        ],
        selectModel: async id => calls.push(['select', id]),
        prepareModel: async id => calls.push(['prepare', id]),
        loadedModel: 'text-1',
        runtimeSettings: async options => calls.push(['settings', options]),
    });

    workspace.setMode('audio');
    workspace.stageModel('audio-1');
    workspace.setOptions({seed: 42, duration_seconds: 120});
    assert.equal(workspace.loadedModel, 'text-1');
    assert.deepEqual(calls, []);

    const result = await workspace.submit({lyrics: 'a song'}, async request => {
        calls.push(['request', request]); return request;
    });
    assert.deepEqual(calls.map(call => call[0]), ['select', 'prepare', 'request']);
    assert.equal(calls[0][1], 'audio-1');
    assert.equal(calls[1][1], 'audio-1');
    assert.deepEqual({...result}, {
        lyrics: 'a song', seed: 42, duration_seconds: 120, model: 'audio-1',
    });
});

test('audio queue assigns consecutive seeds and leaves later items pending after failure', async () => {
    assert.ok(source, 'the workspace audio/mode controller module should exist');
    const {createAudioQueue} = workspaceApi();
    const queue = createAudioQueue({
        run: async item => {
            if (item.seed === 12) throw new Error('first item failed');
            return 'unexpected second submission';
        },
    });
    queue.add({lyrics: 'variant'}, 2, 12);
    await queue.run();
    assert.deepEqual(Array.from(queue.items, item => item.seed), [12, 13]);
    assert.deepEqual(Array.from(queue.items, item => item.status), ['failed', 'pending']);
});

test('audio queue stops at the maximum seed instead of duplicating it', () => {
    const {createAudioQueue} = workspaceApi();
    const queue = createAudioQueue({run: async () => {}});
    queue.add({}, 3, 4294967295);
    assert.deepEqual(Array.from(queue.items, item => item.seed), [4294967295]);
});

test('stopping an audio queue lets its active operation settle and skips the next item', async () => {
    assert.ok(source, 'the workspace audio/mode controller module should exist');
    const {createAudioQueue} = workspaceApi();
    let finishActive;
    const submitted = [];
    const queue = createAudioQueue({
        run: item => {
            submitted.push(item.seed);
            if (item.seed === 21) return new Promise(resolve => { finishActive = resolve; });
            return Promise.resolve();
        },
    });
    queue.add({}, 2, 21);
    const running = queue.run();
    await Promise.resolve();
    queue.stop();
    finishActive();
    await running;
    assert.deepEqual(submitted, [21]);
    assert.deepEqual(Array.from(queue.items, item => item.status), ['done', 'pending']);
});

test('Audio Workspace is mounted by the web app and served as an embedded asset', () => {
    assert.match(htmlSource, /<script src="\/workspace-audio\.js" defer><\/script>/);
    assert.match(appSource, /workspaceUI\.mount\(/);
    assert.match(appSource, /history\.replaceState/);
    assert.match(serverSource, /\{"\/workspace-audio\.js", "workspace-audio\.js", "text\/javascript; charset=utf-8"\}/);
    assert.match(source, /\/api\/jobs\?id=/);
});

test('Workspace mode and model choices do not call runtime APIs', () => {
    const modeStart = appSource.indexOf('function setWorkspaceMode(');
    const modeEnd = appSource.indexOf("document.querySelectorAll('[data-mode]')", modeStart);
    const modelStart = appSource.indexOf('function chooseInstalledModel(');
    const modelEnd = appSource.indexOf("$('search').oninput=renderModels", modelStart);
    const settingsStart = appSource.indexOf('function stageWorkspaceSettingsChange()');
    const settingsEnd = appSource.indexOf('const audioDurationLabel=', settingsStart);
    assert.notEqual(modeStart, -1);
    assert.notEqual(modelStart, -1);
    assert.notEqual(settingsStart, -1);
    assert.doesNotMatch(appSource.slice(modeStart, modeEnd), /api\(|runtimeSettings\(/);
    assert.doesNotMatch(appSource.slice(modelStart, modelEnd), /api\(|runtimeSettings\(/);
    assert.doesNotMatch(appSource.slice(settingsStart, settingsEnd), /runtimeSettings\(/);
});

test('changing the model during an active response preserves its model attribution', () => {
    const start = appSource.indexOf('function chooseInstalledModel(');
    const end = appSource.indexOf("\n}\n$('search')", start) + 2;
    const chooserFunction = appSource.slice(start, end);
    const current = {model: 'active-model'};
    const modelChooser = {value: 'replacement-model'};
    const models = [{id: 'replacement-model', name: 'Replacement', purpose: 'text-to-text'}];
    const context = {
        controller: {}, current, modelChooser, models, workspaceModeName: 'text',
        workspaceUI: {filterModels: list => list}, workspaceMode: {stageModel() {}},
        $: () => ({textContent: ''}), newConversation() {},
        updateModelChip() {}, renderModels() {}, renderHistory() {}, openChat() {},
        syncAudioDurationVisibility() {},
    };
    runInNewContext(`${chooserFunction}\nchooseInstalledModel('replacement-model')`, context);
    assert.equal(current.model, 'active-model');
    assert.equal(modelChooser.value, 'active-model');
});

test('Cover and stems use WAV sources while MP3 remains available for playback', () => {
    const start = source.indexOf('        function renderLibrary()');
    const end = source.indexOf('\n\n        async function refreshLibrary()', start);
    const renderLibrary = source.slice(start, end);
    const elements = new Map();
    const element = tagName => ({
        tagName,
        children: [],
        append(...children) { this.children.push(...children); },
        replaceChildren() { this.children = []; },
    });
    for (const id of ['audio-library', 'audio-source', 'audio-abc-library']) elements.set(id, element());
    const context = {
        library: [{name: 'song.wav'}, {name: 'song.mp3'}, {name: 'score.abc'}],
        document: {createElement: tagName => element(tagName)},
        $: id => elements.get(id),
        pathFor: name => name,
        loadAbc() {},
        runSourceWorkflow() {},
    };
    runInNewContext(`${renderLibrary}\nrenderLibrary()`, context);

    const sourceChoices = Array.from(elements.get('audio-source').children, option => option.value);
    assert.deepEqual(sourceChoices, ['', 'song.wav']);
    const articles = elements.get('audio-library').children.filter(child => child.tagName !== 'p');
    const wavArticle = articles.find(article => article.children.some(child => child.textContent === 'song.wav'));
    const mp3Article = articles.find(article => article.children.some(child => child.textContent === 'song.mp3'));
    assert.ok(wavArticle.children.some(child => child.textContent === 'Use for Cover'));
    assert.ok(wavArticle.children.some(child => child.textContent === 'Split stems'));
    assert.ok(mp3Article.children.some(child => child.tagName === 'audio'));
    assert.ok(!mp3Article.children.some(child => ['Use for Cover', 'Split stems'].includes(child.textContent)));
});

test('uploaded MP3 is converted to WAV before it is sent to Cover', async () => {
    const start = source.indexOf('        async function uploadLocalCoverSource()');
    const end = source.indexOf('\n\n        async function loadAbc(', start);
    const uploadFunction = source.slice(start, end);
    const calls = [];
    const file = {name: 'Track.MP3', size: 1024, async arrayBuffer() { return new Uint8Array([1, 2]); }};
    const context = {
        $: () => ({files: [file]}),
        async uploadBinary(...args) { calls.push(['binary', ...args]); },
        async localFileToWav(input) { calls.push(['convert', input.name]); return new Uint8Array([3, 4]); },
        async uploadWav(...args) { calls.push(['wav', ...args]); },
    };
    const result = await runInNewContext(`(async()=>{${uploadFunction}\nreturn uploadLocalCoverSource()})()`, context);
    assert.equal(result, 'track-src.wav');
    assert.deepEqual(calls.map(call => call[0]), ['convert', 'wav']);
    assert.equal(calls[1][1], 'track-src.wav');
});

test('legacy runtime audio settings do not reuse the Audio form field IDs', () => {
    assert.match(appSource, /audioDuration\.id='runtime-audio-duration'/);
    assert.match(appSource, /audioSeed\.id='runtime-audio-seed'/);
    assert.match(htmlSource, /id="audio-duration"/);
    assert.match(htmlSource, /id="audio-seed"/);
});
