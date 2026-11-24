// SPDX-License-Identifier: GPL-3.0-or-later
// The scope checkboxes in Settings must agree with the engine's own scope order,
// because what travels to the console is a bitmask, not names.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from './english_i18n.mjs';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const code = source.slice(source.indexOf('const SEARCH_SCOPES='),
                          source.indexOf('let runtimeSettingsQueue='));

function fixture(mask) {
    const boxes = {};
    const scope = {
        $: id => boxes[id],
        boxes,
    };
    runInNewContext(
        code + '\nthis.api={SEARCH_SCOPES,searchScopeMask,searchScopeFlags,applySearchScopeMask,currentSearchScopeMask};',
        scope,
    );
    for (const [id] of scope.api.SEARCH_SCOPES) boxes['search-scope-' + id] = { checked: false };
    if (mask !== undefined) scope.api.applySearchScopeMask(mask);
    return scope.api;
}

test('the scope order matches the engine, so 511 is every group', () => {
    const api = fixture();
    // The arrays come from the VM's own realm, so compare their contents.
    assert.equal(api.SEARCH_SCOPES.map(([id]) => id).join(','),
        'web,code,video,reference,qa,package,paper,music,image');
    assert.equal(api.searchScopeMask(Object.fromEntries(api.SEARCH_SCOPES.map(([id]) => [id, true]))), 511);
    assert.equal(api.searchScopeMask({}), 0);
});

test('a mask round-trips through the checkboxes', () => {
    const api = fixture();
    // web and code are bits 0 and 1; reference is bit 3.
    api.applySearchScopeMask(0b1011);
    assert.equal(api.currentSearchScopeMask(), 0b1011);
    const flags = api.searchScopeFlags(0b1011);
    assert.equal(flags.web, true);
    assert.equal(flags.code, true);
    assert.equal(flags.video, false);
    assert.equal(flags.reference, true);
    assert.equal(flags.image, false);
});

test('clearing every box reports zero, which the UI refuses to send', () => {
    const api = fixture(511);
    api.applySearchScopeMask(0);
    assert.equal(api.currentSearchScopeMask(), 0);
});
