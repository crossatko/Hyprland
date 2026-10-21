#pragma once

#include "Kawase.hpp"

#include <optional>
#include <vector>

namespace Render {
    struct SSolidGlassShape;
}

namespace Render::GL {
    class CSolidGlassBlurMaterial final : public IGLBlurMaterial {
      public:
        eBlurType                 type() const noexcept override;
        SBlurMaterialRequirements requirements() const noexcept override;
        float                     sampleRadius() const override;
        bool                      needsBlurredInput() const override;
        void                      bindFinish(CRenderContext& ctx, WP<CShader> shader, const SBlurMaterialContext& context) const override;

      private:
        // lens fields of client-declared blur regions (ext-background-effect-v1), one per layer surface,
        // rebuilt only when the shape of the region (not its position) or the bulb changes
        struct SRegionField {
            WP<Desktop::View::CWLSurface> surface;
            CRegion                       region; // device px, relative to its extents
            float                         bulb = 0.F;
            SP<ITexture>                  texture;
        };

        struct SFieldBinding {
            SP<ITexture> texture;
            CBox         box; // where the field lies, in the blur framebuffer
        };

        std::optional<SFieldBinding>      regionField(const SBlurShape& shape, float bulbPx) const;
        void                              packShapes(const std::vector<SSolidGlassShape>& shapes, float scale) const;
        mutable std::vector<SRegionField> m_regionFields;
        mutable std::vector<float>        m_shapeUniforms;
    };

    class CSolidGlassBlurProvider final : public CDualKawaseBlurProvider {
      public:
        explicit CSolidGlassBlurProvider(CHyprOpenGLImpl& impl);
    };

    float solidGlassDamageRadius(int64_t size, int64_t passes, float refraction, float frost);
}
