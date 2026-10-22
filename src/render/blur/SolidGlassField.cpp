#include "SolidGlassField.hpp"

#include "../../helpers/memory/Memory.hpp"

#include <hyprutils/memory/Casts.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace Render;

static constexpr float RIM_RANGE_OUTSIDE = 2.F;  // px of exact signed distance stored outside the outline (antialiasing)
static constexpr float RIM_RANGE_INSIDE  = 30.F; // and inside it (the rim has faded out long before)
static constexpr float MAX_BULB          = 255.F;

// 5-7-11 chamfer distance: at most ~2% off the euclidean distance, two passes of integer work
static constexpr int CHAMFER_AXIS     = 5;
static constexpr int CHAMFER_DIAGONAL = 7;
static constexpr int CHAMFER_KNIGHT   = 11;
static constexpr int CHAMFER_FAR      = 1 << 28;
static constexpr int CHAMFER_BORDER   = 2;

struct SComponent {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
};

static int smoothingRadius(float bulb) {
    return std::max(1, sc<int>(std::lround(bulb * 0.2F)));
}

// in place chamfer distance to the cells that are 0. The grid has a CHAMFER_BORDER cells wide frame that is
// never written and stands for whatever lies outside the local area
static void chamferDistance(std::vector<int>& grid, int stride, int rows) {
    const auto relax = [&](size_t i, std::ptrdiff_t offset, int cost) {
        const auto CANDIDATE = grid[sc<size_t>(sc<std::ptrdiff_t>(i) + offset)] + cost;
        if (CANDIDATE < grid[i])
            grid[i] = CANDIDATE;
    };

    const auto S = sc<std::ptrdiff_t>(stride);
    for (int y = CHAMFER_BORDER; y < rows - CHAMFER_BORDER; ++y) {
        for (int x = CHAMFER_BORDER; x < stride - CHAMFER_BORDER; ++x) {
            const auto I = (sc<size_t>(y) * stride) + x;
            if (grid[I] == 0)
                continue;
            relax(I, -1, CHAMFER_AXIS);
            relax(I, -S, CHAMFER_AXIS);
            relax(I, -S - 1, CHAMFER_DIAGONAL);
            relax(I, -S + 1, CHAMFER_DIAGONAL);
            relax(I, -S - 2, CHAMFER_KNIGHT);
            relax(I, -S + 2, CHAMFER_KNIGHT);
            relax(I, (-2 * S) - 1, CHAMFER_KNIGHT);
            relax(I, (-2 * S) + 1, CHAMFER_KNIGHT);
        }
    }
    for (int y = rows - CHAMFER_BORDER - 1; y >= CHAMFER_BORDER; --y) {
        for (int x = stride - CHAMFER_BORDER - 1; x >= CHAMFER_BORDER; --x) {
            const auto I = (sc<size_t>(y) * stride) + x;
            if (grid[I] == 0)
                continue;
            relax(I, 1, CHAMFER_AXIS);
            relax(I, S, CHAMFER_AXIS);
            relax(I, S + 1, CHAMFER_DIAGONAL);
            relax(I, S - 1, CHAMFER_DIAGONAL);
            relax(I, S + 2, CHAMFER_KNIGHT);
            relax(I, S - 2, CHAMFER_KNIGHT);
            relax(I, (2 * S) + 1, CHAMFER_KNIGHT);
            relax(I, (2 * S) - 1, CHAMFER_KNIGHT);
        }
    }
}

// box blur of radius r along the rows, then along the columns (row by row, so memory stays sequential), edges clamped
static void boxBlur(std::vector<float>& grid, std::vector<float>& scratch, int width, int height, int radius) {
    const auto SPAN = sc<float>((2 * radius) + 1);
    scratch.resize(grid.size());

    for (int y = 0; y < height; ++y) {
        const auto ROW = sc<size_t>(y) * width;
        float      sum = 0.F;
        for (int i = -radius; i <= radius; ++i) {
            sum += grid[ROW + std::clamp(i, 0, width - 1)];
        }
        for (int x = 0; x < width; ++x) {
            scratch[ROW + x] = sum / SPAN;
            sum += grid[ROW + std::clamp(x + radius + 1, 0, width - 1)] - grid[ROW + std::clamp(x - radius, 0, width - 1)];
        }
    }

    std::vector<float> sums(width, 0.F);
    for (int i = -radius; i <= radius; ++i) {
        const auto ROW = sc<size_t>(std::clamp(i, 0, height - 1)) * width;
        for (int x = 0; x < width; ++x) {
            sums[x] += scratch[ROW + x];
        }
    }
    for (int y = 0; y < height; ++y) {
        const auto ROW   = sc<size_t>(y) * width;
        const auto ENTER = sc<size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width;
        const auto LEAVE = sc<size_t>(std::clamp(y - radius, 0, height - 1)) * width;
        for (int x = 0; x < width; ++x) {
            grid[ROW + x] = sums[x] / SPAN;
            sums[x] += scratch[ENTER + x] - scratch[LEAVE + x];
        }
    }
}

// 4-connected islands of the mask, labels[i] = island index or -1
static std::vector<SComponent> labelComponents(const std::vector<uint8_t>& mask, int width, int height, std::vector<int>& labels) {
    std::vector<SComponent> components;
    std::vector<int>        stack;
    labels.assign(mask.size(), -1);

    for (int start = 0; start < width * height; ++start) {
        if (!mask[start] || labels[start] >= 0)
            continue;

        const int  ID = sc<int>(components.size());
        SComponent component{.minX = start % width, .minY = start / width, .maxX = start % width, .maxY = start / width};
        labels[start] = ID;
        stack.push_back(start);
        while (!stack.empty()) {
            const int I = stack.back();
            stack.pop_back();
            const int X    = I % width;
            const int Y    = I / width;
            component.minX = std::min(component.minX, X);
            component.maxX = std::max(component.maxX, X);
            component.minY = std::min(component.minY, Y);
            component.maxY = std::max(component.maxY, Y);

            const auto visit = [&](int neighbour) {
                if (!mask[neighbour] || labels[neighbour] >= 0)
                    return;
                labels[neighbour] = ID;
                stack.push_back(neighbour);
            };
            if (X > 0)
                visit(I - 1);
            if (X + 1 < width)
                visit(I + 1);
            if (Y > 0)
                visit(I - width);
            if (Y + 1 < height)
                visit(I + width);
        }
        components.emplace_back(component);
    }

    return components;
}

static uint8_t toByte(float value) {
    return sc<uint8_t>(std::clamp(std::lround(value * 255.F), 0L, 255L));
}

static uint8_t angleToByte(float x, float y) {
    const float ANGLE = std::atan2(y, x);
    return sc<uint8_t>(sc<int>(std::lround((ANGLE + sc<float>(M_PI)) / (2.F * sc<float>(M_PI)) * 256.F)) & 0xFF);
}

SSolidGlassField Render::buildSolidGlassField(const std::vector<uint8_t>& mask, int width, int height, float bulb) {
    SSolidGlassField field{.width = width, .height = height};
    if (width <= 0 || height <= 0 || mask.size() != sc<size_t>(width) * sc<size_t>(height))
        return field;

    field.pixels.assign(sc<size_t>(width) * sc<size_t>(height) * 4, 0);

    std::vector<int> labels;
    auto             components = labelComponents(mask, width, height, labels);

    // outer shapes first, shapes inside them overwrite their area afterwards
    std::vector<size_t> order(components.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](size_t a, size_t b) {
        const auto AREA = [&](size_t i) { return sc<int64_t>(components[i].maxX - components[i].minX + 1) * (components[i].maxY - components[i].minY + 1); };
        return AREA(a) > AREA(b);
    });

    const float REQUESTED_BULB = std::clamp(bulb, 1.F, MAX_BULB);
    // the bend follows a blurred copy of the distance field: its iso-lines are rounded where two edges meet
    // (the medial axis, e.g. corner diagonals), so neither the bend nor its direction creases there
    const int            MAX_SMOOTHING = smoothingRadius(REQUESTED_BULB);
    const int            MARGIN        = (2 * MAX_SMOOTHING) + sc<int>(RIM_RANGE_OUTSIDE) + 3;
    const uint8_t        DEEP_ANGLE    = angleToByte(0.F, -1.F);

    std::vector<uint8_t> filled;
    std::vector<int>     stack;
    std::vector<int>     toOutside;
    std::vector<int>     toInside;
    std::vector<float>   signedDistance;
    std::vector<float>   smoothed;
    std::vector<float>   scratch;

    for (const auto INDEX : order) {
        const auto& C      = components[INDEX];
        const int   X0     = C.minX - MARGIN;
        const int   Y0     = C.minY - MARGIN;
        const int   W      = C.maxX - C.minX + 1 + (2 * MARGIN);
        const int   H      = C.maxY - C.minY + 1 + (2 * MARGIN);
        const auto  N      = sc<size_t>(W) * sc<size_t>(H);
        const int   STRIDE = W + (2 * CHAMFER_BORDER);
        const int   ROWS   = H + (2 * CHAMFER_BORDER);

        // this island in local coordinates
        filled.assign(N, 0);
        for (int ly = std::max(0, -Y0); ly < std::min(H, height - Y0); ++ly) {
            const auto ROW = sc<size_t>(Y0 + ly) * width;
            for (int lx = std::max(0, -X0); lx < std::min(W, width - X0); ++lx) {
                if (labels[ROW + X0 + lx] == sc<int>(INDEX))
                    filled[(sc<size_t>(ly) * W) + lx] = 1;
            }
        }

        // flood the outside from the frame of the local grid, whatever it cannot reach (holes, moats and the
        // islands inside them) belongs to this shape
        constexpr uint8_t OUTSIDE = 2;
        const auto        flood   = [&](int i) {
            if (filled[i] != 0)
                return;
            filled[i] = OUTSIDE;
            stack.push_back(i);
        };
        for (int lx = 0; lx < W; ++lx) {
            flood(lx);
            flood(((H - 1) * W) + lx);
        }
        for (int ly = 0; ly < H; ++ly) {
            flood(ly * W);
            flood((ly * W) + W - 1);
        }
        while (!stack.empty()) {
            const int I = stack.back();
            stack.pop_back();
            const int LX = I % W;
            if (LX > 0)
                flood(I - 1);
            if (LX + 1 < W)
                flood(I + 1);
            if (I >= W)
                flood(I - W);
            if (I + W < sc<int>(N))
                flood(I + W);
        }

        // distances of the inside to the outside and of the outside to the inside
        toOutside.assign(sc<size_t>(STRIDE) * ROWS, 0);
        toInside.assign(sc<size_t>(STRIDE) * ROWS, CHAMFER_FAR);
        for (int ly = 0; ly < H; ++ly) {
            for (int lx = 0; lx < W; ++lx) {
                const bool INSIDE = filled[(sc<size_t>(ly) * W) + lx] != OUTSIDE;
                const auto G      = (sc<size_t>(ly + CHAMFER_BORDER) * STRIDE) + lx + CHAMFER_BORDER;
                toOutside[G]      = INSIDE ? CHAMFER_FAR : 0;
                toInside[G]       = INSIDE ? 0 : CHAMFER_FAR;
            }
        }
        chamferDistance(toOutside, STRIDE, ROWS);
        chamferDistance(toInside, STRIDE, ROWS);

        signedDistance.resize(N);
        float halfThickness = 0.F;
        for (int ly = 0; ly < H; ++ly) {
            for (int lx = 0; lx < W; ++lx) {
                const auto I      = (sc<size_t>(ly) * W) + lx;
                const auto G      = (sc<size_t>(ly + CHAMFER_BORDER) * STRIDE) + lx + CHAMFER_BORDER;
                const bool INSIDE = filled[I] != OUTSIDE;
                signedDistance[I] = INSIDE ? (sc<float>(toOutside[G]) / CHAMFER_AXIS) - 0.5F : 0.5F - (sc<float>(toInside[G]) / CHAMFER_AXIS);
                halfThickness     = std::max(halfThickness, signedDistance[I]);
            }
        }

        const float SHAPE_BULB = std::clamp(REQUESTED_BULB, 1.F, std::max(halfThickness, 1.F));
        const int   RADIUS     = std::min(smoothingRadius(SHAPE_BULB), MAX_SMOOTHING);
        smoothed               = signedDistance;
        boxBlur(smoothed, scratch, W, H, RADIUS);
        boxBlur(smoothed, scratch, W, H, RADIUS);

        const auto smoothedAt = [&](int lx, int ly) { return smoothed[(sc<size_t>(std::clamp(ly, 0, H - 1)) * W) + std::clamp(lx, 0, W - 1)]; };
        const auto BULB_BYTE  = sc<uint8_t>(std::clamp(std::lround(SHAPE_BULB), 1L, 255L));

        for (int ly = std::max(0, -Y0); ly < std::min(H, height - Y0); ++ly) {
            const int GY = Y0 + ly;
            for (int lx = std::max(0, -X0); lx < std::min(W, width - X0); ++lx) {
                const auto  I     = (sc<size_t>(ly) * W) + lx;
                const float DEPTH = signedDistance[I];
                const auto  OUT   = ((sc<size_t>(GY) * width) + X0 + lx) * 4;
                if (DEPTH < -RIM_RANGE_OUTSIDE)
                    continue;
                // the antialiasing band outside a shape never overwrites another shape
                if (DEPTH < 0.F && field.pixels[OUT + 3] != 0)
                    continue;

                const float BEND = smoothed[I] / SHAPE_BULB;
                if (DEPTH >= RIM_RANGE_INSIDE && BEND >= 1.F) {
                    // deep inside: no bend, no rim, the normal does not matter
                    field.pixels[OUT + 0] = 255;
                    field.pixels[OUT + 1] = DEEP_ANGLE;
                    field.pixels[OUT + 2] = 255;
                    field.pixels[OUT + 3] = BULB_BYTE;
                    continue;
                }

                const float NORMAL_X  = smoothedAt(lx - 1, ly) - smoothedAt(lx + 1, ly);
                const float NORMAL_Y  = smoothedAt(lx, ly - 1) - smoothedAt(lx, ly + 1);
                const bool  HAS_SLOPE = std::abs(NORMAL_X) + std::abs(NORMAL_Y) > 0.0001F;

                field.pixels[OUT + 0] = toByte(BEND);
                field.pixels[OUT + 1] = HAS_SLOPE ? angleToByte(NORMAL_X, NORMAL_Y) : DEEP_ANGLE;
                field.pixels[OUT + 2] = toByte((DEPTH + RIM_RANGE_OUTSIDE) / (RIM_RANGE_OUTSIDE + RIM_RANGE_INSIDE));
                field.pixels[OUT + 3] = BULB_BYTE;
            }
        }
    }

    return field;
}

SSolidGlassFieldSample Render::sampleSolidGlassField(const SSolidGlassField& field, int x, int y) {
    if (x < 0 || y < 0 || x >= field.width || y >= field.height)
        return {};

    const auto I    = ((sc<size_t>(y) * field.width) + x) * 4;
    const auto BULB = sc<float>(field.pixels[I + 3]);
    if (BULB < 0.5F)
        return {};

    const auto ANGLE = (sc<float>(field.pixels[I + 1]) / 256.F * 2.F * sc<float>(M_PI)) - sc<float>(M_PI);
    return {
        .rimDepth  = (sc<float>(field.pixels[I + 2]) / 255.F * (RIM_RANGE_OUTSIDE + RIM_RANGE_INSIDE)) - RIM_RANGE_OUTSIDE,
        .bendDepth = sc<float>(field.pixels[I + 0]) / 255.F * BULB,
        .normalX   = std::cos(ANGLE),
        .normalY   = std::sin(ANGLE),
        .bulb      = BULB,
    };
}

CRegion Render::closeSolidGlassRegionGaps(const CRegion& region, double gap) {
    if (region.empty() || gap <= 0.0)
        return region.copy();

    CRegion    closed  = region.copy().expand(gap);
    const auto BOUNDS  = closed.getExtents().expand(gap + 1.0);
    CRegion    outside = closed.copy().invert(BOUNDS).expand(gap);
    return outside.invert(BOUNDS);
}
