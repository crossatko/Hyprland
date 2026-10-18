#pragma once

#include <string>

// Debug only: GPU time of the blur passes per blurred element, measured with GL_TIME_ELAPSED_EXT timer queries.
// Enabled by the environment variable HYPRLAND_BLUR_TIMING=1. Results are read back without stalling (a few
// frames late) and printed to stderr every 2 s as averages per label. Disabled, every call is a cheap no-op.
namespace Render::GL::BlurTiming {
    bool enabled();

    // brackets GPU work; queries cannot nest, end() before the next begin()
    void begin(const std::string& label);
    void end();

    // CPU work (ms) reported in the same table
    void addCpu(const std::string& label, float ms);
}
