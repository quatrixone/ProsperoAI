// ProsperoAI native OpenGL frontend.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_log.hpp"
#include "storage.hpp"
#include "update.hpp"
#include "media_output.hpp"
#include "native_ui.hpp"
#include "dev_script.hpp"
#include "gpt_input.hpp"
#include "gpt_ime.hpp"
#include "audio/cues.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#ifdef PROSPERO_UI_VULKAN
#include "ui/display_vulkan.hpp"
#else
#include "platform/ps5/display_egl.hpp"
#endif
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/system.hpp"
#ifdef PS5_LLAMA_VULKAN
#include "gpt_runtime.hpp"
#include "model_manager.hpp"
#include "inference_config.hpp"
#include "runtime_settings.hpp"
#include "http_server.hpp"
extern "C" int sceKernelUsleep(std::uint32_t microseconds);
#endif
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <utility>
#include <vector>

void prospero_setup_sdl_memory();
extern "C" void ps5SetInterfaceThread() noexcept;

namespace
{
// A scripted run's pictures: half the canvas is enough to compare with the PC's.
constexpr int kPictureWidth = 960, kPictureHeight = 540;

// The bound framebuffer, bottom row first, as a 24-bit BMP.
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; y < height && ok; ++y)
    {
        const unsigned char *source = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3] = source[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = source[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = source[x * 4];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

void ime_result(const char *value, void *context)
{
    static_cast<prospero::NativeUI *>(context)->keyboard_result(value);
}
void input_event(const gpt_input_event_t &event, hui::InputFrame &input, prospero::NativeUI &ui,
                 hui::ui::Feedback &feedback)
{
    if (!event.pressed)
        return;
    using hui::Action;
    using hui::Direction;
    constexpr Action actions[] = {Action::confirm,   Action::back, Action::west,
                                  Action::north,     Action::menu, Action::page_prev,
                                  Action::page_next, Action::up,   Action::down,
                                  Action::left,      Action::right};
    if (event.key <= GPT_INPUT_RIGHT)
    {
        input.pressed |= hui::action_bit(actions[event.key]);
        constexpr Direction directions[] = {Direction::up, Direction::down, Direction::left,
                                            Direction::right};
        if (event.key >= GPT_INPUT_UP)
        {
            input.nav = directions[event.key - GPT_INPUT_UP];
            // A held direction steps again; the keyboard stops it at its edge.
            input.nav_repeat = event.repeat;
        }
    }
    else if (event.key == GPT_INPUT_L2 || event.key == GPT_INPUT_R2)
        input.pressed |=
            hui::action_bit(event.key == GPT_INPUT_L2 ? Action::jump_prev : Action::jump_next);
    else if (event.key == GPT_INPUT_L3)
        input.pressed |= hui::action_bit(Action::l3);
    else if (event.key == GPT_INPUT_SCROLL_UP)
        ui.scroll(-120);
    else if (event.key == GPT_INPUT_SCROLL_DOWN)
        ui.scroll(120);
    else if (event.key == GPT_INPUT_TEXT)
        ui.type(event.text, feedback);
    else if (event.key == GPT_INPUT_BACKSPACE)
        ui.backspace(feedback);
    else if (event.key == GPT_INPUT_ENTER)
        ui.submit(feedback);
}

#ifdef PS5_LLAMA_VULKAN
void configure_vulkan_tuning_profile()
{
    const auto clear_profile_knobs = []
    {
        unsetenv("GGML_VK_PS5_DMMV_WG");
        unsetenv("GGML_VK_PS5_DMMV_SUBGROUP");
        unsetenv("GGML_VK_PS5_RM_STDQ");
        unsetenv("GGML_VK_PS5_RM_KQ");
        unsetenv("GGML_VK_PS5_SUBMIT_MFLOP");
    };
    clear_profile_knobs();

    char profile_text[32]{};
    const char *profile = "ps5";
    const std::string profile_path = prospero::storage().data_root + "/vulkan_tuning_profile.txt";
    std::FILE *profile_file = std::fopen(profile_path.c_str(), "r");
    if (!profile_file)
    {
        // Keep a visible, editable persistent default beside the user's models/settings.
        if (std::FILE *created = std::fopen(profile_path.c_str(), "w"))
        {
            std::fputs("ps5\n", created);
            std::fclose(created);
        }
        profile_file = std::fopen(profile_path.c_str(), "r");
    }
    if (std::FILE *file = profile_file)
    {
        const std::size_t size = std::fread(profile_text, 1, sizeof(profile_text) - 1, file);
        const bool failed = std::ferror(file) != 0 || size == sizeof(profile_text) - 1;
        std::fclose(file);
        while (profile_text[0] == ' ' || profile_text[0] == '\t' || profile_text[0] == '\r' ||
               profile_text[0] == '\n')
            std::memmove(profile_text, profile_text + 1, std::strlen(profile_text));
        std::size_t length = std::strlen(profile_text);
        while (length && (profile_text[length - 1] == ' ' || profile_text[length - 1] == '\t' ||
                          profile_text[length - 1] == '\r' || profile_text[length - 1] == '\n'))
            profile_text[--length] = '\0';
        if (!failed &&
            (std::strcmp(profile_text, "ps5") == 0 || std::strcmp(profile_text, "ps5pro") == 0))
            profile = profile_text;
        else if (size != 0)
            prospero::debug::line("app", "invalid Vulkan tuning profile; using ps5");
    }

    setenv("PROSPERO_VULKAN_PROFILE", profile, 1);
    if (std::strcmp(profile, "ps5pro") == 0)
    {
        setenv("GGML_VK_PS5_DMMV_WG", "1", 1);
        setenv("GGML_VK_PS5_DMMV_SUBGROUP", "32", 1);
    }

    // Optional PR #29-style overrides are applied after the selected profile.
    if (std::FILE *file = std::fopen("/app0/vulkan_env.txt", "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof(line), file))
        {
            char *start = line;
            while (*start == ' ' || *start == '\t')
                ++start;
            char *end = start + std::strlen(start);
            while (end > start &&
                   (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';
            char *equals = std::strchr(start, '=');
            if (*start == '#' || equals == nullptr || equals == start || equals[1] == '\0')
                continue;
            *equals = '\0';
            const auto valid_name_char = [](char c, bool first)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                       (!first && c >= '0' && c <= '9');
            };
            bool valid_name = true;
            for (char *p = start; *p; ++p)
                if (!valid_name_char(*p, p == start))
                    valid_name = false;
            if (!valid_name)
                continue;
            setenv(start, equals + 1, 1);
            prospero::debug::line("app", "vulkan_env %s=%s", start, equals + 1);
        }
        std::fclose(file);
    }
    prospero::debug::line("app", "Vulkan tuning profile: %s", profile);
}
#endif
} // namespace

int main()
{
#ifdef PROSPERO_UI_VULKAN
    // Filesystem access first, while this is the only thread: every path the app uses
    // is settled here (vulkan/storage.cpp).
    const prospero::Storage &storage = prospero::prepare_storage();
#ifdef PS5_LLAMA_VULKAN
    gpt_runtime_set_idle_unload_timeout(prospero_settings::get().idle_unload_minutes * 60);
    // The UI renderer also creates a Vulkan instance. llama.cpp snapshots its
    // profiling environment while the first Vulkan instance is initialized,
    // so configure it before constructing the display rather than on the first
    // inference request.
    if (access("/app0/vulkan_profile.txt", F_OK) == 0)
    {
        setenv("GGML_VK_PERF_LOGGER", "1", 1);
        setenv("GGML_VK_PERF_LOGGER_FREQUENCY", "1", 1);
    }
#endif
#endif
    // The debug log's switch is read here, before anything else can go wrong.
    prospero::debug::start("/download0/ProsperoAI/logs");
    constexpr const char *run_marker = "/download0/ProsperoAI/logs/app-running";
    if (access(run_marker, F_OK) == 0)
        prospero::debug::event("previous-run-unclean", "", "unclean-exit");
    if (std::FILE *marker = std::fopen(run_marker, "wb"))
        std::fclose(marker);
    prospero::debug::event("app-start", "", "");
#ifdef PS5_LLAMA_VULKAN
    configure_vulkan_tuning_profile();
#endif
#ifdef PROSPERO_UI_VULKAN
    prospero::debug::line("app", "filesystem access=%d route=%s app=%s data=%s models=%s moved=%u",
                          storage.access, storage.route, storage.app_dir.c_str(),
                          storage.data_root.c_str(), storage.model_root.c_str(),
                          storage.moved_models);
#endif
    prospero_setup_sdl_memory();
    SDL_Init(0); // Existing media decoder, clock and IME helpers; no SDL video.
    hui::ps5::Display display;
    if (!display.open(1920, 1080))
        hui::sys::quit();
    hui::gfx::Renderer renderer;
    if (!renderer.init())
        hui::sys::quit();
    prospero::FontSet fonts;
    if (!fonts.open(renderer, "/app0/assets/fonts"))
        hui::sys::quit();
    hui::audio::Mixer mixer;
    prospero_media_attach(&mixer); // generated sounds share the interface's audio port
    hui::audio::SoundBank sounds;
    const auto loaded = sounds.load("/app0/assets/audio/sfx");
    hui::sys::log("[prosperoai] interface sounds: %d loaded, %d rejected", loaded.files,
                  loaded.rejected);
    hui::ps5::AudioOut audio;
    if (!audio.start(mixer))
        hui::sys::log("[prosperoai] interface audio unavailable");
    gpt_input_init();
    gpt_ime_init();
    prospero::App app;
    prospero::NativeUI ui(app, fonts, renderer);
    ui.request_keyboard = [&](const std::string &value)
    { gpt_ime_request(value.c_str(), ime_result, &ui); };
    prospero::UiFrame frame;
    frame.glass_texture = renderer.glass_texture();
    hui::ui::Feedback feedback;
    // A test run written in the install folder's dev/request.txt; nothing without it.
    prospero::DevScript script;
    if (script.load("/app0/dev/request.txt", "/download0/ProsperoAI/dev"))
        hui::sys::log("[prosperoai] scripted run");
    hui::gfx::Canvas picture;
    auto previous = hui::sys::monotonic_us();
#ifdef PS5_LLAMA_VULKAN
    const unsigned generation_ui_sleep_ms =
        prospero::read_inference_setting("/app0", "ui_sleep_ms.txt", 50, 0, 1000);
#endif
    bool first_frame = true;
    while (!ui.quit_requested() && !script.quit())
    {
        // Animation time runs from frame start to frame start, and is clamped.
        const auto now = hui::sys::monotonic_us();
#ifdef PS5_LLAMA_VULKAN
        if (!first_frame)
        {
            std::lock_guard<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex());
            gpt_runtime_check_configured_idle_unload();
        }
#endif
        const float elapsed = static_cast<float>(now - previous) / 1000000.0f;
        const float dt = std::clamp(elapsed, 0.001f, 0.05f);
        previous = now;
        feedback.clear();
        hui::InputFrame input;
        gpt_input_poll();
        const bool intercepted = gpt_ime_active();
        gpt_input_event_t event;
        while (gpt_input_next(&event))
            if (!intercepted)
                input_event(event, input, ui, feedback);
        // What is held down, for controls that act while a button is held (backspace).
        if (!intercepted)
        {
            constexpr std::pair<gpt_input_key_t, hui::Action> kHeld[] = {
                {GPT_INPUT_CROSS, hui::Action::confirm}, {GPT_INPUT_CIRCLE, hui::Action::back},
                {GPT_INPUT_SQUARE, hui::Action::west},   {GPT_INPUT_TRIANGLE, hui::Action::north},
                {GPT_INPUT_L1, hui::Action::page_prev},  {GPT_INPUT_R1, hui::Action::page_next},
                {GPT_INPUT_L2, hui::Action::jump_prev},  {GPT_INPUT_R2, hui::Action::jump_next}};
            for (const auto &[key, action] : kHeld)
                if (gpt_input_pressed(key))
                    input.held |= hui::action_bit(action);
        }
        gpt_ime_poll();
        script.update(elapsed, app, ui, input, feedback);
        ui.update(input, dt, feedback);
        const float gain = static_cast<float>(app.state().preferences.volume) / 100;
        mixer.set_bus_gain(hui::audio::Bus::ui, gain);
        mixer.set_bus_gain(hui::audio::Bus::sfx, gain);
        for (const auto &cue : feedback.cues)
            sounds.play(mixer, hui::audio::SoundSet::glass, cue);
        frame.reset();
        ui.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        renderer.present(0, display.width(), display.height());
        if (!script.capture().empty())
        {
            // The same frame drawn once more into a small off-screen target: reading
            // the display surface back is slow.
            if (picture.texture() == 0)
                picture.create(kPictureWidth, kPictureHeight, 1);
            picture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(picture.framebuffer(), kPictureWidth, kPictureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, picture.framebuffer());
            script.capture_done(save_picture(script.capture(), kPictureWidth, kPictureHeight));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            previous = hui::sys::monotonic_us(); // saving is slow; the frame was not
        }
        if (!display.swap())
            break;
#ifdef PS5_LLAMA_VULKAN
        if (generation_ui_sleep_ms && gpt_runtime_generations_running() != 0)
            sceKernelUsleep(generation_ui_sleep_ms * 1000);
#endif
        if (first_frame)
        {
            // The console's splash stays up until this first picture; the model
            // catalogue is read behind the opening that follows it.
            hui::sys::hide_splash_screen();
            app.initialize();
#ifdef PS5_LLAMA_VULKAN
            // Best-effort: the on-console UI runs regardless of whether the
            // network/API server comes up.
            prospero_http_server_start(11434);
#endif
            // Once per launch: is a newer version listed (include/update.hpp). A scripted
            // run asks only when its request says "updates on".
            if (!script.active() || script.updates())
                prospero::update::check();
            first_frame = false;
        }
    }
    gpt_ime_shutdown();
    app.shutdown();
    prospero::debug::event("app-stop", "", "");
    std::remove(run_marker);
    prospero_media_attach(nullptr);
    gpt_input_shutdown();
    audio.stop();
    ui.release();
    renderer.release();
    display.close();
    SDL_Quit();
    hui::sys::quit();
}
