#pragma once

#include "../../desktop/DesktopTypes.hpp"
#include "../../helpers/math/Math.hpp"
#include "../Framebuffer.hpp"
#include "../WindowRenderPresentation.hpp"

#include <optional>

namespace Desktop::View {
    class CWLSurface;
}

namespace Render {
    class CRenderContext;
    class ITexture;
    struct SBlurShape {
        CBox  box;
        float radius        = 0.F;
        float roundingPower = 2.F;

        // layer surfaces only: the visible shape is not the surface box, so materials that care
        // can derive it from the surface alpha, or from the client-declared blur region.
        SP<ITexture>                  surfaceTexture;
        WP<Desktop::View::CWLSurface> surface;
        float                         surfaceScale   = 1.F;
        float                         alphaThreshold = 0.F;
        uint8_t                       role           = 0; // eBlurRole
    };

    // what a blur is drawn for, when a material styles it differently (solid_glass: group:groupbar:solid_glass)
    enum eBlurRole : uint8_t {
        BLUR_ROLE_NONE     = 0,
        BLUR_ROLE_GROUPBAR = 1,
    };

    struct SBlurContext {
        std::optional<CBox>               patternBox;
        PHLWINDOWREF                      owner;
        std::optional<SBlurShape>         shape;
        Render::SWindowRenderPresentation workspacePresentation;
    };

    enum class eBlurType : uint8_t {
        BLUR_DUAL_KAWASE  = 0,
        BLUR_FROST        = 1,
        BLUR_RIPPLE       = 2,
        BLUR_DROPS        = 3,
        BLUR_WATER        = 4,
        BLUR_FLUID_JAR    = 5,
        BLUR_PRISM        = 6,
        BLUR_HEAT_SHIMMER = 7,
        BLUR_ACRYLIC      = 8,
        BLUR_AURORA       = 9,
        BLUR_HAZE         = 10,
        BLUR_SOLID_GLASS  = 11,
    };

    // "Jaki kurwa provident????"
    class IBlurProvider {
      public:
        virtual ~IBlurProvider() = default;

        virtual eBlurType        type() const noexcept                          = 0;
        virtual bool             isAnimated(CRenderContext& ctx) const noexcept = 0;
        virtual bool             requiresLiveBlur() const noexcept              = 0;

        virtual void             expandDamage(CRegion& damage, float multiplier = 1.F) const                                                                         = 0;
        virtual SP<IFramebuffer> blur(CRenderContext& ctx, SP<IFramebuffer> source, float strength, const CRegion& originalDamage, const SBlurContext& context = {}) = 0;

      protected:
        IBlurProvider() = default;
    };
};
