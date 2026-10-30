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
    const QWEN_IMAGE_MODEL = 'qwen-image-2.1';
    const AUDIO_DURATION_MODELS = new Set([
        'ace-step-1.5-turbo-q8-0',
        'stable-audio-3-small-sfx-vulkan',
        'yue2-3b-q8-0',
    ]);

    function purposeFor(mode, section) {
        return mode === 'audio' ? AUDIO_PURPOSE[section] : MODE_PURPOSE[mode];
    }

    function filterModels(models, mode, section = 'music') {
        const purpose = purposeFor(mode, section);
        if (!purpose) return [];
        return models.filter(model => model.purpose === purpose &&
            (mode !== 'image' || section !== 'image-to-image' || model.id === QWEN_IMAGE_MODEL));
    }

    function ttsAvailability(models, status) {
        if (status !== 'ready') return status;
        return filterModels(models, 'audio', 'tts').length ? 'available' : 'empty';
    }

    function imageToImageAvailability(models, status) {
        if (status !== 'ready') return status;
        return filterModels(models, 'image', 'image-to-image').length ? 'available' : 'empty';
    }

    function imageReferenceRequired(mode, section) {
        return mode === 'image' && section === 'image-to-image';
    }

    function imageReferenceMissing(mode, section, file) {
        return imageReferenceRequired(mode, section) && !file;
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
        ttsAvailability,
        imageToImageAvailability,
        imageReferenceRequired,
        imageReferenceMissing,
        workspaceModelControlsVisible,
        audioDurationAvailable,
        withInstalledMusicModel,
        createWorkspaceMode,
    };
})(globalThis);
