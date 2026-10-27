#include "SolidGlassOverride.hpp"
#include "Provider.hpp"

#include "../../config/ConfigValue.hpp"
#include "../../debug/log/Logger.hpp"
#include "../../desktop/rule/layerRule/LayerRuleApplicator.hpp"
#include "../../desktop/rule/windowRule/WindowRuleApplicator.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../desktop/view/WLSurface.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../event/EventBus.hpp"

#include <hyprutils/string/Numeric.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_map>

using namespace Render;

static constexpr std::string_view GLASS_RULE_EFFECT = "glass";

// bounds of the decoration:blur:solid_glass:* options (ConfigValues.cpp)
static const std::array<SSolidGlassParamInfo, SOLIDGLASS_PARAM_COUNT> PARAM_INFO = {
    SSolidGlassParamInfo{"refraction", 0.F, 48.F},
    SSolidGlassParamInfo{"bulb", 4.F, 256.F},
    SSolidGlassParamInfo{"clarity", 0.F, 1.F},
    SSolidGlassParamInfo{"aberration", 0.F, 0.25F},
    SSolidGlassParamInfo{"depth", 0.F, 3.F},
    SSolidGlassParamInfo{"edge_curve", 0.2F, 5.F},
    SSolidGlassParamInfo{"light_angle", 0.F, 360.F},
    SSolidGlassParamInfo{"specular_intensity", 0.F, 3.F},
    SSolidGlassParamInfo{"specular_power", 1.F, 200.F},
    SSolidGlassParamInfo{"sheen", 0.F, 1.F},
    SSolidGlassParamInfo{"saturation", 0.F, 3.F},
    SSolidGlassParamInfo{"frost", 0.F, 16.F},
    SSolidGlassParamInfo{"darkening", 0.F, 1.F},
};

const std::array<SSolidGlassParamInfo, SOLIDGLASS_PARAM_COUNT>& Render::solidGlassParamInfo() {
    return PARAM_INFO;
}

bool SSolidGlassOverride::empty() const {
    return !tint && std::ranges::none_of(values, [](const auto& value) { return value.has_value(); });
}

SSolidGlassParams SSolidGlassOverride::applyTo(const SSolidGlassParams& base) const {
    auto result = base;
    for (size_t i = 0; i < SOLIDGLASS_PARAM_COUNT; ++i) {
        if (values[i])
            result.values[i] = *values[i];
    }
    if (tint)
        result.tint = *tint;
    return result;
}

SSolidGlassParams Render::clampSolidGlassParams(SSolidGlassParams params) {
    for (size_t i = 0; i < SOLIDGLASS_PARAM_COUNT; ++i) {
        params.values[i] = std::clamp(params.values[i], PARAM_INFO[i].min, PARAM_INFO[i].max);
    }
    return params;
}

static std::optional<int64_t> parseTint(std::string_view value) {
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) {
        value.remove_prefix(2);
        base = 16;
    }
    if (value.empty() || (base == 16 && value.size() > 8))
        return std::nullopt;

    uint64_t   parsed = 0;
    const auto RES    = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (RES.ec != std::errc{} || RES.ptr != value.data() + value.size() || parsed > 0xFFFFFFFFULL)
        return std::nullopt;
    return sc<int64_t>(parsed);
}

static bool isSeparator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ',' || c == ';';
}

SSolidGlassOverride Render::parseSolidGlassOverride(std::string_view spec) {
    SSolidGlassOverride result;

    size_t              pos = 0;
    while (pos < spec.size()) {
        while (pos < spec.size() && isSeparator(spec[pos])) {
            ++pos;
        }
        size_t end = pos;
        while (end < spec.size() && !isSeparator(spec[end])) {
            ++end;
        }
        if (end == pos)
            break;

        const auto PAIR = spec.substr(pos, end - pos);
        pos             = end;

        const auto SPLIT = PAIR.find_first_of(":=");
        if (SPLIT == std::string_view::npos) {
            result.warnings.emplace_back(std::format("\"{}\" is not key:value", PAIR));
            continue;
        }

        const auto KEY   = PAIR.substr(0, SPLIT);
        const auto VALUE = PAIR.substr(SPLIT + 1);

        if (KEY == "tint") {
            const auto TINT = parseTint(VALUE);
            if (!TINT)
                result.warnings.emplace_back(std::format("tint \"{}\" is not a 0xAARRGGBB color", VALUE));
            else
                result.tint = *TINT;
            continue;
        }

        const auto INFO = std::ranges::find(PARAM_INFO, KEY, &SSolidGlassParamInfo::name);
        if (INFO == PARAM_INFO.end()) {
            result.warnings.emplace_back(std::format("unknown key \"{}\"", KEY));
            continue;
        }

        const auto NUMBER = Hyprutils::String::strToNumber<float>(VALUE);
        if (!NUMBER || !std::isfinite(*NUMBER)) {
            result.warnings.emplace_back(std::format("{} \"{}\" is not a number", KEY, VALUE));
            continue;
        }

        result.values[INFO - PARAM_INFO.begin()] = std::clamp(*NUMBER, INFO->min, INFO->max);
    }

    return result;
}

struct SOverrideCache {
    std::unordered_map<std::string, SP<const SSolidGlassOverride>> bySpec;
    std::vector<SP<const SSolidGlassOverride>>                     parsed; // the non-empty values of bySpec

    void                                                           clear() {
        bySpec.clear();
        parsed.clear();
    }
};

static SOverrideCache& overrideCache() {
    static SOverrideCache cache;
    static auto           reloadListener = Event::bus()->m_events.config.reloaded.listen([] { cache.clear(); });
    return cache;
}

// rule strings come from the config, a handful at most. The cap only guards against a flood of distinct strings
static constexpr size_t       MAX_CACHED_OVERRIDES = 256;

SP<const SSolidGlassOverride> Render::cachedSolidGlassOverride(const std::string& spec) {
    auto& cache = overrideCache();
    if (const auto IT = cache.bySpec.find(spec); IT != cache.bySpec.end())
        return IT->second;

    if (cache.bySpec.size() >= MAX_CACHED_OVERRIDES)
        cache.clear();

    auto parsed = parseSolidGlassOverride(spec);
    for (const auto& warning : parsed.warnings) {
        LOG(Log::WARN, "solid_glass override \"{}\": {}, ignored", spec, warning);
    }

    SP<const SSolidGlassOverride> result = parsed.empty() ? nullptr : makeShared<const SSolidGlassOverride>(std::move(parsed));
    cache.bySpec.emplace(spec, result);
    if (result)
        cache.parsed.emplace_back(result);
    return result;
}

const std::vector<SP<const SSolidGlassOverride>>& Render::cachedSolidGlassOverrides() {
    return overrideCache().parsed;
}

static Desktop::Rule::CLayerRuleEffectContainer::storageType  g_layerGlassEffect  = 0;
static Desktop::Rule::CWindowRuleEffectContainer::storageType g_windowGlassEffect = 0;

void                                                          Render::registerSolidGlassRuleEffects() {
    g_layerGlassEffect  = Desktop::Rule::layerEffects()->registerEffect(std::string{GLASS_RULE_EFFECT});
    g_windowGlassEffect = Desktop::Rule::windowEffects()->registerEffect(std::string{GLASS_RULE_EFFECT});
}

std::optional<std::string> Render::solidGlassLayerRule(PHLLS layer) {
    if (!layer || !layer->m_ruleApplicator || !g_layerGlassEffect)
        return std::nullopt;

    const auto& PROPS = layer->m_ruleApplicator->m_otherProps.props;
    const auto  IT    = PROPS.find(g_layerGlassEffect);
    if (IT == PROPS.end() || !IT->second)
        return std::nullopt;
    return IT->second->effect;
}

std::optional<std::string> Render::solidGlassWindowRule(PHLWINDOW window) {
    if (!window || !window->m_ruleApplicator || !g_windowGlassEffect)
        return std::nullopt;

    const auto& PROPS = window->m_ruleApplicator->m_otherProps.props;
    const auto  IT    = PROPS.find(g_windowGlassEffect);
    if (IT == PROPS.end() || !IT->second)
        return std::nullopt;
    return IT->second->effect;
}

SP<const SSolidGlassOverride> Render::solidGlassOverrideFor(const SBlurContext& context) {
    static auto PPERSURFACE = CConfigValue<Config::BOOL>("decoration:blur:solid_glass:per_surface");
    if (!*PPERSURFACE)
        return nullptr;

    std::optional<std::string> spec;
    if (context.shape) {
        if (const auto SURFACE = context.shape->surface.lock(); SURFACE)
            spec = solidGlassLayerRule(Desktop::View::CLayerSurface::fromView(SURFACE->view()));
    }
    if (!spec)
        spec = solidGlassWindowRule(context.owner.lock());

    return spec ? cachedSolidGlassOverride(*spec) : nullptr;
}
