#version 300 es
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable

precision highp float;

in vec2 v_texcoord;
uniform sampler2D tex;
uniform sampler2D sharpTex;

uniform int solidGlassEnabled;
uniform vec4 solidGlassExtent;
uniform float solidGlassRadius;
uniform float solidGlassRoundingPower;
uniform float solidGlassRefraction;
uniform float solidGlassBulb;
uniform float solidGlassClarity;
uniform float solidGlassAberration;
uniform vec4 solidGlassTint;
uniform float solidGlassStrength;
uniform int solidGlassTransferFunction;
uniform float solidGlassLuminanceScale;
uniform float solidGlassDepth;
uniform float solidGlassEdgeCurve;
uniform vec2 solidGlassLightDir;
uniform float solidGlassSpecularIntensity;
uniform float solidGlassSpecularPower;
uniform float solidGlassSheen;
uniform float solidGlassSaturation;
uniform float solidGlassDarkening;
uniform float solidGlassFrost;
// 0: rounded box, 1: surface alpha (ray marched), 2: precomputed distance field (declared blur region, fallback),
// 3: list of rounded rectangles fitted to the declared blur region (see SolidGlassShapes.hpp)
uniform int solidGlassMaskMode;
uniform sampler2D solidGlassMask;
uniform float solidGlassMaskThreshold;
// where the mask texture lies in the framebuffer (the surface box for alpha, the field box for regions)
uniform vec4 solidGlassMaskBox;
// mode 3: per shape three vec4: rect (x, y, w, h) in device px relative to solidGlassExtent.xy, corner radii
// (top left, top right, bottom right, bottom left), (parent, descendants, 0, 0). Depth first order: the shapes nested
// in a shape directly follow it, `descendants` of them
#define MAX_SHAPES 48
uniform int solidGlassShapeCount;
uniform vec4 solidGlassShapes[MAX_SHAPES * 3];
uniform float noise;
uniform float brightness;

#include "defines.h"
#if USE_CM
uniform int sourceTF;
uniform int targetTF;
#include "CM.glsl"
#endif

#include "cm_helpers.glsl"
#include "blurFinish.glsl"

layout(location = 0) out vec4 fragColor;

// one device pixel in uv, taken in main() where control flow is still uniform
vec2 texelX;
vec2 texelY;

struct SGlassShape {
    float coverage; // 0..1, antialiased boundary
    float depth;    // px from the boundary, drives the bend (may follow a smoothed lens outline)
    float rimDepth; // px from the true boundary, drives rim and inner shadow
    vec2  outward;  // outward normal of the lens, device px space
    float bulb;     // px width of the curved edge for this shape
};

float solidGlassLength(vec2 value, float power) {
    value = abs(value);
    // normalize before pow() so a high rounding power cannot overflow to inf
    float scale = max(max(value.x, value.y), 1.0);
    vec2 normalized = value / scale;
    return pow(pow(normalized.x, power) + pow(normalized.y, power), 1.0 / power) * scale;
}

// signed distance to a rounded box with superellipse corners (exact for power 2) and its gradient (outward normal)
float roundedBoxSDF(vec2 position, vec2 halfSize, float radius, float power, out vec2 gradient) {
    radius = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
    vec2 offset = abs(position) - (halfSize - vec2(radius));
    vec2 signs = vec2(position.x < 0.0 ? -1.0 : 1.0, position.y < 0.0 ? -1.0 : 1.0);
    if (offset.x > 0.0 || offset.y > 0.0) {
        vec2 outside = max(offset, vec2(0.0));
        float cornerDistance = solidGlassLength(outside, power);
        // d/dv (x^p + y^p)^(1/p) = (v / length)^(p - 1)
        vec2 slope = pow(max(outside / max(cornerDistance, 0.0001), vec2(0.000001)), vec2(power - 1.0));
        gradient = signs * slope / max(length(slope), 0.0001);
        return cornerDistance + min(max(offset.x, offset.y), 0.0) - radius;
    }
    gradient = offset.x > offset.y ? vec2(signs.x, 0.0) : vec2(0.0, signs.y);
    return max(offset.x, offset.y) - radius;
}

// the same for circular corners (power 2), without pow(): the shape list path
float circularBoxSDF(vec2 position, vec2 halfSize, float radius, out vec2 gradient) {
    radius = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
    vec2 offset = abs(position) - (halfSize - vec2(radius));
    vec2 signs = vec2(position.x < 0.0 ? -1.0 : 1.0, position.y < 0.0 ? -1.0 : 1.0);
    if (offset.x > 0.0 || offset.y > 0.0) {
        vec2 outside = max(offset, vec2(0.0));
        float cornerDistance = length(outside);
        gradient = signs * outside / max(cornerDistance, 0.0001);
        return cornerDistance + min(max(offset.x, offset.y), 0.0) - radius;
    }
    gradient = offset.x > offset.y ? vec2(signs.x, 0.0) : vec2(0.0, signs.y);
    return max(offset.x, offset.y) - radius;
}

float circularBoxSDF(vec2 position, vec2 halfSize, float radius) {
    radius = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
    vec2 offset = abs(position) - (halfSize - vec2(radius));
    return length(max(offset, vec2(0.0))) + min(max(offset.x, offset.y), 0.0) - radius;
}

float smootherstep(float edge0, float edge1, float value) {
    float progress = clamp((value - edge0) / max(edge1 - edge0, 0.0001), 0.0, 1.0);
    return progress * progress * progress * (progress * (progress * 6.0 - 15.0) + 10.0);
}

vec3 solidGlassSample(sampler2D source, vec2 uv) {
    vec4 color = texture(source, uv);
    return toLinearRGB(color.rgb / max(color.a, 0.001), solidGlassTransferFunction);
}

// ---- shape: rounded box (windows, layers without a usable mask, every shape of the mode 3 list) ----

SGlassShape solidGlassRoundedShape(vec2 position, vec2 halfSize, float radius, float power, bool circular) {
    SGlassShape s;
    float minimumHalfSize = min(halfSize.x, halfSize.y);
    float boundaryRadius = clamp(radius, 0.0, minimumHalfSize);
    s.bulb = clamp(solidGlassBulb, 1.0, max(minimumHalfSize, 1.0));

    // exact distance: 1 px antialiased outline, rim
    vec2 unused;
    float boundarySdf = circular ? circularBoxSDF(position, halfSize, boundaryRadius) : roundedBoxSDF(position, halfSize, boundaryRadius, power, unused);
    s.coverage = clamp(0.5 - boundarySdf, 0.0, 1.0);
    s.rimDepth = max(-boundarySdf, 0.0);

    // The bend follows the true outline offset inwards (radius r - d at depth d, so the lens hugs the corner). Past
    // d = r the inner iso-lines meet on the corner diagonal, which would crease the refracted backdrop: there the
    // normal and depth use a smooth max with a width of a few px that grows from 0 at d = r. The rim keeps the
    // true edge.
    float lensSdf = circular ? circularBoxSDF(position, halfSize, boundaryRadius, s.outward) : roundedBoxSDF(position, halfSize, boundaryRadius, power, s.outward);
    s.depth = max(-lensSdf, 0.0);

    vec2 offset = abs(position) - (halfSize - vec2(boundaryRadius));
    float hard = max(offset.x, offset.y);
    if (hard < 0.0) {
        float width = clamp(s.bulb * 0.2, 2.0, 10.0) * smoothstep(0.0, 8.0, -hard);
        width = max(width, 0.0001);
        float t = clamp(0.5 + 0.5 * (offset.x - offset.y) / width, 0.0, 1.0);
        vec2 signs = vec2(position.x < 0.0 ? -1.0 : 1.0, position.y < 0.0 ? -1.0 : 1.0);
        // not renormalized: the vector shrinks to 0.71 on the diagonal, which softens the fold of the bend there
        s.outward = mix(vec2(0.0, signs.y), vec2(signs.x, 0.0), t);
        s.depth = max(-(mix(offset.y, offset.x, t) + width * t * (1.0 - t) - boundaryRadius), 0.0);
    }
    return s;
}

bool solidGlassBoxShape(out SGlassShape s) {
    vec2 halfSize = solidGlassExtent.zw * 0.5;
    vec2 position = gl_FragCoord.xy - (solidGlassExtent.xy + halfSize);
    s = solidGlassRoundedShape(position, halfSize, solidGlassRadius, clamp(solidGlassRoundingPower, 1.0, 10.0), false);
    return s.coverage > 0.0;
}

// ---- shape: list of rounded rectangles (declared blur region) ----

float solidGlassCornerRadius(vec2 position, vec4 radii) {
    if (position.y < 0.0)
        return position.x < 0.0 ? radii.x : radii.y;
    return position.x < 0.0 ? radii.w : radii.z;
}

SGlassShape solidGlassListShape(int index) {
    vec4 rect = solidGlassShapes[index * 3];
    vec2 halfSize = rect.zw * 0.5;
    vec2 position = gl_FragCoord.xy - (solidGlassExtent.xy + rect.xy + halfSize);
    return solidGlassRoundedShape(position, halfSize, solidGlassCornerRadius(position, solidGlassShapes[index * 3 + 1]), 2.0, true);
}

// the innermost shape whose antialiased outline reaches this pixel, -1 if none. A shape whose box (+1 px) misses
// the pixel is skipped with everything nested in it, so a pixel costs one box test per shape around it plus its
// siblings, not one per shape
int solidGlassFindShape() {
    vec2 position = gl_FragCoord.xy - solidGlassExtent.xy;
    int found = -1;
    int i = 0;
    for (int guard = 0; guard < MAX_SHAPES; guard++) {
        if (i >= solidGlassShapeCount)
            break;
        vec4 rect = solidGlassShapes[i * 3];
        vec2 local = position - rect.xy;
        if (any(lessThan(local, vec2(-1.0))) || any(greaterThan(local, rect.zw + vec2(1.0)))) {
            i += 1 + int(solidGlassShapes[i * 3 + 2].y + 0.5);
            continue;
        }
        vec2 halfSize = rect.zw * 0.5;
        vec2 centered = local - halfSize;
        if (circularBoxSDF(centered, halfSize, solidGlassCornerRadius(centered, solidGlassShapes[i * 3 + 1])) < 0.5)
            found = i;
        i++;
    }
    return found;
}

// ---- shape: precomputed distance field (rasterized declared blur region, see SolidGlassField.hpp) ----
// r: exact signed depth, [-2, 30] px. g: outward normal angle. b: smoothed depth / bulb. a: bulb px, 0 = no glass

bool solidGlassFieldShape(out SGlassShape s) {
    vec2 uv = (gl_FragCoord.xy - solidGlassMaskBox.xy) / solidGlassMaskBox.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return false;
    vec4 field = textureLod(solidGlassMask, uv, 0.0);
    float bulb = field.a * 255.0;
    s.rimDepth = field.r * 32.0 - 2.0;
    s.coverage = bulb < 0.5 ? 0.0 : clamp(s.rimDepth + 0.5, 0.0, 1.0);
    s.rimDepth = max(s.rimDepth, 0.0);
    s.bulb = max(bulb, 1.0);
    s.depth = field.b * s.bulb;
    float angle = field.g * (255.0 / 256.0) * 6.2831853 - 3.1415927;
    s.outward = vec2(cos(angle), sin(angle));
    return s.coverage > 0.0;
}

// ---- shape: surface alpha (no declared region), ray marched per pixel ----

#define MASK_DIRS 8
#define MASK_STEPS 6
#define MASK_REFINE 2

float solidGlassMaskAt(vec2 position) {
    vec2 uv = (position - solidGlassMaskBox.xy) / solidGlassMaskBox.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 0.0;
    return textureLod(solidGlassMask, uv, 0.0).a;
}

// 8 rays with 6 quadratically spaced taps each, then 2 bisection taps on the crossing (at most 8 * 8 taps)
// give the edge distance, the outward normal and the local half thickness of the shape
bool solidGlassAlphaShape(out SGlassShape s) {
    float threshold = solidGlassMaskThreshold;
    float reach = min(solidGlassBulb, 48.0) + 1.0;
    vec2 position = gl_FragCoord.xy;
    float center = solidGlassMaskAt(position);
    s.coverage = 0.0;
    s.depth = reach;
    s.rimDepth = reach;
    s.outward = vec2(0.0, -1.0);
    s.bulb = max(solidGlassBulb, 1.0);
    if (center < threshold)
        return false;

    const vec2 dirs[MASK_DIRS] = vec2[MASK_DIRS](vec2(1.0, 0.0), vec2(0.70710678, 0.70710678), vec2(0.0, 1.0), vec2(-0.70710678, 0.70710678),
                                                 vec2(-1.0, 0.0), vec2(-0.70710678, -0.70710678), vec2(0.0, -1.0), vec2(0.70710678, -0.70710678));
    float crossing[MASK_DIRS];
    for (int k = 0; k < MASK_DIRS; k++) {
        float previousRadius = 0.0;
        crossing[k] = reach;
        for (int i = 1; i <= MASK_STEPS; i++) {
            float t = float(i) / float(MASK_STEPS);
            float radius = 1.0 + (reach - 1.0) * t * t;
            if (solidGlassMaskAt(position + dirs[k] * radius) < threshold) {
                float low = previousRadius;
                float high = radius;
                for (int b = 0; b < MASK_REFINE; b++) {
                    float middle = 0.5 * (low + high);
                    if (solidGlassMaskAt(position + dirs[k] * middle) < threshold)
                        high = middle;
                    else
                        low = middle;
                }
                crossing[k] = 0.5 * (low + high);
                break;
            }
            previousRadius = radius;
        }
    }

    vec2 accumulated = vec2(0.0);
    for (int k = 0; k < MASK_DIRS; k++) {
        float weight = 1.0 - crossing[k] / reach;
        accumulated += dirs[k] * weight * weight;
    }
    float accumulatedLength = length(accumulated);
    float distancePx = reach;
    if (accumulatedLength > 0.0001) {
        s.outward = accumulated / accumulatedLength;
        // soft-min of the crossings projected on the normal, a plain min creases at convex corners
        float smallest = reach;
        for (int k = 0; k < MASK_DIRS; k++) {
            float projected = crossing[k] * dot(dirs[k], s.outward);
            if (crossing[k] < reach && projected > 0.0)
                smallest = min(smallest, projected);
        }
        float softness = max(reach * 0.15, 1.0);
        float sum = 0.0;
        float count = 0.0;
        for (int k = 0; k < MASK_DIRS; k++) {
            float projected = crossing[k] * dot(dirs[k], s.outward);
            if (crossing[k] < reach && projected > 0.0) {
                sum += exp(-(projected - smallest) / softness);
                count += 1.0;
            }
        }
        distancePx = count > 0.0 ? smallest - softness * log(sum / count) : smallest;
    }

    float thinnest = 2.0 * reach;
    for (int k = 0; k < MASK_DIRS / 2; k++)
        thinnest = min(thinnest, crossing[k] + crossing[k + MASK_DIRS / 2]);

    s.bulb = clamp(solidGlassBulb, 1.0, max(thinnest * 0.5, 1.0));
    s.depth = max(distancePx, 0.0);
    s.rimDepth = s.depth;
    s.coverage = smoothstep(threshold, min(threshold * 2.0 + 0.1, 1.0), center);
    return s.coverage > 0.0;
}

// ---- backdrop ----

// the sharp backdrop softened by a small fixed kernel (13 taps on two hex rings, radius in device px),
// independent of the dual kawase size and passes
vec3 solidGlassFrostSample(vec2 uv, vec2 minimumUV, vec2 maximumUV) {
    vec3 sum = solidGlassSample(sharpTex, clamp(uv, minimumUV, maximumUV));
    if (solidGlassFrost <= 0.05)
        return sum;

    sum *= 2.0;
    for (int i = 0; i < 6; i++) {
        float angle = float(i) * 1.0471976 + 0.5235988;
        vec2 dir = vec2(cos(angle), sin(angle));
        vec2 inner = dir * solidGlassFrost * 0.5;
        vec2 outer = dir * solidGlassFrost;
        sum += solidGlassSample(sharpTex, clamp(uv + inner.x * texelX + inner.y * texelY, minimumUV, maximumUV));
        sum += solidGlassSample(sharpTex, clamp(uv + outer.x * texelX + outer.y * texelY, minimumUV, maximumUV)) * 0.5;
    }
    // weights: 2 + 6 * 1 + 6 * 0.5
    return sum / 11.0;
}

// what a shape does to the pixel, everything but the backdrop taps. Kept apart so the outline of a nested shape can
// blend its optics with the ones of the shape around it (one set of backdrop taps for both)
struct SGlassOptics {
    vec2  displacement; // px, where the backdrop is taken from
    float lens;         // 0..1 lens profile, thickens the tint towards the rim
    float highlight;
    float shadow;
};

SGlassOptics solidGlassOptics(SGlassShape s, float effect) {
    SGlassOptics o;

    // convex lens profile: strongest bend at the boundary, nothing at the inner end of the bulb.
    // edge_curve reshapes the fall-off
    float progress = clamp(s.depth / s.bulb, 0.0, 1.0);
    float edge = 1.0 - progress;
    o.lens = pow(1.0 - sqrt(max(1.0 - edge * edge, 0.0)), solidGlassEdgeCurve);

    // the rim shows backdrop pulled in from further inside, like a glass bead. Thin shapes (chips) get a
    // gentler bend that never reaches across their middle
    float thin = clamp(s.bulb / max(solidGlassBulb, 1.0), 0.0, 1.0);
    float bend = min(solidGlassRefraction * solidGlassDepth * sqrt(thin), s.bulb * 1.5) * o.lens * effect;
    o.displacement = -s.outward * bend;

    // lighting: a thin specular rim lit from solidGlassLightDir, a softer counter highlight on the opposite
    // side, a broad sheen on the lit side and an inner edge shadow. All of it follows continuous distances,
    // so it is as smooth as the outline
    vec2 lightDir = normalize(solidGlassLightDir + vec2(0.00001, 0.0));
    float facing = dot(s.outward, lightDir);
    float rimWidth = clamp(s.bulb * 0.04, 1.2, 3.0);
    float rim = exp(-s.rimDepth / rimWidth);
    float softRim = exp(-s.depth / max(s.bulb * 0.3, 1.0));
    float exponent = max(solidGlassSpecularPower / 24.0, 0.5);
    float key = pow(max(facing, 0.0), exponent);
    float counter = pow(max(-facing, 0.0), exponent);
    float highlight = rim * (0.25 + 1.6 * key + 0.7 * counter) + softRim * (0.16 * key + 0.06 * counter);
    highlight += solidGlassSheen * softRim * smootherstep(-0.2, 0.9, facing) * 1.5;
    o.highlight = highlight * solidGlassSpecularIntensity * effect;
    o.shadow = softRim * (1.0 - rim) * (0.12 + 0.12 * max(-facing, 0.0)) * solidGlassSpecularIntensity * effect;
    return o;
}

SGlassOptics solidGlassMixOptics(SGlassOptics a, SGlassOptics b, float t) {
    SGlassOptics o;
    o.displacement = mix(a.displacement, b.displacement, t);
    o.lens = mix(a.lens, b.lens, t);
    o.highlight = mix(a.highlight, b.highlight, t);
    o.shadow = mix(a.shadow, b.shadow, t);
    return o;
}

// declared regions (modes 2, 3) draw only the glass: outside of it the output is transparent (premultiplied),
// so the antialiased outline blends with the real backdrop instead of the clip of the blur
float outputCoverage = 1.0;

vec4 applySolidGlass(vec4 blurred) {
    float effect = clamp(solidGlassStrength, 0.0, 1.0);
    bool regionShape = solidGlassMaskMode == 2 || solidGlassMaskMode == 3;
    if (regionShape)
        outputCoverage = 0.0;
    if (effect <= 0.0) {
        outputCoverage = 1.0;
        return blurred;
    }

    SGlassShape s;
    SGlassOptics optics;
    float coverage = 0.0;
    if (solidGlassMaskMode == 3) {
        int index = solidGlassFindShape();
        if (index < 0)
            return blurred;
        s = solidGlassListShape(index);
        optics = solidGlassOptics(s, effect);
        coverage = s.coverage;

        // the outline of a nested shape fades into the shape around it
        int parent = int(floor(solidGlassShapes[index * 3 + 2].x + 0.5));
        if (parent >= 0 && s.coverage < 1.0) {
            SGlassShape outer = solidGlassListShape(parent);
            optics = solidGlassMixOptics(solidGlassOptics(outer, effect), optics, s.coverage);
            coverage = max(coverage, outer.coverage);
        }
    } else {
        bool inside = false;
        if (solidGlassMaskMode == 2)
            inside = solidGlassFieldShape(s);
        else if (solidGlassMaskMode == 1)
            inside = solidGlassAlphaShape(s);
        else
            inside = solidGlassBoxShape(s);
        if (!inside)
            return blurred;
        optics = solidGlassOptics(s, effect);
        coverage = s.coverage;
    }

    vec2 sourceSize = vec2(textureSize(tex, 0));
    vec2 halfTexel = 0.5 / sourceSize;
    vec2 maximumUV = vec2(1.0) - halfTexel;

    vec2 displacementUV = optics.displacement.x * texelX + optics.displacement.y * texelY;
    vec2 uv = clamp(v_texcoord + displacementUV, halfTexel, maximumUV);

    // the interior shows the (lightly frosted) sharp backdrop; clarity < 1 mixes the big blur back in
    float sharpMix = clamp(solidGlassClarity, 0.0, 1.0);
    vec3 glass = solidGlassFrostSample(uv, halfTexel, maximumUV);
    if (sharpMix < 0.999)
        glass = mix(solidGlassSample(tex, uv), glass, sharpMix);

    // chromatic split: red and blue are refracted a bit more / less. Only the high frequency difference of
    // the sharp backdrop is taken from the shifted taps, so this costs 3 taps instead of 2 more kernels
    float aberration = clamp(solidGlassAberration, 0.0, 0.25);
    if (aberration > 0.0001 && dot(optics.displacement, optics.displacement) > 0.0025) {
        vec2 redUV = clamp(v_texcoord + displacementUV * (1.0 + aberration), halfTexel, maximumUV);
        vec2 blueUV = clamp(v_texcoord + displacementUV * (1.0 - aberration), halfTexel, maximumUV);
        vec3 base = solidGlassSample(sharpTex, uv);
        glass.r += solidGlassSample(sharpTex, redUV).r - base.r;
        glass.b += solidGlassSample(sharpTex, blueUV).b - base.b;
        glass = max(glass, vec3(0.0));
    }

    float luma = dot(glass, vec3(0.2126, 0.7152, 0.0722));
    glass = mix(vec3(luma), glass, solidGlassSaturation);

    // tint / absorption, a little denser towards the thicker rim
    float transmission = exp(-solidGlassTint.a * effect * mix(0.6, 1.0, optics.lens));
    glass = glass * transmission + solidGlassTint.rgb * (1.0 - transmission);

    // darken the backdrop seen through the glass
    glass *= 1.0 - clamp(solidGlassDarkening, 0.0, 1.0) * effect;

    glass *= 1.0 - optics.shadow;
    glass += vec3(solidGlassLuminanceScale) * optics.highlight;

    float outputAlpha = blurred.a;
    vec4 solidGlass = fromLinear(vec4(max(glass, vec3(0.0)) * outputAlpha, outputAlpha), solidGlassTransferFunction);
    if (regionShape) {
        outputCoverage = coverage;
        return solidGlass;
    }
    return mix(blurred, solidGlass, coverage);
}

void main() {
    texelX = dFdx(v_texcoord);
    texelY = dFdy(v_texcoord);

    vec4 color = texture(tex, v_texcoord);
    if (solidGlassEnabled != 0)
        color = applySolidGlass(color);

    fragColor = blurFinish(color, v_texcoord, noise, brightness
#if USE_CM
                           ,
                           sourceTF, targetTF, convertMatrix, srcTFRange, dstTFRange
#endif
                           ) * outputCoverage;
}
