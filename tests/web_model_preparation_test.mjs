// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {test} from 'node:test';

const source = readFileSync(new URL('../assets/web/workspace.js', import.meta.url), 'utf8');
function createMode(calls, prepare) {
    const scope = {globalThis: {}};
    runInNewContext(source, scope);
    return scope.globalThis.ProsperoWorkspace.createWorkspaceMode({
        models: [{id: 'text', purpose: 'text-to-text'}],
        selectModel: async id => calls.push(['select', id]),
        prepareModel: async id => { calls.push(['prepare', id]); await prepare(); },
    });
}

test('failed submit-time preparation can be retried without preparing during staging', async () => {
    const calls = [];
    let attempts = 0;
    const workspace = createMode(calls, async () => {
        if (++attempts === 1) throw new Error('prepare failed');
    });
    workspace.stageModel('text');
    assert.deepEqual(calls, []);
    await assert.rejects(workspace.submit({}, async () => 'sent'), /prepare failed/);
    assert.equal(await workspace.submit({}, async () => 'sent'), 'sent');
    assert.deepEqual(calls.map(call => call[0]), ['select', 'prepare', 'select', 'prepare']);
});
