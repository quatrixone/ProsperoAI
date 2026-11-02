// SPDX-License-Identifier: GPL-3.0-or-later
(function (root) {
    'use strict';

    const MODE_PURPOSE = {
        text: 'text-to-text',
        image: 'text-to-image',
    };
    const AUDIO_PURPOSE = {
        music: 'text-to-audio',
        tts: 'text-to-speech',
    };
    const AUDIO_DURATION_MODELS = new Set([
        'ace-step-1.5-turbo-q8-0',
        'stable-audio-3-small-sfx-vulkan',
        'yue2-3b-q8-0',
    ]);

    function purposeFor(mode, section) {
        return mode === 'audio' ? AUDIO_PURPOSE[section] : MODE_PURPOSE[mode];
    }

    function imageCapabilities(model) {
        return Array.isArray(model?.details?.capabilities) ? model.details.capabilities : [];
    }

    function imageModes(model) {
        return ['text_to_image', 'image_to_image', 'inpainting', 'controlnet']
            .filter(mode => imageCapabilities(model).includes(mode));
    }

    function imageSettings(model, mode, values = {}) {
        const defaults = model?.details?.image_defaults || {};
        const strategy = model?.details?.image_edit_strategy;
        return {
            width: values.width ?? defaults.width ?? 512,
            height: values.height ?? defaults.height ?? 512,
            steps: values.steps ?? defaults.steps ?? 1,
            cfg_scale: values.cfg_scale ?? defaults.cfg_scale ?? 1,
            sampler: values.sampler ?? defaults.sampler ?? 'euler_a',
            scheduler: values.scheduler ?? defaults.scheduler ?? '',
            ...(strategy === 'init_image_strength' && ['image_to_image', 'inpainting'].includes(mode)
                ? {strength: values.strength ?? 0.75} : {}),
            ...(mode === 'controlnet' ? {control_strength: values.control_strength ?? 1, canny: values.canny ?? true} : {}),
            showStrength: strategy === 'init_image_strength' && ['image_to_image', 'inpainting'].includes(mode),
            showControlStrength: mode === 'controlnet',
        };
    }

    function validateImageInputs(mode, uploads = {}) {
        if (['image_to_image', 'inpainting'].includes(mode) && !uploads.image) return 'Choose an image to continue.';
        if (mode === 'inpainting' && !uploads.mask) return 'Paint or choose an inpainting mask.';
        if (mode === 'controlnet' && !uploads.guide) return 'Choose a guide image to continue.';
        return '';
    }

    async function uploadImageInput(api, file) {
        if (!file || !['image/png', 'image/jpeg'].includes(file.type) || file.size < 1 || file.size > 16 * 1024 * 1024)
            throw new Error('Choose a PNG or JPEG image no larger than 16 MiB.');
        const response = await api('/api/image/upload', {
            method: 'POST', body: file, headers: {'Content-Type': file.type},
        });
        const {id} = await response.json();
        if (!/^[0-9a-f]{32}$/.test(id || '')) throw new Error('The image upload returned an invalid upload ID.');
        return id;
    }

    function buildImageRequest({model, mode, prompt, values = {}, uploads = {}, sessionId, messageIndex}) {
        const workflow = mode;
        if (!imageModes(model).includes(workflow)) throw new Error('This model does not support the selected image mode.');
        if (!String(prompt || '').trim()) throw new Error('Enter a prompt before generating.');
        const missing = validateImageInputs(workflow, uploads);
        if (missing) throw new Error(missing);
        const settings = imageSettings(model, workflow, values);
        const {showStrength, showControlStrength, ...options} = settings;
        if (!Number.isInteger(options.width) || !Number.isInteger(options.height) ||
            options.width < 256 || options.width > 1536 || options.height < 256 || options.height > 1536 ||
            options.width % 64 || options.height % 64 || options.width * options.height > 1048576 ||
            !Number.isInteger(options.steps) || options.steps < 1 || options.steps > 50 ||
            !Number.isFinite(options.cfg_scale) || options.cfg_scale < 0 || options.cfg_scale > 20)
            throw new Error('Image dimensions, steps, or guidance are outside the supported range.');
        if (showStrength && (!Number.isFinite(options.strength) || options.strength < 0 || options.strength > 1))
            throw new Error('Strength must be between 0 and 1.');
        if (showControlStrength && (!Number.isFinite(options.control_strength) || options.control_strength < 0 || options.control_strength > 2))
            throw new Error('Control strength must be between 0 and 2.');
        const image_workflow = {workflow, ...options, negative_prompt: String(values.negative_prompt || '')};
        if (showStrength) image_workflow.strength = options.strength;
        if (showControlStrength) {
            image_workflow.control_strength = options.control_strength;
            image_workflow.canny = options.canny;
        }
        if (Number.isInteger(values.seed) && values.seed >= -1 && values.seed <= 4294967295)
            image_workflow.seed = values.seed;
        if (uploads.image) image_workflow.image_id = uploads.image;
        if (uploads.mask) image_workflow.mask_id = uploads.mask;
        if (uploads.guide) image_workflow.guide_id = uploads.guide;
        const request = {async: true, model: model.id, prompt: prompt.trim(), image_workflow};
        if (sessionId) {
            request.output_session_id = sessionId;
            request.output_message_index = messageIndex;
        }
        return request;
    }

    async function waitForImageJob(api, id, {delay = 1000} = {}) {
        for (;;) {
            const job = await api(`/api/jobs?id=${encodeURIComponent(id)}`);
            if (job.state === 'running') {
                await new Promise(resolve => setTimeout(resolve, delay));
                continue;
            }
            if (job.state !== 'done' || job.result?.error)
                throw new Error(job.result?.error || 'Image generation failed.');
            return job.result;
        }
    }

    function filterModels(models, mode, section = 'music') {
        const purpose = purposeFor(mode, section);
        if (!purpose) return [];
        return models.filter(model => model.purpose === purpose &&
            (mode !== 'image' || section !== 'image-to-image' ||
                imageCapabilities(model).includes('image_to_image')));
    }

    function ttsAvailability(models, status) {
        if (status !== 'ready') return status;
        return filterModels(models, 'audio', 'tts').length ? 'available' : 'empty';
    }

    function imageToImageAvailability(models, status) {
        if (status !== 'ready') return status;
        return filterModels(models, 'image', 'image-to-image').length ? 'available' : 'empty';
    }

    function workspaceModelControlsVisible(mode, section) {
        return mode !== 'audio' || section !== 'music';
    }

    function audioDurationAvailable(model) {
        return model?.purpose === AUDIO_PURPOSE.music && AUDIO_DURATION_MODELS.has(model.id);
    }

    function withInstalledMusicModel(models, selectedId, queuedId, submit) {
        const modelId = queuedId === undefined ? selectedId : queuedId;
        const model = (models || []).find(item =>
            item.id === modelId && item.purpose === AUDIO_PURPOSE.music);
        if (!model) {
            const error = new Error('Choose an installed music model before generating.');
            error.code = 'MUSIC_MODEL_UNAVAILABLE';
            return Promise.reject(error);
        }
        return Promise.resolve().then(() => submit(model));
    }

    function createWorkspaceMode({models = [], selectModel, prepareModel} = {}) {
        let mode = 'text';
        let audioSection = 'music';
        let stagedModel = '';
        let options = {};
        return {
            get mode() { return mode; },
            get audioSection() { return audioSection; },
            get model() { return stagedModel; },
            setModels(value) { models = value; },
            setMode(value) {
                if (value !== 'audio' && !MODE_PURPOSE[value]) throw new Error('Unknown workspace mode.');
                mode = value;
            },
            setAudioSection(value) {
                if (!AUDIO_PURPOSE[value]) throw new Error('Unknown Audio section.');
                audioSection = value;
            },
            stageModel(id) { stagedModel = id || ''; },
            setOptions(value) { options = {...value}; },
            async submit(payload, send) {
                const selected = models.find(item => item.id === stagedModel);
                if (!selected || selected.purpose !== purposeFor(mode, audioSection))
                    throw new Error(`Choose an installed ${mode} model before submitting.`);
                await selectModel(stagedModel);
                await prepareModel(stagedModel);
                return send({...options, ...(payload || {}), model: stagedModel});
            },
        };
    }

    root.ProsperoWorkspace = {
        filterModels,
        imageCapabilities,
        imageModes,
        imageSettings,
        validateImageInputs,
        uploadImageInput,
        buildImageRequest,
        waitForImageJob,
        ttsAvailability,
        imageToImageAvailability,
        workspaceModelControlsVisible,
        audioDurationAvailable,
        withInstalledMusicModel,
        createWorkspaceMode,
    };
})(globalThis);
