#pragma once

#include "../../desktop/DesktopTypes.hpp"
#include "../../helpers/memory/Memory.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Render {
    struct SBlurContext;

    // the decoration:blur:solid_glass:* values a surface can override, by option name suffix
    enum eSolidGlassParam : uint8_t {
        SOLIDGLASS_REFRACTION = 0,
        SOLIDGLASS_BULB,
        SOLIDGLASS_CLARITY,
        SOLIDGLASS_ABERRATION,
        SOLIDGLASS_DEPTH,
        SOLIDGLASS_EDGE_CURVE,
        SOLIDGLASS_LIGHT_ANGLE,
        SOLIDGLASS_SPECULAR_INTENSITY,
        SOLIDGLASS_SPECULAR_POWER,
        SOLIDGLASS_SHEEN,
        SOLIDGLASS_SATURATION,
        SOLIDGLASS_FROST,
        SOLIDGLASS_DARKENING,

        SOLIDGLASS_PARAM_COUNT,
    };

    struct SSolidGlassParamInfo {
        std::string_view name;
        float            min = 0.F;
        float            max = 0.F;
    };

    // name and bounds of every float parameter, same as the global options
    const std::array<SSolidGlassParamInfo, SOLIDGLASS_PARAM_COUNT>& solidGlassParamInfo();

    struct SSolidGlassParams {
        std::array<float, SOLIDGLASS_PARAM_COUNT> values = {};
        int64_t                                   tint   = 0; // 0xAARRGGBB
    };

    // A per-surface override: "key:value key:value ...". Keys are the option names without the
    // decoration:blur:solid_glass: prefix (tint takes 0xAARRGGBB), pairs are separated by spaces, commas or
    // semicolons, ':' or '=' separates key and value. Unknown keys and unparsable values are skipped with a
    // warning, out of range values are clamped. Unset keys fall back to the global options
    struct SSolidGlassOverride {
        std::array<std::optional<float>, SOLIDGLASS_PARAM_COUNT> values;
        std::optional<int64_t>                                   tint;
        std::vector<std::string>                                 warnings;

        bool                                                     empty() const;
        SSolidGlassParams                                        applyTo(const SSolidGlassParams& base) const;
    };

    SSolidGlassOverride parseSolidGlassOverride(std::string_view spec);
    SSolidGlassParams   clampSolidGlassParams(SSolidGlassParams params);

    // parsed once per distinct string (warnings are logged then), dropped on config reload. nullptr for an
    // empty / all-invalid spec
    SP<const SSolidGlassOverride> cachedSolidGlassOverride(const std::string& spec);
    // every override parsed since the last config reload: the blur reach and the kawase passes must cover them
    const std::vector<SP<const SSolidGlassOverride>>& cachedSolidGlassOverrides();

    // the "glass" layer and window rule effects (dynamic, as plugins register theirs). Call before the config loads
    void registerSolidGlassRuleEffects();
    // the "glass" rule string of the layer / window, if a rule set one
    std::optional<std::string> solidGlassLayerRule(PHLLS layer);
    std::optional<std::string> solidGlassWindowRule(PHLWINDOW window);
    // window-frame glass (decoration:blur:solid_glass:border or the glass_border window rule): whether the border
    // decoration of this window draws glass under its ring
    bool solidGlassBorderWanted(PHLWINDOW window);

    // the override a blur draw uses: group:groupbar:solid_glass for groupbar tabs, else the layer rule of its surface,
    // else the window rule of its owner, else none
    SP<const SSolidGlassOverride> solidGlassOverrideFor(const SBlurContext& context);
}
