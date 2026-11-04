// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace prospero_stems
{
constexpr std::uint32_t kMinInputSampleRate = 8000;
constexpr std::uint32_t kMaxInputSampleRate = 192000;
constexpr std::size_t kTargetSampleRate = 44100;
constexpr std::size_t kMaxDurationSeconds = 300;
constexpr std::size_t kMaxOutputFrames = kTargetSampleRate * kMaxDurationSeconds;

inline bool output_frame_count(std::size_t input_frames, std::uint32_t sample_rate,
                               std::size_t &output_frames)
{
    if (input_frames == 0 || sample_rate < kMinInputSampleRate ||
        sample_rate > kMaxInputSampleRate ||
        input_frames > static_cast<std::size_t>(sample_rate) * kMaxDurationSeconds)
        return false;

    const long double scaled =
        static_cast<long double>(input_frames) * kTargetSampleRate / sample_rate;
    if (scaled > kMaxOutputFrames)
        return false;
    output_frames = std::max<std::size_t>(1, static_cast<std::size_t>(scaled));
    return true;
}
} // namespace prospero_stems
