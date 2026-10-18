#include "SolidGlass.hpp"

#include "../../Renderer.hpp"
#include "../../Shader.hpp"
#include "../../ShaderLoader.hpp"
#include "../../../config/ConfigValue.hpp"
#include "../../../helpers/Color.hpp"
#include "../../../helpers/cm/ColorManagement.hpp"
#include "../../../desktop/view/WLSurface.hpp"
#include "../../Texture.hpp"
#include "../../blur/SolidGlassField.hpp"
#include "../../blur/SolidGlassShapes.hpp"
#include "BlurTiming.hpp"

#include <drm_fourcc.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace Render;
using namespace Render::GL;
using namespace NColorManagement;

static constexpr float MAX_SOLIDGLASS_REFRACTION = 48.F;
static constexpr float MIN_SOLIDGLASS_BULB       = 4.F;
static constexpr float MAX_SOLIDGLASS_BULB       = 256.F;
static constexpr float MAX_SOLIDGLASS_FROST      = 16.F;

// refraction only samples inwards, frost samples around: both in logical px, padded for monitor scales up to 2
static float solidGlassSampleRadius(float refraction, float frost) {
    const auto CLAMPED = std::clamp(refraction, 0.F, MAX_SOLIDGLASS_REFRACTION);
    const auto SOFT    = std::clamp(frost, 0.F, MAX_SOLIDGLASS_FROST);
    return CLAMPED > 0.F || SOFT > 0.F ? std::ceil((CLAMPED + SOFT) * 2.F + 1.F) : 0.F;
}

static float srgbToLinear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

static float solidGlassLuminanceScale(CRenderContext& ctx) {
    const auto INTERMEDIATE = getDefaultImageDescription();
    const auto WORKBUFFER   = g_pHyprRenderer->workBufferImageDescription(ctx);
    if (!WORKBUFFER)
        return 1.F;

    const auto MINIMUM = INTERMEDIATE->value().getTFMinLuminance();
    const auto MAXIMUM = INTERMEDIATE->value().getTFMaxLuminance();
    const auto RANGE   = std::max(MAXIMUM, sc<float>(WORKBUFFER->value().luminances.max)) - MINIMUM;
    return (MAXIMUM - MINIMUM) / std::max(RANGE, 0.001F);
}

CSolidGlassBlurProvider::CSolidGlassBlurProvider(CHyprOpenGLImpl& impl) : CDualKawaseBlurProvider(impl, makeUnique<CSolidGlassBlurMaterial>()) {
    ;
}

eBlurType CSolidGlassBlurMaterial::type() const noexcept {
    return eBlurType::BLUR_SOLID_GLASS;
}

SBlurMaterialRequirements CSolidGlassBlurMaterial::requirements() const noexcept {
    return {
        .finishFragment = SH_FRAG_SOLIDGLASSFINISH,
        .preparedInput  = true,
        .liveBlur       = true,
    };
}

float CSolidGlassBlurMaterial::sampleRadius() const {
    static auto PSOLIDGLASSREFRACTION = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:refraction");
    static auto PSOLIDGLASSFROST      = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:frost");
    return solidGlassSampleRadius(*PSOLIDGLASSREFRACTION, *PSOLIDGLASSFROST);
}

static constexpr float MASK_ALPHA_THRESHOLD = 0.04F;

// px of field kept around the region extents, must cover the antialiasing band of the field
static constexpr int                                  FIELD_PADDING = 3;

std::optional<CSolidGlassBlurMaterial::SFieldBinding> CSolidGlassBlurMaterial::regionField(const SBlurShape& shape, float bulbPx) const {
    const auto SURFACE = shape.surface.lock();
    if (!SURFACE || !SURFACE->m_hasBackgroundEffect || SURFACE->m_blurRegion.empty())
        return std::nullopt;

    const Vector2D SIZE = {std::round(shape.box.width), std::round(shape.box.height)};
    if (SIZE.x < 1 || SIZE.y < 1 || SIZE.x > 16384 || SIZE.y > 16384)
        return std::nullopt;

    // CRegion::scale rounds every rect outwards, which closes 1 px moats at fractional scales: round each edge to
    // the nearest device px instead
    CRegion scaled;
    SURFACE->m_blurRegion.forEachRect([&](const pixman_box32_t& rect) {
        const auto X1 = std::round(rect.x1 * shape.surfaceScale);
        const auto Y1 = std::round(rect.y1 * shape.surfaceScale);
        const auto X2 = std::round(rect.x2 * shape.surfaceScale);
        const auto Y2 = std::round(rect.y2 * shape.surfaceScale);
        if (X2 > X1 && Y2 > Y1)
            scaled.add(CBox{X1, Y1, X2 - X1, Y2 - Y1});
    });
    scaled.intersect(CBox{0, 0, SIZE.x, SIZE.y});
    if (scaled.empty())
        return std::nullopt;

    // the field covers the region extents only and is keyed by the region relative to them, so a shape that
    // just moves (popup slide-in) keeps its field
    const auto EXTENTS = scaled.getExtents();
    scaled.translate(-EXTENTS.pos());

    // forget fields of dead surfaces
    std::erase_if(m_regionFields, [](const SRegionField& field) { return !field.surface.valid(); });

    auto it = std::ranges::find_if(m_regionFields, [&](const SRegionField& field) { return field.surface.lock() == SURFACE; });
    if (it == m_regionFields.end()) {
        m_regionFields.emplace_back(SRegionField{.surface = SURFACE});
        it = std::prev(m_regionFields.end());
    }

    const CBox FIELD_BOX = {shape.box.x + EXTENTS.x - FIELD_PADDING, shape.box.y + EXTENTS.y - FIELD_PADDING, EXTENTS.w + (2.0 * FIELD_PADDING), EXTENTS.h + (2.0 * FIELD_PADDING)};

    if (it->texture && it->bulb == bulbPx && pixman_region32_equal(it->region.pixman(), scaled.pixman()))
        return SFieldBinding{.texture = it->texture, .box = FIELD_BOX};

    const int            WIDTH  = sc<int>(FIELD_BOX.w);
    const int            HEIGHT = sc<int>(FIELD_BOX.h);
    std::vector<uint8_t> mask(sc<size_t>(WIDTH) * sc<size_t>(HEIGHT), 0);
    scaled.forEachRect([&](const pixman_box32_t& rect) {
        for (int y = std::max(rect.y1 + FIELD_PADDING, 0); y < std::min(rect.y2 + FIELD_PADDING, HEIGHT); ++y) {
            const auto ROW = mask.begin() + sc<std::ptrdiff_t>(sc<size_t>(y) * WIDTH);
            std::fill(ROW + std::max(rect.x1 + FIELD_PADDING, 0), ROW + std::min(rect.x2 + FIELD_PADDING, WIDTH), 1);
        }
    });

    const auto BUILD_START = std::chrono::steady_clock::now();
    auto       field       = buildSolidGlassField(mask, WIDTH, HEIGHT, bulbPx);
    BlurTiming::addCpu(std::format("field build {}x{}", WIDTH, HEIGHT), std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - BUILD_START).count());
    auto texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, field.pixels.data(), sc<uint32_t>(WIDTH) * 4, FIELD_BOX.size());
    if (!texture || !texture->ok())
        return std::nullopt;

    // setTexParameter acts on whatever is bound on the active unit (unit 2 here, see bindFinish).
    // Without these a fresh texture has a mipmap min filter, is incomplete and samples as opaque.
    // The field is sampled 1:1 and stores an angle, it must not be interpolated
    texture->bind();
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    texture->unbind();

    it->region  = std::move(scaled);
    it->bulb    = bulbPx;
    it->texture = texture;
    return SFieldBinding{.texture = texture, .box = FIELD_BOX};
}

// per shape three vec4: device px rect relative to the surface box, corner radii (tl, tr, br, bl), (parent, descendants, 0, 0)
void CSolidGlassBlurMaterial::packShapes(const std::vector<SSolidGlassShape>& shapes, float scale) const {
    m_shapeUniforms.clear();
    for (const auto& shape : shapes) {
        m_shapeUniforms.insert(m_shapeUniforms.end(),
                               {
                                   sc<float>(shape.box.x) * scale,
                                   sc<float>(shape.box.y) * scale,
                                   sc<float>(shape.box.w) * scale,
                                   sc<float>(shape.box.h) * scale,
                                   shape.radii[0] * scale,
                                   shape.radii[1] * scale,
                                   shape.radii[2] * scale,
                                   shape.radii[3] * scale,
                                   sc<float>(shape.parent),
                                   sc<float>(shape.descendants),
                                   0.F,
                                   0.F,
                               });
    }
}

void CSolidGlassBlurMaterial::bindFinish(CRenderContext& ctx, WP<CShader> shader, const SBlurMaterialContext& context) const {
    shader->setUniformInt(SHADER_SOLIDGLASS_ENABLED, 0);
    shader->setUniformInt(SHADER_SOLIDGLASS_MASK_MODE, 0);

    if (!context.blurContext.shape)
        return;

    const auto extent = context.blurContext.shape->box;
    if (extent.width <= 0 || extent.height <= 0)
        return;

    static auto PSOLIDGLASSREFRACTION = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:refraction");
    static auto PSOLIDGLASSBULB       = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:bulb");
    static auto PSOLIDGLASSCLARITY    = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:clarity");
    static auto PSOLIDGLASSABERRATION = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:aberration");
    static auto PSOLIDGLASSTINT       = CConfigValue<Config::INTEGER>("decoration:blur:solid_glass:tint");
    static auto PSOLIDGLASSDEPTH      = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:depth");
    static auto PSOLIDGLASSEDGECURVE  = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:edge_curve");
    static auto PSOLIDGLASSLIGHTANGLE = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:light_angle");
    static auto PSOLIDGLASSSPECINT    = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:specular_intensity");
    static auto PSOLIDGLASSSPECPOWER  = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:specular_power");
    static auto PSOLIDGLASSSHEEN      = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:sheen");
    static auto PSOLIDGLASSSATURATION = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:saturation");
    static auto PSOLIDGLASSFROST      = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:frost");
    static auto PSOLIDGLASSDARKENING  = CConfigValue<Config::FLOAT>("decoration:blur:solid_glass:darkening");

    const auto  TINT            = CHyprColor(*PSOLIDGLASSTINT);
    const auto  TINT_ALPHA      = std::clamp(sc<float>(TINT.a), 0.F, 1.F);
    const auto  TINT_DEPTH      = -std::log(std::max(1.F - TINT_ALPHA, 0.0001F));
    const auto  LUMINANCE_SCALE = solidGlassLuminanceScale(ctx);
    const auto  LIGHT_ANGLE_RAD = *PSOLIDGLASSLIGHTANGLE * (M_PI / 180.0);

    // sizes are configured in logical px
    const auto SCALE   = sc<float>(ctx.m_data.pMonitor->m_scale);
    const auto BULB_PX = std::clamp(*PSOLIDGLASSBULB, MIN_SOLIDGLASS_BULB, MAX_SOLIDGLASS_BULB) * SCALE;

    shader->setUniformInt(SHADER_SOLIDGLASS_ENABLED, 1);

    // the glass follows the declared blur region if the client has one (precomputed field), else the surface
    // alpha (ray marched), else the box. Texture creation binds on the active unit: keep that off units 0 and 1
    // (the blur inputs)
    glActiveTexture(GL_TEXTURE2);
    int          maskMode      = 0;
    float        maskThreshold = 0.5F;
    SP<ITexture> mask;
    CBox         maskBox   = extent;
    const auto   SURFACE   = context.blurContext.shape->surface.lock();
    const auto   FIT_START = BlurTiming::enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto*  GLASS     = SURFACE ? solidGlassShapeCache()->get(SURFACE) : nullptr;
    if (GLASS && BlurTiming::enabled())
        BlurTiming::addCpu(std::format("shape lookup/fit {} shapes", GLASS->shapes.size()),
                           std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - FIT_START).count());
    if (GLASS && GLASS->fitted) {
        // exact rounded rectangles, evaluated per pixel in the shader (no texture, nothing rebuilt on resize)
        maskMode = 3;
        packShapes(GLASS->shapes, context.blurContext.shape->surfaceScale);
        shader->setUniformInt(SHADER_SOLIDGLASS_SHAPE_COUNT, sc<int>(GLASS->shapes.size()));
        shader->setUniform4fv(SHADER_SOLIDGLASS_SHAPES, sc<GLsizei>(m_shapeUniforms.size() / 4), m_shapeUniforms);
        shader->setUniformInt(SHADER_SOLIDGLASS_MASK_MODE, maskMode);
    } else if (const auto FIELD = GLASS ? regionField(*context.blurContext.shape, BULB_PX) : std::nullopt; FIELD) {
        maskMode = 2;
        mask     = FIELD->texture;
        maskBox  = FIELD->box;
    } else {
        const auto& SURFTEX = context.blurContext.shape->surfaceTexture;
        if (SURFTEX && SURFTEX->ok() && SURFTEX->m_type == TEXTURE_RGBA && !SURFTEX->m_opaque && SURFTEX->m_texID) {
            maskMode      = 1;
            mask          = SURFTEX;
            maskThreshold = std::clamp(context.blurContext.shape->alphaThreshold, MASK_ALPHA_THRESHOLD, 0.9F);
        }
    }

    if (mask) {
        mask->bind();
        shader->setUniformInt(SHADER_SOLIDGLASS_MASK, 2);
        shader->setUniformInt(SHADER_SOLIDGLASS_MASK_MODE, maskMode);
        shader->setUniformFloat(SHADER_SOLIDGLASS_MASK_CUTOFF, maskThreshold);
        shader->setUniformFloat4(SHADER_SOLIDGLASS_MASK_BOX, sc<float>(maskBox.x), sc<float>(maskBox.y), sc<float>(maskBox.w), sc<float>(maskBox.h));
    }
    glActiveTexture(GL_TEXTURE0);

    shader->setUniformFloat4(SHADER_SOLIDGLASS_EXTENT, sc<float>(extent.x), sc<float>(extent.y), sc<float>(extent.width), sc<float>(extent.height));
    shader->setUniformFloat(SHADER_SOLIDGLASS_RADIUS, std::max(context.blurContext.shape->radius, 0.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_ROUNDING_POWER, std::max(context.blurContext.shape->roundingPower, 1.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_REFRACTION, std::clamp(*PSOLIDGLASSREFRACTION, 0.F, MAX_SOLIDGLASS_REFRACTION) * SCALE);
    shader->setUniformFloat(SHADER_SOLIDGLASS_BULB, BULB_PX);
    shader->setUniformFloat(SHADER_SOLIDGLASS_CLARITY, std::clamp(*PSOLIDGLASSCLARITY, 0.F, 1.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_ABERRATION, std::clamp(*PSOLIDGLASSABERRATION, 0.F, 0.25F));
    shader->setUniformFloat4(SHADER_SOLIDGLASS_TINT, srgbToLinear(sc<float>(TINT.r)) * LUMINANCE_SCALE, srgbToLinear(sc<float>(TINT.g)) * LUMINANCE_SCALE,
                             srgbToLinear(sc<float>(TINT.b)) * LUMINANCE_SCALE, TINT_DEPTH);
    shader->setUniformFloat(SHADER_SOLIDGLASS_STRENGTH, std::clamp(context.strength, 0.F, 1.F));
    shader->setUniformInt(SHADER_SOLIDGLASS_TRANSFER_FUNCTION, sc<int>(getDefaultImageDescription()->value().transferFunction));
    shader->setUniformFloat(SHADER_SOLIDGLASS_LUMINANCE_SCALE, LUMINANCE_SCALE);
    shader->setUniformFloat(SHADER_SOLIDGLASS_DEPTH, std::clamp(*PSOLIDGLASSDEPTH, 0.F, 3.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_EDGE_CURVE, std::clamp(*PSOLIDGLASSEDGECURVE, 0.2F, 5.F));
    // the blur framebuffer is y-down, the configured angle counts counter-clockwise from the right (90 = up)
    shader->setUniformFloat2(SHADER_SOLIDGLASS_LIGHT_DIR, sc<float>(std::cos(LIGHT_ANGLE_RAD)), -sc<float>(std::sin(LIGHT_ANGLE_RAD)));
    shader->setUniformFloat(SHADER_SOLIDGLASS_SPECULAR_INTENSITY, std::clamp(*PSOLIDGLASSSPECINT, 0.F, 3.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_SPECULAR_POWER, std::clamp(*PSOLIDGLASSSPECPOWER, 1.F, 200.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_SHEEN, std::clamp(*PSOLIDGLASSSHEEN, 0.F, 1.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_SATURATION, std::clamp(*PSOLIDGLASSSATURATION, 0.F, 3.F));
    shader->setUniformFloat(SHADER_SOLIDGLASS_FROST, std::clamp(*PSOLIDGLASSFROST, 0.F, MAX_SOLIDGLASS_FROST) * SCALE);
    shader->setUniformFloat(SHADER_SOLIDGLASS_DARKENING, std::clamp(*PSOLIDGLASSDARKENING, 0.F, 1.F));
}

float Render::GL::solidGlassDamageRadius(int64_t size, int64_t passes, float refraction, float frost) {
    return dualKawaseDamageRadius(size, passes) + solidGlassSampleRadius(refraction, frost);
}
