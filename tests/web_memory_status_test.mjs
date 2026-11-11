// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { test } from 'node:test';

const source = readFileSync(new URL('../assets/web/app.js', import.meta.url), 'utf8');
const start = source.indexOf('async function refreshMemoryModel(){');
const end = source.indexOf('\nasync function refreshSelectedModel()', start);
assert.notEqual(start, -1);
assert.notEqual(end, -1);

function memoryPoll(payload) {
    const element = { textContent: '', title: '' };
    const scope = {
        $: id => id === 'memory-model' ? element : null,
        api: async () => ({ json: async () => payload }),
        document: { visibilityState: 'visible' },
        memoryRefreshBusy: false,
        preparedModel: 'prepared-model',
        Number,
    };
    runInNewContext(source.slice(start, end) + '\nthis.refresh=refreshMemoryModel;', scope);
    return { refresh: scope.refresh, element, scope };
}

test('web memory header shows only valid device-wide allocation for a loaded model', async () => {
    const { refresh, element } = memoryPoll({
        loaded: true,
        id: 'tiny.gguf',
        name: 'Tiny model',
        vulkan_memory_available: true,
        vulkan_free_bytes: 6 * 1024 ** 3,
        vulkan_allocated_bytes: 4 * 1024 ** 3,
        vulkan_heap_bytes: 10 * 1024 ** 3,
    });
    await refresh();
    assert.equal(element.textContent,
        'Loaded: Tiny model · 4.0 / 10.0 GiB allocated (device-wide)');
    assert.match(element.title, /device-wide/i);
});

test('web memory header never renders unavailable allocation as zero', async () => {
    const { refresh, element } = memoryPoll({
        loaded: true,
        id: 'tiny.gguf',
        name: 'Tiny model',
        vulkan_memory_available: false,
        vulkan_free_bytes: null,
        vulkan_allocated_bytes: null,
        vulkan_heap_bytes: 0,
    });
    await refresh();
    assert.equal(element.textContent, 'Loaded: Tiny model · Vulkan allocation unavailable');
    assert.doesNotMatch(element.textContent, /0(?:\.0)? GiB/);
});

test('web memory header restores the existing no-model message after unload', async () => {
    const { refresh, element, scope } = memoryPoll({
        loaded: false,
        vulkan_memory_available: true,
        vulkan_free_bytes: 6 * 1024 ** 3,
        vulkan_allocated_bytes: 4 * 1024 ** 3,
        vulkan_heap_bytes: 10 * 1024 ** 3,
    });
    await refresh();
    assert.equal(element.textContent, 'Memory: no model loaded');
    assert.equal(scope.preparedModel, '');
});
