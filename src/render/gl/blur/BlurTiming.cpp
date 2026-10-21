#include "BlurTiming.hpp"

#include "../../OpenGL.hpp"

#include <EGL/egl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <print>
#include <vector>

using namespace Render::GL;

namespace {
    struct SSection {
        GLuint      q0 = 0, q1 = 0;
        std::string label;
        int         kind = 2; // 0: blur passes, 1: blur finish, 2: other (summary only)
    };

    struct SFrame {
        bool                  open    = false;
        int64_t               monitor = -1;
        GLuint                q0 = 0, q1 = 0;
        std::vector<SSection> sections;
        int64_t               blurPx = 0, blurCalls = 0, damagePx = 0;
        uint64_t              cpuStartNs = 0, cpuEndNs = 0;
    };

    struct SStat {
        double total = 0.0;
        double worst = 0.0;
        int    count = 0;
    };

    struct STimingState {
        PFNGLQUERYCOUNTEREXTPROC              queryCounter = nullptr;
        PFNGLGETQUERYOBJECTUI64VEXTPROC       getResult    = nullptr;
        bool                                  procsLoaded  = false;
        std::vector<GLuint>                   freeQueries;
        std::deque<SFrame>                    pendingFrames;
        std::deque<SSection>                  looseSections; // sections outside a timed frame
        SFrame                                current;
        std::vector<SSection>                 openSections; // timestamps nest
        bool                                  runtimeWanted = false;
        BlurTiming::FrameCallback             callback;
        std::map<std::string, SStat>          stats;
        std::chrono::steady_clock::time_point lastPrint     = std::chrono::steady_clock::now();
        FILE*                                 profile       = nullptr;
        bool                                  profileOpened = false;
    };
}

static STimingState& state() {
    static STimingState s;
    return s;
}

static uint64_t nowNs() {
    return sc<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool BlurTiming::enabled() {
    static const bool ENABLED = [] {
        const auto ENV = std::getenv("HYPRLAND_BLUR_TIMING");
        if (ENV && std::string{ENV} == "1")
            return true;
        const auto PROFILE = std::getenv("HYPRLAND_GLASS_PROFILE");
        return PROFILE && *PROFILE;
    }();
    return ENABLED;
}

static bool summaryEnabled() {
    static const bool ENABLED = [] {
        const auto ENV = std::getenv("HYPRLAND_BLUR_TIMING");
        return ENV && std::string{ENV} == "1";
    }();
    return ENABLED;
}

static FILE* profileFile() {
    auto& s = state();
    if (!s.profileOpened) {
        s.profileOpened    = true;
        const auto PROFILE = std::getenv("HYPRLAND_GLASS_PROFILE");
        if (PROFILE && *PROFILE) {
            s.profile = std::fopen(PROFILE, "w");
            if (s.profile) {
                std::fputs("# F,cpu_end_ns,monitor,cpu_us,gpu_us,passes_us,finish_us,blur_calls,blur_px,damage_px\n", s.profile);
                std::fputs("# P,cpu_ns,monitor,present_ns,presented,refresh_ns\n", s.profile);
            }
        }
    }
    return s.profile;
}

static bool loadProcs() {
    auto& s = state();
    if (!s.procsLoaded) {
        s.procsLoaded  = true;
        s.queryCounter = rc<PFNGLQUERYCOUNTEREXTPROC>(eglGetProcAddress("glQueryCounterEXT"));
        s.getResult    = rc<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    }
    return s.queryCounter && s.getResult;
}

static GLuint stamp() {
    auto&  s     = state();
    GLuint query = 0;
    if (s.freeQueries.empty())
        glGenQueries(1, &query);
    else {
        query = s.freeQueries.back();
        s.freeQueries.pop_back();
    }
    s.queryCounter(query, GL_TIMESTAMP_EXT);
    return query;
}

static bool available(GLuint q) {
    GLuint avail = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
    return avail;
}

static uint64_t result(GLuint q) {
    auto&    s  = state();
    GLuint64 ns = 0;
    s.getResult(q, GL_QUERY_RESULT, &ns);
    s.freeQueries.push_back(q);
    return ns;
}

static void record(const std::string& label, double ms) {
    if (!summaryEnabled())
        return;
    auto& stat = state().stats[label];
    stat.total += ms;
    stat.worst = std::max(stat.worst, ms);
    stat.count++;
}

static void collect() {
    auto& s = state();

    while (!s.looseSections.empty() && available(s.looseSections.front().q1)) {
        auto&      sec = s.looseSections.front();
        const auto T0  = result(sec.q0);
        const auto T1  = result(sec.q1);
        record(sec.label, (T1 > T0 ? T1 - T0 : 0) / 1000000.0);
        s.looseSections.pop_front();
    }

    // frames complete in order: the end stamp of a frame is issued after all of its sections
    while (!s.pendingFrames.empty() && available(s.pendingFrames.front().q1)) {
        auto&      frame  = s.pendingFrames.front();
        const auto T0     = result(frame.q0);
        const auto T1     = result(frame.q1);
        const auto GPU    = T1 > T0 ? T1 - T0 : 0;
        uint64_t   passes = 0;
        uint64_t   finish = 0;
        for (auto& sec : frame.sections) {
            const auto S0 = result(sec.q0);
            const auto S1 = result(sec.q1);
            const auto D  = S1 > S0 ? S1 - S0 : 0;
            if (sec.kind == 0)
                passes += D;
            else if (sec.kind == 1)
                finish += D;
            record(sec.label, D / 1000000.0);
        }
        record("frame gpu", GPU / 1000000.0);
        record("frame cpu", (frame.cpuEndNs - frame.cpuStartNs) / 1000000.0);

        if (auto* f = profileFile())
            std::fputs(std::format("F,{},{},{:.1f},{:.1f},{:.1f},{:.1f},{},{},{}\n", frame.cpuEndNs, frame.monitor, (frame.cpuEndNs - frame.cpuStartNs) / 1000.0, GPU / 1000.0,
                                   passes / 1000.0, finish / 1000.0, frame.blurCalls, frame.blurPx, frame.damagePx)
                           .c_str(),
                       f);

        if (s.callback)
            s.callback(frame.monitor, GPU / 1000000.F);

        s.pendingFrames.pop_front();
    }

    // a GPU that never reports would grow this forever
    while (s.pendingFrames.size() > 64) {
        for (auto& sec : s.pendingFrames.front().sections) {
            s.freeQueries.push_back(sec.q0);
            s.freeQueries.push_back(sec.q1);
        }
        s.freeQueries.push_back(s.pendingFrames.front().q0);
        s.freeQueries.push_back(s.pendingFrames.front().q1);
        s.pendingFrames.pop_front();
    }

    const auto NOW = std::chrono::steady_clock::now();
    if (NOW - s.lastPrint < std::chrono::seconds(2))
        return;
    s.lastPrint = NOW;

    if (s.profile)
        std::fflush(s.profile);

    if (!summaryEnabled() || s.stats.empty())
        return;

    std::println(stderr, "[blur timing] label: avg ms / worst ms / calls (2 s)");
    for (const auto& [label, stat] : s.stats)
        std::println(stderr, "[blur timing]   {}: {:.3f} / {:.3f} / {}", label, stat.total / std::max(stat.count, 1), stat.worst, stat.count);
    s.stats.clear();
}

void BlurTiming::begin(const std::string& label) {
    if (!enabled() || !loadProcs())
        return;
    auto& s = state();
    if (s.openSections.size() > 16)
        return;
    s.openSections.emplace_back(SSection{.q0 = stamp(), .label = label, .kind = label.ends_with(" passes") ? 0 : label.ends_with(" finish") ? 1 : 2});
}

void BlurTiming::end() {
    auto& s = state();
    if (!enabled() || s.openSections.empty())
        return;
    auto section = std::move(s.openSections.back());
    s.openSections.pop_back();
    section.q1 = stamp();
    if (s.current.open)
        s.current.sections.emplace_back(std::move(section));
    else
        s.looseSections.emplace_back(std::move(section));
}

void BlurTiming::addCpu(const std::string& label, float ms) {
    if (!enabled())
        return;
    record(label + " (cpu)", ms);
}

void BlurTiming::addBlurWork(int64_t fullResPixels) {
    auto& s = state();
    if (!s.current.open)
        return;
    s.current.blurPx += fullResPixels;
    s.current.blurCalls++;
}

bool BlurTiming::frameTimingWanted() {
    return enabled() || state().runtimeWanted;
}

void BlurTiming::setFrameTimingWanted(bool wanted) {
    state().runtimeWanted = wanted;
}

void BlurTiming::frameBegin(int64_t monitorID, int64_t damagePixels) {
    if (!frameTimingWanted() || !loadProcs())
        return;
    auto& s = state();
    collect();
    if (s.current.open)
        return;
    s.current            = SFrame{.open = true, .monitor = monitorID, .damagePx = damagePixels};
    s.current.cpuStartNs = nowNs();
    s.current.q0         = stamp();
}

void BlurTiming::frameEnd() {
    auto& s = state();
    if (!s.current.open)
        return;
    while (!s.openSections.empty())
        end();
    s.current.q1       = stamp();
    s.current.cpuEndNs = nowNs();
    s.current.open     = false;
    s.pendingFrames.emplace_back(std::move(s.current));
    s.current = {};
}

void BlurTiming::present(int64_t monitorID, uint64_t presentNs, bool presented, uint32_t refreshNs) {
    if (!enabled())
        return;
    if (auto* f = profileFile())
        std::fputs(std::format("P,{},{},{},{},{}\n", nowNs(), monitorID, presentNs, presented ? 1 : 0, refreshNs).c_str(), f);
}

void BlurTiming::setFrameCallback(FrameCallback cb) {
    state().callback = std::move(cb);
}
