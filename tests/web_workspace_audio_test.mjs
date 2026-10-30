// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {existsSync, readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {test} from 'node:test';

const modulePath = new URL('../assets/web/workspace.js', import.meta.url);
const source = existsSync(modulePath) ? readFileSync(modulePath, 'utf8') : '';
const appSource = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../assets/web/index.html', import.meta.url), 'utf8');
const musicSource = readFileSync(new URL('../assets/web/music.js', import.meta.url), 'utf8');
const cssSource = readFileSync(new URL('../assets/web/app.css', import.meta.url), 'utf8');
const serverSource = readFileSync(new URL('../vulkan/http_server.cpp', import.meta.url), 'utf8');
function workspaceApi() {
    const scope = {globalThis: {}};
    runInNewContext(source, scope);
    return scope.globalThis.ProsperoWorkspace;
}

test('Workspace modes filter installed models by their declared purpose', () => {
    assert.ok(source, 'the workspace mode controller module should exist');
    const {filterModels, ttsAvailability} = workspaceApi();
    const models = [
        {id: 'text-1', purpose: 'text-to-text'},
        {id: 'music-1', purpose: 'text-to-audio'},
        {id: 'image-1', purpose: 'text-to-image'},
        {id: 'tts-1', purpose: 'text-to-speech'},
    ];
    assert.deepEqual(Array.from(filterModels(models, 'text'), model => model.id), ['text-1']);
    assert.deepEqual(Array.from(filterModels(models, 'image'), model => model.id), ['image-1']);
    assert.deepEqual(Array.from(filterModels(models, 'audio', 'music'), model => model.id), ['music-1']);
    assert.deepEqual(Array.from(filterModels(models, 'audio', 'tts'), model => model.id), ['tts-1']);
    assert.deepEqual(Array.from(filterModels(models.filter(model => model.id !== 'image-1'), 'image')), []);
    assert.deepEqual(Array.from(filterModels(models.filter(model => model.id !== 'music-1'), 'audio', 'music')), []);
    assert.deepEqual(Array.from(filterModels(models.filter(model => model.id !== 'tts-1'), 'audio', 'tts')), []);
    assert.equal(ttsAvailability(models, 'loading'), 'loading');
    assert.equal(ttsAvailability(models.filter(model => model.id !== 'tts-1'), 'ready'), 'empty');
    assert.equal(ttsAvailability(models, 'ready'), 'available');
    assert.equal(ttsAvailability(models, 'unavailable'), 'unavailable');
});

test('audio duration is limited to duration-aware models while retaining YuE2', () => {
    const {audioDurationAvailable} = workspaceApi();
    const music = id => ({id, purpose: 'text-to-audio'});

    assert.equal(audioDurationAvailable(music('ace-step-1.5-turbo-q8-0')), true);
    assert.equal(audioDurationAvailable(music('stable-audio-3-small-sfx-vulkan')), true);
    assert.equal(audioDurationAvailable(music('yue2-3b-q8-0')), true);
    assert.equal(audioDurationAvailable(music('sheetsage2-orig')), false);
    assert.equal(audioDurationAvailable(music('htdemucs-q8_0')), false);
    assert.equal(audioDurationAvailable({id: 'kokoro-82m-vulkan', purpose: 'text-to-speech'}), false);
    assert.equal(audioDurationAvailable(null), false);
});

test('Image subsections separate general image generation from Qwen image editing', () => {
    const {filterModels, imageToImageAvailability, imageReferenceRequired, imageReferenceMissing} = workspaceApi();
    const models = [
        {id: 'qwen-image-2.1', purpose: 'text-to-image'},
        {id: 'stable-diffusion', purpose: 'text-to-image'},
        {id: 'qwen-image-2.1', purpose: 'text-to-text'},
    ];
    assert.deepEqual(Array.from(filterModels(models, 'image', 'text-to-image'), model => model.id), [
        'qwen-image-2.1', 'stable-diffusion',
    ]);
    assert.deepEqual(Array.from(filterModels(models, 'image', 'image-to-image'), model => model.purpose), [
        'text-to-image',
    ]);
    assert.equal(filterModels(models, 'image', 'image-to-image')[0].id, 'qwen-image-2.1');
    assert.equal(imageToImageAvailability(models, 'ready'), 'available');
    assert.equal(imageToImageAvailability(models.filter(model => model.id !== 'qwen-image-2.1'), 'ready'), 'empty');
    assert.equal(imageToImageAvailability(models, 'loading'), 'loading');
    assert.equal(imageReferenceRequired('image', 'image-to-image'), true);
    assert.equal(imageReferenceRequired('image', 'text-to-image'), false);
    assert.equal(imageReferenceRequired('text', 'image-to-image'), false);
    assert.equal(imageReferenceMissing('image', 'image-to-image', null), true);
    assert.equal(imageReferenceMissing('image', 'image-to-image', {name: 'ref.png'}), false);
    assert.equal(imageReferenceMissing('image', 'text-to-image', null), false);
});

test('Music Studio owns its model selectors without the duplicate Workspace picker', () => {
    const {workspaceModelControlsVisible} = workspaceApi();
    assert.equal(workspaceModelControlsVisible('audio', 'music'), false);
    assert.equal(workspaceModelControlsVisible('audio', 'tts'), true);
    assert.equal(workspaceModelControlsVisible('image', 'music'), true);
    assert.equal(workspaceModelControlsVisible('text', 'music'), true);
});

test('Text, Image, and TTS share the model picker inside Generation settings', () => {
    const toolbarStart = htmlSource.indexOf('<section class="workspace-toolbar"');
    const toolbarEnd = htmlSource.indexOf('</section>', toolbarStart);
    const settingsStart = htmlSource.indexOf('<details id="workspace-generation">');
    const settingsEnd = htmlSource.indexOf('</details>', settingsStart);
    const musicStart = htmlSource.indexOf('<section id="workspace-audio-music"');
    const musicEnd = htmlSource.indexOf('<section id="workspace-audio-tts"', musicStart);
    const toolbar = htmlSource.slice(toolbarStart, toolbarEnd);
    const settings = htmlSource.slice(settingsStart, settingsEnd);
    const music = htmlSource.slice(musicStart, musicEnd);

    assert.notEqual(toolbarStart, -1);
    assert.notEqual(settingsStart, -1);
    assert.match(settings, /id="workspace-model-controls"[\s\S]*id="workspace-model"/);
    assert.ok(settings.indexOf('id="workspace-voice-group"') < settings.indexOf('id="workspace-text-settings"'),
        'voice options should follow the model picker before unrelated Text controls');
    assert.match(settings, /id="workspace-text-settings"[\s\S]*id="runtime-thinking"/);
    assert.match(settings, /id="model-load-status"/);
    assert.doesNotMatch(toolbar, /workspace-model-controls|workspace-model/);
    assert.doesNotMatch(music, /workspace-model-controls|id="workspace-model"/);
    assert.match(music, /id="model"/);
});

test('the chat heading does not show a second model prompt before selection', () => {
    const start = appSource.indexOf('function updateModelChip()');
    const end = appSource.indexOf('\nfunction openChat()', start);
    const update = appSource.slice(start, end);
    const chip = {textContent: '', title: '', hidden: false};
    const context = {$: () => chip, models: [], current: null};

    assert.notEqual(start, -1);
    runInNewContext(`${update}\nupdateModelChip()`, context);
    assert.equal(chip.hidden, true);

    context.models = [{id: 'tts-model', name: 'Voice model', purpose: 'text-to-speech'}];
    context.current = {model: 'tts-model'};
    runInNewContext(`${update}\nupdateModelChip()`, context);
    assert.equal(chip.hidden, false);
    assert.equal(chip.textContent, 'Model: Voice model');
});

test('a new Workspace conversation opens settings before focusing model selection', () => {
    const start = appSource.indexOf('function newConversation()');
    const end = appSource.indexOf('\nfunction startKind(', start);
    const create = appSource.slice(start, end);
    const settings = {open: false};
    const calls = [];
    const chat = {classList: {add(name) { calls.push(`chat.add:${name}`); }}};
    const context = {
        controller: null,
        currentModelChosen: false,
        $: id => id === 'workspace-generation' ? settings : id === 'chat' ? chat : {focus() { calls.push(id); }},
        matchMedia: () => ({matches: true}),
        openChat() { calls.push('openChat'); },
        renderMessages() { calls.push('renderMessages'); },
        renderHistory() { calls.push('renderHistory'); },
    };

    assert.notEqual(start, -1);
    runInNewContext(`${create}\nnewConversation()`, context);

    assert.equal(settings.open, true);
    assert.equal(context.currentModelChosen, false);
    assert.deepEqual(calls, ['chat.add:show-history', 'openChat', 'renderMessages', 'renderHistory', 'prompt']);
});

test('Image Workspace renders the chosen Image section and Qwen install state', () => {
    const start = appSource.indexOf('function renderWorkspaceImage()');
    const end = appSource.indexOf('\nfunction renderWorkspaceAudio()', start);
    const render = appSource.slice(start, end);
    assert.notEqual(start, -1);
    assert.notEqual(end, -1);
    const elements = Object.fromEntries([
        'workspace-image', 'workspace-image-text', 'workspace-image-edit',
        'workspace-image-i2i-ready', 'workspace-image-i2i-loading',
        'workspace-image-i2i-empty', 'workspace-image-i2i-install', 'workspace-image-i2i-unavailable',
    ].map(id => [id, {hidden: false}]));
    const context = {
        workspaceModeName: 'image', imageSectionName: 'image-to-image',
        models: [{id: 'qwen-image-2.1', purpose: 'text-to-image'}], modelsStatus: 'ready',
        workspaceUI: workspaceApi(), $: id => elements[id],
    };
    runInNewContext(`${render}\nrenderWorkspaceImage()`, context);
    assert.equal(elements['workspace-image'].hidden, false);
    assert.equal(elements['workspace-image-text'].hidden, true);
    assert.equal(elements['workspace-image-edit'].hidden, false);
    assert.equal(elements['workspace-image-i2i-ready'].hidden, false);
    assert.equal(elements['workspace-image-i2i-empty'].hidden, true);
    assert.equal(elements['workspace-image-i2i-install'].hidden, true);

    context.models = [{id: 'stable-diffusion', purpose: 'text-to-image'}];
    runInNewContext(`${render}\nrenderWorkspaceImage()`, context);
    assert.equal(elements['workspace-image-i2i-ready'].hidden, true);
    assert.equal(elements['workspace-image-i2i-empty'].hidden, false);
    assert.equal(elements['workspace-image-i2i-install'].hidden, false);
});

test('Audio/TTS reuses the Workspace voice controls and gives a no-model install state', () => {
    const settingsStart = htmlSource.indexOf('<details id="workspace-generation">');
    const settingsEnd = htmlSource.indexOf('</details>', settingsStart);
    const settings = htmlSource.slice(settingsStart, settingsEnd);
    assert.match(htmlSource, /data-audio-section="tts"/);
    assert.doesNotMatch(htmlSource, /data-mode="tts"/);
    assert.match(htmlSource, /Install a compatible voice model/);
    assert.match(htmlSource, /id="workspace-tts-install"[^>]*data-page="models"/);
    assert.match(settings, /id="workspace-voice-group"[\s\S]*id="workspace-voice"/,
        'the voice and language options should live in Generation settings');
    assert.match(settings, /id="workspace-tts-duration-note"/,
        'TTS should explain that output duration follows the submitted text');
    assert.equal((htmlSource.match(/id="composer"/g) || []).length, 1,
        'TTS should use the existing Workspace composer, not add another');
    for (const id of [
        'workspace-voice-group', 'workspace-voice', 'workspace-language',
        'chatterbox-record-controls', 'voice-record-dialog',
    ]) assert.match(htmlSource, new RegExp(`id="${id}"`));
    assert.match(appSource, /populateWorkspaceVoicePicker\(/);
    assert.match(appSource, /populateWorkspaceLanguagePicker\(/);
    assert.match(appSource, /refreshCustomChatterboxVoice\(/);
    assert.match(appSource, /workspaceUI\.ttsAvailability\(models/);
    assert.match(appSource, /workspace-voice-group'\)\.hidden=!ttsVisible/);
});

test('restored TTS conversations keep voice settings visible before model reselection', () => {
    const start = appSource.indexOf('function openChat()');
    const end = appSource.indexOf("\n$('history-kind')", start);
    const openChat = appSource.slice(start, end);
    const visibilityStart = appSource.indexOf('function syncWorkspaceGenerationSettings()');
    const visibilityEnd = appSource.indexOf('\nsetWorkspaceMode(workspaceModeName)', visibilityStart);
    const visibility = appSource.slice(visibilityStart, visibilityEnd);
    const voiceGroup = {hidden: true};
    const calls = [];
    const elements = {
        'chat-title': {textContent: ''},
        'model-chip': {},
        'workspace-model': {value: ''},
        'model-load-status': {textContent: ''},
        'workspace-text-settings': {hidden: false},
        'workspace-voice-group': voiceGroup,
    };
    const context = {
        current: {model: 'kokoro-model', purpose: 'text-to-speech', title: 'Speak'},
        currentModelChosen: false,
        workspaceChatOpen: false,
        workspaceModeName: 'audio',
        stagedWorkspaceModel: '',
        models: [{id: 'kokoro-model', name: 'Kokoro', purpose: 'text-to-speech'}],
        modelChooser: elements['workspace-model'],
        $: id => elements[id] || {hidden: false},
        page() {},
        setWorkspaceMode() {},
        workspaceModelValue: () => 'kokoro-model',
        updateModelChip() {},
        renderQwenImageAttachment() {},
        sdSeedLabel: {hidden: true}, sdSeedHint: {hidden: true},
        sdSizeLabel: {hidden: true}, sdSizeHint: {hidden: true},
        sdUpscaleLabel: {hidden: true}, sdUpscaleHint: {hidden: true},
        populateWorkspaceVoicePicker(id) { calls.push(id); },
    };

    assert.notEqual(start, -1);
    assert.notEqual(visibilityStart, -1);
    runInNewContext(`${visibility}\n${openChat}\nopenChat()`, context);

    assert.equal(voiceGroup.hidden, false);
    assert.deepEqual(calls, ['kokoro-model']);
});

test('Generation settings show only controls relevant to the selected model purpose', () => {
    const start = appSource.indexOf('function syncWorkspaceGenerationSettings()');
    const end = appSource.indexOf('\nsetWorkspaceMode(workspaceModeName)', start);
    const sync = appSource.slice(start, end);
    const elements = {
        'workspace-text-settings': {hidden: false},
        'workspace-voice-group': {hidden: true},
        'workspace-tts-duration-note': {hidden: true},
    };
    const imageControls = ['sdSeedLabel', 'sdSeedHint', 'sdSizeLabel', 'sdSizeHint',
        'sdUpscaleLabel', 'sdUpscaleHint'];
    for (const id of imageControls) elements[id] = {hidden: true};
    const context = {
        models: [
            {id: 'text-model', purpose: 'text-to-text'},
            {id: 'qwen-image-2.1', purpose: 'text-to-image'},
            {id: 'stable-diffusion', purpose: 'text-to-image'},
            {id: 'kokoro-model', purpose: 'text-to-speech'},
        ],
        current: null,
        modelChooser: {value: 'text-model'},
        stagedWorkspaceModel: '',
        selectedModel: '',
        $: id => elements[id],
        sdSeedLabel: elements.sdSeedLabel,
        sdSeedHint: elements.sdSeedHint,
        sdSizeLabel: elements.sdSizeLabel,
        sdSizeHint: elements.sdSizeHint,
        sdUpscaleLabel: elements.sdUpscaleLabel,
        sdUpscaleHint: elements.sdUpscaleHint,
    };

    assert.notEqual(start, -1, 'the model-specific settings visibility should be centralized');
    for (const [model, expected] of [
        ['text-model', {text: false, image: true, voice: true, durationNote: true}],
        ['qwen-image-2.1', {text: true, image: false, voice: true, durationNote: true}],
        ['stable-diffusion', {text: true, image: false, voice: true, durationNote: true}],
        ['kokoro-model', {text: true, image: true, voice: false, durationNote: false}],
    ]) {
        context.modelChooser.value = model;
        runInNewContext(`${sync}\nsyncWorkspaceGenerationSettings()`, context);
        assert.equal(elements['workspace-text-settings'].hidden, expected.text, `${model} text controls`);
        assert.equal(elements['sdSeedLabel'].hidden, expected.image, `${model} image controls`);
        assert.equal(elements['workspace-voice-group'].hidden, expected.voice, `${model} voice controls`);
        assert.equal(elements['workspace-tts-duration-note'].hidden, expected.durationNote,
            `${model} duration guidance`);
    }
});

test('Music Studio hides duration for models that cannot use it', () => {
    assert.match(htmlSource, /id="duration-control"[^>]*for="duration"/);
    assert.match(musicSource, /function syncAudioDurationVisibility\(\)/);
    assert.match(musicSource, /audioDurationAvailable\(model\)/);
    assert.match(musicSource, /\$\('model'\)\.addEventListener\('change', syncAudioDurationVisibility\)/);

    const start = musicSource.indexOf('function syncAudioDurationVisibility()');
    const end = musicSource.indexOf('\n\n  function loadModels()', start);
    const sync = musicSource.slice(start, end);
    const selector = {value: 'ace-step-1.5-turbo-q8-0'};
    const duration = {hidden: true};
    const context = {
        musicModels: [
            {id: 'ace-step-1.5-turbo-q8-0', purpose: 'text-to-audio'},
            {id: 'sheetsage2-orig', purpose: 'text-to-audio'},
            {id: 'yue2-3b-q8-0', purpose: 'text-to-audio'},
        ],
        window: {ProsperoWorkspace: workspaceApi()},
        $: id => id === 'model' ? selector : duration,
    };
    assert.notEqual(start, -1);
    runInNewContext(`${sync}\nsyncAudioDurationVisibility()`, context);
    assert.equal(duration.hidden, false);
    selector.value = 'sheetsage2-orig';
    runInNewContext(`${sync}\nsyncAudioDurationVisibility()`, context);
    assert.equal(duration.hidden, true);
    selector.value = 'yue2-3b-q8-0';
    runInNewContext(`${sync}\nsyncAudioDurationVisibility()`, context);
    assert.equal(duration.hidden, false);
});

test('changing the Audio subsection does not select or prepare a model', () => {
    assert.ok(source, 'the workspace mode controller module should exist');
    const calls = [];
    const workspace = workspaceApi().createWorkspaceMode({
        models: [
            {id: 'music-1', purpose: 'text-to-audio'},
            {id: 'tts-1', purpose: 'text-to-speech'},
        ],
        selectModel: async id => calls.push(['select', id]),
        prepareModel: async id => calls.push(['prepare', id]),
    });

    workspace.setMode('audio');
    workspace.setAudioSection('tts');
    assert.deepEqual(calls, []);
});

test('Music submission validates the selected or queued installed model before side effects', async () => {
    const {withInstalledMusicModel} = workspaceApi();
    assert.equal(typeof withInstalledMusicModel, 'function', 'Workspace should expose the real Music model guard');

    const models = [
        {id: 'music-installed', purpose: 'text-to-audio'},
        {id: 'text-installed', purpose: 'text-to-text'},
    ];
    const calls = [];
    const attempt = (selectedId, queuedId) => withInstalledMusicModel(
        models,
        selectedId,
        queuedId,
        async model => {
            calls.push(['runtime-settings', model.id]);
            calls.push(['generation', model.id]);
            return model.id;
        },
    );

    await assert.rejects(attempt('', undefined), /Choose an installed music model/);
    await assert.rejects(attempt('music-removed', undefined), /Choose an installed music model/);
    await assert.rejects(attempt('text-installed', undefined), /Choose an installed music model/);
    await assert.rejects(attempt('music-installed', 'music-removed'), /Choose an installed music model/);
    assert.deepEqual(calls, [], 'invalid direct and queued choices must not reach settings or generation');

    assert.equal(await attempt('music-installed', undefined), 'music-installed');
    assert.deepEqual(calls, [
        ['runtime-settings', 'music-installed'],
        ['generation', 'music-installed'],
    ]);
});

test('Music generation passes saved queue model IDs to the installed-model guard', () => {
    assert.ok(musicSource.includes('withInstalledMusicModel('), 'generation should use the tested model guard');
    assert.ok(/model:\s*item\.params\.model\s*\|\|\s*''/.test(musicSource),
        'queue generation should pass the saved model even when it is empty');
});

test('mode, model, and options stay staged until submit selects and prepares the model', async () => {
    assert.ok(source, 'the workspace mode controller module should exist');
    const calls = [];
    const workspace = workspaceApi().createWorkspaceMode({
        models: [
            {id: 'text-1', purpose: 'text-to-text'},
            {id: 'music-1', purpose: 'text-to-audio'},
            {id: 'tts-1', purpose: 'text-to-speech'},
        ],
        selectModel: async id => calls.push(['select', id]),
        prepareModel: async id => calls.push(['prepare', id]),
        loadedModel: 'text-1',
        runtimeSettings: async options => calls.push(['settings', options]),
    });

    workspace.setMode('audio');
    workspace.setAudioSection('tts');
    assert.deepEqual(Array.from(workspaceApi().filterModels([
        {id: 'text-1', purpose: 'text-to-text'},
        {id: 'music-1', purpose: 'text-to-audio'},
        {id: 'tts-1', purpose: 'text-to-speech'},
    ], workspace.mode, workspace.audioSection), model => model.id), ['tts-1']);
    workspace.stageModel('tts-1');
    workspace.setOptions({voice: 'en-US', language: 'en'});
    assert.deepEqual(calls, []);

    const result = await workspace.submit({text: 'Speak this'}, async request => {
        calls.push(['request', request]); return request;
    });
    assert.deepEqual(calls.map(call => call[0]), ['select', 'prepare', 'request']);
    assert.equal(calls[0][1], 'tts-1');
    assert.equal(calls[1][1], 'tts-1');
    assert.deepEqual({...result}, {
        text: 'Speak this', voice: 'en-US', language: 'en', model: 'tts-1',
    });
});

test('Workspace exposes three modes and Music/TTS Audio sections with route state', () => {
    assert.match(htmlSource, /data-mode="text"/);
    assert.match(htmlSource, /data-mode="image"/);
    assert.match(htmlSource, /data-mode="audio"/);
    assert.match(htmlSource, /data-audio-section="music"/);
    assert.match(htmlSource, /data-audio-section="tts"/);
    assert.match(htmlSource, /data-image-section="text-to-image"/);
    assert.match(htmlSource, /data-image-section="image-to-image"/);
    assert.match(htmlSource, /workspace-image-i2i-empty/);
    assert.match(appSource, /workspaceUI\.imageToImageAvailability\(models,modelsStatus\)/);
    assert.doesNotMatch(htmlSource, /class="nav-music"/);
    assert.doesNotMatch(htmlSource, /id="speak"/);
    assert.match(appSource, /location\.pathname === '\/audio'/);
    assert.match(appSource, /history\.pushState/);
    assert.match(appSource, /addEventListener\('popstate'/);
    assert.match(appSource, /audioSection:\s*section/);
    assert.match(appSource, /event\.state\?\.audioSection/);
    assert.match(serverSource, /\{"\/music\.js", "music\.js", "text\/javascript; charset=utf-8"\}/);
});

test('Workspace embeds the Music Studio controls and loads its controller after shared auth', () => {
    assert.match(htmlSource, /id="workspace-audio-music"/);
    for (const id of [
        'lyrics', 'style', 'abc', 'abc-save', 'cover-file', 'cover-go', 'lib-save',
        'model', 'chat-model', 'generate', 'q-run', 'tracks', 'refresh',
    ]) assert.match(htmlSource, new RegExp(`id="${id}"`));
    assert.match(htmlSource, /id="cover-go"/);
    assert.match(htmlSource, /id="q-run"/);
    assert.match(htmlSource, /src="\/lame\.min\.js"/);
    assert.match(htmlSource, /src="\/app\.js" defer><\/script>[\s\S]*src="\/music\.js" defer/);
    assert.doesNotMatch(htmlSource, /href="\/music(?:\.html)?"|data-page="music"|<main[^>]+id="music"/);
    const musicPanelStart = htmlSource.indexOf('<section id="workspace-audio-music"');
    const musicPanelEnd = htmlSource.indexOf('<section id="workspace-audio-tts"', musicPanelStart);
    assert.notEqual(musicPanelStart, -1);
    assert.notEqual(musicPanelEnd, -1);
    assert.doesNotMatch(htmlSource.slice(musicPanelStart, musicPanelEnd), /<main\b/);
    for (const route of ['/mp3-worker.js', '/stems-worker.js'])
        assert.ok(serverSource.includes(`"${route}"`), `${route} remains a public worker asset`);
});

test('Music requests share Workspace auth and model purpose metadata', () => {
    assert.match(appSource, /window\.ProsperoWorkspaceApi\s*=\s*api/);
    assert.match(musicSource, /window\.ProsperoWorkspaceApi\(/);
    assert.match(musicSource, /purpose\s*===\s*['"]text-to-audio['"]/);
    assert.match(musicSource, /purpose\s*===\s*['"]text-to-text['"]/);
    assert.match(musicSource, /\/v1\/models/);
    assert.doesNotMatch(musicSource, /MUSIC_RE/);
    assert.doesNotMatch(musicSource, /localStorage\.getItem\(KEY\)/);
    assert.doesNotMatch(musicSource, /key-bar|key-input|key-save|prospero-key/);
    assert.match(musicSource, /Content-Type['"]:\s*['"]application\/octet-stream/);
    assert.match(musicSource, /body:\s*buf/);
    assert.match(musicSource, /AbortController/);

    const selectionStart = musicSource.indexOf("sel.addEventListener('change'");
    const selectionEnd = musicSource.indexOf("}).catch(function () { sel.hidden = true; });", selectionStart);
    assert.notEqual(selectionStart, -1);
    assert.notEqual(selectionEnd, -1);
    assert.doesNotMatch(musicSource.slice(selectionStart, selectionEnd), /api\(['"]\/api\/models\/selection/);
});

test('Audio subsection navigation is recorded and restored by browser history', () => {
    const start = appSource.indexOf('function setWorkspaceMode(');
    const end = appSource.indexOf('\nasync function renderMediaLibrary()', start);
    const routeState = appSource.slice(start, end);
    assert.notEqual(start, -1);
    assert.notEqual(end, -1);

    const calls = [];
    const workspace = workspaceApi().createWorkspaceMode({
        models: [],
        selectModel: async id => calls.push(['select', id]),
        prepareModel: async id => calls.push(['prepare', id]),
    });
    workspace.setMode('audio');
    workspace.setAudioSection('music');
    const makeButton = dataset => ({
        dataset,
        classList: {toggle(name, active) { if (name === 'selected') this.selected = active; }},
        setAttribute(name, value) { this[name] = value; },
    });
    const audioTabs = [makeButton({audioSection: 'music'}), makeButton({audioSection: 'tts'})];
    const modeTabs = ['text', 'image', 'audio'].map(mode => makeButton({mode}));
    const elements = Object.fromEntries([
        'welcome', 'chat', 'workspace-audio', 'workspace-audio-music', 'workspace-audio-tts', 'workspace-model-controls',
    ].map(id => [id, {hidden: false}]));
    const location = {pathname: '/audio'};
    const entries = [{state: {workspaceMode: 'audio', audioSection: 'music'}, path: '/audio'}];
    let index = 0;
    let popstate;
    const history = {
        get state() { return entries[index].state; },
        pushState(state, _title, path) {
            entries.splice(index + 1);
            entries.push({state, path});
            index++;
            location.pathname = path;
        },
        back() {
            if (index === 0) return;
            index--;
            location.pathname = entries[index].path;
            popstate({state: this.state});
        },
        forward() {
            if (index === entries.length - 1) return;
            index++;
            location.pathname = entries[index].path;
            popstate({state: this.state});
        },
    };
    const conversation = {id: 'saved-voice-conversation', messages: [{role: 'user', content: 'Keep me'}]};
    const conversations = [conversation];
    const context = {
        workspaceModeName: 'audio',
        audioSectionName: 'music',
        imageSectionName: 'text-to-image',
        workspaceChatOpen: true,
        workspaceMode: workspace,
        workspaceUI: workspaceApi(),
        current: conversation,
        conversations,
        models: [],
        modelsStatus: 'ready',
        modelChooser: {value: ''},
        stagedWorkspaceModel: '',
        selectedModel: '',
        imageAttachment: {file: null},
        controller: {abort() { calls.push(['cancel']); }},
        document: {
            querySelectorAll(selector) {
                if (selector === '[data-mode]') return modeTabs;
                if (selector === '[data-audio-section]') return audioTabs;
                return [];
            },
        },
        $: id => elements[id],
        renderWorkspaceModels() {},
        renderQwenImageAttachment() {},
        syncWorkspaceGenerationSettings() {},
        history,
        location,
    };
    context.window = {addEventListener(type, handler) { if (type === 'popstate') popstate = handler; }};

    runInNewContext(`${routeState}\ndocument.querySelectorAll('[data-audio-section]')[1].onclick();`, context);

    assert.deepEqual({...history.state}, {workspaceMode: 'audio', audioSection: 'tts'});
    assert.equal(context.audioSectionName, 'tts');
    assert.equal(audioTabs[1].classList.selected, true);
    assert.equal(elements['workspace-audio-tts'].hidden, true,
        'TTS should use the shared conversation layout once the chat is open');
    assert.equal(elements.chat.hidden, false);
    assert.equal(context.current, conversation);
    assert.equal(context.conversations, conversations);
    assert.deepEqual(calls, []);

    context.workspaceChatOpen = false;
    runInNewContext('renderWorkspaceAudio()', context);
    assert.equal(elements['workspace-audio-tts'].hidden, false,
        'the TTS start panel should remain available before opening a conversation');
    assert.equal(elements.chat.hidden, true);
    context.workspaceChatOpen = true;
    runInNewContext('renderWorkspaceAudio()', context);

    history.back();
    assert.equal(context.audioSectionName, 'music');
    assert.equal(workspace.audioSection, 'music');
    assert.equal(audioTabs[0].classList.selected, true);
    assert.equal(elements['workspace-audio-music'].hidden, false);
    assert.equal(elements['workspace-model-controls'].hidden, true);
    assert.equal(elements.chat.hidden, true);
    assert.equal(context.current, conversation);
    assert.equal(context.conversations, conversations);
    assert.deepEqual(calls, []);

    history.forward();
    assert.equal(context.audioSectionName, 'tts');
    assert.equal(workspace.audioSection, 'tts');
    assert.equal(audioTabs[1].classList.selected, true);
    assert.equal(elements['workspace-audio-tts'].hidden, true);
    assert.equal(elements['workspace-model-controls'].hidden, false);
    assert.equal(elements.chat.hidden, false);
    assert.deepEqual(calls, []);
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
        syncWorkspaceGenerationSettings() {},
    };
    runInNewContext(`${chooserFunction}\nchooseInstalledModel('replacement-model')`, context);
    assert.equal(current.model, 'active-model');
    assert.equal(modelChooser.value, 'active-model');
});

test('choosing a model closes the mobile Generation settings drawer', () => {
    const start = appSource.indexOf('function chooseInstalledModel(');
    const end = appSource.indexOf("\n$('search').oninput=renderModels", start);
    const choose = appSource.slice(start, end);
    const calls = [];
    const current = {model: 'text-model'};
    const modelChooser = {value: 'text-model'};
    const context = {
        controller: null,
        current,
        modelChooser,
        models: [{id: 'text-model', name: 'Text model', purpose: 'text-to-text'}],
        workspaceModeName: 'text',
        workspaceSection: () => 'text',
        workspaceUI: {filterModels: models => models},
        workspaceMode: {stageModel() {}},
        $: () => ({textContent: ''}),
        updateModelChip() {}, renderModels() {}, renderHistory() {}, openChat() {},
        syncWorkspaceGenerationSettings() {},
        closeHistory() { calls.push('close-history'); },
    };

    assert.notEqual(start, -1);
    runInNewContext(`${choose}\nchooseInstalledModel('text-model')`, context);

    assert.equal(current.model, 'text-model');
    assert.deepEqual(calls, ['close-history']);
});

test('embedded Music Studio retains playback, WAV-only stems, and cleanup on refresh', () => {
    const playerStart = musicSource.indexOf('function makePlayer(');
    const playerEnd = musicSource.indexOf('\n  function makeStemGroup(', playerStart);
    const player = musicSource.slice(playerStart, playerEnd);
    const libraryStart = musicSource.indexOf('  function loadLibrary()');
    const libraryEnd = musicSource.indexOf('\n\n  // ---- ABC save/load ----', libraryStart);
    const library = musicSource.slice(libraryStart, libraryEnd);
    assert.notEqual(playerStart, -1);
    assert.notEqual(libraryStart, -1);
    assert.match(player, /new Audio\(\)/);
    assert.match(player, /if \(isMp3Source\) ps5StemsBtn\.style\.display = 'none'/);
    assert.match(player, /playerAudios\.push\(audio\)/);
    assert.match(library, /playerAudios\.forEach\(function \(audio\) \{ audio\.pause\(\); \}\)/);
    assert.match(library, /playerAudios = \[\]/);
});

test('Cover transcribes browser audio through the selected Web Audio constructors', () => {
    const start = musicSource.indexOf("$('cover-go').addEventListener");
    const end = musicSource.indexOf("$('regen').addEventListener", start);
    const cover = musicSource.slice(start, end);
    assert.match(cover, /window\.webkitAudioContext/);
    assert.match(cover, /window\.webkitOfflineAudioContext/);
    assert.match(cover, /new AC\(\)/);
    assert.match(cover, /new OfflineAC\(1, frames, rate\)/);
    assert.match(cover, /frames > 300 \* rate/);
});

test('legacy runtime audio settings and Music Studio controls keep distinct IDs', () => {
    assert.match(appSource, /audioDuration\.id='runtime-audio-duration'/);
    assert.match(appSource, /audioSeed\.id='runtime-audio-seed'/);
    assert.match(htmlSource, /id="duration"/);
    assert.match(htmlSource, /id="seed"/);
    assert.match(htmlSource, /id="workspace-audio"/);
});

test('Music error and delete colors use a contrasting theme token', () => {
    const token = name => cssSource.match(new RegExp(`${name}:([^;}]+)`))?.[1];
    assert.ok(token('--music-error'), 'Music should define a semantic error color token');
    assert.match(cssSource, /#workspace-audio-music \.dot\{[^}]*background:var\(--music-error\)/);
    assert.match(cssSource, /#workspace-audio-music #cover-status\.err,#workspace-audio-music #gen-status\.err\{color:var\(--music-error\)\}/);
    assert.match(cssSource, /#workspace-audio-music \.track \.del\{[^}]*color:var\(--music-error\)/);
    assert.match(cssSource, /body\.daylight\{--music-error:/);
    assert.match(cssSource, /body\.contrast\{--music-error:var\(--ink\)\}/);

    const luminance = hex => {
        const channels = hex.slice(1).match(/.{2}/g).map(value => parseInt(value, 16) / 255);
        const [r, g, b] = channels.map(value => value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4);
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    };
    const contrast = (foreground, background) => {
        const values = [luminance(foreground), luminance(background)].sort((a, b) => b - a);
        return (values[0] + 0.05) / (values[1] + 0.05);
    };
    const daylight = cssSource.match(/body\.daylight\{([^}]+)\}/)?.[1] || '';
    const dark = cssSource.match(/:root\{([^}]+)\}/)?.[1] || '';
    const darkColor = token('--music-error');
    const lightColor = cssSource.match(/body\.daylight\{--music-error:([^;}]+)\}/)?.[1];
    const darkBackground = dark.match(/--bottom:([^;]+)/)?.[1];
    const lightBackground = daylight.match(/--bottom:([^;]+)/)?.[1];
    const darkInk = dark.match(/--ink:([^;]+)/)?.[1];
    const lightInk = daylight.match(/--ink:([^;]+)/)?.[1];
    assert.match(darkColor, /^#[\da-f]{6}$/i);
    assert.match(lightColor, /^#[\da-f]{6}$/i);
    assert.ok(contrast(darkColor, darkBackground) >= 4.5, 'dark Music error color should meet WCAG AA text contrast');
    assert.ok(contrast(lightColor, lightBackground) >= 4.5, 'Daylight Music error color should meet WCAG AA text contrast');
    assert.ok(contrast(darkInk, darkBackground) >= 7, 'dark high-contrast Music error color should meet WCAG AAA text contrast');
    assert.ok(contrast(lightInk, lightBackground) >= 7, 'Daylight high-contrast Music error color should meet WCAG AAA text contrast');
});

test('Music Studio mobile ABC grid children can shrink to the panel width', () => {
    assert.match(cssSource, /#workspace-audio-music #row-abc\{grid-template-columns:minmax\(0,1fr\)\}/,
        'mobile ABC grid track must not preserve its wide min-content size');
});

test('mobile connection status chip can use the full status column width', () => {
    assert.match(cssSource, /@media\(max-width:700px\)\{#status\{max-width:100%\}\}/,
        'mobile status chip must not be capped at 45% of its status column');
});
