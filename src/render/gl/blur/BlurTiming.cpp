#include "BlurTiming.hpp"

#include "../../OpenGL.hpp"

#include <EGL/egl.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <print>
#include <vector>

using namespace Render::GL;

struct SPendingQuery {
    GLuint      query = 0;
    std::string label;
};

struct SStat {
    double total = 0.0;
    double worst = 0.0;
    int    count = 0;
};

struct STimingState {
    PFNGLGETQUERYOBJECTUI64VEXTPROC       getResult = nullptr;
    std::vector<GLuint>                   freeQueries;
    std::vector<SPendingQuery>            pending;
    std::map<std::string, SStat>          stats;
    std::chrono::steady_clock::time_point lastPrint = std::chrono::steady_clock::now();
    bool                                  active    = false;
};

static STimingState& state() {
    static STimingState s;
    return s;
}

bool BlurTiming::enabled() {
    static const bool ENABLED = [] {
        const auto ENV = std::getenv("HYPRLAND_BLUR_TIMING");
        return ENV && std::string{ENV} == "1";
    }();
    return ENABLED;
}

static void record(const std::string& label, double ms) {
    auto& stat = state().stats[label];
    stat.total += ms;
    stat.worst = std::max(stat.worst, ms);
    stat.count++;
}

static void collect() {
    auto& s = state();
    std::erase_if(s.pending, [&s](const SPendingQuery& pending) {
        GLuint available = 0;
        glGetQueryObjectuiv(pending.query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            return false;

        GLuint64 nanoseconds = 0;
        s.getResult(pending.query, GL_QUERY_RESULT, &nanoseconds);
        record(pending.label, sc<double>(nanoseconds) / 1000000.0);
        s.freeQueries.push_back(pending.query);
        return true;
    });

    const auto NOW = std::chrono::steady_clock::now();
    if (NOW - s.lastPrint < std::chrono::seconds(2) || s.stats.empty())
        return;

    s.lastPrint = NOW;
    std::println(stderr, "[blur timing] label: avg ms / worst ms / calls (2 s)");
    for (const auto& [label, stat] : s.stats)
        std::println(stderr, "[blur timing]   {}: {:.3f} / {:.3f} / {}", label, stat.total / std::max(stat.count, 1), stat.worst, stat.count);
    s.stats.clear();
}

void BlurTiming::begin(const std::string& label) {
    if (!enabled())
        return;

    auto& s = state();
    if (!s.getResult)
        s.getResult = rc<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    if (!s.getResult || s.active)
        return;

    collect();

    GLuint query = 0;
    if (s.freeQueries.empty())
        glGenQueries(1, &query);
    else {
        query = s.freeQueries.back();
        s.freeQueries.pop_back();
    }

    glBeginQuery(GL_TIME_ELAPSED_EXT, query);
    s.pending.push_back({.query = query, .label = label});
    s.active = true;
}

void BlurTiming::end() {
    if (!enabled() || !state().active)
        return;

    glEndQuery(GL_TIME_ELAPSED_EXT);
    state().active = false;
}

void BlurTiming::addCpu(const std::string& label, float ms) {
    if (!enabled())
        return;

    record(label + " (cpu)", ms);
}
