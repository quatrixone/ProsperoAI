// ProsperoAI application state. Rendering never calls the model runtime or storage.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gpt_runtime.hpp"
#include "audio_workflows.hpp"
#include "session_store.hpp"
#include "media_preview.hpp"
#include "debug_log.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#ifdef PROSPERO_HOST
#include <pthread.h>
#endif

namespace prospero
{
enum class Capability
{
    Text,
    Image,
    Audio,
    Voice
};
struct Model
{
    std::string id, name, purpose;
    Capability capability = Capability::Text;
};
struct Preferences
{
    unsigned style = 0, output_limit = 0; // 0 = Auto (fill the remaining context)
    unsigned context_length = 0, kv_cache = 0;
    unsigned idle_unload_minutes = 5, kokoro_voice = 0, chatterbox_voice = 0, chatterbox_language = 0;
    unsigned audio_duration_sec = 0, audio_seed = 42, sd_seed = 42, sd_width = 0, sd_height = 0;
    bool thinking = false;
    bool sd_upscale = false, web_search = false;
    unsigned theme = 0, accent = 0, volume = 35, text_size = 0;
    bool reduced_motion = false, high_contrast = false;
    std::string model_id;
};
// What the last finished job has to tell the player, beyond the status line.
enum class Notice
{
    None,
    ModelReady,
    ModelFailed,
    ModelMissing,
    ReplyReady,
    MediaReady,
    GenerationFailed,
    SaveFailed,
    ConversationDeleted,
    SettingsNotSaved
};
// What the worker is busy with, for the indicators.
enum class Activity
{
    Idle,
    Starting,
    PreparingModel,
    OpeningConversation,
    Generating,
    Saving
};
struct SessionSearch
{
    std::string id;
    std::uint64_t updated_us = 0;
    unsigned message_count = 0;
    std::shared_ptr<const std::string> text;
};
struct State
{
    std::vector<Model> models;
    std::array<unsigned, 4> model_counts{}; // installed models per Capability
    std::vector<prospero_session::Record> sessions;
    std::vector<SessionSearch> session_search;
    std::vector<prospero_session::Message> messages;
    std::vector<std::shared_ptr<const ImagePreview>> images;
    prospero_session::Record session{};
    Preferences preferences;
    int selected_model = -1;
    int staged_model = -1;
    bool ready = false, preparing = false, initialized = false, retry_available = false,
         unsaved = false;
    bool loaded_model = false, vulkan_memory_available = false;
    std::string loaded_model_id, loaded_model_name;
    std::uint64_t vulkan_free_bytes = 0, vulkan_allocated_bytes = 0, vulkan_total_bytes = 0;
    std::string status = "Opening your space...", stream;
    std::string operation_status, operation_progress, operation_result;
    std::vector<std::string> operation_artifacts;
    // Figures of the last answer in the conversation on screen; see stats_valid.
    gpt_runtime_stats_t stats{};
    bool stats_valid = false;
    Capability stats_kind = Capability::Text;
    Notice notice = Notice::None;
    unsigned revision = 0;
};

// One serialized worker owns all runtime and session-store access. Result publication
// is release/acquire; a renderer only sees complete snapshots on its own thread.
class App
{
  public:
    App() = default;
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    bool initialize();
    bool refresh_models();
    bool sync_sessions();
    void poll();
    void shutdown();
    const State &state() const
    {
        return state_;
    }
    bool busy() const
    {
        return running_;
    }
    Activity activity() const;
    bool generating() const;
    bool can_send() const;
    bool select_model(unsigned index);
    std::vector<unsigned> model_indices_for(Capability capability) const;
    bool stage_model(unsigned index);
    bool stage_preferences(Preferences preferences);
    bool open_session(unsigned index);
    bool delete_session(unsigned index);
    bool new_session();
    bool send(const std::string &text);
    bool retry();
    bool play_audio();
    bool play_audio(const std::string &path);
    bool start_music(const audio_workflows::MusicRequest &request);
    bool start_cover(const std::string &model_id, const std::string &source_name);
    bool start_stems(const std::string &model_id, const std::string &source_name);
    void set_preferences(Preferences preferences);
    // The notices of the jobs that finished since the last call, oldest first.
    std::vector<Notice> take_notices();

  private:
    enum class Job
    {
        Discover,
        RefreshModels,
        Select,
        Open,
        Delete,
        Generate,
        Save,
        Preferences,
        Play,
        SyncSessions,
        Music,
        Cover,
        Stems,
        VulkanMemory
    };
    bool start(Job job);
    static void *worker(void *context);
    static void stream_callback(const char *text);
    void work();
    void generate();
    void refresh_sessions();
    void refresh_images();
    void join();
    State state_, result_;
    Job job_ = Job::Discover;
    unsigned argument_ = 0;
    std::string session_id_;
    std::string audio_path_;
    std::string workflow_model_id_, workflow_source_name_;
    audio_workflows::MusicRequest music_request_;
    std::string diagnostic_error_;
    prospero::debug::OperationId diagnostic_operation_id_ = 0;
    std::vector<Notice> notices_;
    bool running_ = false, preferences_dirty_ = false;
    std::chrono::steady_clock::time_point last_vulkan_memory_refresh_{};
    std::atomic<bool> done_{false};
    std::atomic_flag stream_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<unsigned> stream_version_{0};
    unsigned displayed_stream_ = 0;
    std::uint64_t model_manager_revision_ = 0;
    std::vector<char> stream_buffer_ = std::vector<char>(prospero_session::MessageBytes);
#ifdef PROSPERO_HOST
    pthread_t thread_{};
#else
    void *thread_ = nullptr;
#endif
    static App *active_;
};
} // namespace prospero
