#include "Material.hpp"

#include "../../Context.hpp"
#include "../../ShaderLoader.hpp"

#include <algorithm>

using namespace Render;
using namespace Render::GL;

SP<ITexture> Render::GL::materialTexture(const SP<IFramebuffer>& framebuffer) {
    if (!framebuffer || !framebuffer->isAllocated())
        return nullptr;

    const auto texture = framebuffer->getTexture();
    return texture && texture->ok() ? texture : nullptr;
}

bool IGLBlurMaterial::isAnimated(CRenderContext& ctx) const noexcept {
    return false;
}

int64_t IGLBlurMaterial::blurSizeForDamage(int64_t size) const {
    return size;
}

// called before each scissored draw of the finish, rect in the blur framebuffer (monitor device px)
void IGLBlurMaterial::bindFinishRect(WP<CShader> shader, const CBox& rect) const {
    ;
}

// false if the finish reads only the prepared (sharp) input this frame, the dual kawase passes are then skipped
bool IGLBlurMaterial::needsBlurredInput() const {
    return true;
}

float IGLBlurMaterial::sampleRadius() const {
    return 0.F;
}

void IGLBlurMaterial::prepareForFrame(CRenderContext& ctx, const SBlurMaterialContext& context) {
    // Isolated renders may sample live effects, but must not advance or maintain them.
    if (ctx.readOnlyEffects())
        return;

    prepare(ctx, context);
}

void IGLBlurMaterial::prepare(CRenderContext& ctx, const SBlurMaterialContext&) {
    ;
}

void IGLBlurMaterial::bindFinish(CRenderContext& ctx, WP<CShader>, const SBlurMaterialContext&) const {
    ;
}

eBlurType CDefaultBlurMaterial::type() const noexcept {
    return eBlurType::BLUR_DUAL_KAWASE;
}

SBlurMaterialRequirements CDefaultBlurMaterial::requirements() const noexcept {
    return {
        .finishFragment = SH_FRAG_BLURFINISH,
    };
}

int64_t CDefaultBlurMaterial::blurSizeForDamage(int64_t size) const {
    return std::clamp<int64_t>(size, 1, 40);
}
