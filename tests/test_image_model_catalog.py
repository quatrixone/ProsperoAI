import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ImageModelCatalogTests(unittest.TestCase):
    def test_curated_image_variants_keep_source_and_workflow_metadata(self):
        presets = json.loads((ROOT / "model-tools/presets.json").read_text())
        by_id = {preset["id"]: preset for preset in presets}
        ps5sd_revision = "0654534bc7042d9164167835fba67e74215f1617"
        primary_sha256 = {
            "sdxs-512-q8-0": "409ab23582ee074c6b9d5395784fc0741b0599fb9d138686c69087c71678eb6a",
            "sdxl-base-q8-0": "2ef973152d2fbe27e37e4fd5ec3569dec1f828ee279975e60acc12bc78c9f981",
            "raehoshi-illustxl-1-1-q8-0": "0753c9ddcb82f4d47d98ca2f65b0e5944a8e4528b848d0741654be0740973e64",
            "ilustmix-v11-1-q8-0": "038dad76321eb847a115eab813ed6ff68c603d4bc0e9eb7c3e1be756dabf3dc2",
            "sdxl-inpainting-q8-0": "4a08b13f91933e06625a7026c787408100e2116326aeda961c9a428e61c2d9c0",
            "sd15-inpainting-q8-0": "2301f1a0712de3dc2ff16181e1023865aa837922216e439491b98b00d12eb5fc",
            "sdxl-canny-q8-0": "2ef973152d2fbe27e37e4fd5ec3569dec1f828ee279975e60acc12bc78c9f981",
            "flux2-klein-4b-q8-0": "0bba6951258ec8f92d51114a8fa13e66828297bfff58a738f52729b3ef66fa28",
            "flux2-klein-9b-q4-0": "a7e77afa96871d16679ff7b949bd25f20c8179f219c4b662cac91e81ed99b944",
            "z-image-turbo-q4-k": "14b375ab4f226bc5378f68f37e899ef3c2242b8541e61e2bc1aff40976086fbd",
        }
        required_asset_checks = {
            "vae/sdxl_vae_fp16_fix.safetensors": (
                "madebyollin/sdxl-vae-fp16-fix", "207b116dae70ace3637169f1ddd2434b91b3a8cd",
                "sdxl_vae.safetensors", 334641162,
                "235745af8d86bf4a4c1b5b4f529868b37019a10f7c0b2e79ad0abca3a22bc6e1"),
            "controlnet/xinsir-canny-sdxl-v2.safetensors": (
                "xinsir/controlnet-canny-sdxl-1.0", "1271357eda52d54b857c650cacb5b51144643ccb",
                "diffusion_pytorch_model_V2.safetensors", 2502139104,
                "b3e4ac47bc814019d50dc842f579301440deb6d8f09ee1b91a30f527ace1b852"),
            "text_encoders/Qwen3-4B-Instruct-2507-Q4_K_M.gguf": (
                "unsloth/Qwen3-4B-Instruct-2507-GGUF", "a06e946bb6b655725eafa393f4a9745d460374c9",
                "Qwen3-4B-Instruct-2507-Q4_K_M.gguf", 2497281120,
                "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597"),
            "text_encoders/Qwen3-8B-Q4_K_M.gguf": (
                "unsloth/Qwen3-8B-GGUF", "a6adef130ffb23ddaf1a62fec9dced968c9bc482",
                "Qwen3-8B-Q4_K_M.gguf", 5027784512,
                "120307ba529eb2439d6c430d94104dabd578497bc7bfe7e322b5d9933b449bd4"),
            "text_encoders/Qwen3VL-4B-Instruct-Q4_K_M.gguf": (
                "Qwen/Qwen3-VL-4B-Instruct-GGUF", "1cd86afb9a95c410a6038ab3b40d8b578c892266",
                "Qwen3VL-4B-Instruct-Q4_K_M.gguf", 2497281664,
                "66358cb18bb6b3b1b6675aa412c7a88ef01d228f481184d13668e5201c730a0a"),
            "vae/flux2-vae.safetensors": (
                "Comfy-Org/flux2-dev", "9630c1b4b6e690497ae0c19bb6b1b35d4ebedcb7",
                "split_files/vae/flux2-vae.safetensors", 336213556,
                "d64f3a68e1cc4f9f4e29b6e0da38a0204fe9a49f2d4053f0ec1fa1ca02f9c4b5"),
            "vae/flux1-ae.safetensors": (
                "Comfy-Org/z_image_turbo", "6fc90a3b1b653e935a0d175e260736de25b84df5",
                "split_files/vae/ae.safetensors", 335304388,
                "afc8e28272cd15db3919bacdb6918ce9c1ed22e96cb12c4d5ed0fba823529e38"),
        }

        expected = {
            "sdxs-512-q8-0": (
                "SDXS-512 (Q8_0)",
                "Q8_0",
                "concedo/sdxs-512-tinySDdistilled-GGUF",
                "3144d898d61492f8382ffcabec055733fc5b2a0e",
                "sdxs-512-tinySDdistilled_Q8_0.gguf",
                682847200,
                ["sdxs-512-tinySDdistilled_Q8_0.gguf"],
                (512, 512, 1, 1, "euler_a", ""),
                ["text_to_image"],
                None,
                "OpenRAIL++",
            ),
            "sdxl-base-q8-0": (
                "SDXL Base 1.0 (Q8_0)",
                "Q8_0",
                "gpustack/stable-diffusion-xl-base-1.0-GGUF",
                "f00927e80a399ecb0bcacca868131d5e9d4e77aa",
                "stable-diffusion-xl-base-1.0-Q8_0.gguf",
                5040957760,
                ["stable-diffusion-xl-base-1.0-Q8_0.gguf", "vae/sdxl_vae_fp16_fix.safetensors"],
                (1024, 1024, 10, 6, "dpm++2m", "ays"),
                ["text_to_image", "image_to_image"],
                "init_image_strength",
                "OpenRAIL++",
            ),
            "raehoshi-illustxl-1-1-q8-0": (
                "Raehoshi IllustXL 1.1 (Q8_0)",
                "Q8_0",
                "Raelina/Raehoshi-illust-XL-11",
                "1dc97091d5130ef87cd7a2df92551692c771b5da",
                "raehoshi-illust-xl-11.safetensors",
                6938063182,
                ["raehoshiIllustXL_v110.safetensors", "vae/sdxl_vae_fp16_fix.safetensors"],
                (1024, 1024, 28, 6, "euler_a", ""),
                ["text_to_image", "image_to_image"],
                "init_image_strength",
                "CreativeML Open RAIL++-M",
            ),
            "ilustmix-v11-1-q8-0": (
                "iLustMix v11.1 (Q8_0)",
                "Q8_0",
                "TripleDug/my-awesome-models",
                "e46210d2d9992c1e094fba7f8fa97e45a97eeb7b",
                "ilustmix_v111.safetensors",
                7105357284,
                ["ilustmix_v111.safetensors", "vae/sdxl_vae_fp16_fix.safetensors"],
                (832, 1216, 10, 6, "dpm++2m", "ays"),
                ["text_to_image", "image_to_image"],
                "init_image_strength",
                "Illustrious License",
            ),
            "sdxl-inpainting-q8-0": (
                "SDXL Inpainting 0.1 (Q8_0)",
                "Q8_0",
                "gpustack/stable-diffusion-xl-inpainting-1.0-GGUF",
                "f01275b603d7ba5cc6774b0faf70905b5d19dc47",
                "stable-diffusion-xl-inpainting-1.0-Q8_0.gguf",
                4873718560,
                ["stable-diffusion-xl-inpainting-1.0-Q8_0.gguf", "vae/sdxl_vae_fp16_fix.safetensors"],
                (1024, 1024, 25, 6, "euler_a", ""),
                ["text_to_image", "image_to_image", "inpainting"],
                "init_image_strength",
                "OpenRAIL++",
            ),
            "sd15-inpainting-q8-0": (
                "SD 1.5 Inpainting (Q8_0)",
                "Q8_0",
                "gpustack/stable-diffusion-v1-5-inpainting-GGUF",
                "21491e4f6af4723f2677172327ea34656da4ef3e",
                "stable-diffusion-v1-5-inpainting-Q8_0.gguf",
                1881270304,
                ["stable-diffusion-v1-5-inpainting-Q8_0.gguf"],
                (512, 512, 25, 7, "euler_a", ""),
                ["text_to_image", "image_to_image", "inpainting"],
                "init_image_strength",
                "CreativeML Open RAIL-M",
            ),
            "sdxl-canny-q8-0": (
                "SDXL Base 1.0 (Q8_0) + Canny ControlNet",
                "Q8_0",
                "gpustack/stable-diffusion-xl-base-1.0-GGUF",
                "f00927e80a399ecb0bcacca868131d5e9d4e77aa",
                "stable-diffusion-xl-base-1.0-Q8_0.gguf",
                5040957760,
                ["stable-diffusion-xl-base-1.0-Q8_0.gguf", "vae/sdxl_vae_fp16_fix.safetensors",
                 "controlnet/xinsir-canny-sdxl-v2.safetensors"],
                (1024, 1024, 25, 6, "euler_a", ""),
                ["text_to_image", "image_to_image", "controlnet"],
                "init_image_strength",
                "OpenRAIL++",
            ),
            "flux2-klein-4b-q8-0": (
                "FLUX.2 Klein 4B (Q8_0)",
                "Q8_0",
                "leejet/FLUX.2-klein-4B-GGUF",
                "3b1f5a9dc3abb32238b053aeb3d823c30afdacbd",
                "flux-2-klein-4b-Q8_0.gguf",
                4300629440,
                ["diffusion_models/flux-2-klein-4b-Q8_0.gguf",
                 "text_encoders/Qwen3-4B-Instruct-2507-Q4_K_M.gguf", "vae/flux2-vae.safetensors"],
                (1024, 1024, 4, 1, "euler", ""),
                ["text_to_image", "image_to_image"],
                "reference_image",
                "Apache-2.0",
            ),
            "flux2-klein-9b-q4-0": (
                "FLUX.2 Klein 9B (Q4_0)",
                "Q4_0",
                "leejet/FLUX.2-klein-9B-GGUF",
                "cc588497a95ffc2937ebbd6b9b3916a11ada6e5b",
                "flux-2-klein-9b-Q4_0.gguf",
                5616208032,
                ["diffusion_models/flux-2-klein-9b-Q4_0.gguf",
                 "text_encoders/Qwen3-8B-Q4_K_M.gguf", "vae/flux2-vae.safetensors"],
                (1024, 1024, 8, 1, "dpm2", ""),
                ["text_to_image", "image_to_image"],
                "reference_image",
                "Non-Commercial",
            ),
            "z-image-turbo-q4-k": (
                "Z-Image Turbo (Q4_K)",
                "Q4_K",
                "leejet/Z-Image-Turbo-GGUF",
                "c61c0e422dc8b541b7548cf33a4ef8302b0f8085",
                "z_image_turbo-Q4_K.gguf",
                3864250304,
                ["diffusion_models/z_image_turbo-Q4_K.gguf",
                 "text_encoders/Qwen3-4B-Instruct-2507-Q4_K_M.gguf", "vae/flux1-ae.safetensors"],
                (1024, 1024, 8, 1, "euler", ""),
                ["text_to_image"],
                None,
                "Apache-2.0",
            ),
        }
        self.assertEqual(set(expected) - by_id.keys(), set())

        for preset_id, (name, quantization, repository, revision, filename, size, required_files,
                        defaults, capabilities, strategy, license_name) in expected.items():
            with self.subTest(preset=preset_id):
                preset = by_id[preset_id]
                self.assertEqual(preset["name"], name)
                self.assertEqual(preset["kind"], 1)
                self.assertEqual(preset["quantization"], quantization)
                self.assertEqual(preset["repository"], repository)
                self.assertEqual(preset["revision"], revision)
                model_file = next(file for file in preset["files"] if file["source"] == filename)
                self.assertEqual(model_file["size"], size)
                self.assertEqual(model_file["sha256"], primary_sha256[preset_id])
                self.assertTrue(all(any(file["path"] == path for file in preset["files"])
                                    for path in required_files))
                self.assertIn(license_name, preset["license"])
                self.assertTrue(all(re.fullmatch(r"[0-9a-f]{40}", file.get("revision", revision))
                                    for file in preset["files"]))
                self.assertEqual(preset["provenance"], {
                    "verified_by": "PS5SD", "repository": "Darkatek7/PS5SD",
                    "revision": ps5sd_revision,
                    "evidence": "docs/CONSOLE.md; ps5/app/catalog.hpp"})
                self.assertEqual(preset["runtime_status"], "experimental")
                for path in required_files:
                    if path in required_asset_checks:
                        repository, asset_revision, source, asset_size, checksum = required_asset_checks[path]
                        asset = next(file for file in preset["files"] if file["path"] == path)
                        self.assertEqual((asset["repository"], asset["revision"], asset["source"],
                                          asset["size"], asset["sha256"]),
                                         (repository, asset_revision, source, asset_size, checksum))
                self.assertEqual(preset["image_capabilities"], capabilities)
                actual = preset["image_defaults"]
                self.assertEqual((actual["width"], actual["height"], actual["steps"],
                                  actual["cfg_scale"], actual["sampler"], actual["scheduler"]), defaults)
                self.assertEqual(preset.get("image_edit_strategy"), strategy)

        self.assertIn("sd-turbo-fp16", by_id)
        self.assertIn("realistic-vision-v51-fp16", by_id)
        self.assertEqual(by_id["sd-turbo-fp16"]["image_defaults"], {
            "width": 512, "height": 512, "steps": 1, "cfg_scale": 1,
            "sampler": "euler_a", "scheduler": ""})
        self.assertEqual(by_id["realistic-vision-v51-fp16"]["image_defaults"], {
            "width": 512, "height": 512, "steps": 20, "cfg_scale": 7,
            "sampler": "euler", "scheduler": ""})
        for preset in (item for item in presets if item["kind"] == 1):
            self.assertTrue(preset["image_capabilities"])
            self.assertEqual(set(preset["image_defaults"]),
                             {"width", "height", "steps", "cfg_scale", "sampler", "scheduler"})
        self.assertFalse(any("qwen-image" in preset["id"].lower() for preset in presets))
        self.assertEqual(by_id["ilustmix-v11-1-q8-0"]["upstream_url"],
                         "https://civitai.com/models/1110783?modelVersionId=2807896")
        self.assertIn("FLUX.2-dev non-commercial license", by_id["flux2-klein-4b-q8-0"]["license"])
        generated = (ROOT / "include/model_presets.hpp").read_text()
        for field in ("image_capabilities", "image_edit_strategy", "image_defaults"):
            self.assertIn(field, generated)


if __name__ == "__main__":
    unittest.main()
