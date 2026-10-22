#include <render/blur/SolidGlassField.hpp>

#include <hyprutils/memory/Casts.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace Render;
using namespace Hyprutils::Memory;

static std::vector<uint8_t> makeMask(int width, int height) {
    return std::vector<uint8_t>(sc<size_t>(width) * height, 0);
}

static void fillRect(std::vector<uint8_t>& mask, int width, int x0, int y0, int x1, int y1, uint8_t value) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            mask[(sc<size_t>(y) * width) + x] = value;
        }
    }
}

TEST(SolidGlassField, emptyMaskHasNoGlass) {
    const auto MASK  = makeMask(16, 8);
    const auto FIELD = buildSolidGlassField(MASK, 16, 8, 48.F);
    ASSERT_EQ(FIELD.pixels.size(), 16U * 8U * 4U);
    EXPECT_EQ(sampleSolidGlassField(FIELD, 4, 4).bulb, 0.F);
}

TEST(SolidGlassField, invalidInputGivesEmptyField) {
    const auto FIELD = buildSolidGlassField(makeMask(4, 4), 5, 4, 48.F);
    EXPECT_TRUE(FIELD.pixels.empty());
}

TEST(SolidGlassField, rectangleDepthAndNormals) {
    constexpr int W    = 120;
    constexpr int H    = 60;
    auto          mask = makeMask(W, H);
    fillRect(mask, W, 10, 10, 110, 50, 1);

    const auto FIELD = buildSolidGlassField(mask, W, H, 12.F);

    // pixel centers: x = 10 is the first column inside, 0.5 px from the left outline
    const auto LEFT = sampleSolidGlassField(FIELD, 10, 30);
    EXPECT_NEAR(LEFT.rimDepth, 0.5F, 0.15F);
    EXPECT_NEAR(LEFT.normalX, -1.F, 0.05F);
    EXPECT_NEAR(LEFT.normalY, 0.F, 0.05F);
    EXPECT_FLOAT_EQ(LEFT.bulb, 12.F);

    const auto TOP = sampleSolidGlassField(FIELD, 60, 10);
    EXPECT_NEAR(TOP.normalX, 0.F, 0.05F);
    EXPECT_NEAR(TOP.normalY, -1.F, 0.05F);

    const auto BOTTOM = sampleSolidGlassField(FIELD, 60, 49);
    EXPECT_NEAR(BOTTOM.normalY, 1.F, 0.05F);

    const auto INNER = sampleSolidGlassField(FIELD, 16, 30);
    EXPECT_NEAR(INNER.rimDepth, 6.5F, 0.15F);
    // the bend depth is smoothed but stays close to the exact one along straight edges
    EXPECT_NEAR(INNER.bendDepth, 6.5F, 0.6F);

    // outside the antialiasing band there is no glass
    EXPECT_EQ(sampleSolidGlassField(FIELD, 2, 30).bulb, 0.F);
    // inside the band there is, with a negative depth
    const auto BAND = sampleSolidGlassField(FIELD, 9, 30);
    EXPECT_GT(BAND.bulb, 0.F);
    EXPECT_LT(BAND.rimDepth, 0.F);
}

TEST(SolidGlassField, cornerNormalTurnsSmoothly) {
    constexpr int W    = 100;
    constexpr int H    = 100;
    auto          mask = makeMask(W, H);
    fillRect(mask, W, 10, 10, 90, 90, 1);

    const auto FIELD = buildSolidGlassField(mask, W, H, 24.F);
    // on the corner diagonal the normal points diagonally out, not along one edge
    const auto DIAGONAL = sampleSolidGlassField(FIELD, 16, 16);
    EXPECT_NEAR(DIAGONAL.normalX, -std::sqrt(0.5F), 0.08F);
    EXPECT_NEAR(DIAGONAL.normalY, -std::sqrt(0.5F), 0.08F);

    // next to the diagonal it must not snap to the edge direction (no crease)
    const auto BESIDE = sampleSolidGlassField(FIELD, 18, 16);
    EXPECT_LT(std::abs(BESIDE.normalY), 0.95F);
    EXPECT_GT(std::abs(BESIDE.normalY), 0.3F);
}

TEST(SolidGlassField, thinShapesClampTheBulb) {
    constexpr int W    = 200;
    constexpr int H    = 40;
    auto          mask = makeMask(W, H);
    fillRect(mask, W, 10, 6, 190, 34, 1); // 28 px high chip

    const auto FIELD = buildSolidGlassField(mask, W, H, 48.F);
    const auto CHIP  = sampleSolidGlassField(FIELD, 100, 20);
    EXPECT_GT(CHIP.bulb, 12.F);
    EXPECT_LT(CHIP.bulb, 15.F);
}

TEST(SolidGlassField, nestedShapeKeepsItsOwnLensAndParentIgnoresIt) {
    constexpr int W    = 200;
    constexpr int H    = 120;
    auto          mask = makeMask(W, H);
    // card with a button cut out by a 1 px moat
    fillRect(mask, W, 10, 10, 190, 110, 1);
    fillRect(mask, W, 59, 39, 141, 81, 0);
    fillRect(mask, W, 60, 40, 140, 80, 1);

    const auto FIELD = buildSolidGlassField(mask, W, H, 24.F);

    // button: measured to its own outline
    const auto BUTTON = sampleSolidGlassField(FIELD, 60, 60);
    EXPECT_NEAR(BUTTON.rimDepth, 0.5F, 0.15F);
    EXPECT_NEAR(BUTTON.normalX, -1.F, 0.1F);

    // moat pixel: part of the card, deep inside it
    const auto MOAT = sampleSolidGlassField(FIELD, 59, 60);
    EXPECT_GT(MOAT.bulb, 0.F);
    EXPECT_GT(MOAT.rimDepth, 20.F);

    // card pixel next to the moat: also deep, the button does not give the card a lens
    const auto CARD = sampleSolidGlassField(FIELD, 58, 60);
    EXPECT_GT(CARD.rimDepth, 20.F);
    EXPECT_NEAR(CARD.bendDepth, CARD.bulb, 0.5F);
}

TEST(SolidGlassField, separateShapesDoNotInfluenceEachOther) {
    constexpr int W    = 200;
    constexpr int H    = 60;
    auto          mask = makeMask(W, H);
    fillRect(mask, W, 10, 10, 90, 50, 1);
    fillRect(mask, W, 95, 10, 190, 50, 1);

    const auto FIELD = buildSolidGlassField(mask, W, H, 12.F);
    const auto RIGHT = sampleSolidGlassField(FIELD, 89, 30);
    const auto LEFT  = sampleSolidGlassField(FIELD, 95, 30);
    EXPECT_NEAR(RIGHT.normalX, 1.F, 0.05F);
    EXPECT_NEAR(LEFT.normalX, -1.F, 0.05F);
    // the gap stays without glass in its middle
    EXPECT_EQ(sampleSolidGlassField(FIELD, 92, 30).bulb, 0.F);
}

TEST(SolidGlassField, shapesTouchingTheBorderAreClosedByIt) {
    constexpr int W    = 60;
    constexpr int H    = 30;
    auto          mask = makeMask(W, H);
    fillRect(mask, W, 0, 0, 60, 30, 1);

    const auto FIELD = buildSolidGlassField(mask, W, H, 12.F);
    const auto EDGE  = sampleSolidGlassField(FIELD, 0, 15);
    EXPECT_NEAR(EDGE.rimDepth, 0.5F, 0.15F);
    EXPECT_NEAR(EDGE.normalX, -1.F, 0.1F);
}

TEST(SolidGlassField, closingFillsMoatsAndKeepsOutline) {
    CRegion region;
    region.add(CBox{10, 10, 180, 100});
    region.subtract(CBox{59, 39, 82, 42});
    region.add(CBox{60, 40, 80, 40});

    const auto CLOSED = closeSolidGlassRegionGaps(region, 1.0);
    EXPECT_TRUE(CLOSED.containsPoint({59.5, 60.5}));
    EXPECT_TRUE(CLOSED.containsPoint({10.5, 10.5}));
    EXPECT_FALSE(CLOSED.containsPoint({9.5, 60.5}));
    EXPECT_FALSE(CLOSED.containsPoint({190.5, 60.5}));

    // a wide gap stays open
    CRegion apart;
    apart.add(CBox{0, 0, 10, 10});
    apart.add(CBox{20, 0, 10, 10});
    EXPECT_FALSE(closeSolidGlassRegionGaps(apart, 1.0).containsPoint({15.5, 5.5}));
}
