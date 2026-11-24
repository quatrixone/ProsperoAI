// SPDX-License-Identifier: GPL-3.0-or-later
// Tests run slices of the web scripts in bare vm contexts, where the page's
// i18n global does not exist. This gives them the catalog's English fallback.
import { runInNewContext as vmRun } from 'node:vm';

export const englishI18n = {
    t: (_id, english, ...args) => english.replace(/\{(\d+)\}/g, (hole, index) => (Number(index) < args.length ? String(args[Number(index)]) : hole)),
    language: () => 'en',
};

export function runInNewContext(code, scope = {}, ...rest) {
    if (scope.i18n === undefined) scope.i18n = englishI18n;
    return vmRun(code, scope, ...rest);
}
