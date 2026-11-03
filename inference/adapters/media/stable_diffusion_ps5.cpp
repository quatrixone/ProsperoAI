#include "runtime_residency.hpp"
#include "runtime_settings.hpp"
#include "../../../include/gpt_runtime.hpp"
#include "ggml-vulkan.h"
static_assert(GGML_MAX_NAME == 160, "SD and llama must share the tensor ABI");
#include "stable-diffusion.h"
#include "../../../include/image_codec.hpp"
#include "../../../include/generated_output_path.hpp"
#include "../../../include/model_paths.hpp"
#include "../../../include/image_checkpoint_layout.hpp"
#include "../../../third_party/miniz/miniz_tdef.h"
#include "../../../third_party/miniz/miniz.h"

#include "ggml-backend.h"

#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

extern "C"
{
    int sceKernelDebugOutText(int, const char *);
    std::uint64_t sceKernelGetProcessTime(void);
}

namespace
{

sd_ctx_t *context;
std::string context_identity;
// 512x512 decodes whole on this heap (SD-Turbo and SD 1.5 inpainting are verified);
// anything larger is split into 512-pixel tiles with the engine's default overlap.
constexpr int kVaeTileSize = 512;
constexpr int kUntiledDecodePixels = 512 * 512;
void (*progress_callback)(const char *);
bool backend_registered;

bool model_index_has(const char *root, const char *pipeline)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/model_index.json", root);
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    char contents[8192]{};
    const std::size_t count = std::fread(contents, 1, sizeof(contents) - 1, file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    return !failed && count > 0 && std::strstr(contents, pipeline) != nullptr;
}

bool is_sd3_model(const char *root)
{
    return root && model_index_has(root, "StableDiffusion3Pipeline");
}

bool is_flux2_klein_model(const char *root)
{
    return root && model_index_has(root, "Flux2KleinPipeline");
}

bool is_sdxl_checkpoint(const char *root)
{
    if (!root || !*root)
        return false;
    char path[512];
    if (std::snprintf(path, sizeof(path), "%s/sd_xl_base_1.0.safetensors", root) >=
        static_cast<int>(sizeof(path)))
        return false;
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}

bool is_realistic_vision_v51(const char *root)
{
    if (!root || !*root)
        return false;
    char path[512];
    if (std::snprintf(path, sizeof(path), "%s/Realistic_Vision_V5.1_fp16-no-ema.safetensors", root) >=
        static_cast<int>(sizeof(path)))
        return false;
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}

void log_line(const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    sceKernelDebugOutText(0, line);
}

void sd_log(sd_log_level_t level, const char *message, void *)
{
    if (level >= SD_LOG_WARN)
        log_line("[prosperoai:sd] %s", message ? message : "");
}

void sd_progress(int step, int steps, float, void *)
{
    if (!progress_callback)
        return;
    char status[96];
    std::snprintf(status, sizeof(status), "Generating image on the GPU  ·  %d/%d", step, steps);
    progress_callback(status);
}

bool write_ppm(const char *path, const sd_image_t &image)
{
    if (!image.data || image.width == 0 || image.height == 0 || image.channel < 3)
        return false;
    std::FILE *file = std::fopen(path, "wbx");
    if (!file)
        return false;
    std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    bool ok = true;
    if (image.channel == 3)
    {
        ok = std::fwrite(image.data, 3, pixels, file) == pixels;
    }
    else
    {
        for (std::size_t pixel = 0; pixel < pixels && ok; ++pixel)
            ok = std::fwrite(image.data + pixel * image.channel, 1, 3, file) == 3;
    }
    return std::fclose(file) == 0 && ok;
}

bool write_tga(const char *path, const sd_image_t &image)
{
    if (!image.data || image.width == 0 || image.height == 0 || image.width > 65535 ||
        image.height > 65535 || image.channel < 3)
        return false;
    std::FILE *file = std::fopen(path, "wbx");
    if (!file)
        return false;
    unsigned char header[18]{};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(image.width);
    header[13] = static_cast<unsigned char>(image.width >> 8);
    header[14] = static_cast<unsigned char>(image.height);
    header[15] = static_cast<unsigned char>(image.height >> 8);
    header[16] = 32;
    header[17] = 0x28;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> row(static_cast<std::size_t>(image.width) * 4);
    for (unsigned y = 0; y < image.height && ok; ++y)
    {
        for (unsigned x = 0; x < image.width; ++x)
        {
            const unsigned char *source =
                image.data + (static_cast<std::size_t>(y) * image.width + x) * image.channel;
            row[x * 4 + 0] = source[2];
            row[x * 4 + 1] = source[1];
            row[x * 4 + 2] = source[0];
            row[x * 4 + 3] = 255;
        }
        ok = std::fwrite(row.data(), 4, image.width, file) == image.width;
    }
    return std::fclose(file) == 0 && ok;
}

bool write_png(const char *path, const sd_image_t &image)
{
    if (!image.data || image.width == 0 || image.height == 0 ||
        (image.channel != 3 && image.channel != 4))
        return false;
    std::size_t png_size = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(
        image.data, static_cast<int>(image.width), static_cast<int>(image.height),
        static_cast<int>(image.channel), &png_size);
    if (!png || png_size == 0)
    {
        mz_free(png);
        return false;
    }
    std::FILE *file = std::fopen(path, "wbx");
    const bool complete = file && std::fwrite(png, 1, png_size, file) == png_size;
    const bool closed = !file || std::fclose(file) == 0;
    mz_free(png);
    return complete && closed;
}

struct DecodedInput
{
    prospero_image::DecodedImage image{};
    ~DecodedInput() { prospero_image::free_image(&image); }
    bool load(const char *path)
    {
        return path && prospero_image::decode_file_rgba(path, &image);
    }
    sd_image_t sd_image() const
    {
        return {image.width, image.height, image.channels, image.pixels};
    }
    // Only the init image is reduced to the model's channel count by the engine. The mask
    // and the ControlNet guide are used as decoded, and a decoded image is RGBA: the mask
    // then fails to broadcast against the init tensor and the guide trips
    // GGML_ASSERT(a->ne[2] == b->ne[2]), which aborts the title. Convert them here. A mask
    // is monochrome (1), a guide is colour (3).
    sd_image_t sd_image_with_channels(std::vector<std::uint8_t> &storage, unsigned wanted) const
    {
        if (!image.pixels || !image.channels || image.channels < wanted)
            return {image.width, image.height, wanted, nullptr};
        const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
        storage.resize(pixels * wanted);
        for (std::size_t i = 0; i < pixels; ++i)
            for (unsigned c = 0; c < wanted; ++c)
                storage[i * wanted + c] = image.pixels[i * image.channels + c];
        return {image.width, image.height, wanted, storage.data()};
    }
};

bool valid_image_options(const gpt_runtime_image_options_t *options, bool flux2_klein)
{
    if (!options)
        return true;
    if (!options->workflow || !options->image_edit_strategy || options->width < 256 ||
        options->height < 256 || options->width > 1536 || options->height > 1536 ||
        options->width % 64 || options->height % 64 ||
        static_cast<std::uint64_t>(options->width) * options->height > 1048576 ||
        options->steps < 1 || options->steps > 50 || options->seed < -1 ||
        options->seed > 4294967295LL || !std::isfinite(options->cfg_scale) ||
        options->cfg_scale < 0 || options->cfg_scale > 20)
        return false;
    const bool text_to_image = std::strcmp(options->workflow, "text_to_image") == 0;
    const bool image_to_image = std::strcmp(options->workflow, "image_to_image") == 0;
    const bool inpainting = std::strcmp(options->workflow, "inpainting") == 0;
    const bool controlnet = std::strcmp(options->workflow, "controlnet") == 0;
    if (!(text_to_image || image_to_image || inpainting || controlnet))
        return false;
    if (image_to_image && !options->image_path)
        return false;
    if (inpainting && (!options->image_path || !options->mask_path))
        return false;
    if (controlnet && !options->control_image_path)
        return false;
    if ((text_to_image && (options->image_path || options->mask_path || options->control_image_path)) ||
        (image_to_image && (options->mask_path || options->control_image_path)) ||
        (inpainting && options->control_image_path) ||
        (controlnet && (options->image_path || options->mask_path)))
        return false;
    if (options->strength != -1 &&
        (!std::isfinite(options->strength) || options->strength < 0 || options->strength > 1 ||
         std::strcmp(options->image_edit_strategy, "init_image_strength") != 0))
        return false;
    if (image_to_image && std::strcmp(options->image_edit_strategy, "init_image_strength") != 0 &&
        std::strcmp(options->image_edit_strategy, "reference_image") != 0)
        return false;
    if (std::strcmp(options->image_edit_strategy, "reference_image") == 0 && !flux2_klein)
        return false;
    if (flux2_klein && std::strcmp(options->image_edit_strategy, "reference_image") != 0)
        return false;
    if (options->control_strength != -1 &&
        (!std::isfinite(options->control_strength) || options->control_strength < 0 ||
         options->control_strength > 2))
        return false;
    if (controlnet && std::strcmp(options->image_edit_strategy, "init_image_strength") != 0)
        return false;
    if (options->canny && !controlnet)
        return false;
    if (options->sampler && *options->sampler &&
        str_to_sample_method(options->sampler) == SAMPLE_METHOD_COUNT)
        return false;
    if (options->scheduler && *options->scheduler &&
        str_to_scheduler(options->scheduler) == SCHEDULER_COUNT)
        return false;
    return true;
}

void release_context()
{
    prospero_runtime::set_model_resident(false);
    if (context)
    {
        free_sd_ctx(context);
        context = nullptr;
        context_identity.clear();
    }
    log_line("[prosperoai] sd_context_released=1\n");
}

bool ensure_context(const char *root)
{
    const std::string desired_identity = "model:" + std::string(root);
    if (context && context_identity == desired_identity)
        return true;
    release_context();
    if (!backend_registered)
    {
        if (ggml_backend_vk_get_device_count() == 0)
            return false;
        ggml_backend_register(ggml_backend_vk_reg());
        backend_registered = true;
    }
    sd_set_log_callback(sd_log, nullptr);
    sd_set_progress_callback(sd_progress, nullptr);

    sd_ctx_params_t parameters;
    sd_ctx_params_init(&parameters);
    const auto layout = prospero_image_layout::checkpoint_layout(root);
    const bool flux2_klein = layout.flux2 || is_flux2_klein_model(root);
    char diffusion_model_path[512];
    char llm_path[512];
    char vae_path[512];
    char checkpoint_path[512];
    const bool sdxl_checkpoint = !layout.found && is_sdxl_checkpoint(root);
    const bool realistic_vision = !layout.found && is_realistic_vision_v51(root);
    if (sdxl_checkpoint &&
        std::snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/sd_xl_base_1.0.safetensors", root) >=
            static_cast<int>(sizeof(checkpoint_path)))
        return false;
    if (realistic_vision &&
        (std::snprintf(checkpoint_path, sizeof(checkpoint_path),
                       "%s/Realistic_Vision_V5.1_fp16-no-ema.safetensors", root) >=
             static_cast<int>(sizeof(checkpoint_path)) ||
         std::snprintf(vae_path, sizeof(vae_path), "%s/vae.safetensors", root) >=
             static_cast<int>(sizeof(vae_path))))
        return false;
    if (flux2_klein && !layout.found &&
        (std::snprintf(diffusion_model_path, sizeof(diffusion_model_path), "%s/diffusion_model.gguf", root) >=
             static_cast<int>(sizeof(diffusion_model_path)) ||
         std::snprintf(llm_path, sizeof(llm_path), "%s/qwen3-4b-q2-k.gguf", root) >=
             static_cast<int>(sizeof(llm_path)) ||
         std::snprintf(vae_path, sizeof(vae_path), "%s/vae.safetensors", root) >=
             static_cast<int>(sizeof(vae_path))))
        return false;
    parameters.model_path = flux2_klein ? nullptr
                            : layout.found                           ? layout.model
                            : (sdxl_checkpoint || realistic_vision)  ? checkpoint_path
                                                                     : root;
    if (flux2_klein)
    {
        parameters.diffusion_model_path = layout.found ? layout.model : diffusion_model_path;
        parameters.llm_path = layout.found ? layout.llm : llm_path;
        parameters.vae_path = layout.found ? layout.vae : vae_path;
        parameters.vae_format = SD_VAE_FORMAT_FLUX2;
    }
    else if (layout.found)
    {
        if (layout.vae[0])
            parameters.vae_path = layout.vae;
        if (layout.control_net[0])
            parameters.control_net_path = layout.control_net;
    }
    if (realistic_vision)
        parameters.vae_path = vae_path;
    parameters.n_threads = 1;
    // Keep each checkpoint's own weight type (the library default). Forcing F16 dates
    // from when the only image model was an fp16 diffusers folder, where it did nothing;
    // for the Q8_0 GGUF presets it doubles resident weights and exhausts the heap.
    parameters.wtype = SD_TYPE_COUNT;
    parameters.rng_type = CUDA_RNG;
    parameters.prediction = (is_sd3_model(root) || flux2_klein)
                                ? FLOW_PRED
                                : EPS_PRED;
    parameters.enable_mmap = false;
    parameters.eager_load = false;
    parameters.diffusion_conv_direct = false;
    parameters.vae_conv_direct = false;
    // Flash attention stays off: it clears FLUX.2 Klein's 2.49 GiB attention workspace
    // reservation, but sampling then kills the title inside RADV with no error. A failed
    // allocation that the caller can report beats losing the app.
    parameters.diffusion_flash_attn = false;
    // Everything runs on Vulkan, as the PS5SD reference build does. PS5's libc flexible
    // heap is a few hundred MiB, so any stage left on the CPU backend either fails to
    // allocate (SDXL's VAE decode, FLUX.2 Klein's text encoder) or crawls: with CPU as
    // the default, the unassigned ControlNet ran a 512x512 guide in 705 s.
    // Large VAE decodes are tiled on the device, see kVaeTileSize.
    parameters.backend = "Vulkan0";
    // Weights stay on the device. Streaming FLUX.2 Klein's 2.5 GB text encoder from the
    // SSD ("te=disk") does free the VRAM its double blocks need, but the run then returns
    // noise instead of an image, so a reported success would be worse than the failure.
    parameters.params_backend = "Vulkan0";
    context = new_sd_ctx(&parameters);
    prospero_runtime::set_model_resident(context != nullptr);
    if (!context || !sd_ctx_supports_image_generation(context))
    {
        release_context();
        return false;
    }
    context_identity = desired_identity;
    return true;
}

} // namespace

extern "C" void ps5_sd_shutdown()
{
    release_context();
}

extern "C" int ps5_sd_generate(const char *root, const char *prompt, char *response,
                               std::size_t response_capacity, std::uint64_t *elapsed_microseconds,
                               const gpt_runtime_image_options_t *image_options,
                               void (*progress)(const char *))
{
    constexpr char oracle_output[] = "/download0/ProsperoAI/work/image/prosperoai-image.ppm";
    char display_output[128];
    char png_output[128];
    if (!root || !*root || !prompt || !*prompt || !response || response_capacity == 0)
        return 1;
    if (!prospero_generated_output_path(display_output, sizeof(display_output), "image",
                                        "prosperoai-image", "tga") ||
        !prospero_generated_output_path(png_output, sizeof(png_output), "image",
                                        "prosperoai-image", "png"))
        return 1;
    const bool sd3 = is_sd3_model(root);
    const auto layout = prospero_image_layout::checkpoint_layout(root);
    const bool sdxl = layout.sdxl || model_index_has(root, "StableDiffusionXLPipeline") ||
                      is_sdxl_checkpoint(root);
    const bool flux2_klein = layout.flux2 || is_flux2_klein_model(root);
    if (!valid_image_options(image_options, flux2_klein))
    {
        std::snprintf(response, response_capacity, "Unsupported or invalid image workflow.");
        return 1;
    }
    DecodedInput init_image, mask_image, control_image;
    std::vector<std::uint8_t> mask_storage, control_storage;
    if (image_options &&
        ((image_options->image_path && !init_image.load(image_options->image_path)) ||
         (image_options->mask_path && !mask_image.load(image_options->mask_path)) ||
         (image_options->control_image_path && !control_image.load(image_options->control_image_path))))
    {
        std::snprintf(response, response_capacity, "Could not decode a workflow input image.");
        return 1;
    }
    if (image_options && image_options->mask_path &&
        (init_image.image.width != mask_image.image.width ||
         init_image.image.height != mask_image.image.height))
    {
        std::snprintf(response, response_capacity, "Input and mask dimensions must match.");
        return 1;
    }
    const bool reference_edit = image_options &&
        std::strcmp(image_options->workflow, "image_to_image") == 0 &&
        std::strcmp(image_options->image_edit_strategy, "reference_image") == 0;
    if (image_options && image_options->sampler && *image_options->sampler &&
        str_to_sample_method(image_options->sampler) == SAMPLE_METHOD_COUNT)
    {
        std::snprintf(response, response_capacity, "Unsupported sampler.");
        return 1;
    }
    if (image_options && image_options->scheduler && *image_options->scheduler &&
        str_to_scheduler(image_options->scheduler) == SCHEDULER_COUNT)
    {
        std::snprintf(response, response_capacity, "Unsupported scheduler.");
        return 1;
    }
    progress_callback = progress;
    if (progress_callback)
        progress_callback(flux2_klein ? "Loading FLUX.2 Klein 4B"
                          : sdxl       ? "Loading Stable Diffusion XL"
                          : sd3        ? "Loading Stable Diffusion 3"
                                            : "Loading SD-Turbo");
    std::remove(oracle_output);
    std::remove(display_output);
    const std::uint64_t start = sceKernelGetProcessTime();
    const auto runtime_settings = prospero_settings::get();
    // The image workflow carries its own flag; the Settings toggle is its default and
    // still drives the native UI, which has no per-request options.
    const bool upscale_requested = image_options ? image_options->upscale
                                                 : runtime_settings.sd_upscale;
    char upscaler_path[512]{};
    if (upscale_requested)
    {
        if (std::snprintf(upscaler_path, sizeof(upscaler_path), "%s/upscalers/RealESRGAN_x4plus.pth",
                          prospero::model_root()) >= static_cast<int>(sizeof(upscaler_path)))
        {
            std::snprintf(response, response_capacity, "Upscaler path is too long.");
            return 2;
        }
        std::FILE *upscaler_file = std::fopen(upscaler_path, "rb");
        if (!upscaler_file)
        {
            std::snprintf(response, response_capacity,
                          "Real-ESRGAN model missing. Place RealESRGAN_x4plus.pth in models/upscalers/.");
            return 2;
        }
        std::fclose(upscaler_file);
    }
    if (!ensure_context(root))
    {
        std::snprintf(response, response_capacity, "Could not load the image model.");
        return 2;
    }

    sd_img_gen_params_t parameters;
    sd_img_gen_params_init(&parameters);
    parameters.prompt = prompt;
    parameters.negative_prompt = image_options && image_options->negative_prompt
                                     ? image_options->negative_prompt : "";
    parameters.ref_images = nullptr;
    parameters.ref_images_count = 0;
    const unsigned default_size = (sd3 || flux2_klein) ? 768 : 512;
    parameters.width = image_options ? static_cast<int>(image_options->width)
                        : runtime_settings.sd_width ? runtime_settings.sd_width : default_size;
    parameters.height = image_options ? static_cast<int>(image_options->height)
                         : runtime_settings.sd_height ? runtime_settings.sd_height : default_size;
    parameters.seed = image_options ? image_options->seed
                        : runtime_settings.sd_seed == 0 ? -1
                                                        : static_cast<std::int64_t>(runtime_settings.sd_seed);
    parameters.batch_count = 1;
    // An untiled decode needs several GiB of device workspace beside the resident
    // weights, which the heap does not have once an SDXL-sized model is loaded.
    // Tile anything larger than the size that is proven to decode whole.
    if (parameters.width * parameters.height > kUntiledDecodePixels)
    {
        parameters.vae_tiling_params.enabled = true;
        parameters.vae_tiling_params.tile_size_w = kVaeTileSize;
        parameters.vae_tiling_params.tile_size_h = kVaeTileSize;
    }
    parameters.sample_params.sample_steps = image_options ? static_cast<int>(image_options->steps)
                                            : flux2_klein ? 4
                                            : sdxl ? 10
                                            : sd3 ? 28
                                            : is_realistic_vision_v51(root) ? 20
                                                  : 1;
    parameters.sample_params.sample_method = image_options && image_options->sampler && *image_options->sampler
                                                 ? str_to_sample_method(image_options->sampler)
                                             : (sdxl || sd3 || flux2_klein || is_realistic_vision_v51(root))
                                                 ? EULER_SAMPLE_METHOD
                                                 : EULER_A_SAMPLE_METHOD;
    if (image_options && image_options->scheduler && *image_options->scheduler)
        parameters.sample_params.scheduler = str_to_scheduler(image_options->scheduler);
    else if (flux2_klein)
        parameters.sample_params.scheduler = FLUX2_SCHEDULER;
    if (flux2_klein)
        parameters.sample_params.guidance.distilled_guidance =
            image_options ? image_options->cfg_scale : 1.0f;
    parameters.sample_params.guidance.txt_cfg = image_options ? image_options->cfg_scale
                                               : is_realistic_vision_v51(root) ? 7.0f
                                               : sdxl ? 5.0f : sd3 ? 4.5f : 1.0f;
    sd_image_t reference_image{};
    if (image_options)
    {
        if (reference_edit)
        {
            reference_image = init_image.sd_image();
            parameters.ref_images = &reference_image;
            parameters.ref_images_count = 1;
        }
        else if (image_options->image_path)
            parameters.init_image = init_image.sd_image();
        if (image_options->mask_path)
            parameters.mask_image = mask_image.sd_image_with_channels(mask_storage, 1);
        if (image_options->control_image_path)
        {
            parameters.control_image = control_image.sd_image_with_channels(control_storage, 3);
            parameters.control_strength = image_options->control_strength == -1
                                              ? 0.9f : image_options->control_strength;
            if (image_options->canny && !preprocess_canny(parameters.control_image,
                                                           0.08f, 0.08f, 0.8f, 1.0f, false))
            {
                std::snprintf(response, response_capacity, "Canny preprocessing failed.");
                return 1;
            }
        }
        if (image_options->strength != -1)
            parameters.strength = image_options->strength;
        else if (image_options->image_path && !reference_edit)
            parameters.strength = 0.75f;
    }

    sd_image_t *images = nullptr;
    int image_count = 0;
    const bool generated = generate_image(context, &parameters, &images, &image_count);
    sd_image_t *upscaled_images = nullptr;
    int upscaled_count = 0;
    bool upscaled = false;
    if (generated && image_count == 1 && images && upscale_requested)
    {
        if (progress_callback)
            progress_callback("Upscaling image 4× with Real-ESRGAN");
        // Free the diffusion weights before loading ESRGAN to keep the Vulkan memory peak lower.
        release_context();
        upscaler_ctx_t *upscaler = new_upscaler_ctx(upscaler_path, false, 1, 128,
                                                     "Vulkan0", "Vulkan0");
        if (upscaler)
        {
            upscaled = upscale(upscaler, images[0], 4, &upscaled_images, &upscaled_count) &&
                       upscaled_count == 1 && upscaled_images && upscaled_images[0].data;
            free_upscaler_ctx(upscaler);
        }
        if (!upscaled)
            log_line("[prosperoai:sd] Real-ESRGAN failed; saving original image\n");
    }
    const sd_image_t *output_image = upscaled ? &upscaled_images[0]
                                               : (images && image_count == 1 ? &images[0] : nullptr);
    const bool written = generated && output_image &&
                         write_ppm(oracle_output, *output_image) &&
                         write_tga(display_output, *output_image) &&
                         write_png(png_output, *output_image);
    free_sd_images(upscaled_images, upscaled_count);
    free_sd_images(images, image_count);
    const std::uint64_t elapsed = sceKernelGetProcessTime() - start;
    if (elapsed_microseconds)
        *elapsed_microseconds = elapsed;
    progress_callback = nullptr;
    if (!written)
    {
        std::remove(display_output);
        std::remove(png_output);
        std::snprintf(response, response_capacity, "Image generation failed. Check the system log.");
        return 3;
    }
    std::snprintf(response, response_capacity,
                  upscaled ? "Image generated and upscaled 4× locally.\nPNG: %s\nPreview: %s"
                  : upscale_requested
                      ? "Image generated locally; ESRGAN failed, saved original size.\nPNG: %s\nPreview: %s"
                      : "Image generated locally.\nPNG: %s\nPreview: %s",
                  png_output, display_output);
    sceKernelDebugOutText(0, "[prosperoai] sd_complete=1\n");
    return 0;
}
