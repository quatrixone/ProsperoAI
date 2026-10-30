// Host checks for real controller ownership, persistence, and multilingual layout.
#include "native_app.hpp"
#include "audio_workflows.hpp"
#include "native_audio_queue.hpp"
#include "debug_log.hpp"
#include "model_manager.hpp"
#include "runtime_settings.hpp"
#include "gfx/font.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
std::thread::id ui_thread;
unsigned selected = 0;
bool fail_generation = false, fail_save = false, fail_after_generation = false;
bool fail_prepare = false;
bool fail_audio_generation = false;
std::atomic<bool> long_response_paused{false};
std::atomic<bool> allow_long_response_finish{false};
unsigned generations = 0, select_calls = 0, prepare_calls = 0, audio_generations = 0;
unsigned fixture_model_count = 500;
bool audio_only_fixture = false;
bool fixture_model_loaded = false, fixture_vulkan_memory_available = false;
std::uint64_t fixture_vulkan_free_bytes = 0, fixture_vulkan_total_bytes = 0;
std::string audio_root, last_audio_prompt, last_audio_options;
struct Saved
{
    prospero_session::Record record;
    std::vector<prospero_session::Message> messages;
};
std::map<std::string, Saved> saved;
unsigned next_session = 0;
void off_ui()
{
    assert(std::this_thread::get_id() != ui_thread);
}
bool told(prospero::App &app, prospero::Notice notice)
{
    const auto notices = app.take_notices();
    return std::find(notices.begin(), notices.end(), notice) != notices.end();
}
void settle(prospero::App &app)
{
    for (int i = 0; i < 2000; ++i)
    {
        app.poll();
        if (!app.busy())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "worker did not complete");
}

void test_stopped_queue_waiting_for_busy_worker()
{
    bool active = true;
    bool stop_requested = false;
    constexpr int no_active_item = -1;
    std::vector<std::string> pending{"pending", "pending"};

    assert(prospero::native_audio_queue::dispatch_decision(active, stop_requested, no_active_item,
                                                           true) ==
           prospero::native_audio_queue::DispatchDecision::wait);
    stop_requested = true;
    assert(prospero::native_audio_queue::dispatch_decision(active, stop_requested, no_active_item,
                                                           false) ==
           prospero::native_audio_queue::DispatchDecision::stopped);
    assert(!active && !stop_requested && pending[0] == "pending" && pending[1] == "pending");
}

void test_native_vulkan_memory_snapshot()
{
    assert(prospero::debug::set_enabled(true));
    fixture_model_loaded = true;
    fixture_vulkan_memory_available = true;
    fixture_vulkan_free_bytes = 6ull << 30;
    fixture_vulkan_total_bytes = 10ull << 30;

    prospero::App app;
    assert(app.initialize());
    settle(app);
    assert(app.state().loaded_model_id == "model-0");
    assert(app.state().loaded_model_name == "model-0");
    assert(app.state().vulkan_memory_available);
    assert(app.state().vulkan_free_bytes == (6ull << 30));
    assert(app.state().vulkan_allocated_bytes == (4ull << 30));
    assert(app.state().vulkan_total_bytes == (10ull << 30));

    fixture_vulkan_memory_available = false;
    app.poll();
    settle(app);
    assert(app.state().vulkan_memory_available);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    app.poll();
    settle(app);
    assert(app.state().loaded_model);
    assert(app.state().loaded_model_name == "model-0");
    assert(!app.state().vulkan_memory_available);
    assert(app.state().vulkan_allocated_bytes == 0);

    fixture_model_loaded = false;
    fixture_vulkan_memory_available = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    app.poll();
    settle(app);
    assert(app.state().loaded_model_id.empty());
    assert(app.state().loaded_model_name.empty());
    assert(!app.state().loaded_model);
    assert(app.state().vulkan_memory_available);
    assert(app.state().vulkan_allocated_bytes == (4ull << 30));
    app.shutdown();
    fixture_model_loaded = false;
    assert(prospero::debug::set_enabled(false));
    std::ifstream trace(std::string(prospero::debug::folder()) + "/debug-trace.txt");
    const std::string trace_contents((std::istreambuf_iterator<char>(trace)),
                                     std::istreambuf_iterator<char>());
    assert(trace_contents.find("refresh Vulkan memory:") != std::string::npos);
}
} // namespace

void gpt_runtime_refresh_models()
{
    off_ui();
}
void gpt_runtime_set_idle_unload_timeout(unsigned)
{
}
unsigned gpt_runtime_model_count()
{
    off_ui();
    return fixture_model_count;
}
unsigned gpt_runtime_selected_model()
{
    off_ui();
    return selected;
}
const char *gpt_runtime_model_id(unsigned index)
{
    off_ui();
    static char id[48];
    if (index == 1)
        std::snprintf(id, sizeof(id), "sheetsage-test");
    else if (index == 4)
        std::snprintf(id, sizeof(id), "htdemucs-test");
    else if (index == 5)
        std::snprintf(id, sizeof(id), "music-test");
    else
        std::snprintf(id, sizeof(id), "model-%u", index);
    return id;
}
const char *gpt_runtime_model_name(unsigned index)
{
    return gpt_runtime_model_id(index);
}
const char *gpt_runtime_model_purpose(unsigned index)
{
    off_ui();
    if (audio_only_fixture || index == 1 || index == 4 || index == 5)
        return "text-to-audio";
    if (index == 2)
        return "text-to-image";
    if (index == 3)
        return "text-to-speech";
    return "text-to-text";
}
bool gpt_runtime_loaded_model(char *id, std::size_t ic, char *name, std::size_t nc)
{
    off_ui();
    if (!fixture_model_loaded)
    {
        if (id && ic)
            id[0] = 0;
        if (name && nc)
            name[0] = 0;
        return false;
    }
    if (id && ic)
        std::snprintf(id, ic, "%s", "model-0");
    if (name && nc)
        std::snprintf(name, nc, "%s", "model-0");
    return true;
}
bool gpt_runtime_vulkan_memory(std::uint64_t *free_bytes, std::uint64_t *total_bytes)
{
    off_ui();
    if (!fixture_vulkan_memory_available || !free_bytes || !total_bytes)
        return false;
    *free_bytes = fixture_vulkan_free_bytes;
    *total_bytes = fixture_vulkan_total_bytes;
    return true;
}
bool gpt_runtime_select_model(unsigned index)
{
    off_ui();
    ++select_calls;
    selected = index;
    return true;
}
int gpt_runtime_prepare()
{
    off_ui();
    ++prepare_calls;
    return fail_prepare ? -1 : 0;
}
bool gpt_runtime_context_full()
{
    off_ui();
    return false;
}
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,
                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress,
                         const char *image_path)
{
    assert(image_path == nullptr);
    off_ui();
    ++generations;
    if (count == 1 && settings.model_id &&
        (std::strcmp(settings.model_id, "sheetsage-test") == 0 ||
         std::strcmp(settings.model_id, "htdemucs-test") == 0 ||
         std::strcmp(settings.model_id, "music-test") == 0))
    {
        ++audio_generations;
        last_audio_prompt = messages[0].content;
        last_audio_options = prospero_settings::get().audio_options;
        if (fail_audio_generation)
        {
            std::snprintf(output, capacity, "%s", "Audio runtime failure");
            return 1;
        }
        const std::string artifact = audio_root + "/generated.wav";
        std::ofstream file(artifact, std::ios::binary);
        file << "fixture";
        std::snprintf(output, capacity, "Generated audio: %s", artifact.c_str());
        return 0;
    }
    assert(count >= 2 && std::strcmp(messages[count - 1].role, "user") == 0);
    if (std::strcmp(messages[count - 1].content, "Long response") == 0)
    {
        assert(settings.max_output_tokens == 65536 && settings.context_length == 32768);
        assert(settings.model_id && std::strcmp(settings.model_id, "model-499") == 0);
        const std::string text(24000, 'x');
        for (unsigned i = 0; i < 20; ++i)
        {
            progress(text.c_str());
            if (i == 0)
            {
                long_response_paused.store(true, std::memory_order_release);
                while (!allow_long_response_finish.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::snprintf(output, capacity, "%s", text.c_str());
        stats->generated_tokens = 65536;
        return 0;
    }
    if (std::strcmp(messages[count - 1].content, "Workspace staged options") == 0)
    {
        assert(settings.model_id && std::strcmp(settings.model_id, "model-0") == 0);
        assert(settings.max_output_tokens == 2048 && settings.context_length == 4096);
        assert(settings.thinking);
    }
    if (std::strcmp(messages[count - 1].content, "Workspace staged options without model change") ==
        0)
    {
        assert(settings.model_id && std::strcmp(settings.model_id, "model-0") == 0);
        assert(settings.max_output_tokens == 4096 && settings.context_length == 8192);
        assert(!settings.thinking);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    progress("Thinking about your question...");
    std::snprintf(output, capacity, "%s",
                  fail_generation ? "Test runtime failure" : "Hello, 世界. A complete response.");
    stats->generated_tokens = 10;
    if (fail_after_generation)
        fail_save = true;
    return fail_generation ? 1 : 0;
}

extern "C" std::int64_t sceKernelGetDirectMemorySize()
{
    return 16ll << 30;
}
extern "C" int sceKernelDirectMemoryQuery(std::int64_t, int, void *, std::size_t)
{
    return -1;
}
namespace prospero_session
{
bool create(Record *record, const char *id, const char *name, const char *purpose)
{
    off_ui();
    std::snprintf(record->id, sizeof(record->id), "session-%u", ++next_session);
    std::snprintf(record->model_id, sizeof(record->model_id), "%s", id);
    std::snprintf(record->model_name, sizeof(record->model_name), "%s", name);
    std::snprintf(record->purpose, sizeof(record->purpose), "%s", purpose);
    return true;
}
bool save(Record *record, const Message *messages, unsigned count)
{
    off_ui();
    if (fail_save)
        return false;
    record->message_count = count;
    saved[record->id] = {*record, {messages, messages + count}};
    return true;
}
bool load(const char *id, Record *record, Message *messages, unsigned capacity)
{
    off_ui();
    auto it = saved.find(id);
    if (it == saved.end() || it->second.messages.size() > capacity)
        return false;
    *record = it->second.record;
    std::copy(it->second.messages.begin(), it->second.messages.end(), messages);
    return true;
}
unsigned scan(Record *records, unsigned capacity)
{
    off_ui();
    unsigned count = 0;
    for (auto &[id, entry] : saved)
        if (count < capacity)
            records[count++] = entry.record;
    return count;
}
bool erase(const char *id)
{
    off_ui();
    return saved.erase(id) != 0;
}
bool archive_text(const char *, unsigned, const char *, const OutputMetadata &, char *, std::size_t)
{
    off_ui();
    return true;
}
bool archive_media(const char *, unsigned, const char *, const char *, char *, std::size_t,
                   const OutputMetadata *)
{
    off_ui();
    return false;
}
} // namespace prospero_session

int main(int argc, char **argv)
{
    assert(argc == 4);
    prospero::debug::start(argv[3]);
    test_stopped_queue_waiting_for_busy_worker();
    std::remove(PROSPERO_SETTINGS_PATH);
    {
        std::ofstream settings(PROSPERO_SETTINGS_PATH);
        settings << "style=2\ntokens=192\nkv=99\n";
    }
    ui_thread = std::this_thread::get_id();
    test_native_vulkan_memory_snapshot();
    {
        prospero::App app;
        assert(app.initialize());
        settle(app);
        assert(app.state().models.size() == 500 && app.state().model_counts[0] == 495);
        assert(app.state().preferences.style == 2 && app.state().preferences.output_limit == 192);
        assert(app.state().preferences.idle_unload_minutes == 5);
        assert(app.state().preferences.context_length == 0 &&
               app.state().preferences.kv_cache == 0 && !app.state().preferences.thinking);
        const auto before_refresh = app.state();
        assert(app.refresh_models());
        assert(!app.refresh_models());
        settle(app);
        assert(app.state().models.size() == 500);
        assert(app.state().selected_model == before_refresh.selected_model);
        assert(app.state().messages.size() == before_refresh.messages.size());
        assert(app.state().ready == before_refresh.ready);
        // Starting without an explicitly selected model leaves the app unprepared.
        assert(!app.state().ready && app.state().selected_model == -1);
        assert(app.activity() == prospero::Activity::Idle);
        assert(!told(app, prospero::Notice::ModelReady));
        assert(app.select_model(499));
        settle(app);
        assert(app.state().preferences.model_id == "model-499");
        assert(!app.send(" \n\t"));
        assert(app.send("你好, explain the night sky."));
        assert(!app.select_model(3));
        assert(!app.new_session());
        assert(!app.send("Duplicate"));
        settle(app);
        assert(app.state().messages.size() == 2 && app.state().sessions.size() == 1);
        assert(app.state().session_search.size() == 1 && app.state().session_search[0].text);
        assert(prospero_session::contains(app.state().session_search[0].text->c_str(),
                                          "complete response"));
        assert(app.state().messages.back().content.find("世界") != std::string::npos);
        assert(app.state().stats_valid && app.state().stats.generated_tokens == 10);
        assert(told(app, prospero::Notice::ReplyReady));
        // A replacement model may fail to prepare; keep the saved conversation
        // visible and allow its original model to be prepared again.
        fail_prepare = true;
        assert(app.select_model(12));
        settle(app);
        assert(!app.state().ready && !app.state().preparing);
        assert(app.state().selected_model == 12 && app.state().messages.size() == 2);
        assert(std::strcmp(app.state().session.model_id, "model-499") == 0);
        assert(app.state().status.find("could not be prepared") != std::string::npos);
        assert(!app.can_send());
        fail_prepare = false;
        assert(app.open_session(0));
        settle(app);
        assert(app.state().ready && app.state().selected_model == 499);
        assert(app.state().messages.size() == 2 && app.can_send());
        assert(app.select_model(12));
        settle(app);
        assert(app.state().messages.empty());
        assert(app.open_session(0));
        settle(app);
        assert(app.state().selected_model == 499 && app.state().messages.size() == 2);
        fail_generation = true;
        assert(app.send("A question that fails"));
        settle(app);
        assert(app.state().retry_available && app.state().messages.size() == 3);
        assert(!app.state().stats_valid && told(app, prospero::Notice::GenerationFailed));
        fail_generation = false;
        assert(app.retry());
        settle(app);
        assert(!app.state().retry_available && app.state().messages.size() == 4);
        fail_save = true;
        assert(app.send("Must not lose my message"));
        settle(app);
        assert(app.state().retry_available && app.state().messages.size() == 5);
        fail_save = false;
        assert(app.retry());
        settle(app);
        assert(app.state().messages.size() == 6);
        fail_after_generation = true;
        assert(app.send("Keep a response even if the final save fails"));
        settle(app);
        assert(app.state().unsaved && app.state().messages.size() == 8);
        assert(!app.select_model(12) && !app.new_session());
        const auto previous_generations = generations;
        fail_save = fail_after_generation = false;
        assert(app.retry());
        settle(app);
        assert(!app.state().unsaved && generations == previous_generations);
        // A message whose answer failed is replaced by the next one, never doubled.
        fail_generation = true;
        assert(app.send("A first try"));
        settle(app);
        assert(app.state().retry_available && app.state().messages.size() == 9);
        fail_generation = false;
        assert(app.send("A second try"));
        settle(app);
        assert(app.state().messages.size() == 10);
        assert(app.state().messages[8].content == "A second try" &&
               std::strcmp(app.state().messages[9].role, "assistant") == 0);
        assert(app.state().preferences.output_limit == 192); // Legacy preference loaded above.
        assert(app.state().preferences.context_length == 0);
        auto preferences = app.state().preferences;
        preferences.reduced_motion = true;
        preferences.accent = 2;
        preferences.context_length = 32768;
        preferences.kv_cache = 3;
        preferences.thinking = true;
        preferences.output_limit = 65536;
        app.set_preferences(preferences);
        settle(app);
        // An API client may have selected another model between UI requests.
        selected = 12;
        long_response_paused.store(false, std::memory_order_relaxed);
        allow_long_response_finish.store(false, std::memory_order_relaxed);
        assert(app.send("Long response"));
        bool reached_long_stream = false;
        for (int i = 0; i < 2000 && !reached_long_stream; ++i)
        {
            app.poll();
            reached_long_stream = long_response_paused.load(std::memory_order_acquire);
            if (!reached_long_stream)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        app.poll();
        const bool saw_long_stream =
            reached_long_stream && app.busy() && app.state().stream.size() == 24000;
        allow_long_response_finish.store(true, std::memory_order_release);
        settle(app);
        assert(saw_long_stream);
        assert(app.state().messages.back().content.size() == 24000);
        assert(app.state().stats.generated_tokens == 65536);
    }
    {
        prospero::model_manager::publish("", prospero::model_manager::Stage::none);
        prospero::App app;
        assert(app.initialize());
        settle(app);
        // A remembered model is not prepared automatically; the user selects it per session.
        assert(app.state().selected_model == -1 && !app.state().ready);
        assert(app.state().preferences.reduced_motion && app.state().preferences.accent == 2);
        assert(app.state().preferences.context_length == 32768);
        assert(app.state().preferences.kv_cache == 3 && app.state().preferences.thinking);
        assert(app.state().preferences.output_limit == 65536);
        saved.begin()->second.record.model_id[0] = 'x';
        assert(app.open_session(0));
        settle(app);
        assert(!app.can_send() && !app.state().messages.empty());
        assert(told(app, prospero::Notice::ModelMissing));
        assert(app.delete_session(0));
        settle(app);
        assert(app.state().messages.empty() && saved.empty());
    }
    hui::gfx::Font regular, fallback;
    std::ifstream primary_file(argv[1], std::ios::binary), fallback_file(argv[2], std::ios::binary);
    std::string primary{std::istreambuf_iterator<char>(primary_file), {}},
        secondary{std::istreambuf_iterator<char>(fallback_file), {}};
    assert(regular.load(primary) && fallback.load(secondary));
    regular.add_fallback(&fallback, 5);
    assert(regular.has_glyph(0x4e16) && regular.has_glyph(0x754c));
    std::vector<hui::gfx::GlyphQuad> quads;
    const float width = regular.layout("Hello 世界", 0, 0, 28, hui::gfx::Align::left, quads);
    assert(width == regular.measure("Hello 世界", 28));
    assert(quads.front().texture == 0 && quads.back().texture == 5 && quads.back().range > 0);
    // A fallback is asked only for what the face itself lacks.
    regular.clear_fallbacks();
    assert(!regular.has_glyph(0x4e16) && regular.has_glyph(0x00e9) && regular.has_glyph(0x0416));
    regular.add_fallback(&fallback, 5);
    const std::string original = "世界世界世界世界世界世界世界世界";
    std::string joined;
    for (const auto &line : regular.wrap(original, 28, 140))
    {
        assert(regular.measure(line, 28) <= 140);
        joined += line;
    }
    assert(original == joined);
    // Words still break at spaces; only a word wider than the line is cut.
    const auto words =
        regular.wrap("one two https://a.very.long.address/that/has/no/spaces three", 28, 220);
    assert(words.size() >= 4 && words.front() == "one two");
    for (const auto &line : words)
        assert(regular.measure(line, 28) <= 220);
    const std::string image_path = std::string(PROSPERO_SETTINGS_PATH) + ".tga";
    unsigned char tga[30]{};
    tga[2] = 2;
    tga[12] = 2;
    tga[14] = 2;
    tga[16] = 24;
    tga[17] = 0x20;
    tga[20] = 255; // red top left
    tga[22] = 255; // green top right
    tga[24] = 255; // blue bottom left
    tga[27] = tga[28] = tga[29] = 255;
    {
        std::ofstream out(image_path, std::ios::binary);
        out.write(reinterpret_cast<char *>(tga), sizeof(tga));
    }
    const auto image = prospero::load_image_preview(image_path);
    assert(image && image->width == 2 && image->height == 2 && image->rgba.size() == 16);
    assert(image->rgba[0] == 255 && image->rgba[1] == 0 && image->rgba[6] == 0 &&
           image->rgba[10] == 255);
    {
        std::ofstream out(image_path, std::ios::binary);
        out.write(reinterpret_cast<char *>(tga), 25);
    }
    assert(!prospero::load_image_preview(image_path));
    std::remove(image_path.c_str());
    std::ifstream journal(std::string(argv[3]) + "/events.jsonl");
    const std::string events(std::istreambuf_iterator<char>(journal), {});
    assert(events.find("\"operation_id\":") != std::string::npos);
    const auto operation_id_for = [&](const std::string &operation)
    {
        const auto operation_pos = events.find("\"operation\":\"" + operation + "\"");
        assert(operation_pos != std::string::npos);
        const auto id_pos = events.rfind("\"operation_id\":", operation_pos);
        assert(id_pos != std::string::npos);
        const auto value_pos = id_pos + std::string("\"operation_id\":").size();
        const auto value_end = events.find(',', value_pos);
        assert(value_end != std::string::npos);
        return events.substr(value_pos, value_end - value_pos);
    };
    const auto failed_generation_id = operation_id_for("generation-save-failed");
    assert(events.find("\"operation_id\":" + failed_generation_id +
                       ",\"operation\":\"native-generation-started\"") != std::string::npos);
    assert(events.find("\"error\":\"runtime-generation-failed\"") != std::string::npos);
    assert(events.find("\"error\":\"user-message-save-failed\"") != std::string::npos);
    assert(events.find("\"operation\":\"generation-save-failed\"") != std::string::npos);
    assert(events.find("\"error\":\"assistant-message-save-failed\"") != std::string::npos);
    assert(events.find("A question that fails") == std::string::npos);
    assert(events.find("Hello, 世界.") == std::string::npos);

    // Workspace model and workflow choices are staged until their worker operation runs.
    fixture_model_count = 6;
    audio_only_fixture = false;
    selected = 0;
    select_calls = prepare_calls = audio_generations = 0;
    audio_root = PROSPERO_AUDIO_DIR;
    assert(mkdir(audio_root.c_str(), 0777) == 0 || errno == EEXIST);
    {
        std::ofstream wav(audio_root + "/uploaded.wav", std::ios::binary);
        const unsigned char bytes[] = {'R',  'I',  'F', 'F', 40,   0,    0, 0, 'W', 'A', 'V', 'E',
                                       'f',  'm',  't', ' ', 16,   0,    0, 0, 1,   0,   1,   0,
                                       0x44, 0xac, 0,   0,   0x88, 0x58, 1, 0, 2,   0,   16,  0,
                                       'd',  'a',  't', 'a', 4,    0,    0, 0, 0,   0,   0,   0};
        wav.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    }
    prospero::model_manager::publish("", prospero::model_manager::Stage::none);
    {
        prospero::App app;
        assert(app.initialize());
        settle(app);
        assert(app.model_indices_for(prospero::Capability::Text) == std::vector<unsigned>{0});
        assert(app.model_indices_for(prospero::Capability::Image) == std::vector<unsigned>{2});
        assert(app.model_indices_for(prospero::Capability::Voice) == std::vector<unsigned>{3});
        assert(app.model_indices_for(prospero::Capability::Audio) ==
               (std::vector<unsigned>{1, 4, 5}));
        const auto runtime_before_stage = prospero_settings::get();
        auto staged = app.state().preferences;
        staged.output_limit = 2048;
        staged.context_length = 4096;
        staged.thinking = true;
        assert(app.stage_model(0));
        assert(app.stage_preferences(staged));
        assert(select_calls == 0 && prepare_calls == 0);
        const auto runtime_after_stage = prospero_settings::get();
        assert(runtime_after_stage.output_limit == runtime_before_stage.output_limit &&
               runtime_after_stage.context == runtime_before_stage.context &&
               runtime_after_stage.thinking == runtime_before_stage.thinking);
        assert(app.send("Workspace staged options"));
        settle(app);
        assert(select_calls == 1 && prepare_calls == 1);
        assert(app.state().selected_model == 0 && app.state().ready);
        const auto runtime_before_options = prospero_settings::get();
        staged = app.state().preferences;
        staged.output_limit = 4096;
        staged.context_length = 8192;
        staged.thinking = false;
        assert(app.stage_preferences(staged));
        const auto runtime_after_options = prospero_settings::get();
        assert(runtime_after_options.output_limit == runtime_before_options.output_limit &&
               runtime_after_options.context == runtime_before_options.context &&
               runtime_after_options.thinking == runtime_before_options.thinking);
        assert(app.send("Workspace staged options without model change"));
        settle(app);
        assert(select_calls == 1 && prepare_calls == 1);
        const unsigned selections_before_switch = select_calls;
        assert(app.stage_model(2));
        assert(select_calls == selections_before_switch);
        assert(app.send("Workspace switched model"));
        settle(app);
        assert(select_calls == selections_before_switch + 1);
        assert(std::strcmp(app.state().session.model_id, "model-2") == 0 && app.can_send());

        const unsigned audio_before = audio_generations;
        assert(app.start_cover("sheetsage-test", "uploaded.wav"));
        settle(app);
        assert(audio_generations == audio_before + 1);
        assert(last_audio_prompt.find("uploaded.wav") != std::string::npos);
        assert(last_audio_options.find("input_audio=" + audio_root + "/uploaded.wav") !=
               std::string::npos);
        assert(!app.state().operation_status.empty() && app.state().operation_status == "done");

        const unsigned generation_before_missing = generations;
        const unsigned selection_before_missing = select_calls;
        assert(app.start_stems("htdemucs-test", "missing.wav"));
        settle(app);
        assert(generations == generation_before_missing &&
               select_calls == selection_before_missing);
        assert(app.state().operation_status == "failed" &&
               app.state().status.find("missing") != std::string::npos);

        fail_audio_generation = true;
        assert(app.start_cover("sheetsage-test", "uploaded.wav"));
        settle(app);
        fail_audio_generation = false;
        assert(app.state().operation_status == "failed" &&
               app.state().operation_result.find("Audio runtime failure") != std::string::npos);

        prospero::audio_workflows::MusicRequest request;
        request.model_id = "music-test";
        request.style = "ambient";
        assert(app.start_music(request));
        assert(app.busy());
        assert(!app.start_stems("htdemucs-test", "uploaded.wav"));
        settle(app);
        assert(app.state().operation_status == "done");
    }
    fixture_model_count = 1;
    audio_only_fixture = true;
    prospero::model_manager::publish("", prospero::model_manager::Stage::none);
    const unsigned selections_before_empty_filter = select_calls;
    const unsigned preparations_before_empty_filter = prepare_calls;
    {
        prospero::App app;
        assert(app.initialize());
        settle(app);
        assert(app.model_indices_for(prospero::Capability::Audio) == std::vector<unsigned>{0});
        assert(app.model_indices_for(prospero::Capability::Text).empty());
        assert(app.model_indices_for(prospero::Capability::Image).empty());
        assert(app.model_indices_for(prospero::Capability::Voice).empty());
        assert(select_calls == selections_before_empty_filter &&
               prepare_calls == preparations_before_empty_filter);
    }
    std::remove((audio_root + "/uploaded.wav").c_str());
    std::remove((audio_root + "/generated.wav").c_str());
    rmdir(audio_root.c_str());
    std::remove(PROSPERO_SETTINGS_PATH);
    std::puts("Controller: 500 models, worker ownership, switching, session restore, retry, "
              "persistence and Unicode passed.");
}
