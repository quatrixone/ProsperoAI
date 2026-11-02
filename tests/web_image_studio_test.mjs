// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
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
        showStrength: false, showControlStrength: false,
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
            strength: 0.6, sampler: 'euler_a', scheduler: 'ays', negative_prompt: 'blur',
            image_id: 'b'.repeat(32),
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
        /generation failed/);
});

test('Image Create, Models, and Gallery navigation reuses existing pages and model catalog', () => {
    assert.match(htmlSource, /data-image-destination="create"/);
    assert.match(htmlSource, /data-image-destination="models"/);
    assert.match(htmlSource, /data-image-destination="gallery"/);
    assert.match(htmlSource, /data-kind="1"/);
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
