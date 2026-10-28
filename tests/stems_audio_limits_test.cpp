// SPDX-License-Identifier: GPL-3.0-or-later
#include "stems_audio_limits.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

int main()
{
    std::size_t output_frames = 0;
    assert(prospero_stems::output_frame_count(44100, 44100, output_frames));
    assert(output_frames == 44100);
    assert(prospero_stems::output_frame_count(48000, 48000, output_frames));
    assert(output_frames == 44100);
    assert(prospero_stems::output_frame_count(192000u * 300u, 192000, output_frames));
    assert(output_frames == prospero_stems::kMaxOutputFrames);
    assert(!prospero_stems::output_frame_count(86400, 1, output_frames));
    assert(!prospero_stems::output_frame_count(8000u * 301u, 8000, output_frames));
    assert(!prospero_stems::output_frame_count(44100, 7999, output_frames));
}
