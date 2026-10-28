// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_workflows.hpp"
#include "gpt_runtime.hpp"
#include "model_manager.hpp"
#include "runtime_settings.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
unsigned selected = 0;
bool fail_generation = false;
unsigned generation_calls = 0;
std::string expected_id;
std::string expected_prompt;
std::string expected_lyrics;
std::string expected_options;
unsigned expected_seed = 0;
unsigned expected_duration = 0;
bool require_serialized = false;
std::string generation_response;

std::string read_bytes(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void progress(std::vector<std::string> &phases, std::string_view phase)
{
    phases.emplace_back(phase);
}

bool has_phase(const std::vector<std::string> &phases, const std::string &phase)
{
    return std::find(phases.begin(), phases.end(), phase) != phases.end();
}

void assert_settings_equal(const prospero_settings::Runtime &left,
                           const prospero_settings::Runtime &right)
{
    assert(left.context == right.context && left.output_limit == right.output_limit &&
           left.kv == right.kv && left.thinking == right.thinking &&
           left.sd_upscale == right.sd_upscale && left.web_search == right.web_search &&
           left.idle_unload_minutes == right.idle_unload_minutes &&
           left.web_search_timeout_seconds == right.web_search_timeout_seconds &&
           left.kokoro_voice == right.kokoro_voice &&
           left.chatterbox_voice == right.chatterbox_voice &&
           left.chatterbox_language == right.chatterbox_language &&
           left.audio_duration_sec == right.audio_duration_sec &&
           left.audio_seed == right.audio_seed && left.sd_seed == right.sd_seed &&
           left.sd_width == right.sd_width && left.sd_height == right.sd_height &&
           left.search_scopes == right.search_scopes && left.audio_lyrics == right.audio_lyrics &&
           left.audio_options == right.audio_options);
}
} // namespace

unsigned gpt_runtime_model_count()
{
    return 4;
}

unsigned gpt_runtime_selected_model()
{
    return selected;
}

const char *gpt_runtime_model_id(unsigned index)
{
    static const char *ids[] = {"music-model", "sheetsage-model", "htdemucs-model", "text-model"};
    return index < 4 ? ids[index] : "";
}

const char *gpt_runtime_model_purpose(unsigned index)
{
    return index == 3 ? "text-to-text" : index < 3 ? "text-to-audio" : "unknown";
}

bool gpt_runtime_select_model(unsigned index)
{
    if (index >= 4)
        return false;
    selected = index;
    return true;
}

int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,
                         gpt_runtime_stats_t *, gpt_runtime_progress_fn runtime_progress)
{
    assert(count == 1 && std::strcmp(messages[0].role, "user") == 0);
    assert(settings.model_id && expected_id == settings.model_id);
    assert(messages[0].content && expected_prompt == messages[0].content);
    if (require_serialized)
        assert(!prospero::model_manager::runtime_mutex().try_lock());

    const auto runtime = prospero_settings::get();
    assert(runtime.audio_lyrics == expected_lyrics);
    assert(runtime.audio_options == expected_options);
    assert(runtime.audio_seed == expected_seed);
    assert(runtime.audio_duration_sec == expected_duration);
    ++generation_calls;
    if (runtime_progress)
        runtime_progress("fake inference progress");
    const std::string response = generation_response.empty()
                                     ? fail_generation ? "fake generation failed"
                                                       : "generated locally at " PROSPERO_AUDIO_DIR
                                                         "result.wav"
                                     : generation_response;
    std::snprintf(output, capacity, "%s", response.c_str());
    return fail_generation ? 1 : 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    std::ofstream(root / "one.wav", std::ios::binary) << "wav";
    std::ofstream(root / "two.abc", std::ios::binary) << "X:1\n";
    std::ofstream(root / "ignored.txt") << "not audio";
    std::filesystem::create_directory(root / "folder.wav");
    std::filesystem::create_symlink(root / "one.wav", root / "linked.mp3");
    const auto entries = prospero::audio_workflows::list_library(root.string());
    assert(entries.size() == 2);
    assert(entries[0].name == "one.wav" || entries[1].name == "one.wav");
    assert(entries[0].size == 3 || entries[1].size == 3);

    std::string text;
    assert(prospero::audio_workflows::read_library_text(root.string(), "two.abc", text));
    assert(text == "X:1\n");
    assert(!prospero::audio_workflows::read_library_text(root.string(), "../escape.wav", text));
    assert(!prospero::audio_workflows::read_library_text(root.string(), "missing.wav", text));

    const std::string original = "existing score bytes";
    std::ofstream(root / "existing.abc", std::ios::binary) << original;
    assert(prospero::audio_workflows::save_abc(root.string(), "score.abc", "X:2\nK:C\n"));
    assert(prospero::audio_workflows::read_library_text(root.string(), "score.abc", text));
    assert(text == "X:2\nK:C\n");
    assert(!prospero::audio_workflows::save_abc(root.string(), "../library/existing.abc", "x"));
    assert(!prospero::audio_workflows::save_abc(root.string(), "existing.abc",
                                                std::string(256 * 1024 + 1, 'x')));
    assert(read_bytes(root / "existing.abc") == original);
    std::ofstream(root / "oversized.abc", std::ios::binary) << std::string(256 * 1024 + 1, 'x');
    assert(!prospero::audio_workflows::read_library_text(root.string(), "oversized.abc", text));

    std::filesystem::create_directories(PROSPERO_AUDIO_DIR);
    std::ofstream(std::string(PROSPERO_AUDIO_DIR) + "source.wav", std::ios::binary) << "source";
    std::ofstream(std::string(PROSPERO_AUDIO_DIR) + "result.wav", std::ios::binary) << "audio";
    std::ofstream(std::string(PROSPERO_AUDIO_DIR) + "source-cover.abc", std::ios::binary)
        << "X:1\n";
    std::ofstream(std::string(PROSPERO_AUDIO_DIR) + "source-vocals.wav", std::ios::binary)
        << "vocals";

    const auto previous = prospero_settings::get();
    auto settings = previous;
    settings.audio_lyrics = "original lyrics";
    settings.audio_options = "original=option";
    settings.audio_seed = 7;
    settings.audio_duration_sec = 9;
    assert(prospero_settings::set(settings));

    std::vector<std::string> phases;
    const unsigned before_wrong_capability = generation_calls;
    prospero::audio_workflows::MusicRequest invalid_request;
    invalid_request.model_id = "text-model";
    invalid_request.style = "not audio";
    const auto wrong_capability = prospero::audio_workflows::create_music(
        invalid_request, [&](std::string_view phase) { progress(phases, phase); });
    assert(!wrong_capability.ok && generation_calls == before_wrong_capability);

    expected_id = "music-model";
    expected_prompt = "jazz style";
    expected_lyrics = "new lyrics";
    expected_options = "original=option\nabc_file=" PROSPERO_AUDIO_DIR "score.abc";
    expected_seed = 123;
    expected_duration = 17;
    prospero::audio_workflows::MusicRequest request;
    request.model_id = expected_id;
    request.style = expected_prompt;
    request.lyrics = expected_lyrics;
    request.abc_name = "score.abc";
    request.abc_content = "X:3\n";
    request.track_name = "test-track";
    request.seed = expected_seed;
    request.duration_seconds = expected_duration;
    require_serialized = true;
    const auto music = prospero::audio_workflows::create_music(request, [&](std::string_view phase)
                                                               { progress(phases, phase); });
    require_serialized = false;
    assert(music.ok && !music.retryable && music.artifacts.size() == 1);
    assert(music.artifacts[0] == std::string(PROSPERO_AUDIO_DIR) + "result.wav");
    assert(has_phase(phases, "validation") && has_phase(phases, "preparation") &&
           has_phase(phases, "inference") && has_phase(phases, "artifact-publication") &&
           has_phase(phases, "fake inference progress"));
    const auto restored = prospero_settings::get();
    assert_settings_equal(restored, settings);
    assert(selected == 0);

    phases.clear();
    expected_id = "sheetsage-model";
    expected_prompt = "Cover to ABC: source.wav";
    expected_lyrics = settings.audio_lyrics;
    expected_options = "original=option\ninput_audio=" PROSPERO_AUDIO_DIR "source.wav";
    expected_seed = settings.audio_seed;
    expected_duration = settings.audio_duration_sec;
    generation_response =
        "Cover transcribed locally. Artifacts: " PROSPERO_AUDIO_DIR "source-cover.abc";
    const auto cover = prospero::audio_workflows::cover_to_abc(
        expected_id, "source.wav", [&](std::string_view phase) { progress(phases, phase); });
    assert(cover.ok && cover.artifacts.size() == 1);
    assert(cover.artifacts[0] == std::string(PROSPERO_AUDIO_DIR) + "source-cover.abc");
    assert_settings_equal(prospero_settings::get(), settings);
    const unsigned before_invalid = generation_calls;
    const auto missing = prospero::audio_workflows::cover_to_abc(
        expected_id, "missing.wav", [&](std::string_view phase) { progress(phases, phase); });
    assert(!missing.ok && generation_calls == before_invalid);
    assert(selected == 1);

    phases.clear();
    expected_id = "htdemucs-model";
    expected_prompt = "Split into stems: source.wav";
    expected_options = "original=option\ninput_audio=" PROSPERO_AUDIO_DIR "source.wav";
    generation_response = "Stems written: " PROSPERO_AUDIO_DIR "source-vocals.wav";
    const auto stems = prospero::audio_workflows::separate_stems(
        expected_id, "source.wav", [&](std::string_view phase) { progress(phases, phase); });
    assert(stems.ok && stems.artifacts.size() == 1);
    assert(stems.artifacts[0] == std::string(PROSPERO_AUDIO_DIR) + "source-vocals.wav");
    assert_settings_equal(prospero_settings::get(), settings);
    assert(selected == 2);

    fail_generation = true;
    expected_id = "music-model";
    expected_prompt = "failure style";
    expected_lyrics.clear();
    expected_options = settings.audio_options;
    expected_seed = 42;
    expected_duration = 0;
    request = {};
    request.model_id = expected_id;
    request.style = expected_prompt;
    generation_response.clear();
    const auto failed = prospero::audio_workflows::create_music(request, [&](std::string_view phase)
                                                                { progress(phases, phase); });
    assert(!failed.ok && !failed.retryable && failed.response == "fake generation failed");
    assert_settings_equal(prospero_settings::get(), settings);

    fail_generation = false;
    generation_response = "NEEDS_RESTART GPU memory low. Restart the title and retry.";
    expected_prompt = "memory test";
    request = {};
    request.model_id = expected_id;
    request.style = expected_prompt;
    const auto memory_failure = prospero::audio_workflows::create_music(
        request, [&](std::string_view phase) { progress(phases, phase); });
    assert(!memory_failure.ok && memory_failure.retryable);
    assert(memory_failure.response.find("NEEDS_RESTART") == std::string::npos);
    assert(memory_failure.response.find("Restart the title") == std::string::npos);
    assert_settings_equal(prospero_settings::get(), settings);

    assert(prospero_settings::set(previous));
    std::puts("PASS shared audio workflow library and runtime operations");
}
