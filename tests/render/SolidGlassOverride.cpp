#include <config/values/ConfigValues.hpp>
#include <desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <render/blur/Provider.hpp>
#include <render/blur/SolidGlassOverride.hpp>
#include <render/pass/RectPassElement.hpp>
#include <render/pass/TexPassElement.hpp>

#include <gtest/gtest.h>

#include <ranges>
#include <string_view>

using namespace Render;

static SSolidGlassParams globals() {
    SSolidGlassParams params;
    for (size_t i = 0; i < SOLIDGLASS_PARAM_COUNT; ++i) {
        params.values[i] = solidGlassParamInfo()[i].min;
    }
    params.values[SOLIDGLASS_REFRACTION] = 28.F;
    params.values[SOLIDGLASS_DEPTH]      = 1.F;
    params.tint                          = 0x14EEF5FF;
    return params;
}

TEST(SolidGlassOverride, parsesKeyValuePairs) {
    const auto OVERRIDE = parseSolidGlassOverride("refraction:12 depth=0.5, tint:0x20FFFFFF;frost:0");

    EXPECT_TRUE(OVERRIDE.warnings.empty());
    ASSERT_TRUE(OVERRIDE.values[SOLIDGLASS_REFRACTION].has_value());
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_REFRACTION], 12.F);
    ASSERT_TRUE(OVERRIDE.values[SOLIDGLASS_DEPTH].has_value());
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_DEPTH], 0.5F);
    ASSERT_TRUE(OVERRIDE.values[SOLIDGLASS_FROST].has_value());
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_FROST], 0.F);
    ASSERT_TRUE(OVERRIDE.tint.has_value());
    EXPECT_EQ(*OVERRIDE.tint, 0x20FFFFFF);
    EXPECT_FALSE(OVERRIDE.values[SOLIDGLASS_BULB].has_value());
}

TEST(SolidGlassOverride, everyOptionNameIsAKey) {
    for (const auto& info : solidGlassParamInfo()) {
        const auto OVERRIDE = parseSolidGlassOverride(std::format("{}:{}", info.name, info.max));
        EXPECT_TRUE(OVERRIDE.warnings.empty()) << info.name;
        EXPECT_FALSE(OVERRIDE.empty()) << info.name;

        // the bounds match the global option
        const auto VALUE = std::ranges::find_if(Config::Values::CONFIG_VALUES,
                                                [&info](const auto& value) { return std::string_view{value->name()} == std::format("decoration:blur:solid_glass:{}", info.name); });
        ASSERT_NE(VALUE, Config::Values::CONFIG_VALUES.end()) << info.name;
        const auto FLOAT_VALUE = dynamicPointerCast<Config::Values::CFloatValue>(*VALUE);
        ASSERT_TRUE(FLOAT_VALUE) << info.name;
        EXPECT_FLOAT_EQ(*FLOAT_VALUE->m_min, info.min) << info.name;
        EXPECT_FLOAT_EQ(*FLOAT_VALUE->m_max, info.max) << info.name;
    }
}

TEST(SolidGlassOverride, clampsOutOfRangeValues) {
    const auto OVERRIDE = parseSolidGlassOverride("refraction:500 bulb:1 depth:-2");

    EXPECT_TRUE(OVERRIDE.warnings.empty());
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_REFRACTION], 48.F);
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_BULB], 4.F);
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_DEPTH], 0.F);

    auto params                          = globals();
    params.values[SOLIDGLASS_SATURATION] = 9.F;
    EXPECT_FLOAT_EQ(clampSolidGlassParams(params).values[SOLIDGLASS_SATURATION], 3.F);
}

TEST(SolidGlassOverride, skipsUnknownAndInvalidWithWarnings) {
    const auto OVERRIDE = parseSolidGlassOverride("wobble:3 depth:abc tint:0xZZ refraction lens:1 sheen:0.5");

    EXPECT_EQ(OVERRIDE.warnings.size(), 5);
    EXPECT_FALSE(OVERRIDE.values[SOLIDGLASS_DEPTH].has_value());
    EXPECT_FALSE(OVERRIDE.values[SOLIDGLASS_REFRACTION].has_value());
    EXPECT_FALSE(OVERRIDE.tint.has_value());
    ASSERT_TRUE(OVERRIDE.values[SOLIDGLASS_SHEEN].has_value());
    EXPECT_FLOAT_EQ(*OVERRIDE.values[SOLIDGLASS_SHEEN], 0.5F);
}

TEST(SolidGlassOverride, emptySpecIsEmpty) {
    EXPECT_TRUE(parseSolidGlassOverride("").empty());
    EXPECT_TRUE(parseSolidGlassOverride("  ,; ").empty());
    EXPECT_TRUE(parseSolidGlassOverride("nonsense:1").empty());
    EXPECT_EQ(cachedSolidGlassOverride("nonsense:1"), nullptr);
}

TEST(SolidGlassOverride, overrideWinsUnsetKeysFallBack) {
    const auto BASE     = globals();
    const auto OVERRIDE = parseSolidGlassOverride("depth:0.25 tint:0x00000000");
    const auto RESULT   = OVERRIDE.applyTo(BASE);

    EXPECT_FLOAT_EQ(RESULT.values[SOLIDGLASS_DEPTH], 0.25F);
    EXPECT_EQ(RESULT.tint, 0);
    for (size_t i = 0; i < SOLIDGLASS_PARAM_COUNT; ++i) {
        if (i == SOLIDGLASS_DEPTH)
            continue;
        EXPECT_FLOAT_EQ(RESULT.values[i], BASE.values[i]) << solidGlassParamInfo()[i].name;
    }

    // no override = the globals unchanged
    const auto NONE = parseSolidGlassOverride("").applyTo(BASE);
    EXPECT_EQ(NONE.values, BASE.values);
    EXPECT_EQ(NONE.tint, BASE.tint);
}

TEST(SolidGlassOverride, cacheParsesOncePerString) {
    const auto A = cachedSolidGlassOverride("depth:0.3");
    const auto B = cachedSolidGlassOverride("depth:0.3");
    ASSERT_TRUE(A);
    EXPECT_EQ(A, B);
    EXPECT_TRUE(std::ranges::find(cachedSolidGlassOverrides(), A) != cachedSolidGlassOverrides().end());
}

TEST(SolidGlassOverride, ruleEffectsAreRegistered) {
    registerSolidGlassRuleEffects();
    const auto LAYER  = Desktop::Rule::layerEffects()->get("glass");
    const auto WINDOW = Desktop::Rule::windowEffects()->get("glass");
    ASSERT_TRUE(LAYER.has_value());
    ASSERT_TRUE(WINDOW.has_value());
    EXPECT_TRUE(Desktop::Rule::layerEffects()->isEffectDynamic(*LAYER));
    EXPECT_TRUE(Desktop::Rule::windowEffects()->isEffectDynamic(*WINDOW));

    // registering twice keeps one effect
    registerSolidGlassRuleEffects();
    EXPECT_EQ(Desktop::Rule::layerEffects()->get("glass"), LAYER);
}

TEST(SolidGlassOverride, perSurfaceProbeOption) {
    const auto VALUE =
        std::ranges::find_if(Config::Values::CONFIG_VALUES, [](const auto& value) { return std::string_view{value->name()} == "decoration:blur:solid_glass:per_surface"; });
    ASSERT_NE(VALUE, Config::Values::CONFIG_VALUES.end());
    const auto BOOL_VALUE = dynamicPointerCast<Config::Values::CBoolValue>(*VALUE);
    ASSERT_TRUE(BOOL_VALUE);
    EXPECT_TRUE(BOOL_VALUE->defaultVal());
}

TEST(SolidGlassOverride, groupbarOptionAndRole) {
    const auto VALUE = std::ranges::find_if(Config::Values::CONFIG_VALUES, [](const auto& value) { return std::string_view{value->name()} == "group:groupbar:solid_glass"; });
    ASSERT_NE(VALUE, Config::Values::CONFIG_VALUES.end());
    const auto STRING_VALUE = dynamicPointerCast<Config::Values::CStringValue>(*VALUE);
    ASSERT_TRUE(STRING_VALUE);
    EXPECT_EQ(STRING_VALUE->defaultVal(), STRVAL_EMPTY);
    EXPECT_EQ(STRING_VALUE->refreshBits(), Config::Supplementary::REFRESH_BLUR_FB);

    // blurs are role-less unless a drawer says otherwise
    EXPECT_EQ(SBlurShape{}.role, BLUR_ROLE_NONE);
    EXPECT_EQ(CRectPassElement::SRectData{}.blurRole, BLUR_ROLE_NONE);
    EXPECT_EQ(CTexPassElement::SRenderData{}.blurRole, BLUR_ROLE_NONE);
}

TEST(SolidGlassOverride, borderGlassOptionAndRule) {
    registerSolidGlassRuleEffects();
    const auto EFFECT = Desktop::Rule::windowEffects()->get("glass_border");
    ASSERT_TRUE(EFFECT.has_value());
    EXPECT_TRUE(Desktop::Rule::windowEffects()->isEffectDynamic(*EFFECT));
    EXPECT_FALSE(Desktop::Rule::layerEffects()->get("glass_border").has_value());

    const auto VALUE =
        std::ranges::find_if(Config::Values::CONFIG_VALUES, [](const auto& value) { return std::string_view{value->name()} == "decoration:blur:solid_glass:border"; });
    ASSERT_NE(VALUE, Config::Values::CONFIG_VALUES.end());
    const auto STRING_VALUE = dynamicPointerCast<Config::Values::CStringValue>(*VALUE);
    ASSERT_TRUE(STRING_VALUE);
    EXPECT_EQ(STRING_VALUE->defaultVal(), STRVAL_EMPTY);
    EXPECT_FALSE(CRectPassElement::SRectData{}.blurHole.has_value());
}
