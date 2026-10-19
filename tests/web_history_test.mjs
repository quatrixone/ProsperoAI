// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const historyCode = source.slice(source.indexOf('function renderHistory(){'), source.indexOf('function openChat(){'));
const createCode = source.slice(source.indexOf('function newConversation(){'), source.indexOf('function startKind('));
const syncStart = source.indexOf('async function persistConversation(');
const syncCode = source.slice(syncStart, source.indexOf("$('delete-dialog').addEventListener", syncStart));
const mediaStart = source.indexOf('function extractMedia(');
const mediaCode = source.slice(mediaStart, source.indexOf('function decodeTGA(', mediaStart));
function element() {
    return { value: '', textContent: '', children: [], events: {},
        classList: { toggle() {}, remove() {} }, setAttribute() {}, focus() {},
        replaceChildren(...children) { this.children = children; },
        append(...children) { this.children.push(...children); },
        addEventListener(name, callback) { this.events[name] = callback; },
        showModal() { this.open = true; } };
}
function fixture() {
    const elements = new Map();
    const lookup = id => { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); };
    lookup('history-kind').value = 'all';
    const context = { $: lookup, document: { createElement: element },
        conversationMeta: {},
        conversations: [
            { id: 'first', title: 'Text title', model: 'text', messages: [{ content: 'a galaxy of stars' }] },
            { id: 'image', title: 'Mountain', model: 'image', messages: [] },
            { id: 'voice', title: 'Voice', model: 'removed-model', purpose: 'text-to-speech', modelName: 'Saved voice model', messages: [{ content: 'spoken words' }] },
            { id: 'old', title: 'Unclassified old model', model: 'uninstalled', messages: [] }
        ],
        models: [{ id: 'text', purpose: 'text-to-text', name: 'Text model' }, { id: 'image', purpose: 'text-to-image', name: 'Image model' }],
        current: null, controller: null, pendingDelete: '', selectedModel: 'image', currentModelChosen: false,
        api() { return Promise.resolve({}); },
        persistConversation() { return Promise.resolve(); },
        save(key, value) { context.saved = { key, value }; },
        showWelcome() { context.current = null; }, closeHistory() {}, openChat() {}, renderMessages() {} };
    runInNewContext(historyCode + createCode, context);
    return { context, lookup, rows: () => lookup('history').children };
}
test('category, content/model search and filtered open/delete keep conversation identity', async () => {
    const { context: c, lookup: $, rows } = fixture();
    c.renderHistory(); assert.equal(rows().length, 4);
    $('history-search').value = 'STARS'; c.renderHistory();
    assert.equal(rows().length, 1); rows()[0].children[0].onclick(); assert.equal(c.current.id, 'first');
    $('history-kind').value = 'text-to-speech'; $('history-search').value = 'SAVED VOICE'; c.renderHistory();
    assert.equal(rows().length, 1); rows()[0].children[0].onclick(); assert.equal(c.current.id, 'voice');
    rows()[0].children[1].children[1].children[4].onclick(); assert.equal(c.pendingDelete, 'voice');
    $('delete-dialog').returnValue = 'delete'; await $('delete-dialog').events.close();
    assert.equal(c.conversations.length, 3); assert.ok(!c.conversations.some(x => x.id === 'voice'));
    assert.equal(c.saved.value.length, 3);
    assert.ok(c.current); assert.notEqual(c.current.id, 'voice'); assert.equal(c.current.messages.length, 0);
    $('history-kind').value = 'other'; $('history-search').value = ''; c.renderHistory();
    assert.equal(rows().length, 1); assert.equal(rows()[0].children[0].textContent, 'Unclassified old model');
    $('history-search').value = 'does not exist'; c.renderHistory(); assert.equal(rows().length, 0);
    assert.equal($('history-count').textContent, '0 of 3 conversations');
});
test('new conversations wait for an explicit model selection', () => {
    const { context: c, lookup: $, rows } = fixture();
    c.newConversation(); assert.equal(c.current.purpose, 'other'); assert.equal(c.current.modelName, ''); assert.equal(c.current.model, '');
    const id = c.current.id; c.models = [];
    $('history-kind').value = 'other'; c.renderHistory();
    assert.ok(!c.conversations.some(item=>item.id===id));
    assert.equal(c.current.id,id);
});
test('browser history migrates to the console and receives native conversations', async () => {
    const native = { id: 'session-native-1', title: 'From PS5', model: 'text', modelName: 'Text model',
        purpose: 'text-to-text', messages: [{ role: 'user', content: 'Saved on console' }],
        updated_us: 3000000, message_count: 1 };
    const local = { id: 'browser-1', title: 'From web', model: 'text', modelName: 'Text model',
        purpose: 'text-to-text', messages: [{ role: 'user', content: 'Saved in browser' }], updatedAt: 1000 };
    const server = new Map([[native.id, native]]);
    const saved = {};
    const context = { conversations: [local], conversationMeta: {}, conversationSyncReady: false, conversationSyncBusy: false,
        controller: null, current: null, document: { visibilityState: 'visible' },
        save(key, value) { saved[key] = value; }, renderHistory() {}, renderMessages() {}, openChat() {},
        $(id) { return { textContent: '' }; },
        async api(path, options = {}) {
            if (path === '/api/conversations' && options.method === 'POST') {
                const item = JSON.parse(options.body), updated_us = 4000000;
                server.set(item.id, { ...item, updated_us, message_count: item.messages.length });
                return { json: async () => ({ ...server.get(item.id) }) };
            }
            if (path === '/api/conversations')
                return { json: async () => ({ data: [...server.values()].map(({ messages, ...item }) => item) }) };
            const id = decodeURIComponent(path.split('/').at(-1));
            return { json: async () => ({ ...server.get(id) }) };
        }
    };
    await runInNewContext(syncCode + '\nsyncConversations()', context);
    assert.ok(server.has(local.id));
    assert.equal(context.conversations.find(item => item.id === local.id).messages[0].content, 'Saved in browser');
    assert.equal(context.conversations.find(item => item.id === native.id).messages[0].content, 'Saved on console');
    assert.equal(saved['prospero-sync-initialized'], true);
});

test('generated PNG is preferred while saved conversations use their archived image', () => {
    const context = {};
    runInNewContext(mediaCode, context);
    const generated = context.extractMedia(
        'Image generated locally.\nPNG: /download0/prosperoai-image-0.png\nPreview: /download0/prosperoai-image-0.tga');
    assert.equal(generated.path, '/download0/prosperoai-image-0.png');
    const saved = context.extractMedia(
        'PNG: /download0/prosperoai-image-0.png\nPreview: /download0/ProsperoAI/sessions/abc/media/2-image.tga');
    assert.equal(saved.path, '/download0/ProsperoAI/sessions/abc/media/2-image.tga');
});
