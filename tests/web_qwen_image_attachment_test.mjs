// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {test} from 'node:test';

const moduleSource = readFileSync(new URL('../assets/web/qwen-image-attachment.js', import.meta.url), 'utf8');
const appSource = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../assets/web/index.html', import.meta.url), 'utf8');
const serverSource = readFileSync(new URL('../vulkan/http_server.cpp', import.meta.url), 'utf8');
const scope = {globalThis: {}};
runInNewContext(moduleSource, scope);
const attachmentApi = scope.globalThis.ProsperoQwenImageAttachment;

test('attachment accepts bounded PNG and JPEG files only', () => {
    assert.equal(attachmentApi.validateFile({type: 'image/png', size: 1}), null);
    assert.equal(attachmentApi.validateFile({type: 'image/jpeg', size: 16 * 1024 * 1024}), null);
    assert.match(attachmentApi.validateFile({type: 'image/gif', size: 10}), /PNG or JPEG/);
    assert.match(attachmentApi.validateFile({type: 'image/png', size: 16 * 1024 * 1024 + 1}), /16 MiB/);
});

test('selecting a replacement resets its upload ID and clear removes both values', () => {
    const attachment = attachmentApi.createAttachmentState();
    const first = {type: 'image/png', size: 1};
    const replacement = {type: 'image/jpeg', size: 2};
    attachment.select(first);
    attachment.uploadId = 'first-upload';
    attachment.select(replacement);
    assert.equal(attachment.file, replacement);
    assert.equal(attachment.uploadId, '');
    attachment.clear();
    assert.equal(attachment.file, null);
    assert.equal(attachment.uploadId, '');
});

test('upload uses the authenticated API helper and keeps the upload ID for retry', async () => {
    const attachment = attachmentApi.createAttachmentState();
    const file = {type: 'image/png', size: 7, bytes: new Uint8Array([1, 2, 3])};
    attachment.select(file);
    const calls = [];
    const api = async (path, options) => {
        calls.push([path, options]);
        return {json: async () => ({id: 'a'.repeat(32)})};
    };
    const uploadId = await attachmentApi.ensureUploadId(attachment, api);
    await assert.rejects(async () => {
        throw new Error('generation failed');
    }, /generation failed/);
    assert.equal(await attachmentApi.ensureUploadId(attachment, api), uploadId);
    assert.equal(calls.length, 1);
    assert.equal(calls[0][0], '/api/image/upload');
    assert.equal(calls[0][1].method, 'POST');
    assert.equal(calls[0][1].body, file);
    assert.equal(calls[0][1].headers['Content-Type'], 'application/octet-stream');
});

test('a lost generation response invalidates only the upload ID for retry', async () => {
    const attachment = attachmentApi.createAttachmentState();
    const file = {type: 'image/png', size: 7};
    attachment.select(file);
    let uploads = 0;
    const api = async () => ({json: async () => ({id: (++uploads).toString(16).padStart(32, '0')})});

    await attachmentApi.ensureUploadId(attachment, api);
    attachment.invalidateUploadId();
    assert.equal(attachment.file, file);
    assert.equal(attachment.uploadId, '');
    assert.equal(await attachmentApi.ensureUploadId(attachment, api), '0'.repeat(31) + '2');
    assert.equal(uploads, 2);
    assert.match(appSource, /imageId&&\(error\.name==='AbortError'\|\|error instanceof TypeError\)[\s\S]{0,100}invalidateUploadId/);
});

test('composer markup is accessible and loads the attachment module before app code', () => {
    assert.match(htmlSource, /<input[^>]+id="qwen-image-file"[^>]+type="file"[^>]+accept="image\/png,image\/jpeg"[^>]+aria-label="Attach a reference image"/);
    assert.match(htmlSource, /id="qwen-image-attach"[^>]+aria-label="Attach a reference image"/);
    assert.match(htmlSource, /id="qwen-image-remove"[^>]+aria-label="Remove reference image"/);
    assert.match(htmlSource, /id="qwen-image-replace"[^>]+aria-label="Replace reference image"/);
    assert.match(htmlSource, /<script src="\/qwen-image-attachment\.js" defer><\/script>[\s\S]*?<script src="\/app\.js" defer><\/script>/);
    assert.match(serverSource, /\{"\/qwen-image-attachment\.js", "qwen-image-attachment\.js", "text\/javascript; charset=utf-8"\}/);
    assert.match(appSource, /requestBody\.image_id=imageId;requestBody\.conversation_id=conversation\.id/);
    assert.match(appSource, /userMessage\.image=responseMessage\.image/);
    assert.match(appSource, /message\.image/);
});
