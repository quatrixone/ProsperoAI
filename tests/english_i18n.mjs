// SPDX-License-Identifier: GPL-3.0-or-later
// Tests run slices of the web scripts in bare vm contexts, where the page's
// i18n global does not exist. This loads the real i18n.js (without a table),
// so tests get the catalog's own English fallback and placeholder handling.
import { readFileSync } from 'node:fs';
import { runInNewContext as vmRun } from 'node:vm';

const here = new URL('.', import.meta.url);
const source = readFileSync(new URL('../assets/web/i18n.js', here), 'utf8');
const sandbox = { globalThis: {}, module: { exports: {} } };
sandbox.globalThis = sandbox;
vmRun(source, sandbox, { filename: 'i18n.js' });
export const englishI18n = sandbox.module.exports;

export function runInNewContext(code, scope = {}, ...rest) {
    if (scope.i18n === undefined) scope.i18n = englishI18n;
    return vmRun(code, scope, ...rest);
}
