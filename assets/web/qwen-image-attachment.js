'use strict';

(() => {
    const maxBytes = 16 * 1024 * 1024;

    function validateFile(file) {
        if (!file || !['image/png', 'image/jpeg'].includes(file.type))
            return 'Choose a PNG or JPEG image.';
        if (file.size > maxBytes)
            return 'Images must be 16 MiB or smaller.';
        return null;
    }

    function createAttachmentState() {
        return {
            file: null,
            uploadId: '',
            select(file) {
                this.file = file;
                this.uploadId = '';
            },
            clear() {
                this.file = null;
                this.uploadId = '';
            },
            invalidateUploadId() {
                this.uploadId = '';
            },
        };
    }

    async function uploadImage(file, api) {
        const error = validateFile(file);
        if (error)
            throw new Error(error);
        const response = await api('/api/image/upload', {
            method: 'POST',
            headers: {'Content-Type': 'application/octet-stream'},
            body: file,
        });
        const payload = await response.json();
        if (typeof payload.id !== 'string' || !/^[0-9a-f]{32}$/.test(payload.id))
            throw new Error('The image upload returned an invalid ID.');
        return payload.id;
    }

    async function ensureUploadId(state, api) {
        if (!state.file)
            return '';
        if (!state.uploadId)
            state.uploadId = await uploadImage(state.file, api);
        return state.uploadId;
    }

    globalThis.ProsperoQwenImageAttachment = {
        validateFile,
        createAttachmentState,
        uploadImage,
        ensureUploadId,
    };
})();
