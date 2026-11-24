// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
// Behaviour tests for the web string catalog. Run: node tests/web_i18n_test.js [--real-table]
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WEB = path.join(__dirname, '..', 'assets', 'web');
const realTable = process.argv.includes('--real-table');

function memoryStorage(initial) {
  const data = new Map(Object.entries(initial || {}));
  return {
    getItem: key => (data.has(key) ? data.get(key) : null),
    setItem: (key, value) => { data.set(key, String(value)); },
  };
}

function node(attrs, text) {
  const own = { ...attrs };
  return {
    textContent: text,
    getAttribute: name => (name in own ? own[name] : null),
    setAttribute: (name, value) => { own[name] = value; },
    hasAttribute: name => name in own,
  };
}

function page(nodes) {
  return { querySelectorAll: selector => nodes.filter(n => selector.split(',').some(s => n.hasAttribute(s.trim().replace(/^\[|\]$/g, '')))) };
}

function load(storage, document) {
  const context = { localStorage: storage, document };
  context.globalThis = context;
  vm.createContext(context);
  for (const file of ['i18n.js', 'i18n-it.js']) {
    vm.runInContext(fs.readFileSync(path.join(WEB, file), 'utf8'), context, { filename: file });
  }
  return context.i18n;
}

const cases = {
  t_falls_back_to_english_for_unknown_id() {
    const i18n = load(memoryStorage());
    i18n.setLanguage('it');
    assert.strictEqual(i18n.t('no.such.id', 'Plain English'), 'Plain English');
    assert.strictEqual(i18n.t('no.such.id', 'Hello {0}', 'Ada'), 'Hello Ada');
  },
  italian_table_applies_to_marked_nodes() {
    const i18n = load(memoryStorage());
    i18n.register('it', { 'tab.settings': 'Impostazioni' });
    const marked = node({ 'data-i18n': 'tab.settings' }, 'Settings');
    const dom = page([marked]);
    i18n.apply(dom);
    assert.strictEqual(marked.textContent, 'Settings');
    i18n.setLanguage('it');
    i18n.apply(dom);
    i18n.apply(dom);
    assert.strictEqual(marked.textContent, 'Impostazioni');
    i18n.setLanguage('en');
    i18n.apply(dom);
    assert.strictEqual(marked.textContent, 'Settings');
  },
  language_change_keeps_text_written_by_script() {
    const i18n = load(memoryStorage());
    i18n.register('it', { 'status.connecting': 'Connessione…' });
    const status = node({ 'data-i18n': 'status.connecting' }, 'Connecting…');
    const dom = page([status]);
    i18n.apply(dom);
    status.textContent = 'PS5 · Connected';
    i18n.setLanguage('it');
    i18n.apply(dom);
    assert.strictEqual(status.textContent, 'PS5 · Connected');
  },
  language_change_notifies_listeners() {
    const i18n = load(memoryStorage());
    const seen = [];
    i18n.onChange(language => seen.push(language));
    i18n.setLanguage('it');
    assert.throws(() => i18n.setLanguage('xx'), { name: 'RangeError' });
    i18n.setLanguage('en');
    assert.deepStrictEqual(seen, ['it', 'en']);
  },
  attribute_translation() {
    const i18n = load(memoryStorage());
    i18n.register('it', { 'models.search': 'Cerca nei modelli' });
    const field = node({ 'data-i18n-attr': 'placeholder:models.search', placeholder: 'Search models' });
    i18n.setLanguage('it');
    i18n.apply(page([field]));
    assert.strictEqual(field.getAttribute('placeholder'), 'Cerca nei modelli');
  },
  language_survives_reload() {
    const storage = memoryStorage();
    load(storage).setLanguage('it');
    assert.strictEqual(storage.getItem('language'), 'it');
    assert.strictEqual(load(storage).language(), 'it');
    assert.strictEqual(load(memoryStorage()).language(), 'en');
    assert.strictEqual(load(memoryStorage({ language: 'xx' })).language(), 'en');
  },
  html_lang_follows_language() {
    const document = { documentElement: { lang: 'en' } };
    const i18n = load(memoryStorage(), document);
    i18n.setLanguage('it');
    assert.strictEqual(document.documentElement.lang, 'it');
    i18n.setLanguage('en');
    assert.strictEqual(document.documentElement.lang, 'en');
    const stored = { documentElement: { lang: 'en' } };
    load(memoryStorage({ language: 'it' }), stored).apply(page([]));
    assert.strictEqual(stored.documentElement.lang, 'it');
  },
  every_italian_entry_has_matching_placeholders() {
    const i18n = load(memoryStorage());
    i18n.register('it', { 'x.one': 'Ciao {0}, {1}', 'x.two': 'Fatto %s' });
    i18n.setLanguage('it');
    assert.strictEqual(i18n.t('x.one', 'Hi {0}, {1}', 'a', 'b'), 'Ciao a, b');
    assert.strictEqual(i18n.t('x.two', 'Done %s', 'ok'), 'Fatto ok');
    for (const [id, text] of Object.entries(i18n.table('it'))) {
      const holders = new Set(text.match(/\{\d+\}|%s/g) || []);
      const filled = i18n.t(id, 'x', ...Array.from(holders, (_, k) => 'V' + k));
      assert.ok(!/\{\d+\}|%s/.test(filled), id + ' keeps an unresolved placeholder');
    }
  },
  real_table_translates_settings_tab() {
    const i18n = load(memoryStorage());
    i18n.setLanguage('it');
    assert.strictEqual(i18n.t('tab.settings', 'Settings'), 'Impostazioni');
    for (const [id, text] of Object.entries(i18n.table('it'))) {
      assert.ok(text.length > 0, id + ' is empty');
    }
  },
};

let failed = 0;
for (const [name, run] of Object.entries(cases)) {
  if (name.startsWith('real_table') && !realTable) continue;
  try { run(); console.log('ok ' + name); } catch (error) { failed += 1; console.log('FAIL ' + name + ': ' + error.message); }
}
process.exit(failed ? 1 : 0);
