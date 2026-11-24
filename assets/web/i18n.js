// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
// String catalog for the web page. English is the source text and the fallback;
// other languages register a table of {id: text}. The language lives in
// localStorage["language"].
(function () {
  const STORAGE_KEY = 'language';
  const SUPPORTED = ['en', 'it'];
  const tables = Object.create(null);
  const ORIGINAL = '_i18nEnglish';
  const SHOWN = '_i18nShown';
  const listeners = [];

  function readStorage() {
    try { return globalThis.localStorage ? globalThis.localStorage.getItem(STORAGE_KEY) : null; } catch { return null; }
  }
  let current = SUPPORTED.includes(readStorage()) ? readStorage() : 'en';

  // Keep <html lang> in step with the language, for screen readers and hyphenation.
  function syncHtmlLang() {
    const root = globalThis.document && globalThis.document.documentElement;
    if (root) root.lang = current;
  }

  function fill(text, args) {
    return args.length ? text.replace(/\{(\d+)\}|%s/g, (hole, index) => {
      const position = index === undefined ? 0 : Number(index);
      return position < args.length ? String(args[position]) : hole;
    }) : text;
  }

  function lookup(id) {
    const table = tables[current];
    return table && Object.prototype.hasOwnProperty.call(table, id) ? table[id] : null;
  }

  function t(id, english, ...args) {
    const found = lookup(id);
    return fill(found === null ? english : found, args);
  }

  function register(language, table) {
    tables[language] = { ...(tables[language] || {}), ...table };
  }

  function setLanguage(language) {
    if (!SUPPORTED.includes(language)) throw new RangeError('unsupported language: ' + language);
    current = language;
    syncHtmlLang();
    try { if (globalThis.localStorage) globalThis.localStorage.setItem(STORAGE_KEY, language); } catch { /* storage unavailable: keep it for this page only */ }
    for (const listener of listeners) listener(language);
  }

  // Lets a page redraw the text it builds in script after the language changes.
  function onChange(listener) { listeners.push(listener); }

  // The English text is captured the first time a node is seen, so apply() can
  // be called again, in any language, without losing the source text.
  // Known limit: a script that rewrites a marked node with exactly the English
  // text it already shows is indistinguishable from untouched text, so that
  // node is treated as live state and is not translated again; scripts that
  // draw such text should call i18n.t() themselves and redraw from onChange().
  function apply(root) {
    syncHtmlLang();
    for (const element of root.querySelectorAll('[data-i18n]')) {
      if (element[ORIGINAL] === undefined) element[ORIGINAL] = element.textContent;
      // Text that a script wrote after our last pass is live state: leave it alone.
      if (element[SHOWN] !== undefined && element.textContent !== element[SHOWN]) continue;
      element[SHOWN] = t(element.getAttribute('data-i18n'), element[ORIGINAL]);
      element.textContent = element[SHOWN];
    }
    for (const element of root.querySelectorAll('[data-i18n-attr]')) {
      const originals = element[ORIGINAL + 'Attr'] || (element[ORIGINAL + 'Attr'] = {});
      for (const pair of element.getAttribute('data-i18n-attr').split(';')) {
        const [name, id] = pair.split(':').map(part => part.trim());
        if (!name || !id) continue;
        if (originals[name] === undefined) originals[name] = element.getAttribute(name) || '';
        element.setAttribute(name, t(id, originals[name]));
      }
    }
  }

  const api = { t, register, setLanguage, language: () => current, apply, onChange, table: language => ({ ...(tables[language] || {}) }) };
  globalThis.i18n = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})();
