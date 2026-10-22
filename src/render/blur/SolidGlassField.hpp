#pragma once

#include "../../helpers/math/Math.hpp"

#include <cstdint>
#include <vector>

namespace Render {
    // Lens data for the solid_glass blur variant, precomputed on the CPU from a binary shape mask
    // (a rasterized client blur region) and uploaded as an RGBA8 texture, so the shader needs one tap per pixel.
    //
    // Every 4-connected island of the mask is its own glass shape. An island enclosed by another one (e.g. a
    // button cut out of its card by a thin moat) keeps its own lens, and the enclosing shape ignores it: its
    // distances are measured to its own outline with all of its holes filled, and so are the moat pixels.
    //
    // The bend follows a blurred copy of the distance field, so it has no crease where two edges meet (corner
    // diagonals). The rim and the antialiased outline use the exact distance.
    //
    // Per pixel, in byte order B, G, R, A (DRM_FORMAT_ARGB8888 in memory), sampled 1:1 with nearest filtering:
    //  R: exact signed px distance to the outline (positive inside), [-2, 30] mapped to [0, 255]
    //  G: direction of the outward normal, atan2(y, x) from [-pi, pi) mapped to [0, 256)
    //  B: smoothed depth / bulb, [0, 1]
    //  A: bulb of the shape in px (the requested bulb, clamped to half the shape thickness), 0 = no glass
    struct SSolidGlassField {
        int                  width  = 0;
        int                  height = 0;
        std::vector<uint8_t> pixels;
    };

    // mask: width * height bytes, non-zero is inside. bulb: requested edge width in px
    SSolidGlassField buildSolidGlassField(const std::vector<uint8_t>& mask, int width, int height, float bulb);

    struct SSolidGlassFieldSample {
        float rimDepth  = 0.F;
        float bendDepth = 0.F;
        float normalX   = 0.F;
        float normalY   = 0.F;
        float bulb      = 0.F;
    };

    // decodes one pixel the way the shader does
    SSolidGlassFieldSample sampleSolidGlassField(const SSolidGlassField& field, int x, int y);

    // morphological closing: fills gaps up to 2 * gap wide (the moats that separate nested glass shapes) and
    // keeps the outer outline. The blur is clipped to this instead of the raw declared region
    CRegion closeSolidGlassRegionGaps(const CRegion& region, double gap);
}
