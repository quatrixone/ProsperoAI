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

test('legacy runtime audio settings do not reuse the Audio form field IDs', () => {
    assert.match(appSource, /audioDuration\.id='runtime-audio-duration'/);
    assert.match(appSource, /audioSeed\.id='runtime-audio-seed'/);
    assert.match(htmlSource, /id="audio-duration"/);
    assert.match(htmlSource, /id="audio-seed"/);
});
