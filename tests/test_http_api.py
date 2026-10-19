# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HttpApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
        cls.scratch = tempfile.TemporaryDirectory(prefix="prospero-http-", dir=ROOT / "build/tests")
        tmp = Path(cls.scratch.name)
        cls.binary = tmp / "http-test"
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler or not (ROOT / ".deps/llama.cpp/vendor/nlohmann/json.hpp").exists():
            raise unittest.SkipTest("C++ compiler and make deps required")
        subprocess.run([compiler, "-std=c++20", "-O1", "-pthread", "-ffunction-sections", "-fdata-sections",
                        "-DPROSPERO_SESSION_TEST",
                        '-DPROSPERO_WEB_ASSET_ROOT="'+str(ROOT / 'assets/web')+'/"',
                        '-DPROSPERO_SETTINGS_PATH="'+str(tmp / "runtime.cfg")+'"', "-Wl,--gc-sections", "-I"+str(ROOT / "vulkan"), "-I"+str(ROOT / "include"),
                        "-I"+str(ROOT / "search-engine/include"), "-I"+str(ROOT / "search-engine/src"),
                        "-I"+str(ROOT / ".deps/llama.cpp/vendor"),
                        str(ROOT / "tests/http_api_harness.cpp"), str(ROOT / "src/session_store.cpp"),
                        str(ROOT / "src/debug_log.cpp"),
                        str(ROOT / "search-engine/src/query.cpp"),
                        str(ROOT / "search-engine/src/providers.cpp"),
                        str(ROOT / "search-engine/src/scan.cpp"),
                        str(ROOT / "search-engine/src/cache.cpp"),
                        str(ROOT / "search-engine/src/search.cpp"),
                        str(ROOT / "search-engine/src/transport.cpp"),
                        "-DPROSPERO_HOST", str(ROOT / "src/voice_recorder.cpp"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def request(self, path="/v1/chat/completions", body=None, key=None, headers=None, method=None,
                environment=None):
        if body is None and path.endswith("completions"):
            body = {"model": "tiny.gguf", "messages": [{"role": "user", "content": "hello"}]}
        payload = json.dumps(body, ensure_ascii=False).encode() if body is not None else b""
        lines = [f"{method or ('POST' if body is not None else 'GET')} {path} HTTP/1.1", "Host: test",
                 f"content-length: {len(payload)}"] + (headers or [])
        wire = subprocess.check_output([str(self.binary)] + ([key] if key else []),
                                      env={**os.environ, **(environment or {})},
                                      input="\r\n".join(lines).encode()+b"\r\n\r\n"+payload)
        head, data = wire.split(b"\r\n\r\n", 1)
        return head.decode(), data.decode()

    def test_runtime_settings_round_trip(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\nstyle=2\ntokens=192\ntheme=1\n')
        header, body = self.request('/api/settings/runtime', body={'context':'32k', 'kv':'auto', 'thinking':True, 'sd_upscale':True, 'idle_unload_minutes':2, 'kokoro_voice':'af_bella', 'audio_duration_sec':30, 'audio_seed':123, 'sd_seed':456})
        self.assertIn('200 OK', header)
        expected = json.loads(body)
        self.assertEqual({k:expected[k] for k in ('context','kv','thinking','sd_upscale','idle_unload_minutes','kokoro_voice','audio_duration_sec','audio_seed','sd_seed')},
                         {'context':'32k','kv':'auto','thinking':True,'sd_upscale':True,'idle_unload_minutes':2,'kokoro_voice':'af_bella','audio_duration_sec':30,'audio_seed':123,'sd_seed':456})
        self.assertEqual(json.loads(body), expected)
        for line in ('style=2','tokens=192','theme=1'):
            self.assertIn(line,config.read_text().splitlines())
        for line in ('audio_duration_sec=30','audio_seed=123','sd_seed=456','sd_upscale=1'):
            self.assertIn(line,config.read_text().splitlines())
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        header, body = self.request('/api/settings/runtime', body={'context':'256k', 'kv':'auto', 'thinking':True, 'idle_unload_minutes':5})
        self.assertIn('200 OK', header)
        expected.update(context='256k',kv='auto',thinking=True,idle_unload_minutes=5,kokoro_voice='af_bella')
        self.assertEqual(json.loads(body), expected)
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        for invalid in ({'context':'2k'}, {'kv':'q8_0'}, {'kv':'q2_0'}, {'thinking':'false'}, {'sd_upscale':1}, {'idle_unload_minutes':3}, {'kokoro_voice':'missing'}, {'audio_duration_sec':301}, {'audio_seed':-1}, {'audio_seed':4294967296}, {'sd_seed':-1}, {'sd_seed':4294967296}, {'unknown':1}, {'context':'16k','kv':'bad'}, []):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])
            self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        self.assertIn('401 Unauthorized', self.request('/api/settings/runtime', key='secret')[0])
        self.request('/api/settings/runtime', body={'context':'auto','kv':'auto','thinking':False,'idle_unload_minutes':2})

    def test_search_settings_and_route_require_enabled_configuration(self):
        header, body = self.request('/api/settings/runtime')
        self.assertIn('200 OK', header)
        self.assertFalse(json.loads(body)['web_search'])
        header, body = self.request('/api/search', body={'query': 'ProsperoAI'})
        self.assertIn('409 Conflict', header)
        self.request('/api/settings/runtime', body={'web_search': True, 'web_search_timeout': 10})
        settings = json.loads(self.request('/api/settings/runtime')[1])
        self.assertTrue(settings['web_search'])
        self.assertEqual(settings['web_search_timeout'], 10)
        header, body = self.request('/api/search', body={'query': 'ProsperoAI', 'limit': 3})
        self.assertIn('502 Bad Gateway', header)
        self.assertIn('PS5 build only', json.loads(body)['error']['message'])
        # Even a failed search says which providers were asked, so a caller can tell
        # throttling from an empty index.
        self.assertTrue(json.loads(body)['providers'])

    def test_search_scopes_round_trip_and_select_providers(self):
        # What the Settings checkboxes send is this bitmask, in the engine's scope
        # order: bit 0 web, bit 1 code, bit 3 reference.
        settings = json.loads(self.request('/api/settings/runtime')[1])
        self.assertEqual(settings['search_scopes'], 511)
        self.request('/api/settings/runtime', body={'web_search': True, 'search_scopes': 0b1011})
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['search_scopes'], 0b1011)
        for invalid in ({'search_scopes': 0}, {'search_scopes': 512}, {'search_scopes': -1},
                        {'search_scopes': '11'}):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['search_scopes'], 0b1011)

        # A request may narrow the search further, and only the enabled scopes are asked.
        asked = json.loads(self.request('/api/search', body={'query': 'ProsperoAI',
                                                             'scopes': ['web', 'code']})[1])
        sources = {provider['id'].split('.')[0] for provider in asked['providers']}
        self.assertEqual(sources, {'web', 'code'})
        # A scope switched off in Settings is not asked even when the caller names it.
        asked = json.loads(self.request('/api/search', body={'query': 'ProsperoAI',
                                                             'scopes': ['web', 'video']})[1])
        self.assertEqual({provider['id'].split('.')[0] for provider in asked['providers']}, {'web'})
        header, _ = self.request('/api/search', body={'query': 'ProsperoAI', 'scopes': ['video']})
        self.assertIn('400 Bad Request', header)
        header, _ = self.request('/api/search', body={'query': 'ProsperoAI', 'scopes': ['nonsense']})
        self.assertIn('400 Bad Request', header)
        self.request('/api/settings/runtime', body={'search_scopes': 511, 'web_search': False})

    def test_fetch_route_validates_its_address(self):
        self.request('/api/settings/runtime', body={'web_search': True})
        for invalid in ({'url': 'ftp://example.org'}, {'url': 'not-a-url'}, {},
                        {'url': 'https://example.org', 'limit': 10},
                        {'url': 'https://example.org', 'limit': 999999}):
            header, _ = self.request('/api/fetch', body=invalid)
            self.assertIn('400 Bad Request', header)
        header, body = self.request('/api/fetch', body={'url': 'https://example.org'})
        self.assertIn('502 Bad Gateway', header)
        self.assertIn('console build', json.loads(body)['error']['message'])
        self.request('/api/settings/runtime', body={'web_search': False})
        self.assertIn('409 Conflict', self.request('/api/fetch', body={'url': 'https://example.org'})[0])
    def test_chatterbox_language_is_selectable_and_validated(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\n')
        header, body = self.request('/api/settings/runtime')
        self.assertIn('200 OK', header)
        settings = json.loads(body)
        self.assertEqual(settings['chatterbox_language'], 'en')
        codes = [language['id'] for language in settings['chatterbox_languages']]
        self.assertEqual(codes[0], 'en')
        for code in ('de', 'ko', 'tr'):
            self.assertIn(code, codes)
        self.assertTrue(all(language['name'] for language in settings['chatterbox_languages']))
        header, body = self.request('/api/settings/runtime', body={'chatterbox_language': 'de'})
        self.assertIn('200 OK', header)
        self.assertEqual(json.loads(body)['chatterbox_language'], 'de')
        self.assertIn('chatterbox_language=3', config.read_text().splitlines())
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['chatterbox_language'], 'de')
        # A code this build cannot encode must be refused, not silently taken as English.
        for invalid in ({'chatterbox_language': 'zh'}, {'chatterbox_language': 'EN'},
                        {'chatterbox_language': ''}, {'chatterbox_language': 3}):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])
            self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['chatterbox_language'], 'de')
        self.request('/api/settings/runtime', body={'chatterbox_language': 'en'})

    def test_conversations_share_the_native_session_store(self):
        conversation = {'id':'web-sync-fixture','title':'Shared from web', 'model':'tiny.gguf',
                        'modelName':'Tiny model','purpose':'text-to-text',
                        'messages':[{'role':'user','content':'Continue on the console'},
                                    {'role':'assistant','content':'This is the shared answer.'}]}
        header, saved = self.request('/api/conversations', body=conversation)
        self.assertIn('201 Created', header)
        record = json.loads(saved)
        self.assertEqual(record['message_count'], 2)
        catalog = json.loads(self.request('/api/conversations')[1])['data']
        self.assertTrue(any(item['id'] == conversation['id'] for item in catalog))
        detail = json.loads(self.request('/api/conversations/' + conversation['id'])[1])
        self.assertEqual(detail['title'], conversation['title'])
        self.assertEqual(detail['messages'], conversation['messages'])
        self.assertIn('200 OK', self.request('/api/conversations/' + conversation['id'], method='DELETE')[0])
        self.assertIn('404 Not Found', self.request('/api/conversations/' + conversation['id'])[0])
        self.assertIn('401 Unauthorized', self.request('/api/conversations', key='secret')[0])

    def test_text_completion_archives_export_after_conversation_is_saved(self):
        session_id = 'text-export-fixture'
        conversation = {'id':session_id,'title':'Text export','model':'tiny.gguf',
                        'modelName':'Tiny model','purpose':'text-to-text',
                        'messages':[{'role':'user','content':'hello'}]}
        header, _ = self.request('/api/conversations', body=conversation)
        self.assertIn('201 Created', header)
        try:
            _, response = self.request(body={
                'model':'tiny.gguf',
                'messages':[{'role':'user','content':'hello'}],
                'output_session_id':session_id,
                'output_message_index':1,
            })
            self.assertEqual(json.loads(response)['choices'][0]['message']['content'], 'Hello café 😀')

            root = Path('/tmp/prosperoai-session-test')
            exports = list((root / 'work' / 'text' / session_id).glob('001-*.txt'))
            self.assertEqual(len(exports), 1)
            self.assertEqual(exports[0].read_text(encoding='utf-8'), 'Hello café 😀')
            self.assertFalse(Path(str(exports[0]) + '.json').exists())
            reference = root / 'sessions' / session_id / 'output-001.txt'
            self.assertEqual(reference.read_text(encoding='utf-8').strip(), str(exports[0]))
        finally:
            self.request('/api/conversations/' + session_id, method='DELETE')

    def test_request_context_does_not_replace_shared_settings(self):
        shared = {'context': '32k', 'kv': 'auto', 'thinking': True, 'idle_unload_minutes': 2, 'kokoro_voice': 'af_heart'}
        shared = json.loads(self.request('/api/settings/runtime', body=shared)[1])
        try:
            for route, body in [
                ('/v1/chat/completions', {'model': 'tiny.gguf', 'context_tokens': 16384,
                  'messages': [{'role': 'user', 'content': 'context-settings'}]}),
                ('/api/generate', {'model': 'tiny.gguf', 'prompt': 'context-settings',
                  'options': {'num_ctx': 16384}}),
                ('/api/chat', {'model': 'tiny.gguf', 'options': {'num_ctx': 16384},
                  'messages': [{'role': 'user', 'content': 'context-settings'}]}),
            ]:
                _, wire = self.request(route, body)
                data = json.loads(wire)
                content = (data['choices'][0]['message']['content'] if 'choices' in data
                           else data.get('response') or data['message']['content'])
                self.assertEqual(content, '32768:16384')
                self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), shared)
            _, wire = self.request(body={'model': 'tiny.gguf',
                                  'messages': [{'role': 'user', 'content': 'context-settings'}]})
            self.assertEqual(json.loads(wire)['choices'][0]['message']['content'], '32768:0')
        finally:
            self.request('/api/settings/runtime', body={'context': 'auto', 'kv': 'auto', 'thinking': False, 'idle_unload_minutes': 2})

    def test_model_memory_status(self):
        head, body = self.request("/api/models/memory")
        self.assertIn("200 OK", head)
        self.assertEqual(json.loads(body), {
            "loaded": False, "id": "", "name": "", "vulkan_heap_bytes": 0,
            "largest_direct_block_bytes": 0, "flexible_free_bytes": 0,
            "estimate_confidence": "live-memory-snapshot",
        })
        self.assertIn("401 Unauthorized", self.request("/api/models/memory", key="secret")[0])

    def test_models_and_selection(self):
        _, body = self.request("/v1/models")
        self.assertEqual([m["id"] for m in json.loads(body)["data"]], ["tiny.gguf", "second.gguf"])
        _, body = self.request("/api/models/selection")
        self.assertEqual(json.loads(body)["id"], "tiny.gguf")
        head, body = self.request("/api/models/selection", body={"id": "second.gguf"})
        self.assertIn("200 OK", head)
        self.assertEqual(json.loads(body)["id"], "second.gguf")
        head, body = self.request("/api/models/selection", body={"id": "second.gguf", "prepare": False})
        self.assertIn("200 OK", head)
        self.assertTrue(json.loads(body)["preparation_deferred"])
        self.assertIn("400 Bad Request", self.request("/api/models/selection", body={"id": "second.gguf", "prepare": "false"})[0])
        self.assertIn("404 Not Found", self.request("/api/models/selection", body={"id": "missing.gguf"})[0])
        self.assertIn("400 Bad Request", self.request("/api/models/selection", body={"id": 3})[0])
        self.assertIn("401 Unauthorized", self.request("/api/models/selection", key="secret")[0])

        header, body = self.request("/api/models", body={"id": "tiny.gguf"}, method="DELETE")
        self.assertIn("409 Conflict", header)
        self.assertIn("active", json.loads(body)["error"])
        header, body = self.request("/api/models", body={"id": "second.gguf"}, method="DELETE")
        self.assertIn("200 OK", header)
        self.assertEqual(json.loads(body)["deleted"], "second.gguf")
        self.assertIn("400 Bad Request", self.request("/api/models", body={"id": 3}, method="DELETE")[0])
        self.assertIn("401 Unauthorized", self.request("/api/models", body={"id": "second.gguf"},
                                                       method="DELETE", key="secret")[0])
        _, body = self.request(body={"model":"second.gguf", "messages":[{"role":"user","content":"x"}]})
        data = json.loads(body)
        self.assertEqual(data["model"], "second.gguf")
        self.assertEqual(data["usage"]["total_tokens"], 14)

    def test_model_memory_estimate_requires_settings(self):
        head, body = self.request("/api/models/estimate", body={"id": "second.gguf"})
        self.assertIn("400 Bad Request", head)
        self.assertIn("context_tokens", body)
        head, body = self.request(
            "/api/models/estimate",
            body={"id": "second.gguf", "context_tokens": 4096, "output_limit": 512,
                  "thinking": False},
        )
        self.assertIn("200 OK", head)
        self.assertFalse(json.loads(body)["estimate_available"])

    def test_model_selection_reports_prepare_failure_and_runtime_busy(self):
        header, body = self.request(
            "/api/models/selection",
            body={"id": "second.gguf"},
            environment={"PROSPERO_TEST_FAIL_PREPARE": "1"},
        )
        self.assertIn("500 Internal Server Error", header)
        self.assertIn("could not be prepared", json.loads(body)["error"])

        header, body = self.request(
            "/api/models/selection",
            body={"id": "second.gguf"},
            environment={"PROSPERO_TEST_RUNTIME_BUSY": "1"},
        )
        self.assertIn("409 Conflict", header)
        self.assertIn("runtime is busy", json.loads(body)["error"])

    def test_ollama_uses_requested_model(self):
        for route, body in [
            ("/api/generate", {"model": "second.gguf", "prompt": "selected-model"}),
            ("/api/chat", {"model": "second.gguf", "messages": [{"role": "user", "content": "selected-model"}]}),
        ]:
            _, response = self.request(route, body)
            data = json.loads(response)
            self.assertEqual(data.get("response") or data["message"]["content"], "second.gguf")

    def test_extended_generation_limits(self):
        for route in ["/api/generate", "/api/chat"]:
            body = {"model": "tiny.gguf", "prompt": "limits",
                    "messages": [{"role": "user", "content": "limits"}],
                    "options": {"num_predict": 131072, "num_ctx": 262144}}
            _, response = self.request(route, body)
            data = json.loads(response)
            self.assertEqual(data.get("response") or data["message"]["content"], "131072:262144")
            body["options"] = {"num_predict": 131073, "num_ctx": 262145}
            _, response = self.request(route, body)
            data = json.loads(response)
            self.assertEqual(data.get("response") or data["message"]["content"], "131072:262144")
            body["prompt"] = "long"
            body["messages"][0]["content"] = "long"
            _, response = self.request(route, body)
            data = json.loads(response)
            self.assertEqual(len(data.get("response") or data["message"]["content"]), 24000)
        _, response = self.request(body={"model": "tiny.gguf", "max_tokens": 131072, "context_tokens": 262144,
                                       "messages": [{"role": "user", "content": "limits"}]})
        self.assertEqual(json.loads(response)["choices"][0]["message"]["content"], "131072:262144")
        header, response = self.request(body={"model": "tiny.gguf", "max_tokens": 131073,
                                             "messages": [{"role": "user", "content": "limits"}]})
        self.assertIn("400 Bad Request", header)
        self.assertEqual(json.loads(response)["error"]["code"], "invalid_max_tokens")

    def test_context_validation_and_auto_output_default(self):
        for value in [0, -1, 262145, "8192"]:
            header, response = self.request(body={"model": "tiny.gguf", "context_tokens": value,
                                                 "messages": [{"role": "user", "content": "limits"}]})
            self.assertIn("400 Bad Request", header)
            self.assertEqual(json.loads(response)["error"]["code"], "invalid_context_tokens")
        _, response = self.request(body={"model": "tiny.gguf",
                                       "messages": [{"role": "user", "content": "limits"}]})
        self.assertEqual(json.loads(response)["choices"][0]["message"]["content"], "0:0")
        _, response = self.request("/api/generate", {"model": "tiny.gguf", "prompt": "limits"})
        self.assertEqual(json.loads(response)["response"], "131072:0")

    def test_voice_recording_status_validation_and_authentication(self):
        route='/api/voices/chatterbox/recording'
        header,body=self.request(route)
        self.assertIn('200 OK',header)
        self.assertEqual(json.loads(body),{'state':'idle','seconds':0.0,'level':0,'preview':'','error':''})
        self.assertIn('400 Bad Request',self.request(route,body={'action':'unknown'})[0])
        self.assertIn('409 Conflict',self.request(route,body={'action':'save'})[0])
        self.assertIn('401 Unauthorized',self.request(route,body={'action':'start'},key='secret')[0])

    def test_ollama_benchmark_batch_overrides(self):
        _, response = self.request('/api/generate', {
            'model': 'tiny.gguf', 'prompt': 'benchmark-settings',
            'options': {'num_batch': 512, 'num_ubatch': 128},
        })
        self.assertEqual(json.loads(response)['response'], '512:128')

    def test_public_ui_preserves_private_api(self):
        header, body = self.request("/", key="private")
        self.assertIn("200 OK", header)
        self.assertIn("Content-Security-Policy:", header)
        self.assertIn('id="workspace"', body)
        header, body = self.request("/app.js", key="private")
        self.assertIn("200 OK", header)
        self.assertIn("/v1/chat/completions", body)
        self.assertIn("/api/models/search", body)
        for route in ("/v1/models", "/api/tags", "/api/models/download",
                      "/api/models/search", "/api/models/presets", "/api/models",
                      "/../api_key.txt", "/fonts/../../api_key.txt"):
            header, _ = self.request(route, key="private")
            self.assertIn("401 Unauthorized", header)
        header, _ = self.request("/../api_key.txt")
        self.assertIn("404 Not Found", header)

    def test_presets_and_download_progress(self):
        _, body = self.request("/api/models/presets")
        presets = json.loads(body)["data"]
        self.assertEqual({preset["kind"] for preset in presets}, {0, 1, 2, 3})
        self.assertTrue(all(preset["size"] > 0 for preset in presets))
        head, _ = self.request("/api/models/presets", body={"index":4})
        self.assertIn("200 OK", head)
        head, _ = self.request("/api/models/presets", body={"index":99})
        self.assertIn("400 Bad Request", head)
        _, body = self.request("/api/models/download")
        progress = json.loads(body)
        self.assertEqual((progress["completed"], progress["total"]), (50, 100))

    def test_browse_candidate_reports_fit(self):
        _, body = self.request("/api/models/download")
        items = json.loads(body)["items"]
        self.assertEqual(items[0]["fit"], "tight")

    def test_cancel_download(self):
        head, body = self.request("/api/models/download", method="DELETE")
        self.assertIn("409 Conflict", head)
        self.assertIn("no download is running", body)
        head, body = self.request("/api/models/download", method="DELETE",
                                  environment={"PROSPERO_TEST_DOWNLOADING": "1"})
        self.assertIn("200 OK", head)
        state = json.loads(body)
        self.assertEqual((state["state"], state["cancelling"]), ("downloading", True))
        _, body = self.request("/api/models/download")
        self.assertIs(json.loads(body)["cancelling"], False)
        _, page = self.request("/app.js")
        self.assertIn("method:'DELETE'", page)

    def test_sse_unicode_usage_and_finish(self):
        body = {"model":"tiny.gguf", "messages":[{"role":"user","content":"x"}],
                "stream":True, "max_tokens":1, "stream_options":{"include_usage":True}}
        head, wire = self.request(body=body)
        self.assertIn("text/event-stream", head)
        frames = [line[6:] for line in wire.splitlines() if line.startswith("data: ")]
        self.assertEqual(frames[-1], "[DONE]")
        chunks = [json.loads(frame) for frame in frames[:-1]]
        content = "".join(c["choices"][0]["delta"].get("content", "") for c in chunks if c["choices"])
        self.assertEqual(content, "Hello café 😀")
        self.assertEqual(chunks[-2]["choices"][0]["finish_reason"], "length")
        self.assertEqual(chunks[-1]["choices"], [])
        self.assertEqual(chunks[-1]["usage"]["completion_tokens"], 4)

    def tools_request(self, content="lookup", stream=False):
        return {"model":"tiny.gguf", "messages":[{"role":"user","content":content}], "stream":stream,
                "tools":[{"type":"function", "function":{"name":"lookup", "parameters":{
                    "type":"object", "properties":{"query":{"type":"string", "minLength":2}},
                    "required":["query"], "additionalProperties":False}}}]}

    def test_tool_calls_and_roundtrip(self):
        _, wire = self.request(body=self.tools_request())
        response = json.loads(wire)["choices"][0]
        self.assertEqual(response["finish_reason"], "tool_calls")
        call = response["message"]["tool_calls"][0]
        self.assertEqual(json.loads(call["function"]["arguments"])["query"], 'a}b"c')
        request = self.tools_request()
        request["messages"] += [response["message"], {"role":"tool", "tool_call_id":call["id"], "content":"result"}]
        _, wire = self.request(body=request)
        self.assertEqual(json.loads(wire)["choices"][0]["message"]["content"], "tool result received")

    def test_stream_tool_calls(self):
        _, wire = self.request(body=self.tools_request(stream=True))
        chunks = [json.loads(line[6:]) for line in wire.splitlines() if line.startswith("data: {")]
        calls = [c["choices"][0]["delta"]["tool_calls"] for c in chunks if "tool_calls" in c["choices"][0]["delta"]]
        self.assertEqual(calls[0][0]["index"], 0)
        self.assertTrue(calls[0][0]["id"].startswith("call_"))
        self.assertEqual(calls[0][0]["type"], "function")
        self.assertEqual(calls[0][0]["function"]["name"], "lookup")
        self.assertIn("query", json.loads(calls[0][0]["function"]["arguments"]))
        self.assertEqual(chunks[-1]["choices"][0]["finish_reason"], "tool_calls")

    def test_tool_argument_schemas_and_parallel_calls(self):
        for prompt in ("bad-extra-argument", "bad-missing-argument", "bad-argument-type"):
            head, wire = self.request(body=self.tools_request(prompt))
            self.assertIn("502 Bad Gateway", head, prompt)
            self.assertEqual(json.loads(wire)["error"]["code"], "invalid_tool_output")

        _, wire = self.request(body=self.tools_request("multiple-tools"))
        self.assertEqual(len(json.loads(wire)["choices"][0]["message"]["tool_calls"]), 2)
        request = self.tools_request("multiple-tools")
        request["parallel_tool_calls"] = False
        head, wire = self.request(body=request)
        self.assertIn("502 Bad Gateway", head)

    def test_tool_declarations_and_history_fail_closed(self):
        for mutate in (
            lambda body: body["tools"][0]["function"]["parameters"].update({"required":[5]}),
            lambda body: body["tools"][0]["function"]["parameters"].update({"required":["missing"]}),
            lambda body: body["tools"][0]["function"]["parameters"].update({"required":["query", "query"]}),
            lambda body: body["tools"][0]["function"]["parameters"]["properties"]["query"].update({"pattern":"^x"}),
            lambda body: body["tools"][0]["function"].update({"strict":"yes"}),
            lambda body: body["tools"].append(body["tools"][0]),
        ):
            request = self.tools_request()
            mutate(request)
            head, _ = self.request(body=request)
            self.assertIn("400 Bad Request", head)

        request = self.tools_request()
        request["messages"] = [{"role":"tool", "tool_call_id":"unmatched", "content":"x"}]
        self.assertIn("400 Bad Request", self.request(body=request)[0])
        request["messages"] = [{"role":"assistant", "tool_calls":[{
            "id":"call_1", "type":"function", "function":{"name":"lookup", "arguments":"{}"}}]},
            {"role":"tool", "tool_call_id":"wrong-id", "content":"x"}]
        self.assertIn("400 Bad Request", self.request(body=request)[0])
        request["messages"] = [{"role":"assistant", "tool_calls":[{
            "id":"call_1", "type":"function", "function":{"name":"lookup", "arguments":json.dumps({"query":"valid"})}}]}]
        self.assertIn("400 Bad Request", self.request(body=request)[0])

    def test_invalid_requests_and_outputs(self):
        for body, status in [({}, "400"), ({"model":"missing", "messages":[{"role":"user","content":"x"}]}, "404"),
                             (self.tools_request("bad-tool"), "502"), (self.tools_request("bad-json"), "502")]:
            head, wire = self.request(body=body)
            self.assertIn(status, head)
            self.assertIn("message", json.loads(wire)["error"])

    def test_tool_choice_and_parallel_validation(self):
        request = self.tools_request()
        request["tool_choice"] = {"type":"function", "function":{"name":"lookup"}}
        request["parallel_tool_calls"] = False
        _, body = self.request(body=request)
        self.assertEqual(json.loads(body)["choices"][0]["finish_reason"], "tool_calls")
        request["tool_choice"]["function"]["name"] = "missing"
        head, _ = self.request(body=request)
        self.assertIn("400", head)
        request = self.tools_request()
        request["tool_choice"] = "none"
        _, body = self.request(body=request)
        self.assertNotIn("tool_calls", json.loads(body)["choices"][0]["message"])

    def test_bearer_auth_on_all_routes(self):
        for route in ["/v1/models", "/api/tags", "/api/models/download", "/api/models/search"]:
            head, _ = self.request(route, key="secret")
            self.assertIn("401", head)
        head, _ = self.request("/v1/models", key="secret", headers=["authorization: Bearer secret"])
        self.assertIn("200", head)

    def test_security_status_exposes_mode_but_never_the_key(self):
        head, body = self.request("/api/settings/security")
        self.assertIn("200 OK", head)
        self.assertEqual(json.loads(body), {
            "authentication": "disabled", "transport": "http",
            "listener_scope": "all_interfaces", "encrypted": False,
        })
        head, body = self.request("/api/settings/security", key="secret")
        self.assertIn("200 OK", head)
        self.assertEqual(json.loads(body)["authentication"], "required")
        self.assertNotIn("secret", body)

    def test_nested_text_parts_and_escaped_content(self):
        text = 'quoted " and } braces café 😀'
        body = {"model":"tiny.gguf", "messages":[{"role":"user", "content":[{"type":"text","text":text}]},
                                                  {"role":"user", "content":"echo"}]}
        _, wire = self.request(body=body)
        self.assertEqual(json.loads(wire)["choices"][0]["message"]["content"], text)

    def test_request_limit(self):
        head, wire = self.request(body={"padding":"x"*270000})
        self.assertIn("413", head)
        self.assertEqual(json.loads(wire)["error"]["code"], "request_too_large")

if __name__ == "__main__":
    unittest.main()
