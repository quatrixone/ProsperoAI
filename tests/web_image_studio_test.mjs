// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createContext, runInNewContext, runInContext} from 'node:vm';
import {test} from 'node:test';

const workspaceSource = readFileSync(new URL('../assets/web/workspace.js', import.meta.url), 'utf8');
const appSource = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../assets/web/index.html', import.meta.url), 'utf8');
const cssSource = readFileSync(new URL('../assets/web/app.css', import.meta.url), 'utf8');
const presetCatalog = JSON.parse(readFileSync(new URL('../model-tools/presets.json', import.meta.url), 'utf8'));

function workspaceApi() {
    const scope = {globalThis: {}};
    scope.setTimeout = setTimeout;
    runInNewContext(workspaceSource, scope);
    return scope.globalThis.ProsperoWorkspace;
}

const model = (id, capabilities, defaults = {}, strategy = 'init_image_strength') => ({
    id,
    name: id,
    purpose: 'text-to-image',
    details: {capabilities, image_defaults: defaults, image_edit_strategy: strategy},
});

test('Image Studio exposes only server-declared modes and handles empty capability state', () => {
    const {imageModes, imageCapabilities} = workspaceApi();
    const catalog = [
        model('new-model-text', ['text_to_image']),
        model('new-model-edit', ['text_to_image', 'image_to_image']),
        model('new-model-paint', ['text_to_image', 'inpainting']),
        model('new-model-guide', ['text_to_image', 'controlnet']),
        {id: 'no-metadata', purpose: 'text-to-image'},
    ];
    assert.deepEqual(Array.from(imageModes(catalog[0])), ['text_to_image']);
    assert.deepEqual(Array.from(imageModes(catalog[1])), ['text_to_image', 'image_to_image']);
    assert.deepEqual(Array.from(imageModes(catalog[2])), ['text_to_image', 'inpainting']);
    assert.deepEqual(Array.from(imageModes(catalog[3])), ['text_to_image', 'controlnet']);
    assert.deepEqual(Array.from(imageCapabilities(catalog[4])), []);
    assert.deepEqual(Array.from(imageModes(catalog[4])), []);
});

test('every server-listed image model, including new catalog IDs, remains reachable from the picker', () => {
    const {filterModels} = workspaceApi();
    const catalog = [model('new-sd-model', ['text_to_image']), model('new-flux-model', ['text_to_image', 'image_to_image']),
        {id: 'text-model', purpose: 'text-to-text'}];
    assert.deepEqual(Array.from(filterModels(catalog, 'image'), item => item.id), ['new-sd-model', 'new-flux-model']);
});

test('all curated image presets are reachable and retain their declared defaults', () => {
    const {filterModels, imageSettings} = workspaceApi();
    const presets = presetCatalog.filter(preset => preset.kind === 1);
    const serverModels = presets.map(preset => model(preset.id, preset.image_capabilities,
        preset.image_defaults, preset.image_edit_strategy));
    const pickerModels = filterModels(serverModels, 'image');

    assert.deepEqual(Array.from(pickerModels, item => item.id), Array.from(presets, item => item.id));
    for (const preset of presets) {
        const settings = imageSettings(serverModels.find(item => item.id === preset.id), 'text_to_image');
        for (const key of ['width', 'height', 'steps', 'cfg_scale', 'sampler', 'scheduler'])
            assert.equal(settings[key], preset.image_defaults[key], `${preset.id} ${key}`);
    }
});

test('model changes stage server defaults and hide incompatible workflow options', () => {
    const {imageSettings} = workspaceApi();
    const sd = model('new-sd', ['text_to_image', 'image_to_image', 'inpainting'],
        {width: 1024, height: 768, steps: 25, cfg_scale: 6, sampler: 'euler_a', scheduler: 'ays'});
    const flux = model('new-flux', ['text_to_image', 'image_to_image'],
        {width: 1024, height: 1024, steps: 4, cfg_scale: 1, sampler: 'euler', scheduler: 'flux2'}, 'reference_image');
    const control = model('new-control', ['text_to_image', 'controlnet']);

    assert.deepEqual({...imageSettings(sd, 'text_to_image')}, {
        width: 1024, height: 768, steps: 25, cfg_scale: 6, sampler: 'euler_a', scheduler: 'ays',
        upscale: 0, showStrength: false, showControlStrength: false,
    });
    assert.equal(imageSettings(sd, 'image_to_image').showStrength, true);
    assert.equal(imageSettings(sd, 'inpainting').showStrength, true);
    assert.equal(imageSettings(flux, 'image_to_image').showStrength, false);
    assert.equal(imageSettings(control, 'controlnet').showControlStrength, true);
    assert.equal(imageSettings(control, 'text_to_image').showControlStrength, false);
});

test('image workflow validation requires the matching source, mask, or guide', () => {
    const {validateImageInputs} = workspaceApi();
    assert.match(validateImageInputs('image_to_image', {}), /choose an image/i);
    assert.match(validateImageInputs('inpainting', {image: {}}), /mask/i);
    assert.match(validateImageInputs('controlnet', {}), /guide/i);
    assert.equal(validateImageInputs('text_to_image', {}), '');
    assert.equal(validateImageInputs('inpainting', {image: {}, mask: {}}), '');
});

test('image uploads accept PNG/JPEG files and return opaque server upload IDs', async () => {
    const {uploadImageInput} = workspaceApi();
    const calls = [];
    const api = async (path, options) => {
        calls.push({path, options});
        return {json: async () => ({id: 'a'.repeat(32)})};
    };
    const file = {type: 'image/png', size: 8, name: 'source.png'};
    assert.equal(await uploadImageInput(api, file), 'a'.repeat(32));
    assert.equal(calls[0].path, '/api/image/upload');
    assert.equal(calls[0].options.body, file);
    assert.equal(calls[0].options.headers['Content-Type'], 'image/png');
    await assert.rejects(uploadImageInput(api, {type: 'image/gif', size: 8}), /PNG or JPEG/i);
});

test('image workflow requests contain only opaque upload IDs and staged model options', () => {
    const {buildImageRequest} = workspaceApi();
    const request = buildImageRequest({
        model: model('generic-image', ['text_to_image', 'image_to_image']),
        mode: 'image_to_image',
        prompt: 'A new scene',
        values: {width: 768, height: 768, steps: 20, cfg_scale: 5, strength: 0.6,
            sampler: 'euler_a', scheduler: 'ays', negative_prompt: 'blur'},
        uploads: {image: 'b'.repeat(32)},
        sessionId: 'conversation-1',
        messageIndex: 1,
    });
    assert.deepEqual(JSON.parse(JSON.stringify(request)), {
        async: true,
        model: 'generic-image',
        prompt: 'A new scene',
        output_session_id: 'conversation-1',
        output_message_index: 1,
        image_workflow: {
            workflow: 'image_to_image', width: 768, height: 768, steps: 20, cfg_scale: 5,
            strength: 0.6, sampler: 'euler_a', scheduler: 'ays', upscale: 0,
            negative_prompt: 'blur', image_id: 'b'.repeat(32),
        },
    });
    assert.equal(JSON.stringify(request).includes('/download0/'), false);
});

test('image job polling returns completed output and surfaces terminal failures', async () => {
    const {waitForImageJob} = workspaceApi();
    const states = [{state: 'running'}, {state: 'done', result: {image_path: '/download0/work/image/result.png'}}];
    const calls = [];
    const result = await waitForImageJob(async path => {
        calls.push(path);
        return states.shift();
    }, 42, {delay: 0});
    assert.equal(result.image_path, '/download0/work/image/result.png');
    assert.deepEqual(calls, ['/api/jobs?id=42', '/api/jobs?id=42']);
    await assert.rejects(waitForImageJob(async () => ({state: 'error', result: {error: 'generation failed'}}), 43),
        error => error.message === 'generation failed' && error.code === 'IMAGE_JOB_TERMINAL');
});

test('Image Create and Gallery navigation reuses existing pages, without its own Models entry', () => {
    assert.match(htmlSource, /data-image-destination="create"/);
    assert.match(htmlSource, /data-image-destination="gallery"/);
    // Models is reached from the top-level navigation only.
    assert.doesNotMatch(htmlSource, /data-image-destination="models"/);
    assert.doesNotMatch(appSource, /destination==='models'/);
    assert.match(htmlSource, /data-page="models"/);
    assert.match(appSource, /data-image-destination/);
    assert.match(appSource, /imageOnly/);
    assert.doesNotMatch(appSource + htmlSource, /qwen-image|Qwen Image/i);
});

test('Image controls are labeled, keyboard operable, and responsive', () => {
    for (const id of ['image-prompt', 'image-model', 'image-width', 'image-height', 'image-steps',
        'image-cfg-scale', 'image-sampler', 'image-scheduler', 'image-negative-prompt', 'image-source-file',
        'image-guide-file', 'image-mask-canvas', 'image-generate']) {
        assert.match(htmlSource, new RegExp(`id="${id}"`));
    }
    assert.match(htmlSource, /role="tablist"[^>]*aria-label="Image mode"/);
    assert.match(htmlSource, /aria-pressed/);
    assert.match(cssSource, /@media\s*\(max-width:\s*700px\)/);
    assert.match(cssSource, /:focus-visible/);
});

test('Gallery image Edit and Guide actions stage content through authenticated API reads', () => {
    assert.match(appSource, /Stage image for editing/);
    assert.match(appSource, /Use as ControlNet guide/);
    assert.match(appSource, /stageGalleryImage/);
    assert.match(appSource, /api\(file\.path\)/);
    assert.match(appSource, /imageOnly/);
});

test('Image Gallery decodes the media API response before filtering its entries', () => {
    assert.match(appSource, /api\('\/api\/media'\)\.then\(response=>response\.json\(\)\)/);
});

test('image output is retained in conversation history and refreshed in the existing image Gallery', () => {
    assert.match(appSource, /buildImageRequest/);
    assert.match(workspaceSource, /output_session_id/);
    assert.match(appSource, /waitForImageJob/);
    assert.match(appSource, /renderMediaLibrary\(true\)/);
    assert.match(appSource, /conversation\.messages\.push/);
    assert.match(appSource, /retry/);
});

test('the upscale factor is sent with every image request, including off', () => {
    const {buildImageRequest} = workspaceApi();
    const base = {
        model: model('generic-image', ['text_to_image']),
        mode: 'text_to_image',
        prompt: 'A lighthouse',
    };
    // Always sent: omitting it would fall back to the Settings choice, so picking Off
    // in the Studio could not turn an upscale off.
    assert.equal(buildImageRequest({...base, values: {upscale: 0}}).image_workflow.upscale, 0);
    assert.equal(buildImageRequest({...base, values: {upscale: 2}}).image_workflow.upscale, 2);
    assert.equal(buildImageRequest({...base, values: {upscale: 4}}).image_workflow.upscale, 4);
    assert.equal(buildImageRequest(base).image_workflow.upscale, 0);
    assert.match(appSource, /upscale:Number\(\$\('image-upscale'\)\.value\)/);
    assert.match(htmlSource, /<select id="image-upscale">.*value="2".*value="4".*<\/select>/);
});

function topLevelFunction(source, signature) {
    const start = source.indexOf(signature);
    if (start < 0) return '';
    const end = source.indexOf('\n}', start);
    return end < 0 ? '' : source.slice(start, end + 2);
}

function imageSubmitHarness(jobResults) {
    const calls = [], polls = [], persisted = [];
    const model = {id: 'catalog-image', name: 'Catalog Image'};
    const elements = new Map();
    const getElement = id => {
        if (!elements.has(id)) elements.set(id, {
            value: id === 'image-prompt' ? 'A castle' : '', textContent: '', hidden: true, disabled: false,
            focus() {}, replaceChildren() {},
        });
        return elements.get(id);
    };
    const context = createContext({
        $: getElement, document: {getElementById: getElement},
        api: async (path, options = {}) => {
            calls.push({path, options, body: options.body ? JSON.parse(options.body) : null});
            return {json: async () => ({updated_us: 1, job_id: 71})};
        },
        workspaceUI: {
            validateImageInputs: () => '',
            waitForImageJob: async (_api, id) => {
                polls.push(id);
                const result = jobResults.shift();
                if (result instanceof Error) throw result;
                return result;
            },
            buildImageRequest: ({model: selected, prompt, sessionId, messageIndex}) => ({
                model: selected.id, prompt, output_session_id: sessionId, output_message_index: messageIndex,
            }),
        },
        imageModel: () => model, imageInputValues: () => ({}),
        selectWorkspaceModel: async () => {}, prepareWorkspaceModel: async () => {},
        save() {}, renderMessages() {}, renderHistory() {}, renderMediaLibrary: async () => {},
        appendMedia() {},
        persistConversation: async conversation => persisted.push(JSON.parse(JSON.stringify(conversation))),
        crypto: {randomUUID: () => 'conversation-71'},
    });
    const submit = topLevelFunction(appSource, 'async function submitImageGeneration()');
    runInContext(`let current=null, conversations=[], imageSubmitting=false, imageJobId=null, imageAttempt=null;
        let imageModeName='text_to_image';
        ${submit}`, context);
    return {
        calls, polls, persisted, elements,
        submit: () => runInContext('submitImageGeneration()', context),
        state: () => JSON.parse(runInContext(`JSON.stringify({
            messages: current?.messages || [], imageJobId, imageSubmitting,
            conversationId: current?.id || null, assistant: current?.messages?.at(-1)?.content || ''
        })`, context)),
        element: getElement,
    };
}

test('Retry resumes a transiently interrupted image job and reuses its conversation messages', async () => {
    const interrupted = new Error('temporary jobs API error');
    const harness = imageSubmitHarness([interrupted, {response: 'Image complete.'}]);

    await harness.submit();
    const afterError = harness.state();
    assert.equal(afterError.messages.length, 2, 'the failed attempt retains its user/assistant pair');
    assert.equal(afterError.imageJobId, 71, 'the accepted job ID remains available to Retry');
    assert.match(harness.persisted.at(-1).messages.at(-1).content, /status could not be checked/i,
        'the server copy must not be left with an empty assistant message');
    assert.equal(harness.element('image-retry').textContent, 'Resume polling');

    await harness.submit();
    assert.equal(harness.calls.filter(call => call.path === '/api/generate').length, 1,
        'resuming a poll must not submit another generation');
    assert.deepEqual(harness.polls, [71, 71]);
    assert.equal(harness.state().messages.length, 2, 'retry must not duplicate the prompt');
    assert.equal(harness.state().assistant, 'Image complete.');
    assert.equal(harness.persisted.at(-1).messages.length, 2,
        'the persisted conversation must match the reused user/assistant pair');
});

test('Retry starts a new image job only after terminal failure and reuses the failed message pair', async () => {
    const terminal = Object.assign(new Error('generation failed'), {code: 'IMAGE_JOB_TERMINAL'});
    const harness = imageSubmitHarness([terminal, {response: 'Image complete.'}]);

    await harness.submit();
    assert.equal(harness.state().imageJobId, null, 'terminal failure releases the completed job ID');
    assert.equal(harness.element('image-retry').textContent, 'Retry generation');
    assert.equal(harness.state().messages.length, 2);
    assert.match(harness.state().assistant, /generation failed/i);
    assert.equal(harness.calls.filter(call => call.path === '/api/generate').length, 1,
        'a new job starts only after the explicit Retry action following terminal failure');

    await harness.submit();
    assert.equal(harness.calls.filter(call => call.path === '/api/generate').length, 2);
    assert.equal(harness.state().messages.length, 2, 'a terminal retry reuses rather than duplicates messages');
    assert.equal(harness.state().assistant, 'Image complete.');
    assert.equal(harness.persisted.at(-1).messages.length, 2);
});

test('Archive-warning Retry is labeled as a new generation after a completed job', async () => {
    const harness = imageSubmitHarness([
        new Error('temporary jobs API error'),
        {response: 'Archive warning: image could not be saved.'},
    ]);

    await harness.submit();
    assert.equal(harness.element('image-retry').textContent, 'Resume polling');
    await harness.submit();
    assert.equal(harness.element('image-retry').hidden, false);
    assert.equal(harness.element('image-retry').textContent, 'Retry generation');
});

test('Gallery Edit and Guide report an actionable error when no compatible model is installed', async () => {
    const status = {textContent: ''}, feedback = {textContent: ''}, imageModel = {value: ''};
    let apiCalls = 0, stagedFiles = 0, modeChanges = 0, createNavigations = 0;
    const stage = topLevelFunction(appSource, 'async function stageGalleryImage(file,mode)');
    for (const mode of ['image_to_image', 'controlnet']) {
        const context = {
            $: id => ({'image-status': status, feedback, 'image-model': imageModel})[id],
            api: async () => { apiCalls++; return {blob: async () => ({})}; },
            models: [],
            workspaceUI: {filterModels: () => [], imageCapabilities: () => []},
            safeImageFile: () => ({}),
            setImageFile: () => { stagedFiles++; },
            setImageMode: () => { modeChanges++; },
            document: {querySelector: () => ({click: () => { createNavigations++; }})},
        };
        status.textContent = '';
        await runInNewContext(`${stage}\nstageGalleryImage({path:'/archive/image.png'}, '${mode}')`, context);
        assert.match(status.textContent, /no installed.*model/i, `${mode} should show a no-model status`);
        assert.doesNotMatch(status.textContent, /staged/i);
    }

    assert.equal(apiCalls, 0, 'do not fetch or stage an image that cannot be used');
    assert.equal(stagedFiles, 0);
    assert.equal(modeChanges, 0);
    assert.equal(createNavigations, 2, 'show the actionable status in the Create workspace');
});

test('Inpainting masks have labeled keyboard rectangle controls that paint and erase mask regions', () => {
    for (const id of ['image-mask-keyboard-controls', 'image-mask-x', 'image-mask-y', 'image-mask-width',
        'image-mask-height', 'image-mask-paint-rectangle', 'image-mask-erase-rectangle'])
        assert.ok(htmlSource.includes(`id="${id}"`), `expected keyboard mask control #${id}`);
    for (const label of ['Left (%)', 'Top (%)', 'Width (%)', 'Height (%)'])
        assert.ok(htmlSource.includes(label), `expected accessible ${label} input`);
    assert.ok(/Set a rectangle.*Paint selected rectangle.*Erase selected rectangle/s.test(htmlSource),
        'keyboard mask instructions should explain the edit and paint/erase actions');
    assert.match(appSource, /getElementById\('image-mask-paint-rectangle'\)\.onclick/);
    assert.match(appSource, /getElementById\('image-mask-erase-rectangle'\)\.onclick/);

    const drawCalls = [], previewCalls = [];
    const maskContext = {fillStyle: '', fillRect(...args) { drawCalls.push({style: this.fillStyle, args}); }};
    const previewContext = {fillStyle: '', fillRect(...args) { previewCalls.push({style: this.fillStyle, args}); },
        clearRect(...args) { previewCalls.push({operation: 'clearRect', args}); }};
    const elements = {
        'image-mask-x': {value: '10'}, 'image-mask-y': {value: '20'},
        'image-mask-width': {value: '30'}, 'image-mask-height': {value: '40'},
        'image-mask-output': {width: 200, height: 100, getContext: () => maskContext},
        'image-mask-canvas': {width: 200, height: 100, getContext: () => previewContext},
    };
    const edit = topLevelFunction(appSource, 'function editMaskRectangle(erase=false)');
    assert.notEqual(edit, '', 'the keyboard editor has a rectangle painting implementation');
    const context = createContext({$: id => elements[id], imageSourceFile: {name: 'source.png'}});
    runInContext(`let imageMaskPainted=false; ${edit}; editMaskRectangle(false)`, context);
    assert.deepEqual(drawCalls.at(-1), {style: '#fff', args: [20, 20, 60, 40]});
    assert.equal(runInContext('imageMaskPainted', context), true);
    runInContext('editMaskRectangle(true)', context);
    assert.deepEqual(drawCalls.at(-1), {style: '#000', args: [20, 20, 60, 40]});
    assert.deepEqual(previewCalls.at(-1), {operation: 'clearRect', args: [20, 20, 60, 40]});
});
