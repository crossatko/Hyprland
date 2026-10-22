#include "PassElement.hpp"

std::optional<CBox> IPassElement::boundingBox(Render::CRenderContext& ctx) {
    return std::nullopt;
}

CRegion IPassElement::opaqueRegion(Render::CRenderContext& ctx) {
    return {};
}

// where a live blur of this element can be visible (monitor-local logical), defaults to the bounding box
CRegion IPassElement::liveBlurRegion(Render::CRenderContext& ctx) {
    const auto BB = boundingBox(ctx);
    return BB ? CRegion{*BB} : CRegion{};
}

bool IPassElement::disableSimplification(Render::CRenderContext& ctx) {
    return false;
}

bool IPassElement::requiresFullDamage(Render::CRenderContext& ctx) {
    return false;
}

void IPassElement::discard(Render::CRenderContext& ctx) {
    ;
}

bool IPassElement::undiscardable(Render::CRenderContext& ctx) {
    return false;
}

std::vector<UP<IPassElement>> IPassElement::draw(Render::CRenderContext& ctx) {
    return {};
}
