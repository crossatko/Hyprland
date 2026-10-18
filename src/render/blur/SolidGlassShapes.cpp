#include "SolidGlassShapes.hpp"
#include "SolidGlassField.hpp"

#include "../../desktop/view/WLSurface.hpp"
#include "../../protocols/core/Compositor.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <print>
#include <cstdlib>

using namespace Render;

// one band of an island: its rows y1..y2 span minX..maxX (holes inside are ignored)
struct SIslandRow {
    int y1   = 0;
    int y2   = 0;
    int minX = 0;
    int maxX = 0;
};

struct SIsland {
    int                     x1   = 0;
    int                     y1   = 0;
    int                     x2   = 0;
    int                     y2   = 0;
    int64_t                 area = 0;
    std::vector<SIslandRow> rows;
};

static size_t findRoot(std::vector<size_t>& parents, size_t index) {
    while (parents[index] != index) {
        parents[index] = parents[parents[index]];
        index          = parents[index];
    }
    return index;
}

// 4-connected islands of a pixman region. Its rects come in y-x banded order, rects of one band never touch
static std::vector<SIsland> regionIslands(const std::vector<pixman_box32_t>& rects) {
    std::vector<size_t> parents(rects.size());
    std::iota(parents.begin(), parents.end(), 0);

    size_t previousBand = 0;
    size_t band         = 0;
    while (band < rects.size()) {
        size_t bandEnd = band;
        while (bandEnd < rects.size() && rects[bandEnd].y1 == rects[band].y1)
            ++bandEnd;

        // join with the band right above if it touches
        if (band > 0 && rects[previousBand].y2 == rects[band].y1) {
            size_t above = previousBand;
            size_t below = band;
            while (above < band && below < bandEnd) {
                if (rects[above].x1 < rects[below].x2 && rects[below].x1 < rects[above].x2)
                    parents[findRoot(parents, above)] = findRoot(parents, below);

                if (rects[above].x2 < rects[below].x2)
                    ++above;
                else
                    ++below;
            }
        }

        previousBand = band;
        band         = bandEnd;
    }

    std::vector<SIsland> islands;
    std::vector<int>     islandOf(rects.size(), -1);
    for (size_t i = 0; i < rects.size(); ++i) {
        const auto ROOT = findRoot(parents, i);
        if (islandOf[ROOT] < 0) {
            islandOf[ROOT] = sc<int>(islands.size());
            islands.push_back(SIsland{.x1 = rects[i].x1, .y1 = rects[i].y1, .x2 = rects[i].x2, .y2 = rects[i].y2});
        }

        auto&       island = islands[islandOf[ROOT]];
        const auto& RECT   = rects[i];
        island.x1          = std::min(island.x1, RECT.x1);
        island.y1          = std::min(island.y1, RECT.y1);
        island.x2          = std::max(island.x2, RECT.x2);
        island.y2          = std::max(island.y2, RECT.y2);
        island.area += sc<int64_t>(RECT.x2 - RECT.x1) * (RECT.y2 - RECT.y1);

        if (island.rows.empty() || island.rows.back().y1 != RECT.y1)
            island.rows.push_back(SIslandRow{.y1 = RECT.y1, .y2 = RECT.y2, .minX = RECT.x1, .maxX = RECT.x2});
        else {
            island.rows.back().minX = std::min(island.rows.back().minX, RECT.x1);
            island.rows.back().maxX = std::max(island.rows.back().maxX, RECT.x2);
        }
    }

    return islands;
}

// how far a circular corner of the given radius cuts into the row centred at rowCenter
static double cornerInset(double rowCenter, double top, double bottom, double radius) {
    const auto FROM_EDGE = std::min(rowCenter - top, bottom - rowCenter);
    if (FROM_EDGE >= radius)
        return 0.0;

    const auto DY = radius - FROM_EDGE;
    return radius - std::sqrt(std::max(radius * radius - DY * DY, 0.0));
}

static std::optional<SSolidGlassShape> fitIsland(const SIsland& island) {
    const double WIDTH  = island.x2 - island.x1;
    const double HEIGHT = island.y2 - island.y1;
    const double MID_Y  = (island.y1 + island.y2) / 2.0;

    // area the four corners cut away from the bounding box: (1 - pi / 4) * r^2 each
    std::array<double, 4> missing = {0.0, 0.0, 0.0, 0.0}; // tl, tr, bl, br
    for (const auto& row : island.rows) {
        const double ROW_HEIGHT = row.y2 - row.y1;
        const double UPPER      = std::clamp(MID_Y - row.y1, 0.0, ROW_HEIGHT);
        const double LOWER      = ROW_HEIGHT - UPPER;
        const double LEFT       = row.minX - island.x1;
        const double RIGHT      = island.x2 - row.maxX;
        missing[0] += LEFT * UPPER;
        missing[1] += RIGHT * UPPER;
        missing[2] += LEFT * LOWER;
        missing[3] += RIGHT * LOWER;
    }

    constexpr double CORNER_AREA = 1.0 - (std::numbers::pi / 4.0);
    const double     MAX_RADIUS  = std::min(WIDTH, HEIGHT) / 2.0;
    // tl, tr, bl, br
    std::array<double, 4> radii = {0.0, 0.0, 0.0, 0.0};
    for (size_t i = 0; i < 4; ++i)
        radii[i] = std::min(std::sqrt(missing[i] / CORNER_AREA), MAX_RADIUS);

    // every row has to follow the outline (first and last row of a band are enough, the inset is monotonic)
    for (const auto& row : island.rows) {
        for (const auto Y : {row.y1, row.y2 - 1}) {
            const bool   UPPER_HALF = Y + 0.5 < MID_Y;
            const double LEFT       = radii[UPPER_HALF ? 0 : 2];
            const double RIGHT      = radii[UPPER_HALF ? 1 : 3];
            if (std::abs((row.minX - island.x1) - cornerInset(Y + 0.5, island.y1, island.y2, LEFT)) > 1.5 + (0.1 * LEFT))
                return std::nullopt;
            if (std::abs((island.x2 - row.maxX) - cornerInset(Y + 0.5, island.y1, island.y2, RIGHT)) > 1.5 + (0.1 * RIGHT))
                return std::nullopt;
        }
    }

    return SSolidGlassShape{
        .box   = CBox{sc<double>(island.x1), sc<double>(island.y1), WIDTH, HEIGHT},
        .radii = {sc<float>(radii[0]), sc<float>(radii[1]), sc<float>(radii[3]), sc<float>(radii[2])},
    };
}

static void appendSubtree(const std::vector<SSolidGlassShape>& shapes, size_t index, int parent, std::vector<SSolidGlassShape>& out) {
    const auto POSITION = out.size();
    out.push_back(shapes[index]);
    out[POSITION].parent      = parent;
    out[POSITION].level       = parent < 0 ? 0 : out[parent].level + 1;
    out[POSITION].descendants = 0;

    for (size_t child = 0; child < shapes.size(); ++child) {
        if (shapes[child].parent == sc<int>(index))
            appendSubtree(shapes, child, sc<int>(POSITION), out);
    }

    out[POSITION].descendants = sc<int>(out.size() - POSITION - 1);
}

std::vector<SSolidGlassShape> Render::orderSolidGlassShapes(const std::vector<SSolidGlassShape>& shapes) {
    std::vector<SSolidGlassShape> ordered;
    ordered.reserve(shapes.size());
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].parent < 0)
            appendSubtree(shapes, i, -1, ordered);
    }
    return ordered;
}

std::optional<std::vector<SSolidGlassShape>> Render::fitSolidGlassShapes(const CRegion& region, const CBox& bounds) {
    if (region.empty())
        return std::nullopt;

    const auto RECTS = region.getRects();
    if (RECTS.size() > 16384)
        return std::nullopt;

    // islands entirely outside of the surface (items parked off screen) are dropped
    auto ISLANDS = regionIslands(RECTS);
    std::erase_if(ISLANDS, [&bounds](const SIsland& island) {
        return island.x2 <= bounds.x || island.y2 <= bounds.y || island.x1 >= bounds.x + bounds.w || island.y1 >= bounds.y + bounds.h;
    });
    if (ISLANDS.empty())
        return std::vector<SSolidGlassShape>{};
    if (ISLANDS.size() > MAX_SOLIDGLASS_SHAPES)
        return std::nullopt;

    std::vector<SSolidGlassShape> shapes;
    shapes.reserve(ISLANDS.size());
    for (const auto& island : ISLANDS) {
        auto shape = fitIsland(island);
        if (!shape)
            return std::nullopt;
        shapes.push_back(*shape);
    }

    // the parent of a shape is the smallest other shape around it
    for (size_t i = 0; i < shapes.size(); ++i) {
        const auto& INNER    = shapes[i].box;
        double      bestArea = 0.0;
        for (size_t j = 0; j < shapes.size(); ++j) {
            const auto& OUTER = shapes[j].box;
            const auto  AREA  = OUTER.w * OUTER.h;
            if (j == i || AREA <= INNER.w * INNER.h)
                continue;
            if (OUTER.x > INNER.x || OUTER.y > INNER.y || OUTER.x + OUTER.w < INNER.x + INNER.w || OUTER.y + OUTER.h < INNER.y + INNER.h)
                continue;
            if (shapes[i].parent >= 0 && AREA >= bestArea)
                continue;

            shapes[i].parent = sc<int>(j);
            bestArea         = AREA;
        }
    }

    // a shape may only have holes where its children sit (plus their moat): anything else (a frame, a U) is no
    // rounded rectangle even if its outline is
    for (size_t i = 0; i < shapes.size(); ++i) {
        const auto& ISLAND  = ISLANDS[i];
        int64_t     outline = 0;
        for (const auto& row : ISLAND.rows)
            outline += sc<int64_t>(row.maxX - row.minX) * (row.y2 - row.y1);

        double holes = sc<double>(outline - ISLAND.area);
        for (const auto& child : shapes) {
            if (child.parent == sc<int>(i))
                holes -= (child.box.w + 2.0) * (child.box.h + 2.0);
        }

        if (holes > std::max(16.0, sc<double>(outline) * 0.02))
            return std::nullopt;
    }

    return orderSolidGlassShapes(shapes);
}

float Render::solidGlassCornerRadius(const Vector2D& point, const CBox& box, const std::array<float, 4>& radii) {
    const bool LEFT = point.x < box.middle().x;
    if (point.y < box.middle().y)
        return LEFT ? radii[0] : radii[1];
    return LEFT ? radii[3] : radii[2];
}

float Render::solidGlassRoundedBoxDistance(const Vector2D& point, const CBox& box, const std::array<float, 4>& radii) {
    const auto HALF    = box.size() / 2.0;
    const auto RADIUS  = std::clamp(sc<double>(solidGlassCornerRadius(point, box, radii)), 0.0, std::min(HALF.x, HALF.y));
    const auto OFFSET  = Vector2D{std::abs(point.x - box.middle().x), std::abs(point.y - box.middle().y)} - (HALF - Vector2D{RADIUS, RADIUS});
    const auto OUTSIDE = Vector2D{std::max(OFFSET.x, 0.0), std::max(OFFSET.y, 0.0)};
    return sc<float>(OUTSIDE.size() + std::min(std::max(OFFSET.x, OFFSET.y), 0.0) - RADIUS);
}

const CSolidGlassShapeCache::SEntry* CSolidGlassShapeCache::get(const SP<Desktop::View::CWLSurface>& surface) {
    if (!surface || !surface->m_hasBackgroundEffect || surface->m_blurRegion.empty())
        return nullptr;

    std::erase_if(m_entries, [](const SEntry& entry) { return !entry.surface.valid(); });

    auto       it       = std::ranges::find_if(m_entries, [&](const SEntry& entry) { return entry.surface.lock() == surface; });
    const auto RESOURCE = surface->resource();
    const auto SIZE     = RESOURCE ? RESOURCE->m_current.size : Vector2D{};
    if (it != m_entries.end() && it->size == SIZE && pixman_region32_equal(it->region.pixman(), surface->m_blurRegion.pixman()))
        return &*it;

    if (it == m_entries.end()) {
        m_entries.emplace_back(SEntry{.surface = surface});
        it = std::prev(m_entries.end());
    }

    it->region  = surface->m_blurRegion.copy();
    it->size    = SIZE;
    auto shapes = fitSolidGlassShapes(it->region, CBox{{}, SIZE});
    it->fitted  = shapes.has_value();
    it->shapes  = shapes ? std::move(*shapes) : std::vector<SSolidGlassShape>{};
    it->clip    = CRegion{};

    if (it->fitted) {
        for (const auto& shape : it->shapes) {
            if (shape.parent < 0)
                it->clip.add(shape.box.copy().expand(1.0));
        }
    } else
        it->clip = closeSolidGlassRegionGaps(it->region, 1.0);

    return &*it;
}

UP<CSolidGlassShapeCache>& Render::solidGlassShapeCache() {
    static UP<CSolidGlassShapeCache> cache = makeUnique<CSolidGlassShapeCache>();
    return cache;
}
