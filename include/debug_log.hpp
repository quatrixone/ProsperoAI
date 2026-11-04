// ProsperoAI - The debug log a user can switch on and send to us.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace prospero::debug
{
// Settings > Diagnostics > Debug log. Off by default; while it is on, the app writes
// a timed trace of what it does to debug-trace.txt in its logs folder (the trace of
// the run before is kept as debug-trace.prev.txt). Every line carries the seconds
// since the app started and a tag:
//   app      start-up, version, storage and filesystem access
//   model    the models found, the one chosen, how its preparation went
//   answer   each request to a model with its result and figures
//   notice   every message the app showed
//   screen   the page opened;  setting  a setting changed
//   download what the model downloader did;  http  requests to the built-in server
//   sys      what the model runtimes themselves report
//   trace    the log's own events
// The switch is a file beside the logs folder, so it holds before any setting is read.

// Reads the switch and, when it is on, opens the trace. `folder` is the logs folder.
void start(const char *folder);
bool enabled();
// False when the switch could not be saved.
bool set_enabled(bool on);
void line(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
// A line the app or a runtime printed for the console's own log.
void system_line(const char *text);
// Persistent bounded journal. Arguments must be identifiers/codes, never user content.
using OperationId = std::uint64_t;
OperationId begin_operation(const char *operation, const char *model_id);
void event(const char *operation, const char *model_id, const char *error_code,
           OperationId operation_id = 0);
// Runtime snapshots contain measurements and settings only, never user content.
void model_memory_sample(const char *model_id, unsigned context_tokens, unsigned output_limit,
                         bool thinking, std::uint64_t vulkan_heap_bytes,
                         std::uint64_t flexible_free_bytes);
struct MemorySummary
{
    unsigned samples = 0;
    std::uint64_t largest_vulkan_heap_bytes = 0;
    std::uint64_t least_flexible_free_bytes = 0;
};
MemorySummary model_memory_summary(const char *model_id, unsigned context_tokens,
                                   unsigned output_limit, bool thinking);
// The logs folder, as the app names it.
const char *folder();
} // namespace prospero::debug
