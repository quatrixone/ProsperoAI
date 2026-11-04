# SPDX-License-Identifier: GPL-3.0-or-later
import base64
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
        cls.audio_dir = tmp / "audio"
        cls.audio_dir.mkdir()
        cls.image_dir = tmp / "image"
        cls.image_dir.mkdir()
        cls.binary = tmp / "http-test"
        compiler = shutil.which(os.environ.get("HOST_CXX", "clang++")) or shutil.which("g++")
        if not compiler or not (ROOT / ".deps/llama.cpp/vendor/nlohmann/json.hpp").exists():
            raise unittest.SkipTest("C++ compiler and make deps required")
        command = [compiler, "-std=c++20", "-O1", "-pthread", "-ffunction-sections", "-fdata-sections",
                        "-DPROSPERO_SESSION_TEST",
                        '-DPROSPERO_WEB_ASSET_ROOT="'+str(ROOT / 'assets/web')+'/"',
                        '-DPROSPERO_SETTINGS_PATH="'+str(tmp / "runtime.cfg")+'"', "-Wl,--gc-sections", "-I"+str(ROOT / "vulkan"), "-I"+str(ROOT / "include"),
                        '-DPROSPERO_AUDIO_DIR="'+str(cls.audio_dir)+'/"',
                        '-DPROSPERO_IMAGE_DIR="'+str(cls.image_dir)+'/"',
                        "-I"+str(ROOT / "search-engine/include"), "-I"+str(ROOT / "search-engine/src"),
                        "-I"+str(ROOT / ".deps/llama.cpp/vendor"),
                        "-I"+str(ROOT / ".deps/stable-diffusion-vulkan/thirdparty"),
                        str(ROOT / "tests/http_api_harness.cpp"), str(ROOT / "src/session_store.cpp"),
                        str(ROOT / "src/image_codec.cpp"),
                        str(ROOT / "src/audio_workflows.cpp"),
                        str(ROOT / "src/debug_log.cpp"),
                        str(ROOT / "search-engine/src/query.cpp"),
                        str(ROOT / "search-engine/src/providers.cpp"),
                        str(ROOT / "search-engine/src/scan.cpp"),
                        str(ROOT / "search-engine/src/cache.cpp"),
                        str(ROOT / "search-engine/src/search.cpp"),
                        str(ROOT / "search-engine/src/transport.cpp"),
                        "-DPROSPERO_HOST", str(ROOT / "src/voice_recorder.cpp"),
                        "-o", str(cls.binary)]
        command.insert(command.index("-Wl,--gc-sections") + 1, "-Wl,--wrap=remove")
        subprocess.run(command, check=True)
        cls.lifecycle_binary = tmp / "http-lifecycle-test"
        command.remove("-Wl,--wrap=remove")
        command[command.index(str(ROOT / "tests/http_api_harness.cpp"))] = str(ROOT / "tests/http_lifecycle_test.cpp")
        command[-1] = str(cls.lifecycle_binary)
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        self.audio_calls_file = Path(self.scratch.name) / "audio-calls.txt"
        self.audio_calls_file.unlink(missing_ok=True)
        self.image_calls_file = Path(self.scratch.name) / "image-calls.txt"
        self.image_calls_file.unlink(missing_ok=True)
        self.generate_calls_file = Path(self.scratch.name) / "generate-calls.txt"
        self.generate_calls_file.unlink(missing_ok=True)
        for path in self.image_dir.iterdir():
            path.unlink()

    def audio_generate_calls(self):
        if not self.audio_calls_file.exists():
            return 0
        return len(self.audio_calls_file.read_text().splitlines())

    def tearDown(self):
        for name in ("workspace.abc", "legacy.wav", "uploaded.wav", "uploaded-cover.abc"):
            (self.audio_dir / name).unlink(missing_ok=True)

    def test_shutdown_joins_async_jobs_and_idle_connections(self):
        subprocess.run([str(self.lifecycle_binary)], check=True, timeout=20)

    def test_audio_routes_use_workspace(self):
        head, body = self.request_raw("/audio", b"", method="GET")
        self.assertIn("200 OK", head)
        self.assertIn('data-mode="audio"', body)
        self.assertIn('id="workspace-audio"', body)
        self.assertIn('id="lyrics"', body)
        self.assertIn('id="cover-go"', body)
        self.assertIn('id="q-run"', body)
        self.assertIn('src="/app.js" defer></script>', body)
        self.assertIn('src="/music.js" defer', body)

        head, controller = self.request_raw("/music.js", b"", method="GET")
        self.assertIn("200 OK", head)
        self.assertIn("ProsperoWorkspaceApi", controller)

        head, body = self.request_raw("/music", b"", method="GET")
        self.assertIn("302 Found", head)
        self.assertIn("Location: /audio", head)
        self.assertIn("Cache-Control: no-store", head)
        self.assertEqual(body, "")

        abc = "X:1\nK:C\nCDEF|\n"
        head, _ = self.request("/api/music/abc", {"name": "workspace.abc", "content": abc})
        self.assertIn("200 OK", head)
        self.assertEqual((self.audio_dir / "workspace.abc").read_text(), abc)

        head, _ = self.request("/api/music/upload", {
            "name": "legacy.wav", "part": 0,
            "data": base64.b64encode(b"legacy upload").decode("ascii"),
        })
        self.assertIn("200 OK", head)
        self.assertEqual((self.audio_dir / "legacy.wav").read_bytes(), b"legacy upload")

        head, body = self.request("/api/music/files")
        self.assertIn("200 OK", head)
        names = {entry["name"] for entry in json.loads(body)["files"]}
        self.assertTrue({"workspace.abc", "legacy.wav"}.issubset(names))

    def test_audio_workflow_dispatch_uses_shared_operations(self):
        import wave

        source = self.audio_dir / "uploaded.wav"
        with wave.open(str(source), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\0\0")

        environment = {
            "PROSPERO_TEST_AUDIO_CALLS_FILE": str(self.audio_calls_file),
            "PROSPERO_TEST_AUDIO_FIXTURE": "1",
        }
        initial_calls = self.audio_generate_calls()
        head, body = self.request("/api/generate", {
            "async": False, "audio_workflow": "cover", "model": "sheetsage-test",
            "source": "uploaded.wav",
        }, environment=environment)
        self.assertIn("200 OK", head)
        self.assertEqual(self.audio_generate_calls(), initial_calls + 1)
        self.assertTrue(json.loads(body)["done"])

        calls_before = self.audio_generate_calls()
        head, _ = self.request("/api/generate", {
            "async": False, "audio_workflow": "cover", "model": "sheetsage-test",
            "source": "../outside.wav",
        }, environment=environment)
        self.assertIn("400 Bad Request", head)
        self.assertEqual(self.audio_generate_calls(), calls_before)

    def test_audio_workflow_async_job_polls_to_success_once(self):
        import wave

        source = self.audio_dir / "uploaded.wav"
        with wave.open(str(source), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\0\0")

        initial_calls = self.audio_generate_calls()
        submission, polled = self.request_audio_job_with_poll({
            "async": True, "audio_workflow": "cover", "model": "sheetsage-test",
            "source": "uploaded.wav",
        }, environment={
            "PROSPERO_TEST_AUDIO_CALLS_FILE": str(self.audio_calls_file),
            "PROSPERO_TEST_AUDIO_FIXTURE": "1",
        })
        self.assertIsInstance(submission.get("job_id"), int)
        self.assertEqual(polled["id"], submission["job_id"])
        self.assertEqual(polled["state"], "done")
        self.assertTrue(polled["result"]["done"])
        self.assertEqual(self.audio_generate_calls(), initial_calls + 1)

    def test_restart_does_not_kill_the_process(self):
        header, body = self.request('/api/system/restart', body={})
        self.assertIn('409 Conflict', header)
        self.assertIn('disabled', json.loads(body)['error'])

    def test_image_upload_returns_an_opaque_id_for_png_and_jpeg(self):
        for name in ("one-pixel.png", "one-pixel.jpg"):
            payload = (ROOT / "tests/fixtures" / name).read_bytes()
            head, body = self.request_raw(
                "/api/image/upload", payload, key="secret",
                headers=["Authorization: Bearer secret"], method="POST",
            )
            self.assertIn("200 OK", head)
            upload_id = json.loads(body)["id"]
            self.assertRegex(upload_id, r"^[0-9a-f]{32}$")
            self.assertEqual((self.image_dir / f".image-upload-{upload_id}.upload").read_bytes(),
                             payload)

    def test_image_upload_rejects_unauthenticated_requests(self):
        payload = (ROOT / "tests/fixtures/one-pixel.png").read_bytes()
        head, _ = self.request_raw("/api/image/upload", payload, key="secret", method="POST")
        self.assertIn("401 Unauthorized", head)
        self.assertEqual(list(self.image_dir.iterdir()), [])

    def test_image_upload_rejects_invalid_and_incomplete_bodies_without_files(self):
        sentinel = self.image_dir / ".image-upload-unrelated.upload"
        sentinel.write_bytes(b"preserve unrelated file")
        valid = (ROOT / "tests/fixtures/one-pixel.png").read_bytes()
        cases = [
            (b"", len(b"")),
            (b"not an image", len(b"not an image")),
            ((ROOT / "tests/fixtures/bmp-as-png.png").read_bytes(), None),
            ((ROOT / "tests/fixtures/zero-width.png").read_bytes(), None),
            ((ROOT / "tests/fixtures/wide-2049.png").read_bytes(), None),
            (valid[:-1], len(valid) + 20),
        ]
        for payload, declared_length in cases:
            with self.subTest(payload_size=len(payload), content_length=declared_length):
                head, _ = self.request_raw(
                    "/api/image/upload", payload, key="secret",
                    headers=["Authorization: Bearer secret"], method="POST",
                    content_length=declared_length,
                )
                self.assertIn("400 Bad Request", head)
                self.assertEqual({path.name for path in self.image_dir.iterdir()}, {sentinel.name})

    def test_image_upload_requires_bounded_content_length_before_opening_files(self):
        sentinel = self.image_dir / ".image-upload-unrelated.upload"
        sentinel.write_bytes(b"preserve unrelated file")
        headers = ["Authorization: Bearer secret"]
        oversized, _ = self.request_raw(
            "/api/image/upload", b"", key="secret", headers=headers,
            method="POST", content_length=16 * 1024 * 1024 + 1,
        )
        self.assertIn("413 Payload Too Large", oversized)

        overflow, _ = self.request_raw(
            "/api/image/upload", b"", key="secret", headers=headers,
            method="POST", content_length=str(1 << 64),
        )
        self.assertIn("413 Payload Too Large", overflow)

        missing, _ = self.request_without_length(
            "/api/image/upload", (ROOT / "tests/fixtures/one-pixel.png").read_bytes(),
            key="secret", headers=headers,
        )
        self.assertIn("400 Bad Request", missing)
        self.assertEqual({path.name for path in self.image_dir.iterdir()}, {sentinel.name})

    def test_memory_readout_does_not_wait_for_a_running_generation(self):
        # Generation holds the runtime lock for its whole run, which is exactly when a
        # caller wants to know which model is resident. Taking it here used to deadlock
        # the readout for the length of the generation.
        head, body = self.request("/api/models/memory", environment={
            "PROSPERO_TEST_VULKAN_MEMORY": "valid",
            "PROSPERO_TEST_MODEL_LOADED": "1",
            "PROSPERO_TEST_RUNTIME_BUSY": "1",
        })
        self.assertIn("200 OK", head)
        payload = json.loads(body)
        self.assertTrue(payload["loaded"])
        self.assertEqual(payload["name"], "SheetSage test fixture")
        self.assertEqual(payload["vulkan_free_bytes"], 6 * 1024**3)

    def test_image_upload_expiry_skips_active_leases(self):
        result = subprocess.run([str(self.binary), "--test-image-upload-lifecycle"],
                                check=True, capture_output=True, text=True)
        self.assertIn("image upload lifecycle: ok", result.stdout)

    def test_image_workflow_requires_owned_source_before_runtime(self):
        header, body = self.request("/api/generate", {
            "model": "sdxl-base-q8-0", "prompt": "turn this into a painting",
            "image_workflow": {"workflow": "image_to_image", "strength": 0.5},
        }, environment={
            "PROSPERO_TEST_IMAGE_METADATA": "1",
            "PROSPERO_TEST_GENERATE_CALLS_FILE": str(self.generate_calls_file),
        })
        self.assertIn("400 Bad Request", header)
        self.assertIn("image_id", json.loads(body)["error"])
        self.assertFalse(self.generate_calls_file.exists())

    def image_workflow_environment(self, **extra):
        return {
            "PROSPERO_TEST_IMAGE_METADATA": "1",
            "PROSPERO_TEST_IMAGE_WORKFLOWS": "1",
            "PROSPERO_TEST_GENERATE_CALLS_FILE": str(self.generate_calls_file),
            "PROSPERO_TEST_IMAGE_OUTPUT_SOURCE": str(ROOT / "tests/fixtures/one-pixel.png"),
            **extra,
        }

    def test_image_workflows_forward_fields_and_consume_leases(self):
        environment = self.image_workflow_environment()
        cases = [
            ({"workflow": "text_to_image", "width": 768, "height": 1024, "steps": 17,
              "seed": -1, "cfg_scale": 2.5, "negative_prompt": "no text",
              "sampler": "heun", "scheduler": "karras"}, None),
            ({"workflow": "image_to_image", "strength": 0.4, "seed": 4294967295,
              "cfg_scale": 20}, "image_id"),
            ({"workflow": "inpainting", "strength": 0.65}, ("image_id", "mask_id")),
            ({"workflow": "controlnet", "control_strength": 1.7, "canny": True},
             "guide_id"),
        ]
        forwarded = []
        for workflow, upload_fields in cases:
            body = {"model": "sdxl-base-q8-0", "prompt": "a mountain at dawn",
                    "image_workflow": workflow}
            upload_ids = []
            if isinstance(upload_fields, str):
                upload_ids.append((upload_fields, self.upload_image()[0]))
            elif isinstance(upload_fields, tuple):
                upload_ids.extend((field, self.upload_image()[0]) for field in upload_fields)
            body["image_workflow"].update(upload_ids)
            head, response = self.request("/api/generate", body, environment=environment)
            self.assertIn("200 OK", head)
            result = json.loads(response)
            self.assertTrue(result["done"])
            self.assertTrue(Path(result["image_path"]).is_file())
            if upload_ids:
                for _, image_id in upload_ids:
                    self.assertFalse((self.image_dir / f".image-upload-{image_id}.upload").exists())
            forwarded.append(json.loads(self.generate_calls_file.read_text().splitlines()[-1]))

        self.assertEqual([item["workflow"] for item in forwarded],
                         ["text_to_image", "image_to_image", "inpainting", "controlnet"])
        self.assertEqual((forwarded[0]["width"], forwarded[0]["height"], forwarded[0]["steps"]),
                         (768, 1024, 17))
        self.assertEqual(forwarded[0]["seed"], -1)
        self.assertEqual(forwarded[0]["sampler"], "heun")
        self.assertEqual(forwarded[0]["scheduler"], "karras")
        self.assertAlmostEqual(forwarded[1]["strength"], 0.4)
        self.assertEqual(forwarded[1]["seed"], 4294967295)
        self.assertEqual(forwarded[1]["cfg_scale"], 20.0)
        self.assertTrue(forwarded[1]["image_path"].endswith(".upload"))
        self.assertTrue(forwarded[2]["mask_path"].endswith(".upload"))
        self.assertAlmostEqual(forwarded[3]["control_strength"], 1.7)
        self.assertTrue(forwarded[3]["canny"])

    def test_upscale_is_per_request_and_defaults_to_the_settings_toggle(self):
        environment = self.image_workflow_environment()
        base = {"model": "sdxl-base-q8-0", "prompt": "a mountain at dawn"}

        def run(workflow):
            head, response = self.request("/api/generate", {**base, "image_workflow": workflow},
                                          environment=environment)
            self.assertIn("200 OK", head)
            self.assertTrue(json.loads(response)["done"])
            return json.loads(self.generate_calls_file.read_text().splitlines()[-1])

        head, _ = self.request("/api/settings/runtime", {"sd_upscale": 0}, method="POST",
                               environment=environment)
        self.assertIn("200 OK", head)
        text_to_image = {"workflow": "text_to_image"}
        self.assertEqual(run(text_to_image)["upscale"], 0)
        self.assertEqual(run({**text_to_image, "upscale": 2})["upscale"], 2)
        self.assertEqual(run({**text_to_image, "upscale": 4})["upscale"], 4)
        self.assertEqual(run({**text_to_image, "upscale": 0})["upscale"], 0)
        # A boolean is the pre-factor spelling and still means 4x.
        self.assertEqual(run({**text_to_image, "upscale": True})["upscale"], 4)
        self.assertEqual(run({**text_to_image, "upscale": False})["upscale"], 0)
        for rejected in (1, 3, 8, "4"):
            head, response = self.request("/api/generate",
                                          {**base, "image_workflow":
                                           {**text_to_image, "upscale": rejected}},
                                          environment=environment)
            self.assertIn("400 Bad Request", head, rejected)
            self.assertIn("upscale must be 0, 2, or 4", json.loads(response)["error"])

        # Without the field the request follows the saved Settings choice.
        head, _ = self.request("/api/settings/runtime", {"sd_upscale": 2}, method="POST",
                               environment=environment)
        self.assertIn("200 OK", head)
        self.assertEqual(run(text_to_image)["upscale"], 2)
        self.assertEqual(run({**text_to_image, "upscale": 0})["upscale"], 0)
        head, _ = self.request("/api/settings/runtime", {"sd_upscale": True}, method="POST",
                               environment=environment)
        self.assertIn("200 OK", head)
        self.assertEqual(json.loads(self.request("/api/settings/runtime",
                                                 environment=environment)[1])["sd_upscale"], 4)
        head, _ = self.request("/api/settings/runtime", {"sd_upscale": 0}, method="POST",
                               environment=environment)
        self.assertIn("200 OK", head)

        head, response = self.request("/api/generate",
                                      {**base, "image_workflow": {**text_to_image, "upscale": "yes"}},
                                      environment=environment)
        self.assertIn("400", head)
        self.assertIn("upscale must be 0, 2, or 4", response)

    def test_image_workflow_rejects_bounds_capabilities_and_invalid_uploads_before_runtime(self):
        image_id, image_path = self.upload_image()
        base = {"model": "sdxl-base-q8-0", "prompt": "valid prompt",
                "image_workflow": {"workflow": "image_to_image", "image_id": image_id}}
        invalid_workflows = [
            {"width": 255}, {"width": 1024}, {"width": 1024, "height": 1088},
            {"width": 1536, "height": 1536},
            {"steps": 0}, {"steps": 51}, {"seed": -2}, {"seed": 4294967296},
            {"seed": 1.5}, {"cfg_scale": -0.1}, {"cfg_scale": 20.1},
            {"strength": -0.1}, {"strength": 1.1}, {"sampler": "unknown"},
            {"scheduler": "unknown"},
        ]
        for invalid in invalid_workflows:
            with self.subTest(invalid=invalid):
                self.generate_calls_file.unlink(missing_ok=True)
                body = json.loads(json.dumps(base))
                body["image_workflow"].update(invalid)
                head, response = self.request("/api/generate", body, environment={
                    "PROSPERO_TEST_IMAGE_METADATA": "1",
                    "PROSPERO_TEST_GENERATE_CALLS_FILE": str(self.generate_calls_file),
                })
                self.assertIn("400 Bad Request", head)
                self.assertIn("error", json.loads(response))
                self.assertFalse(self.generate_calls_file.exists())

        for workflow in (
            {"workflow": "controlnet", "guide_id": image_id},
            {"workflow": "inpainting", "image_id": image_id, "mask_id": image_id},
        ):
            head, _ = self.request("/api/generate", {**base, "image_workflow": workflow},
                                   environment={"PROSPERO_TEST_IMAGE_METADATA": "1"})
            self.assertIn("400 Bad Request", head)

        for workflow, expected in (({"workflow": "inpainting"}, "image_id and mask_id"),
                                   ({"workflow": "controlnet"}, "guide_id")):
            head, response = self.request("/api/generate", {**base, "image_workflow": workflow},
                environment={"PROSPERO_TEST_IMAGE_METADATA": "1",
                             "PROSPERO_TEST_IMAGE_WORKFLOWS": "1"})
            self.assertIn("400 Bad Request", head)
            self.assertIn(expected, json.loads(response)["error"])

        body = {**base, "image_workflow": {"workflow": "image_to_image",
                                           "image_id": "0" * 32}}
        self.assertIn("400 Bad Request", self.request("/api/generate", body,
            environment={"PROSPERO_TEST_IMAGE_METADATA": "1"})[0])
        self.assertTrue(image_path.exists())

        corrupt_id = "f" * 32
        corrupt_path = self.image_dir / f".image-upload-{corrupt_id}.upload"
        corrupt_path.write_bytes(b"not a decodable image")
        body["image_workflow"]["image_id"] = corrupt_id
        self.assertIn("400 Bad Request", self.request("/api/generate", body,
            environment={"PROSPERO_TEST_IMAGE_METADATA": "1"})[0])
        self.assertFalse(corrupt_path.exists())

        expired_id, expired_path = self.upload_image()
        os.utime(expired_path, (1, 1))
        body["image_workflow"]["image_id"] = expired_id
        self.assertIn("400 Bad Request", self.request("/api/generate", body,
            environment={"PROSPERO_TEST_IMAGE_METADATA": "1"})[0])
        self.assertFalse(expired_path.exists())

    def test_reference_image_models_reject_strength_and_failed_generation_discards_lease(self):
        image_id, image_path = self.upload_image()
        body = {"model": "sdxl-base-q8-0", "prompt": "edit",
                "image_workflow": {"workflow": "image_to_image", "image_id": image_id,
                                   "strength": 0.5}}
        head, response = self.request("/api/generate", body, environment={
            "PROSPERO_TEST_IMAGE_METADATA": "1", "PROSPERO_TEST_REFERENCE_IMAGE": "1",
            "PROSPERO_TEST_GENERATE_CALLS_FILE": str(self.generate_calls_file),
        })
        self.assertIn("400 Bad Request", head)
        self.assertIn("strength", json.loads(response)["error"])
        self.assertFalse(self.generate_calls_file.exists())
        self.assertTrue(image_path.exists())

        body["image_workflow"].pop("strength")
        head, response = self.request("/api/generate", body, environment={
            **self.image_workflow_environment(), "PROSPERO_TEST_IMAGE_GENERATE_FAIL": "1",
        })
        self.assertIn("502 Bad Gateway", head)
        self.assertIn("fixture image generation failure", json.loads(response)["error"])
        self.assertFalse(image_path.exists())

    def test_image_workflow_archives_output_and_async_jobs_complete(self):
        session_id = "image-workflow-archive"
        self.save_conversation_with_image(session_id)
        try:
            head, body = self.request("/api/generate", {
                "model": "sdxl-base-q8-0", "prompt": "gallery output",
                "image_workflow": {"workflow": "text_to_image"},
                "output_session_id": session_id, "output_message_index": 0,
            }, environment=self.image_workflow_environment())
            self.assertIn("200 OK", head)
            archived = json.loads(body)["image_path"]
            self.assertTrue(Path(archived).is_file())
            media = json.loads(self.request("/api/media")[1])["data"]
            self.assertIn(archived, {item["path"] for item in media})

            _, polled = self.request_audio_job_with_poll({
                "async": True, "model": "sdxl-base-q8-0", "prompt": "async image",
                "image_workflow": {"workflow": "text_to_image"},
            }, environment=self.image_workflow_environment())
            self.assertEqual(polled["state"], "done")
            self.assertTrue(polled["result"]["done"])
            self.assertTrue(Path(polled["result"]["image_path"]).is_file())
        finally:
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=["Authorization: Bearer secret"])

    def test_output_reference_archived_image_can_be_deleted_from_media(self):
        session_id = "image-output-delete"
        unowned = Path("/tmp/prosperoai-session-test/work/image/unowned-delete-case/unowned.png")
        self.save_conversation_with_image(session_id)
        try:
            head, body = self.request("/api/generate", {
                "model": "sdxl-base-q8-0", "prompt": "gallery output",
                "image_workflow": {"workflow": "text_to_image"},
                "output_session_id": session_id, "output_message_index": 0,
            }, environment=self.image_workflow_environment())
            self.assertIn("200 OK", head)
            archived = Path(json.loads(body)["image_path"])
            self.assertTrue(archived.is_file())
            conversation = json.loads(self.request(
                "/api/conversations/" + session_id, key="secret",
                headers=["Authorization: Bearer secret"])[1])
            self.assertNotIn(str(archived), conversation["messages"][0]["content"])
            self.assertIn(str(archived), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })

            # A file in the work tree that no conversation points at can still be
            # removed; there is just nothing to rewrite.
            unowned.parent.mkdir(parents=True, exist_ok=True)
            unowned.write_bytes((ROOT / "tests/fixtures/one-pixel.png").read_bytes())
            head, _ = self.request("/api/media", {"path": str(unowned)}, method="DELETE")
            self.assertIn("200 OK", head)
            self.assertFalse(unowned.exists())
            unowned.parent.rmdir()

            # A file that has gone missing behind our back still clears its entry:
            # refusing left it listed in Media with no way to ever remove it.
            archived.unlink()
            head, _ = self.request("/api/media", {"path": str(archived)}, method="DELETE")
            self.assertIn("200 OK", head)
            self.assertNotIn(str(archived), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })
        finally:
            unowned.unlink(missing_ok=True)
            try:
                unowned.parent.rmdir()
            except OSError:
                pass
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=["Authorization: Bearer secret"])

    def test_concurrent_archive_cannot_be_orphaned_by_output_reference_delete(self):
        session_id = "image-output-race"
        self.save_conversation_with_image(session_id)
        gate = Path(self.scratch.name) / "reference-delete-gate"
        try:
            result = subprocess.run(
                [str(self.binary), "--test-output-reference-delete-race"],
                env={**os.environ, **self.image_workflow_environment(),
                     "PROSPERO_TEST_REMOVE_GATE": str(gate)},
                capture_output=True, text=True, timeout=20,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("output reference race: ok", result.stdout)
        finally:
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=["Authorization: Bearer secret"])

    def upload_image(self):
        payload = (ROOT / "tests/fixtures/one-pixel.jpg").read_bytes()
        head, body = self.request_raw(
            "/api/image/upload", payload, key="secret",
            headers=["Authorization: Bearer secret"], method="POST",
        )
        self.assertIn("200 OK", head)
        image_id = json.loads(body)["id"]
        return image_id, self.image_dir / f".image-upload-{image_id}.upload"

    def test_openai_chat_rejects_legacy_image_id_use(self):
        image_id, _ = self.upload_image()
        head, body = self.request(body={
            "model": "tiny.gguf", "image_id": image_id,
            "messages": [{"role": "user", "content": "hello"}],
        }, key="secret", headers=["Authorization: Bearer secret"])
        self.assertIn("400 Bad Request", head)
        self.assertIn("invalid_image_request", body)

    def test_media_named_in_the_text_and_recorded_as_output_is_one_reference(self):
        # An archived generation is usually both: the message names the file and the
        # conversation records it as that message's output. Counting those separately
        # refused to delete nearly every archived generation.
        session_id = "image-double-reference"
        headers = ["Authorization: Bearer secret"]
        self.save_conversation_with_image(session_id)
        try:
            head, body = self.request("/api/generate", {
                "model": "sdxl-base-q8-0", "prompt": "gallery output",
                "image_workflow": {"workflow": "text_to_image"},
                "output_session_id": session_id, "output_message_index": 0,
            }, environment=self.image_workflow_environment())
            self.assertIn("200 OK", head)
            archived = Path(json.loads(body)["image_path"])
            head, _ = self.request("/api/conversations", {
                "id": session_id, "title": "Image conversation", "model": "image-model-test",
                "modelName": "Image model test", "purpose": "text-to-image",
                "messages": [{"role": "assistant", "content": f"Saved to {archived}"}],
            }, key="secret", headers=headers)
            self.assertIn("200 OK", head + "200 OK")
            self.assertIn(str(archived), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })

            head, response = self.request("/api/media", {"path": str(archived)}, method="DELETE")
            self.assertIn("200 OK", head, response)
            self.assertFalse(archived.exists())
            detail = json.loads(self.request("/api/conversations/" + session_id, key="secret",
                                             headers=headers)[1])
            self.assertIn("[media deleted]", detail["messages"][0]["content"])
            self.assertNotIn(str(archived), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })
        finally:
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=headers)

    def test_images_can_be_upscaled_on_their_own(self):
        source = self.image_dir / "to-enlarge.png"
        source.write_bytes((ROOT / "tests/fixtures/one-pixel.png").read_bytes())
        produced = self.image_dir / "upscaled-x4.png"
        try:
            head, body = self.request("/api/images/upscale",
                                      {"path": str(source), "factor": 4})
            self.assertIn("200 OK", head, body)
            self.assertEqual(json.loads(body)["image_path"], str(produced))
            self.assertTrue(produced.is_file())

            for rejected in ({"path": str(source), "factor": 3},
                             {"path": str(source)},
                             {"path": "/etc/passwd", "factor": 4},
                             {"factor": 4}):
                head, response = self.request("/api/images/upscale", rejected)
                self.assertIn("400 Bad Request", head, rejected)
                self.assertIn("factor of 2 or 4", json.loads(response)["error"])

            head, response = self.request("/api/images/upscale",
                                          {"path": str(source), "factor": 2},
                                          environment={"PROSPERO_TEST_UPSCALE_FAIL": "1"})
            self.assertIn("409 Conflict", head)
            self.assertIn("could not upscale", json.loads(response)["error"])
        finally:
            source.unlink(missing_ok=True)
            produced.unlink(missing_ok=True)
            (self.image_dir / "upscaled-x2.png").unlink(missing_ok=True)

    def test_media_lists_work_folder_files_with_no_conversation(self):
        # Music Studio writes straight to the audio folder and never saves a
        # conversation, so its tracks were missing from Media entirely.
        track = self.audio_dir / "studio-track.wav"
        picture = self.image_dir / "studio-picture.png"
        track.write_bytes(b"RIFF0000WAVEfmt ")
        picture.write_bytes((ROOT / "tests/fixtures/one-pixel.png").read_bytes())
        try:
            listed = {item["path"]: item for item in
                      json.loads(self.request("/api/media")[1])["data"]}
            self.assertIn(str(track), listed)
            self.assertIn(str(picture), listed)
            self.assertEqual(listed[str(track)]["kind"], "audio")
            self.assertEqual(listed[str(picture)]["kind"], "image")
            self.assertEqual(listed[str(track)]["conversation_id"], "")

            head, _ = self.request("/api/media", {"path": str(track)}, method="DELETE")
            self.assertIn("200 OK", head)
            self.assertFalse(track.exists())
            self.assertNotIn(str(track), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })
        finally:
            track.unlink(missing_ok=True)
            picture.unlink(missing_ok=True)

    def test_media_generated_outside_a_conversation_folder_can_be_deleted(self):
        # Most of the gallery is flat work/image files a message merely mentions; the
        # delete button has to reach those too, not only per-conversation archive copies.
        session_id = "flat-media-delete"
        flat = Path("/tmp/prosperoai-session-test/work/image/prosperoai-image-flat-1.png")
        headers = ["Authorization: Bearer secret"]
        self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                     headers=headers)
        flat.parent.mkdir(parents=True, exist_ok=True)
        flat.write_bytes((ROOT / "tests/fixtures/one-pixel.png").read_bytes())
        try:
            head, _ = self.request("/api/conversations", {
                "id": session_id, "title": "Flat media", "model": "image-model-test",
                "modelName": "Image model test", "purpose": "text-to-image",
                "messages": [{"role": "assistant", "content": f"Saved to {flat}"}],
            }, key="secret", headers=headers)
            self.assertIn("201 Created", head)
            self.assertIn(str(flat), {
                item["path"] for item in json.loads(self.request("/api/media")[1])["data"]
            })
            head, body = self.request("/api/media", {"path": str(flat)}, method="DELETE")
            self.assertIn("200 OK", head, body)
            self.assertFalse(flat.exists())
            detail = json.loads(self.request("/api/conversations/" + session_id, key="secret",
                                             headers=headers)[1])
            self.assertIn("[media deleted]", detail["messages"][0]["content"])
        finally:
            flat.unlink(missing_ok=True)
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=headers)

    def save_conversation_with_image(self, session_id, purpose="text-to-image", image=None):
        headers = ["Authorization: Bearer secret"]
        self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                     headers=headers)
        message = {"role": "user", "content": "edit this"}
        if image is not None:
            message["image"] = image
        return self.request("/api/conversations", {
            "id": session_id, "title": "Image conversation", "model": "image-model-test",
            "modelName": "Image model test", "purpose": purpose, "messages": [message],
        }, key="secret", headers=headers)

    def test_conversation_sync_roundtrips_only_owned_png_image_paths(self):
        session_id = "image-sync-session"
        self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                     headers=["Authorization: Bearer secret"])
        archived = Path("/tmp/prosperoai-session-test/work/image") / session_id / "000-123.png"
        archived.parent.mkdir(parents=True, exist_ok=True)
        archived.write_bytes((ROOT / "tests/fixtures/one-pixel.png").read_bytes())
        try:
            head, _ = self.save_conversation_with_image(session_id, image=str(archived))
            self.assertIn("201 Created", head)
            detail = json.loads(self.request(
                "/api/conversations/" + session_id, key="secret",
                headers=["Authorization: Bearer secret"])[1])
            self.assertEqual(detail["messages"][0]["image"], str(archived))
            invalid = {"id": session_id, "title": "Image conversation", "model": "image-model-test",
                       "modelName": "Image model test", "purpose": "text-to-image",
                       "messages": [{"role": "user", "content": "edit this",
                                     "image": "/tmp/unowned-image.png"}]}
            self.assertIn("400 Bad Request", self.request(
                "/api/conversations", invalid, key="secret",
                headers=["Authorization: Bearer secret"])[0])
            invalid["messages"][0]["image"] = str(archived) + "\0outside"
            self.assertIn("400 Bad Request", self.request(
                "/api/conversations", invalid, key="secret",
                headers=["Authorization: Bearer secret"])[0])
        finally:
            self.request("/api/conversations/" + session_id, method="DELETE", key="secret",
                         headers=["Authorization: Bearer secret"])

    def request(self, path="/v1/chat/completions", body=None, key=None, headers=None, method=None,
                environment=None):
        if body is None and path.endswith("completions"):
            body = {"model": "tiny.gguf", "messages": [{"role": "user", "content": "hello"}]}
        payload = json.dumps(body, ensure_ascii=False).encode() if body is not None else b""
        return self.request_raw(path, payload, key, headers, method, environment)

    def request_raw(self, path, payload, key=None, headers=None, method=None, environment=None,
                    content_length=None):
        wire = self.request_raw_bytes(path, payload, key, headers, method, environment,
                                      content_length)
        head, data = wire.split(b"\r\n\r\n", 1)
        return head.decode(), data.decode()

    def request_raw_bytes(self, path, payload=b"", key=None, headers=None, method=None,
                          environment=None, content_length=None):
        if content_length is None:
            content_length = len(payload)
        lines = [f"{method or ('POST' if payload else 'GET')} {path} HTTP/1.1", "Host: test",
                 f"content-length: {content_length}"] + (headers or [])
        return subprocess.check_output([str(self.binary)] + ([key] if key else []),
                                       env={**os.environ, **(environment or {})},
                                       input="\r\n".join(lines).encode()+b"\r\n\r\n"+payload)

    def request_without_length(self, path, payload, key=None, headers=None, environment=None):
        lines = [f"POST {path} HTTP/1.1", "Host: test"] + (headers or [])
        wire = subprocess.check_output(
            [str(self.binary)] + ([key] if key else []),
            env={**os.environ, **(environment or {})},
            input="\r\n".join(lines).encode() + b"\r\n\r\n" + payload,
        )
        head, data = wire.split(b"\r\n\r\n", 1)
        return head.decode(), data.decode()

    def request_audio_job_with_poll(self, body, environment=None):
        payload = json.dumps(body, ensure_ascii=False).encode()
        request = (f"POST /api/generate HTTP/1.1\r\nHost: test\r\n"
                   f"Content-Length: {len(payload)}\r\n\r\n").encode() + payload
        envelope = subprocess.check_output(
            [str(self.binary), "--test-audio-job-poll"],
            env={**os.environ, **(environment or {})}, input=request, timeout=10,
        )
        responses = json.loads(envelope)
        return json.loads(responses["submission"]), json.loads(responses["poll"])

    def test_binary_music_upload_preserves_existing_tracks_and_cleans_interrupted_uploads(self):
        existing = self.audio_dir / "song.wav"
        existing.write_bytes(b"original track")
        headers = ["X-Upload-Name: song.wav", "Authorization: Bearer secret"]
        head, _ = self.request_raw("/api/music/uploadbin", b"replacement", key="secret",
                                   headers=headers, method="POST")
        self.assertIn("409 Conflict", head)
        self.assertEqual(existing.read_bytes(), b"original track")

        head, _ = self.request_raw("/api/music/uploadbin", b"partial", key="secret",
                                   headers=["X-Upload-Name: interrupted.wav",
                                            "Authorization: Bearer secret"],
                                   method="POST", content_length=20)
        self.assertIn("500 Internal Server Error", head)
        self.assertFalse((self.audio_dir / "interrupted.wav").exists())
        self.assertEqual(sorted(path.name for path in self.audio_dir.iterdir()), ["song.wav"])

        temp_collision = self.audio_dir / ".music-upload-0"
        temp_collision.write_bytes(b"keep this unrelated file")
        head, _ = self.request_raw("/api/music/uploadbin", b"new track", key="secret",
                                   headers=["X-Upload-Name: fresh.wav",
                                            "Authorization: Bearer secret"],
                                   method="POST")
        self.assertIn("200 OK", head)
        self.assertEqual((self.audio_dir / "fresh.wav").read_bytes(), b"new track")
        self.assertEqual(temp_collision.read_bytes(), b"keep this unrelated file")
        self.assertEqual(sorted(path.name for path in self.audio_dir.iterdir()),
                         [".music-upload-0", "fresh.wav", "song.wav"])

    def test_concurrent_same_name_uploads_publish_only_one_complete_track(self):
        result = subprocess.run([str(self.binary), "--test-upload-race"], check=True,
                                capture_output=True, text=True, timeout=60)
        statuses = result.stdout.splitlines()
        self.assertEqual(len(statuses), 40)
        for index in range(0, len(statuses), 2):
            self.assertEqual(sorted(statuses[index:index + 2]),
                             ["HTTP/1.1 200 OK", "HTTP/1.1 409 Conflict"])
        final_track = (self.audio_dir / "race.wav").read_bytes()
        self.assertIn(final_track, [b"a" * (512 * 1024), b"b" * (512 * 1024)])

    def test_mastering_reads_all_bytes_of_odd_sized_24_bit_mono_audio(self):
        import struct

        wav = bytearray(b"RIFF")
        wav.extend(struct.pack("<I", 40))
        wav.extend(b"WAVEfmt ")
        wav.extend(struct.pack("<IHHIIHH", 16, 1, 1, 44100, 132300, 3, 24))
        wav.extend(b"data")
        wav.extend(struct.pack("<I", 3))
        wav.extend(b"\x01\x00\x7f\x00")  # one full-scale sample plus RIFF padding
        source = self.audio_dir / "odd-mono.wav"
        source.write_bytes(wav)
        head, _ = self.request("/api/music/master", {"file": source.name})
        self.assertIn("200 OK", head)
        mastered = self.audio_dir / "odd-mono-mastered.wav"
        sample = struct.unpack_from("<h", mastered.read_bytes(), 44)[0]
        self.assertGreater(sample, 30000)

    def test_audio_lyrics_round_trip_and_persistence(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\ntokens=192\n')
        lyrics = '[verse]\nCântă, cântă, inima mea\n[chorus]\nPiesa-ntreaga e a ta\n'
        header, body = self.request('/api/settings/runtime', body={'audio_lyrics': lyrics})
        self.assertIn('200 OK', header)
        self.assertEqual(json.loads(body)['audio_lyrics'], lyrics)
        lines = config.read_text().splitlines()
        self.assertTrue(any(line.startswith('audio_lyrics_b64=') for line in lines))
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['audio_lyrics'], lyrics)
        header, body = self.request('/api/settings/runtime', body={'audio_lyrics': ''})
        self.assertIn('200 OK', header)
        self.assertEqual(json.loads(body)['audio_lyrics'], '')
        self.assertFalse(any(line.startswith('audio_lyrics_b64=') for line in config.read_text().splitlines()))
        for invalid in ({'audio_lyrics': 5}, {'audio_lyrics': 'x' * 4097}):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])

    def test_audio_options_round_trip_and_persistence(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\ntokens=192\n')
        options = 'cot=full\nsemantic_temperature=1.1\nsemantic_prefix_file=/download0/ProsperoAI/work/audio/song-semantic.json\n'
        header, body = self.request('/api/settings/runtime', body={'audio_options': options})
        self.assertIn('200 OK', header)
        self.assertEqual(json.loads(body)['audio_options'], options)
        lines = config.read_text().splitlines()
        self.assertTrue(any(line.startswith('audio_options_b64=') for line in lines))
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['audio_options'], options)
        header, body = self.request('/api/settings/runtime', body={'audio_options': ''})
        self.assertIn('200 OK', header)
        self.assertEqual(json.loads(body)['audio_options'], '')
        self.assertFalse(any(line.startswith('audio_options_b64=') for line in config.read_text().splitlines()))
        for invalid in ({'audio_options': 5}, {'audio_options': 'x' * 4097}, {'audio_options': 'cot=full\n\x07bell'}):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])

    def test_runtime_settings_round_trip(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\nstyle=2\ntokens=192\ntheme=1\n')
        header, body = self.request('/api/settings/runtime', body={'context':'32k', 'kv':'auto', 'thinking':True, 'sd_upscale':2, 'idle_unload_minutes':2, 'kokoro_voice':'af_bella', 'audio_duration_sec':30, 'audio_seed':123, 'sd_seed':456})
        self.assertIn('200 OK', header)
        expected = json.loads(body)
        self.assertEqual({k:expected[k] for k in ('context','kv','thinking','sd_upscale','idle_unload_minutes','kokoro_voice','audio_duration_sec','audio_seed','sd_seed')},
                         {'context':'32k','kv':'auto','thinking':True,'sd_upscale':2,'idle_unload_minutes':2,'kokoro_voice':'af_bella','audio_duration_sec':30,'audio_seed':123,'sd_seed':456})
        self.assertEqual(json.loads(body), expected)
        for line in ('style=2','tokens=192','theme=1'):
            self.assertIn(line,config.read_text().splitlines())
        for line in ('audio_duration_sec=30','audio_seed=123','sd_seed=456','sd_upscale=2'):
            self.assertIn(line,config.read_text().splitlines())
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        header, body = self.request('/api/settings/runtime', body={'context':'256k', 'kv':'auto', 'thinking':True, 'idle_unload_minutes':5})
        self.assertIn('200 OK', header)
        expected.update(context='256k',kv='auto',thinking=True,idle_unload_minutes=5,kokoro_voice='af_bella')
        self.assertEqual(json.loads(body), expected)
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        for invalid in ({'context':'2k'}, {'kv':'q2_0'}, {'thinking':'false'}, {'sd_upscale':1}, {'sd_upscale':3}, {'sd_upscale':'4'}, {'idle_unload_minutes':3}, {'kokoro_voice':'missing'}, {'audio_duration_sec':301}, {'audio_seed':-1}, {'audio_seed':4294967296}, {'sd_seed':-1}, {'sd_seed':4294967296}, {'unknown':1}, {'context':'16k','kv':'bad'}, []):
            self.assertIn('400 Bad Request', self.request('/api/settings/runtime', body=invalid)[0])
            self.assertEqual(json.loads(self.request('/api/settings/runtime')[1]), expected)
        self.assertIn('401 Unauthorized', self.request('/api/settings/runtime', key='secret')[0])
        self.request('/api/settings/runtime', body={'context':'auto','kv':'auto','thinking':False,'idle_unload_minutes':2})

    def test_runtime_kv_choices_round_trip_and_persist(self):
        config = Path(self.scratch.name) / 'runtime.cfg'
        config.write_text('version=2\ntokens=192\n')
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['kv'], 'auto')

        for index, kv in enumerate(('auto', 'f16', 'q8_0', 'q4_0')):
            header, body = self.request('/api/settings/runtime', body={'kv': kv})
            self.assertIn('200 OK', header)
            self.assertEqual(json.loads(body)['kv'], kv)
            self.assertIn(f'kv={index}', config.read_text().splitlines())
            self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['kv'], kv)

        for invalid in ('q2_0', '', 'Q4_0', 3):
            self.assertIn('400 Bad Request',
                          self.request('/api/settings/runtime', body={'kv': invalid})[0])
            self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['kv'], 'q4_0')

        config.write_text('version=2\nkv=9\n')
        self.assertEqual(json.loads(self.request('/api/settings/runtime')[1])['kv'], 'auto')

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

    def test_manual_model_refresh_rescans_installed_models(self):
        header, body = self.request('/api/models/refresh', method='POST', body={})
        self.assertIn('200 OK', header)
        self.assertEqual([model['name'] for model in json.loads(body)['models']],
                         ['tiny.gguf', 'second.gguf'])

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
        head, body = self.request("/api/models/memory", environment={
            "PROSPERO_TEST_VULKAN_MEMORY": "valid",
            "PROSPERO_TEST_MODEL_LOADED": "1",
        })
        self.assertIn("200 OK", head)
        payload = json.loads(body)
        self.assertTrue(payload["loaded"])
        self.assertEqual(payload["vulkan_memory_available"], True)
        self.assertEqual(payload["vulkan_free_bytes"], 6 * 1024**3)
        self.assertEqual(payload["vulkan_allocated_bytes"], 4 * 1024**3)
        self.assertEqual(payload["vulkan_heap_bytes"], 10 * 1024**3)
        self.assertEqual(payload["name"], "SheetSage test fixture")
        self.assertIn("largest_direct_block_bytes", payload)
        self.assertIn("flexible_free_bytes", payload)
        self.assertEqual(payload["estimate_confidence"], "live-memory-snapshot")

        _, body = self.request("/api/models/memory", environment={
            "PROSPERO_TEST_VULKAN_MEMORY": "unavailable",
            "PROSPERO_TEST_MODEL_LOADED": "1",
        })
        payload = json.loads(body)
        self.assertFalse(payload["vulkan_memory_available"])
        self.assertIsNone(payload["vulkan_free_bytes"])
        self.assertIsNone(payload["vulkan_allocated_bytes"])
        self.assertEqual(payload["vulkan_heap_bytes"], 0)

        _, body = self.request("/api/models/memory", environment={
            "PROSPERO_TEST_VULKAN_MEMORY": "valid",
        })
        payload = json.loads(body)
        self.assertFalse(payload["loaded"])
        self.assertTrue(payload["vulkan_memory_available"])
        self.assertEqual(payload["vulkan_allocated_bytes"], 4 * 1024**3)
        self.assertEqual(payload["vulkan_heap_bytes"], 10 * 1024**3)
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

    def test_image_model_metadata_is_exposed_by_both_catalog_routes(self):
        _, body = self.request("/v1/models", environment={"PROSPERO_TEST_IMAGE_METADATA": "1"})
        image = next(model for model in json.loads(body)["data"] if model["id"] == "sdxl-base-q8-0")
        self.assertEqual(image["details"]["capabilities"], ["text_to_image", "image_to_image"])
        self.assertEqual(image["details"]["image_edit_strategy"], "init_image_strength")
        self.assertEqual(image["details"]["image_defaults"], {
            "width": 1024, "height": 1024, "steps": 10, "cfg_scale": 6.0,
            "sampler": "dpm++2m", "scheduler": "ays"})

        _, body = self.request("/api/models/presets")
        preset = next(item for item in json.loads(body)["data"] if item["id"] == "sdxl-canny-q8-0")
        self.assertEqual(preset["image_capabilities"], ["text_to_image", "image_to_image", "controlnet"])
        self.assertEqual(preset["image_edit_strategy"], "init_image_strength")
        self.assertEqual(preset["image_defaults"]["steps"], 25)
        self.assertEqual(preset["verified_by"], "PS5SD")
        self.assertEqual(preset["runtime_status"], "experimental")

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
        self.assertEqual({preset["kind"] for preset in presets}, {0, 1, 2, 3, 4})
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

    def test_tags_catalog_is_complete_valid_json(self):
        head, body = self.request_raw("/api/tags", b"", method="GET",
                                      environment={"PROSPERO_TEST_MANY_MODELS": "1"})
        self.assertIn("200 OK", head)
        models = json.loads(body)["models"]
        self.assertEqual(len(models), 40)
        for index, entry in enumerate(models):
            expected = f"prospero-test-model-catalog-entry-{index}.gguf"
            self.assertEqual(entry["model"], expected)
            self.assertEqual(entry["name"], expected)
            self.assertEqual(entry["details"]["family"], "text-to-text")

if __name__ == "__main__":
    unittest.main()
