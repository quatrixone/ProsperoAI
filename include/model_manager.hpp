// Shared model-selection coordinator for the native app and HTTP interfaces.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gpt_runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace prospero::model_manager
{
enum class Stage : std::uint8_t
{
    none,
    selected,
    preparing,
    ready,
    failed
};

struct Snapshot
{
    std::string model_id;
    std::string error;
    Stage stage = Stage::none;
    std::uint64_t revision = 0;
};

inline std::mutex &runtime_mutex()
{
    static std::mutex value;
    return value;
}

inline std::mutex &state_mutex()
{
    static std::mutex value;
    return value;
}

inline Snapshot &shared_state()
{
    static Snapshot value;
    return value;
}

inline Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(state_mutex());
    return shared_state();
}

inline void publish(const char *model_id, Stage stage, const char *error = nullptr)
{
    std::lock_guard<std::mutex> lock(state_mutex());
    auto &state = shared_state();
    state.model_id = model_id ? model_id : "";
    state.stage = stage;
    state.error = error ? error : "";
    ++state.revision;
}

// `try_only` lets HTTP callers receive a busy response instead of waiting behind
// generation. The native worker uses the blocking path after its own busy check.
inline bool select(const char *id, bool prepare, bool try_only = false,
                   std::string *error = nullptr)
{
    std::unique_lock<std::mutex> runtime_lock(runtime_mutex(), std::defer_lock);
    if (try_only)
    {
        if (!runtime_lock.try_lock())
        {
            if (error)
                *error = "The runtime is busy. Try again when the current operation finishes.";
            return false;
        }
    }
    else
        runtime_lock.lock();

    if (!id || !*id)
    {
        if (error)
            *error = "A model ID is required.";
        return false;
    }

    unsigned index = gpt_runtime_model_count();
    for (unsigned i = 0; i < gpt_runtime_model_count(); ++i)
    {
        const char *candidate = gpt_runtime_model_id(i);
        if (candidate && std::string(candidate) == id)
        {
            index = i;
            break;
        }
    }
    if (index == gpt_runtime_model_count())
    {
        if (error)
            *error = "The model is not installed.";
        return false;
    }

    publish(id, prepare ? Stage::preparing : Stage::selected);
    if (!gpt_runtime_select_model(index))
    {
        const char *reason = "The model cannot be selected while the runtime is busy.";
        publish(id, Stage::failed, reason);
        if (error)
            *error = reason;
        return false;
    }
    if (prepare && gpt_runtime_prepare() != 0)
    {
        const char *reason = "The selected model could not be prepared.";
        publish(id, Stage::failed, reason);
        if (error)
            *error = reason;
        return false;
    }
    publish(id, prepare ? Stage::ready : Stage::selected);
    if (error)
        error->clear();
    return true;
}
} // namespace prospero::model_manager
