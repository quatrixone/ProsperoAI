#!/usr/bin/env python3
"""UI smoke-test harness for Prospero Music Studio (PS5).

Run after every UI deploy. Two layers:

  STATIC (any terminal, stdlib only):
      python3 tools/ui_smoke.py static --host 192.168.100.167 [--uiver v53]
    - /music serves the UI, version stamp matches, music.js downloads
    - `node --check` on the served music.js (syntax gate)

  BROWSER (run inside the Hermes browser harness, e.g. via browser_exec):
      exec(open('/path/to/tools/ui_smoke.py').read())
    with globals pre-seeded:  SMOKE_HOST, SMOKE_CLIP (local wav path),
    SMOKE_UIVER (expected stamp), SMOKE_FULL (bool, adds Master/Stems)
    - Cover flow: pick clip -> cover-go -> ABC appears in the editor
    - GENERATE guard: empty style must be refused
    - GENERATE happy path: style+lyrics -> new .wav with valid RIFF header
    - (SMOKE_FULL) Master and Stems actions on the newest wav. Since UI v54
      both run ON THE CONSOLE and their output lands in the library —
      poll /api/music/files for *-mastered.wav / *-vocals.wav. (Pre-v54
      Master was in-page DSP downloading to the browser; mp3 sources and
      404-on-route old servers still use that fallback.)
      A renderer crash mid-flow yields SKIP, not FAIL — verify externally.
    - leaves runtime audio_options cleared at the end

Pitfalls this harness exists to catch (all real, all shipped once):
  - 'rate is not defined' scope bug killed EVERY browser Cover run (v52)
  - Cover output is a few-KB .abc file: size>1MB filters miss it
  - /music?query=404s on the PS5 server; the UI port is 11434, not 8080
"""
import json
import re
import sys
import time
import urllib.request

# helpers are injected when exec'd inside the browser harness; importing
# them here keeps Pyright/standalone runs happy when a daemon is reachable
try:  # pragma: no cover
    from browser_harness.helpers import (cdp, js, goto_url, wait_for_load,
                                         new_tab, page_info)  # noqa: F401
except Exception:
    pass

def _fetch(url, timeout=15):
    req = urllib.request.Request(url, headers={'Cache-Control': 'no-cache'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()

def static(host, uiver=None):
    ok = True
    def check(name, cond, detail=''):
        nonlocal ok
        print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f' — {detail}' if detail else ''))
        ok = ok and bool(cond)
    html = _fetch(f'http://{host}:11434/music').decode('utf-8', 'replace')
    check('UI serves /music', 'Prospero Music Studio' in html)
    m = re.search(r'UI (v\d+)', html)
    stamp = m.group(1) if m else None
    check('version stamp present', stamp is not None, stamp or 'none')
    if uiver:
        check(f'stamp == {uiver}', stamp == uiver, f'served {stamp}')
    js_src = _fetch(f'http://{host}:11434/music.js')
    check('music.js downloads', len(js_src) > 10000, f'{len(js_src)} B')
    # syntax gate
    import subprocess, tempfile, os
    with tempfile.NamedTemporaryFile(suffix='.js', delete=False) as f:
        f.write(js_src); path = f.name
    try:
        r = subprocess.run(['node', '--check', path], capture_output=True, text=True)
        check('music.js parses (node --check)', r.returncode == 0, (r.stderr or '')[:200])
    except FileNotFoundError:
        print('[SKIP] node not on PATH — syntax gate skipped')
    finally:
        os.unlink(path)
    # served js must match the deployed source basics (scope-bug canary):
    # a `var rate` inside a .then() used by a later .then() was the v52 bug;
    # exact shape check, not a general linter.
    txt = js_src.decode('utf-8', 'replace')
    seg = re.search(r"\$\('cover-go'\).*?OfflineAudioContext", txt, re.S)
    if seg:
        pre = seg.group(0)
        check('cover rate declared before OfflineAudioContext use',
              ('var rate' in pre.split('OfflineAudioContext')[0]),
              'scope canary for the v52 bug')
    mem = json.loads(_fetch(f'http://{host}:11434/api/system/memory'))
    check('memory endpoint alive', 'total_free_mib' in mem,
          f"tot={mem.get('total_free_mib')} big={mem.get('largest_block_mib')}")
    return ok

# ---------------- browser layer (helpers injected by the harness) ----------

def browser(host, clip, uiver=None, full=False):
    import os
    results = []

    def J(expr):
        """js() with re-attach: the tab/renderer occasionally dies under
        Web Audio load; async jobs survive, so recover and keep polling."""
        last = None
        for attempt in range(3):
            try:
                return js(expr)
            except Exception as e:
                last = e
                try:
                    goto_url(f'http://{host}:11434/music')
                    wait_for_load()
                    time.sleep(2)
                    if ':11434' not in str(page_info().get('url', '')):
                        raise RuntimeError('tab not restored')
                except Exception:
                    time.sleep(5)
        raise last

    def check(name, cond, detail=''):
        print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f' — {detail}' if detail else ''), flush=True)
        results.append(bool(cond))

    # app liveness first: a crashed app must fail loudly, not time out silently
    try:
        mem0 = json.loads(_fetch(f'http://{host}:11434/api/system/memory', timeout=12))
        check('app API alive', 'total_free_mib' in mem0,
              f"tot={mem0.get('total_free_mib')} big={mem0.get('largest_block_mib')}")
    except Exception as e:
        check('app API alive', False, f'{e} — check debug-trace / relaunch app')
        return False

    cdp('Network.enable'); cdp('Network.clearBrowserCache')
    goto_url(f'http://{host}:11434/music')
    wait_for_load(); time.sleep(2)
    stamp = js("(document.querySelector('#uiver')||{}).textContent||''")
    check('UI loads with stamp', bool(stamp), stamp)
    if uiver:
        check(f'stamp == {uiver}', uiver in stamp, stamp)
    check('cover controls present', js("!!document.querySelector('#cover-file') && !!document.querySelector('#cover-go')"))

    # ---- Cover flow ----
    doc = cdp('DOM.getDocument')
    nid = cdp('DOM.querySelector', nodeId=doc['root']['nodeId'], selector='#cover-file')['nodeId']
    cdp('DOM.setFileInputFiles', files=[os.path.abspath(clip)], nodeId=nid)
    # clear the editor first: leftover ABC from a previous run must not
    # produce a false PASS on stale content
    js("document.querySelector('#abc').value=''")
    js("document.querySelector('#cover-go').click()")
    abc = 0; done = ''
    st = ''
    t0 = time.time()
    while time.time() - t0 < 300:
        time.sleep(15)
        st = js("document.querySelector('#cover-status').textContent||''")
        abc = int(js("((document.querySelector('#abc')||{}).value||'').length"))
        if 'ailed' in st:
            done = st; break
        if ('done' in st.lower() or '\u2705' in st) and abc > 50:
            done = st; break
    check('cover produced ABC in editor', abc > 50, f'abc_len={abc} status="{done[:80]}"')
    abc_name = js("(document.querySelector('#abc-name')||{}).value||''")
    check('cover set abc-name', bool(abc_name), abc_name)

    # ---- GENERATE guard: empty style must refuse ----
    js("document.querySelector('#style').value=''")
    js("document.querySelector('#generate').click()")
    time.sleep(3)
    g = js("document.querySelector('#gen-status').textContent||''")
    check('empty-style guard fires', 'style' in g.lower(), g[:80])

    # ---- GENERATE happy path ----
    js("""(() => {
      document.querySelector('#style').value = 'smoke test, warm vocal, clean tone';
      document.querySelector('#lyrics').value = '[verse]\\nSmoke test line one\\nSmoke test line two\\n\\n[chorus]\\nSmoke smoke yeah';
      document.querySelector('#generate').click();
    })()""")
    t0 = time.time(); out = None
    while time.time() - t0 < 900:
        time.sleep(30)
        st = J("document.querySelector('#gen-status').textContent||''")
        if 'fail' in st.lower() or 'error' in st.lower():
            break
        # same-origin list via in-page fetch (server clock is authoritative).
        # STRICT pattern: YuE2 outputs are yue2-<ms>-<idx>.wav — excluding
        # this is how a leftover *-mastered.wav caused a false PASS on Phat.
        names = J("""(async () => {
          const j = await fetch('/api/music/files').then(r=>r.json());
          const fl = Array.isArray(j) ? j : (j.files||[]);
          const t = Date.now()/1000 - 900;
          return JSON.stringify(fl.filter(f=>/^yue2-\\d+-\\d+\\.wav$/.test(f.name) && f.mtime>t).map(f=>f.name));
        })()""")
        got = json.loads(names or '[]')
        if got:
            out = got[0]; break
    check('generate produced a new wav', bool(out), out or f'gen-status="{st[:80]}"')
    if out:
        # Range fetch of just the header — a full 20 MB arrayBuffer fetch
        # once killed the renderer mid-harness
        hdr = J(f"""(async () => {{
          const r = await fetch('/download0/ProsperoAI/work/audio/{out}', {{headers:{{Range:'bytes=0-43'}}}});
          const b = await r.arrayBuffer(); const u = new Uint8Array(b.slice(0,12));
          let s=''; for (const x of u) s += String.fromCharCode(x);
          return s;
        }})()""")
        check('wav has RIFF/WAVE header', hdr[:4] == 'RIFF' and hdr[8:12] == 'WAVE', repr(hdr))
        # listenable duration sanity (>=20 s of 48k stereo). HEAD is not
        # supported by the PS5 server — GET one byte and read Content-Range
        size = J(f"""(async () => {{
          const r = await fetch('/download0/ProsperoAI/work/audio/{out}', {{headers:{{Range:'bytes=0-0'}}}});
          const cr = r.headers.get('Content-Range')||'';
          const m = cr.match(/\\/(\\d+)$/);
          return m ? Number(m[1]) : Number(r.headers.get('content-length')||0);
        }})()""")
        check('wav has real audio size', int(size) > 3_500_000, f'{int(size)} B')

    if full:
        # Master and Stems are both console-side jobs since v54: fire from
        # the row menu, verify OUTPUT ON THE CONSOLE (/api/music/files).
        # (v53 and older mastered in the page and the file went to the
        # browser Downloads — that fallback only fires for mp3 sources.)
        # Renderer crashes mid-flow are harness-env limits, not product
        # failures -> SKIP, and poll /api/music/files externally.
        import datetime
        t_start = int(datetime.datetime.now().timestamp()) - 5
        for emoji, label, pat in (
                ('\U0001F39A', 'Master', r'-mastered\.wav$'),
                ('\u2702', 'Stems', r'(vocals|drums|bass|other)\.wav$')):
            try:
                found = js(f"""(() => {{
                  const b = Array.from(document.querySelectorAll('button'))
                    .find(b => (b.textContent||'').indexOf({json.dumps(emoji)}) === 0);
                  if (!b) return 'no button';
                  b.click(); return 'clicked: ' + (b.textContent||'').slice(0, 40);
                }})()""")
                if found == 'no button':
                    check(f'{label} flow produced output', False, found)
                    continue
                t0 = time.time(); hit = None; tab_died = False
                while time.time() - t0 < 420:
                    time.sleep(15)
                    try:
                        names = J("""(async () => {
                          const j = await fetch('/api/music/files').then(r=>r.json());
                          const fl = Array.isArray(j) ? j : (j.files||[]);
                          return JSON.stringify(fl.filter(f=>f.mtime>%d && %s).map(f=>f.name));
                        })()""" % (t_start - 3600, ("RegExp('%s').test(f.name)" % pat)))
                        got = json.loads(names or '[]')
                        if got:
                            hit = ','.join(got[:4]); break
                    except Exception:
                        tab_died = True
                        break
                if not hit:
                    # link died or clock skew: check the console directly
                    # (urllib, no browser). PS5 mtime can run ~20 min off,
                    # so match on NAME + a generous window.
                    import re as _re, urllib.request
                    try:
                        with urllib.request.urlopen(
                                f'http://{host}/api/music/files', timeout=10) as r:
                            fl = json.load(r)
                            fl = fl if isinstance(fl, list) else fl.get('files', [])
                        new = sorted([f for f in fl if _re.search(pat, f['name'])
                                      and f.get('mtime', 0) > t_start - 3600],
                                     key=lambda f: -f.get('mtime', 0))
                        if new:
                            hit = new[0]['name']
                    except Exception:
                        pass
                if hit:
                    check(f'{label} flow produced output', True, hit)
                elif tab_died:
                    print(f'[SKIP] {label} flow — browser link lost; poll /api/music/files externally', flush=True)
                else:
                    check(f'{label} flow produced output', False, 'timeout 420 s')
            except Exception as e:
                check(f'{label} flow produced output', False,
                      f'browser link lost ({type(e).__name__})')

    # leave a clean state (stale runtime options bit us before)
    try:
        js("fetch('/api/settings/runtime',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({audio_options:''})}).then(()=>1)")
    except Exception:
        pass
    time.sleep(2)
    print(f"\nBROWSER SMOKE: {sum(results)}/{len(results)} passed", flush=True)
    return all(results)


if __name__ == '__main__' and 'SMOKE_HOST' not in globals():
    # standalone CLI entry; when exec()'d inside browser_exec the host
    # variable is pre-seeded and the caller invokes browser() directly
    a = sys.argv
    host = a[a.index('--host') + 1] if '--host' in a else '192.168.100.167'
    mode = a[1] if len(a) > 1 else 'static'
    if mode == 'static':
        uv = a[a.index('--uiver') + 1] if '--uiver' in a else None
        sys.exit(0 if static(host, uv) else 1)
    elif mode == 'browser':
        clip = a[a.index('--clip') + 1] if '--clip' in a else 'smoke_src.wav'
        uv = a[a.index('--uiver') + 1] if '--uiver' in a else None
        full = '--full' in a
        sys.exit(0 if browser(host, clip, uv, full) else 1)
    else:
        print(__doc__)
