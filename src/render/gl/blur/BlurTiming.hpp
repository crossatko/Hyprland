#pragma once

#include <cstdint>
#include <functional>
#include <string>

// GPU timing of frames and blur work, measured with GL_TIMESTAMP_EXT queries (read back without stalling, a few
// frames late).
//
// Debug switches (environment, read once):
//   HYPRLAND_BLUR_TIMING=1          per-label averages of the blur passes / finish printed to stderr every 2 s
//   HYPRLAND_GLASS_PROFILE=<file>   one CSV line per rendered frame (GPU frame time, blur passes / finish time, blurred
//                                   pixels, damage) plus one line per presentation event, for offline analysis
//
// Frame timing (frameBegin / frameEnd) is also used at runtime by the solid_glass quality controller (quality = auto):
// then only two timestamp queries per frame are issued and no per-blur sections. Disabled, every call is a no-op.
namespace Render::GL::BlurTiming {
    // per-blur sections (debug only)
    bool enabled();
    // brackets GPU work. Sections must not overlap
    void begin(const std::string& label);
    void end();
    // CPU work (ms) reported in the same table
    void addCpu(const std::string& label, float ms);
    // pixels the blur passes touched in this frame (full resolution equivalent) and blur calls
    void addBlurWork(int64_t fullResPixels);

    // frame level: around the render pass of a normal monitor frame
    bool frameTimingWanted();
    void setFrameTimingWanted(bool wanted); // runtime consumer (quality = auto)
    void frameBegin(int64_t monitorID, int64_t damagePixels);
    void frameEnd();
    // presentation feedback (profile only)
    void present(int64_t monitorID, uint64_t presentNs, bool presented, uint32_t refreshNs);

    // called for every completed frame with its GPU time, a few frames after it was rendered
    using FrameCallback = std::function<void(int64_t monitorID, float gpuMs)>;
    void setFrameCallback(FrameCallback cb);
}
