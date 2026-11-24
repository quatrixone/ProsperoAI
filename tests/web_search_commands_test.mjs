// SPDX-License-Identifier: GPL-3.0-or-later
// The /search and /fetch commands in the web conversation: what the model ends up
// seeing, and what is refused before anything is sent to the console.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from './english_i18n.mjs';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const code = source.slice(source.indexOf('async function prosperoPrepareSearch('),
                          source.indexOf('const $ = id => document.getElementById(id);'));

function fixture(payloads) {
    const calls = [];
    const scope = {
        api: async (path, options) => {
            calls.push({ path, body: JSON.parse(options.body) });
            const payload = payloads[path];
            if (!payload) throw Error('unexpected route ' + path);
            return { json: async () => payload };
        },
        Error,
    };
    runInNewContext(code + '\nthis.api_={prosperoPrepareSearch};', scope);
    return { prepare: scope.api_.prosperoPrepareSearch, calls };
}

const history = [{ role: 'user', content: '/search vulkan memory' }];

test('a search puts the results in a system message and leaves the query as the prompt', async () => {
    const { prepare, calls } = fixture({
        '/api/search': { data: [
            { title: 'VMA', url: 'https://github.com/x/vma', snippet: 'allocator', source: 'code.github' },
        ] },
    });
    const prepared = await prepare('/search vulkan memory', history, true);
    assert.equal(calls[0].path, '/api/search');
    assert.equal(calls[0].body.query, 'vulkan memory');
    assert.equal(prepared.length, 2);
    assert.equal(prepared[0].role, 'system');
    assert.match(prepared[0].content, /untrusted data/);
    assert.match(prepared[0].content, /https:\/\/github.com\/x\/vma/);
    // Which provider found a result is part of what the model is told.
    assert.match(prepared[0].content, /source: code\.github/);
    assert.equal(prepared[1].content, 'vulkan memory');
});

test('a fetch sends the address and hands the page text to the model', async () => {
    const { prepare, calls } = fixture({
        '/api/fetch': { url: 'https://example.org/a', text: 'Readable page text', bytes: 18 },
    });
    const prepared = await prepare('/fetch https://example.org/a', [{ role: 'user', content: 'x' }], true);
    assert.equal(calls[0].path, '/api/fetch');
    assert.equal(calls[0].body.url, 'https://example.org/a');
    assert.match(prepared[0].content, /untrusted data/);
    assert.match(prepared[0].content, /Readable page text/);
    assert.equal(prepared[1].content, 'https://example.org/a');
});

test('ordinary messages are passed through untouched', async () => {
    const { prepare, calls } = fixture({});
    const messages = [{ role: 'user', content: 'tell me a story' }];
    assert.equal(await prepare('tell me a story', messages, true), messages);
    assert.equal(calls.length, 0);
});

test('bad commands are refused before the console is asked', async () => {
    const { prepare, calls } = fixture({});
    await assert.rejects(() => prepare('/search', history, true), /followed by your query/);
    await assert.rejects(() => prepare('/fetch', history, true), /followed by a page address/);
    await assert.rejects(() => prepare('/fetch not-a-url', history, true), /http or https address/);
    await assert.rejects(() => prepare('/search x', history, true), /2–256 characters/);
    await assert.rejects(() => prepare('/search vulkan', history, false), /text conversations only/);
    assert.equal(calls.length, 0);
});

test('a search that found nothing is reported instead of prompting the model blind', async () => {
    const { prepare } = fixture({ '/api/search': { data: [] } });
    await assert.rejects(() => prepare('/search vulkan memory', history, true), /returned a usable result/i);
});
