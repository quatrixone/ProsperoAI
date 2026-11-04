// Deterministic host fixtures for rendering the production frontend with Mesa.
// No model weights, console connection, or deployment is involved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "update.hpp"
#include "model_downloader_ps5.hpp"
#include "native_ui.hpp"
#include "dev_script.hpp"
#include "gfx/renderer.hpp"
#include "gfx/gl_program.hpp"
#ifdef PROSPERO_UI_VULKAN
#include "backend.hpp"
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

namespace
{
unsigned selected = 0;
const char *names[] = {"Mistral 7B", "Qwen3.5 9B", "SD-Turbo", "Stable Audio", "Kokoro"};
const char *ids[] = {"mistral-7b", "qwen35-9b", "sd-turbo", "stable-audio", "kokoro"};
const char *purposes[] = {"text-to-text", "text-to-text", "text-to-image", "text-to-audio",
                          "text-to-speech"};
struct Saved
{
    prospero_session::Record record;
    std::vector<prospero_session::Message> messages;
};
std::map<std::string, Saved> saved;
unsigned serial = 0;
std::string image_fixture;
bool fail_next_generation = false;
} // namespace
extern "C" std::FILE *__real_fopen(const char *, const char *);
extern "C" std::FILE *__wrap_fopen(const char *path, const char *mode)
{
    // The visual fixture supplies the console's generated image inside build/.
    if (std::strncmp(path, "/download0/", 11) == 0 && std::strstr(path, ".tga"))
        return __real_fopen(image_fixture.c_str(), mode);
    return __real_fopen(path, mode);
}
unsigned gpt_runtime_model_count()
{
    return std::getenv("PROSPERO_EMPTY") ? 0 : std::getenv("PROSPERO_STRESS") ? 500 : 5;
}
unsigned gpt_runtime_selected_model()
{
    return selected;
}
const char *gpt_runtime_model_id(unsigned index)
{
    if (index < 5)
        return ids[index];
    static char id[48];
    std::snprintf(id, sizeof(id), "local-model-%03u", index);
    return id;
}
const char *gpt_runtime_model_name(unsigned index)
{
    return index < 5 ? names[index] : gpt_runtime_model_id(index);
}
const char *gpt_runtime_model_purpose(unsigned index)
{
    return purposes[index % 5];
}
bool gpt_runtime_loaded_model(char *id, std::size_t ic, char *name, std::size_t nc) {
    if(id && ic) id[0]=0; if(name && nc) name[0]=0; return false;
}
bool gpt_runtime_select_model(unsigned index)
{
    selected = index;
    return true;
}
int gpt_runtime_prepare()
{
    // Long enough for the "Preparing" state to be seen in a capture.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    return 0;
}
bool gpt_runtime_context_full()
{
    return false;
}
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &, char *output, std::size_t capacity,
                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (fail_next_generation)
    {
        fail_next_generation = false;
        std::snprintf(output, capacity, "%s", "The model stopped before it could answer.");
        return 1;
    }
    if (selected == 2 || selected == 3)
    {
        std::snprintf(output, capacity, "%s",
                      selected == 2 ? "/download0/prosperoai-image-0.tga"
                                    : "/download0/prosperoai-audio.wav");
        stats->elapsed_microseconds = selected == 2 ? 6400000 : 11800000;
        return 0;
    }
    const bool everywhere = std::strstr(messages[count - 1].content, "\xE4\xBD\xA0") != nullptr;
    const char *reply =
        everywhere
            ? "\xE6\x98\x9F\xE6\x98\x9F\xE6\x98\xAF\xE9\x81\xA5\xE8\xBF\x9C\xE7\x9A\x84"
              "\xE5\xA4\xAA\xE9\x98\xB3\xE3\x80\x82 \xE6\x98\x9F\xE3\x81\xAF\xE9\x81\xA0"
              "\xE3\x81\x84\xE5\xA4\xAA\xE9\x99\xBD\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82\n"
              "\xEB\xB3\x84\xEC\x9D\x80 \xEB\xA8\xBC \xED\x83\x9C\xEC\x96\x91\xEC\x9E\x85"
              "\xEB\x8B\x88\xEB\x8B\xA4. \xD0\x97\xD0\xB2\xD1\x91\xD0\xB7\xD0\xB4\xD1\x8B "
              "\xE2\x80\x94 \xD0\xB4\xD0\xB0\xD0\xBB\xD1\x91\xD0\xBA\xD0\xB8\xD0\xB5 "
              "\xD1\x81\xD0\xBE\xD0\xBB\xD0\xBD\xD1\x86\xD0\xB0.\nLes \xC3\xA9toiles sont des "
              "soleils lointains. \xCE\xA4\xCE\xB1 \xCE\xAC\xCF\x83\xCF\x84\xCF\x81\xCE\xB1 "
              "\xCE\xB5\xCE\xAF\xCE\xBD\xCE\xB1\xCE\xB9 \xCE\xAE\xCE\xBB\xCE\xB9\xCE\xBF\xCE\xB9. "
              "\xD7\x9B\xD7\x95\xD7\x9B\xD7\x91\xD7\x99\xD7\x9D \xE0\xB8\x94\xE0\xB8\xB2\xE0"
              "\xB8\xA7"
            : "Start with the things that make us human.\n\nA garden under warm light. A table big "
              "enough for everyone.\nA window that frames Earth, so home never feels too far "
              "away.\n\nThe technology keeps you alive. The small rituals make you belong.";
    std::string stream;
    for (const char *at = reply; *at; ++at)
    {
        stream += *at;
        // Whole code points only, as the runtime streams them.
        if ((static_cast<unsigned char>(at[1]) & 0xc0) != 0x80)
            progress(stream.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::snprintf(output, capacity, "%s", reply);
    stats->prompt_tokens = 412;
    stats->generated_tokens = 64;
    stats->prefill_microseconds = 380000;
    stats->elapsed_microseconds = 4890000;
    return 0;
}
void gpt_runtime_refresh_models()
{
}

// Nothing is fetched on a PC; one scene pretends that a preset is being downloaded.
namespace prospero_model_download
{
static State preview_state = State::Idle;
bool preset_installed(std::size_t)
{
    return false;
}
bool download_preset(std::size_t)
{
    return false;
}
void poll()
{
}
State state()
{
    return preview_state;
}
int active_preset()
{
    return preview_state == State::Downloading ? 0 : -1;
}
void progress(std::uint64_t *completed, std::uint64_t *total)
{
    *completed = preview_state == State::Downloading ? 42 : 0;
    *total = preview_state == State::Downloading ? 100 : 0;
}
void status(char *output, std::size_t capacity)
{
    std::snprintf(output, capacity, "%s",
                  preview_state == State::Downloading ? "Downloading: 1827 / 4350 MiB (42%)" : "");
}
bool cancel()
{
    const bool running = preview_state == State::Downloading;
    preview_state = State::Idle;
    return running;
}
bool cancelling()
{
    return false;
}
std::size_t candidate_count()
{
    return 0;
}
bool candidate(std::size_t, Candidate *)
{
    return false;
}
bool search(const char *)
{
    return false;
}
bool browse(const char *)
{
    return false;
}
bool download(std::size_t)
{
    return false;
}
} // namespace prospero_model_download

// A newer version is offered when a scene says so; nothing is downloaded on a PC.
namespace prospero::update
{
static int preview_offer = 0; // 1: an offer waits to be taken
static Phase preview_phase = Phase::idle;
void check()
{
}
bool take_offer(Offer *offer)
{
    if (preview_offer != 1)
        return false;
    preview_offer = 2;
    offer->installable = true;
    std::snprintf(offer->version, sizeof(offer->version), "01.003.000");
    offer->size = 86728290;
    offer->notes = "- Saved conversations open again from the list.\n"
                   "- A running model download can be cancelled.\n"
                   "\nDownloads\n"
                   "- Downloads use a larger socket receive buffer; a distant server no longer "
                   "holds them to about 1 MB/s.\n"
                   "- Models, settings, conversations and logs live in /data/prosperoai.\n"
                   "Note: models from the earlier folder are moved at the first start.";
    return true;
}
bool begin()
{
    preview_phase = Phase::downloading;
    return true;
}
void poll(Progress *progress)
{
    *progress = Progress{};
    progress->phase = preview_phase;
    if (preview_phase == Phase::downloading)
    {
        progress->done = 36400000;
        progress->total = 86728290;
        std::snprintf(progress->time_left, sizeof(progress->time_left), "about 4 s left");
    }
    else if (preview_phase == Phase::failed)
        std::snprintf(progress->error, sizeof(progress->error),
                      "The download doesn't match the catalog's listing.");
}
void cancel()
{
    preview_phase = Phase::cancelled;
}
bool apply()
{
    return true;
}
void finish()
{
    preview_phase = Phase::idle;
}
} // namespace prospero::update

namespace prospero_session
{
bool create(Record *r, const char *id, const char *name, const char *purpose)
{
    std::snprintf(r->id, sizeof(r->id), "preview-%u", ++serial);
    std::snprintf(r->model_id, sizeof(r->model_id), "%s", id);
    std::snprintf(r->model_name, sizeof(r->model_name), "%s", name);
    std::snprintf(r->purpose, sizeof(r->purpose), "%s", purpose);
    return true;
}
bool save(Record *r, const Message *messages, unsigned count)
{
    r->message_count = count;
    saved[r->id] = {*r, {messages, messages + count}};
    return true;
}
bool load(const char *id, Record *record, Message *messages, unsigned capacity)
{
    const auto it = saved.find(id);
    if (it == saved.end() || it->second.messages.size() > capacity)
        return false;
    *record = it->second.record;
    std::copy(it->second.messages.begin(), it->second.messages.end(), messages);
    return true;
}
unsigned scan(Record *records, unsigned capacity)
{
    unsigned count = 0;
    for (const auto &[id, entry] : saved)
        if (count < capacity)
            records[count++] = entry.record;
    return count;
}
bool erase(const char *id)
{
    return saved.erase(id) != 0;
}
bool archive_text(const char *, unsigned, const char *, const OutputMetadata &, char *, std::size_t)
{
    return true;
}
bool archive_media(const char *id, unsigned index, const char *kind, const char *, char *path,
                   std::size_t capacity, const OutputMetadata *)
{
    std::snprintf(path, capacity, "/download0/ProsperoAI/sessions/%s/media/%s-%u.%s", id, kind,
                  index, std::strcmp(kind, "image") == 0 ? "tga" : "wav");
    return true;
}
} // namespace prospero_session

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    const std::string fonts_directory = argv[1], output = argv[2];
    const bool record = std::getenv("PROSPERO_REEL") != nullptr;
    const bool empty = std::getenv("PROSPERO_EMPTY") != nullptr;
    const int width = record ? 1280 : 1920, height = record ? 720 : 1080;
    image_fixture = output + "/image-fixture.tga";
    {
        std::ofstream file(image_fixture, std::ios::binary);
        unsigned char header[18]{};
        header[2] = 2;
        header[13] = 1;
        header[14] = 128;
        header[16] = 24;
        header[17] = 0x20;
        file.write(reinterpret_cast<char *>(header), sizeof(header));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 256; ++x)
            {
                unsigned char pixel[] = {static_cast<unsigned char>(80 + x / 2),
                                         static_cast<unsigned char>(40 + y),
                                         static_cast<unsigned char>(190 - x / 3)};
                file.write(reinterpret_cast<char *>(pixel), 3);
            }
    }
#ifdef PROSPERO_UI_VULKAN
    assert(prospero::vkui::open(false, width, height));
#else
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0, minor = 0;
    assert(eglInitialize(display, &major, &minor) && eglBindAPI(EGL_OPENGL_API));
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    assert(context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
#endif
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    assert(renderer.init());
    prospero::FontSet fonts;
    assert(fonts.open(renderer, fonts_directory));
    GLuint target = 0, storage = 0;
    glGenFramebuffers(1, &target);
    glGenRenderbuffers(1, &storage);
    glBindRenderbuffer(GL_RENDERBUFFER, storage);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, storage);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    std::remove(PROSPERO_SETTINGS_PATH);
    prospero::App app;
    prospero::NativeUI ui(app, fonts, renderer);
    assert(app.initialize());
    hui::ui::Feedback feedback;
    prospero::UiFrame frame;
    frame.glass_texture = renderer.glass_texture();
    std::map<hui::audio::Cue, unsigned> cues;
    auto render = [&]
    {
        frame.reset();
        ui.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        renderer.present(target, width, height);
        assert(glGetError() == GL_NO_ERROR);
    };
    unsigned frame_number = 0;
    std::vector<unsigned char> recording_pixels(static_cast<std::size_t>(width) * height * 3);
    auto update = [&](hui::InputFrame input = {})
    {
        feedback.clear();
        ui.update(input, 1.0f / 60, feedback);
        for (const auto &cue : feedback.cues)
            ++cues[cue.cue];
        if (record && ++frame_number % 3 == 0)
        {
            render();
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, recording_pixels.data());
            assert(std::fwrite(recording_pixels.data(), 1, recording_pixels.size(), stdout) ==
                   recording_pixels.size());
        }
    };
    auto frames = [&](int count)
    {
        for (int i = 0; i < count; ++i)
            update();
    };
    auto settle = [&]
    {
        for (int i = 0; i < 4000; ++i)
        {
            update();
            if (!app.busy())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.busy());
        frames(70);
    };
    auto press = [&](hui::Action action, hui::Direction direction = hui::Direction::none)
    {
        hui::InputFrame input;
        input.pressed = hui::action_bit(action);
        input.nav = direction;
        update(input);
        frames(45);
    };
    auto capture = [&](const char *name)
    {
        render();
        const auto error = glGetError();
        std::fprintf(stderr, "%s: %zu calls, %zu instances, GL 0x%x\n", name,
                     renderer.last_draw_calls(), renderer.last_instances(), error);
        assert(error == GL_NO_ERROR);
        if (record)
            return;
        std::vector<unsigned char> rgb(1920 * 1080 * 3);
        glReadPixels(0, 0, 1920, 1080, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        std::FILE *file = std::fopen((output + "/" + name + ".ppm").c_str(), "wb");
        assert(file);
        std::fprintf(file, "P6\n1920 1080\n255\n");
        for (int y = 1079; y >= 0; --y)
            std::fwrite(rgb.data() + y * 1920 * 3, 1, 1920 * 3, file);
        assert(std::fclose(file) == 0);
    };
    auto type = [&](const char *value)
    {
        for (const char *at = value; *at; ++at)
        {
            feedback.clear();
            ui.type(*at, feedback);
            update();
        }
    };

    if (const char *request = std::getenv("PROSPERO_SCRIPT"))
    {
        // The console's scripted run, played against the stand-ins in real time.
        prospero::DevScript script;
        if (!script.load(request, output))
        {
            std::fprintf(stderr, "no script in %s (or its token was already played)\n", request);
            return 2;
        }
        auto before = std::chrono::steady_clock::now();
        while (!script.quit() && !ui.quit_requested())
        {
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - before).count();
            before = now;
            feedback.clear();
            hui::InputFrame input;
            script.update(elapsed, app, ui, input, feedback);
            ui.update(input, std::min(std::max(elapsed, 0.001f), 0.05f), feedback);
            if (!script.capture().empty())
            {
                const std::string path = script.capture();
                const std::string name = path.substr(path.find_last_of('/') + 1,
                                                     path.size() - path.find_last_of('/') - 5);
                capture(name.c_str());
                script.capture_done(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        std::remove(image_fixture.c_str());
        app.shutdown();
        std::remove(PROSPERO_SETTINGS_PATH);
        return 0;
    }
    frames(40);
    capture("opening");
    settle();
    frames(120); // the opening has lifted and the first toast has gone
    if (empty)
    {
        capture("welcome-empty");
        press(hui::Action::page_next);
        capture("models-empty");
        std::remove(image_fixture.c_str());
        app.shutdown();
        std::remove(PROSPERO_SETTINGS_PATH);
        return 0;
    }
    frames(200);
    capture("welcome");
    press(hui::Action::page_next);
    capture("models");
    // A preset is on its way: Triangle asks, the second button cancels.
    prospero_model_download::preview_state = prospero_model_download::State::Downloading;
    frames(30);
    capture("downloading");
    press(hui::Action::north);
    capture("cancel-download");
    press(hui::Action::right, hui::Direction::right);
    press(hui::Action::confirm);
    assert(prospero_model_download::preview_state == prospero_model_download::State::Idle);
    capture("download-cancelled");
    frames(420); // its notice has gone
    // A newer version is listed: the question, its notes, the update at work, a failure.
    prospero::update::preview_offer = 1;
    frames(30);
    capture("update-offer");
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::confirm);
    capture("update-notes");
    press(hui::Action::back);
    press(hui::Action::right, hui::Direction::right);
    press(hui::Action::confirm);
    capture("update-working");
    prospero::update::preview_phase = prospero::update::Phase::failed;
    frames(30);
    capture("update-failed");
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::confirm);
    frames(30);
    press(hui::Action::right, hui::Direction::right);
    press(hui::Action::right, hui::Direction::right);
    capture("models-image");
    press(hui::Action::confirm);
    capture("switch-model");
    press(hui::Action::back);
    press(hui::Action::west);
    capture("search");
    press(hui::Action::back);
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::page_next);
    capture("settings");
    press(hui::Action::back);
    press(hui::Action::down, hui::Direction::down);
    capture("settings-generation");
    press(hui::Action::up, hui::Direction::up);
    press(hui::Action::confirm);
    press(hui::Action::right, hui::Direction::right);
    settle();
    capture("daylight");
    press(hui::Action::left, hui::Direction::left);
    settle();
    press(hui::Action::back);
    for (int i = 0; i < 4; ++i)
        press(hui::Action::down, hui::Direction::down);
    capture("diagnostics");
    press(hui::Action::down, hui::Direction::down);
    capture("about");
    for (int i = 0; i < 5; ++i)
        press(hui::Action::up, hui::Direction::up);
    press(hui::Action::page_prev);
    press(hui::Action::page_prev);

    // A conversation: the empty page, the wait, the answer as it arrives, the result.
    press(hui::Action::west);
    capture("conversation-new");
    assert(app.select_model(0));
    settle();
    type("Imagine a quiet lunar outpost. What makes it feel like home?");
    feedback.clear();
    ui.submit(feedback);
    frames(6);
    capture("thinking");
    for (int i = 0; i < 4000 && app.state().stream.size() < 150; ++i)
    {
        update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    capture("streaming");
    settle();
    ui.scroll(-10000);
    settle();
    capture("conversation");
    assert(app.send("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB"
                    "\xE3\x81\xA1\xE3\x81\xAF\xE3\x80\x82\xEC\x95\x88\xEB\x85\x95\xED\x95\x98"
                    "\xEC\x84\xB8\xEC\x9A\x94. \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82. "
                    "Tell me about the stars."));
    settle();
    frames(20); // the faces for the new scripts are read on the frame after they are seen
    capture("multilingual");
    fail_next_generation = true;
    assert(app.send("A question the fixture refuses."));
    settle();
    capture("failed");
    frames(420);
    assert(app.select_model(2));
    settle();
    capture("toast");
    assert(app.send("An image preview fixture"));
    settle();
    assert(app.state().images.size() == 1);
    capture("image");
    assert(app.select_model(3));
    settle();
    assert(app.send("A soft atmosphere for reading"));
    settle();
    capture("audio");
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::down, hui::Direction::down);
    capture("conversations");
    press(hui::Action::jump_next);
    capture("conversations-text");
    press(hui::Action::west);
    capture("conversation-search");
    for (char value : std::string("stars")) ui.type(value, feedback);
    capture("conversation-search-results");
    ui.submit(feedback);
    press(hui::Action::jump_prev);
    capture("conversations-filtered");
    press(hui::Action::confirm);
    settle();
    assert(std::strcmp(app.state().session.purpose, "text-to-text") == 0);
    capture("filtered-conversation-open");
    press(hui::Action::left, hui::Direction::left);
    press(hui::Action::north);
    capture("delete");
    press(hui::Action::back);
    press(hui::Action::right, hui::Direction::right);
    if (!record)
    {
        // With reduced motion nothing may move once the page is at rest.
        press(hui::Action::page_next);
        auto preferences = app.state().preferences;
        preferences.reduced_motion = true;
        preferences.high_contrast = true;
        app.set_preferences(preferences);
        settle();
        frames(500);
        render();
        capture("high-contrast");
        std::vector<unsigned char> before(static_cast<std::size_t>(width) * height * 3);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, before.data());
        frames(120);
        render();
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, recording_pixels.data());
        if (before != recording_pixels)
        {
            for (const auto &entry : {std::pair{"motion-before", &before},
                                      std::pair{"motion-after", &recording_pixels}})
            {
                std::ofstream diagnostic(output + "/" + entry.first + ".rgb", std::ios::binary);
                diagnostic.write(reinterpret_cast<const char *>(entry.second->data()),
                                 entry.second->size());
            }
        }
        assert(before == recording_pixels);
        std::fprintf(stderr, "Reduced motion: identical frames after two seconds at rest.\n");
    }
    std::fprintf(stderr, "Cues asked for:");
    for (const auto &[cue, count] : cues)
        std::fprintf(stderr, " %s x%u", hui::audio::cue_name(cue), count);
    std::fprintf(stderr, "\n");
    std::remove(image_fixture.c_str());
    app.shutdown();
    std::remove(PROSPERO_SETTINGS_PATH);
}

bool prospero_http_server_mobile_url(char *output, std::size_t capacity) { if(output && capacity) output[0]=0; return false; }
int prospero_http_server_auth_status() { return 0; }

void gpt_runtime_set_idle_unload_timeout(unsigned) {}
namespace prospero_model_download { bool delete_model_files(const char *) { return false; } }
