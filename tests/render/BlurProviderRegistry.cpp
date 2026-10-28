#include <render/blur/Registry.hpp>

#include <gtest/gtest.h>

using namespace Render;

class CTestProvider : public IBlurProvider {
  public:
    eBlurType type() const noexcept override {
        return eBlurType::BLUR_PLUGIN;
    }
    bool isAnimated(CRenderContext&) const noexcept override {
        return false;
    }
    bool requiresLiveBlur() const noexcept override {
        return false;
    }
    void expandDamage(CRegion&, float) const override {
        ;
    }
    SP<IFramebuffer> blur(CRenderContext&, SP<IFramebuffer>, float, const CRegion&, const SBlurContext&) override {
        return nullptr;
    }
};

static BlurProviderFactory testFactory() {
    return [] { return makeUnique<CTestProvider>(); };
}

TEST(BlurProviderRegistry, registerAndCreate) {
    CBlurProviderRegistry reg;
    int                   a = 0;

    EXPECT_FALSE(reg.has("tint"));
    EXPECT_EQ(reg.create("tint"), nullptr);

    EXPECT_TRUE(reg.registerProvider(&a, "tint", testFactory()));
    EXPECT_TRUE(reg.has("tint"));

    const auto PROVIDER = reg.create("tint");
    ASSERT_NE(PROVIDER, nullptr);
    EXPECT_EQ(PROVIDER->type(), eBlurType::BLUR_PLUGIN);
}

TEST(BlurProviderRegistry, rejectsInvalidAndDuplicates) {
    CBlurProviderRegistry reg;
    int                   a = 0, b = 0;

    EXPECT_FALSE(reg.registerProvider(&a, "", testFactory()));
    EXPECT_FALSE(reg.registerProvider(&a, "null", BlurProviderFactory{}));
    EXPECT_TRUE(reg.registerProvider(&a, "tint", testFactory()));
    EXPECT_FALSE(reg.registerProvider(&b, "tint", testFactory()));
}

TEST(BlurProviderRegistry, unregisterChecksOwner) {
    CBlurProviderRegistry reg;
    int                   a = 0, b = 0;

    EXPECT_TRUE(reg.registerProvider(&a, "tint", testFactory()));
    EXPECT_FALSE(reg.unregisterProvider(&b, "tint"));
    EXPECT_TRUE(reg.has("tint"));
    EXPECT_TRUE(reg.unregisterProvider(&a, "tint"));
    EXPECT_FALSE(reg.has("tint"));
    EXPECT_FALSE(reg.unregisterProvider(&a, "tint"));
}

TEST(BlurProviderRegistry, unregisterAllOnlyRemovesOwnedProviders) {
    CBlurProviderRegistry reg;
    int                   a = 0, b = 0;

    EXPECT_TRUE(reg.registerProvider(&a, "one", testFactory()));
    EXPECT_TRUE(reg.registerProvider(&a, "two", testFactory()));
    EXPECT_TRUE(reg.registerProvider(&b, "other", testFactory()));

    EXPECT_TRUE(reg.unregisterAll(&a));
    EXPECT_FALSE(reg.has("one"));
    EXPECT_FALSE(reg.has("two"));
    EXPECT_TRUE(reg.has("other"));
    EXPECT_FALSE(reg.unregisterAll(&a));
}
