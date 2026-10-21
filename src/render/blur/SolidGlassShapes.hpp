#pragma once

#include "../../helpers/math/Math.hpp"
#include "../../helpers/memory/Memory.hpp"

#include <array>
#include <optional>
#include <vector>

namespace Desktop::View {
    class CWLSurface;
}

namespace Render {
    // shapes the solid_glass shader evaluates per pixel, keep in sync with MAX_SHAPES in solidglassfinish.frag
    constexpr size_t MAX_SOLIDGLASS_SHAPES = 48;

    // One glass shape of a client-declared blur region (ext-background-effect-v1), as an exact rounded rectangle.
    struct SSolidGlassShape {
        CBox box; // logical px, surface relative
        // logical px, circular corners: top left, top right, bottom right, bottom left
        std::array<float, 4> radii       = {0.F, 0.F, 0.F, 0.F};
        int                  parent      = -1; // index of the shape this one lies in, -1 = outermost
        int                  level       = 0;  // nesting depth, 0 = outermost
        int                  descendants = 0;  // shapes nested in this one, they follow it directly (preorder)
    };

    // Recovers the rounded rectangles a client rasterized into its blur region. Every 4-connected island of the region
    // is fitted to one rounded rectangle (bounding box + a radius per corner from the area it cuts away) and checked
    // against the island's row extents. An island lying inside another one (a button cut out of its card by a thin
    // moat) becomes a child of it: it gets its own lens ring, the parent's lens follows only the parent's outline.
    // Returns nullopt if any island is no rounded rectangle (or there are too many), callers then fall back to a
    // rasterized field.
    // Islands entirely outside of bounds (the surface, logical px) are ignored.
    std::optional<std::vector<SSolidGlassShape>> fitSolidGlassShapes(const CRegion& region, const CBox& bounds);

    // Orders shapes depth first (every shape directly followed by the ones nested in it) and fills level and
    // descendants from parent, so the shader can skip a whole subtree with one box test. parent is remapped.
    std::vector<SSolidGlassShape> orderSolidGlassShapes(const std::vector<SSolidGlassShape>& shapes);

    // reference for the shader: the corner radius that applies to a point (by quadrant), and the signed distance to
    // a rounded rectangle with circular corners, negative inside
    float solidGlassCornerRadius(const Vector2D& point, const CBox& box, const std::array<float, 4>& radii);
    float solidGlassRoundedBoxDistance(const Vector2D& point, const CBox& box, const std::array<float, 4>& radii);

    // per surface cache of the fitted shapes, refitted only when the declared region changes
    class CSolidGlassShapeCache {
      public:
        struct SEntry {
            WP<Desktop::View::CWLSurface> surface;
            CRegion                       region; // the declared region the entry was built from (logical px)
            Vector2D                      size;   // logical surface size it was built for
            bool                          fitted = false;
            std::vector<SSolidGlassShape> shapes;
            CRegion                       clip; // where the blur has to be drawn: the outer shapes plus 1 px for antialiasing
            // a few boxes covering everything the blur of this surface can show (clip, or the region extents if the
            // region could not be fitted), logical px. Bounds the blur work: a staircase of region rects would cost
            // one draw call per rect
            CRegion bounds;
        };

        // nullptr if the surface has no declared region
        const SEntry* get(const SP<Desktop::View::CWLSurface>& surface);

      private:
        std::vector<SEntry> m_entries;
    };

    UP<CSolidGlassShapeCache>& solidGlassShapeCache();
}
