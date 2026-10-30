// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace prospero::native_audio_queue
{
enum class DispatchDecision
{
    wait,
    stopped,
    start
};

inline DispatchDecision dispatch_decision(bool &active, bool &stop_requested, int active_item,
                                          bool worker_busy)
{
    if (!active || active_item >= 0 || worker_busy)
        return DispatchDecision::wait;
    if (stop_requested)
    {
        active = false;
        stop_requested = false;
        return DispatchDecision::stopped;
    }
    return DispatchDecision::start;
}
} // namespace prospero::native_audio_queue
