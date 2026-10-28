# Fork patch series

This branch is upstream `hyprwm/Hyprland` main (currently at 7a5e469c7, branch `glass-main-20261028`) plus the commits below, in this order. It is rebased onto upstream main every
release (`scripts/rebase-fork.sh`, which also builds and runs the tests). Rules for every patch: one feature per commit
with its tests, new code in fork-owned files, upstream files only get small additive hunks, every new option is optional
and probe-able so a config written for the fork still loads on stock Hyprland.

"Upstream files" are files that exist upstream (conflict risk on rebase). "Fork-owned" are files only the fork has.
Probe an option from Lua with `pcall(hl.get_config, "<name>")` (fails on builds without it) or `hyprctl -j getoption <name>`.
Rule effects (`glass`, `glass_border`) cannot be probed directly: probe the option named in their entry instead, and only
add the rule when it exists (an unknown rule field is a config error on stock).

## 1. blur: add solid_glass blur variant
- Why: Apple "liquid glass" style blur (sharp backdrop, lens edge, rim light). Option `decoration:blur:variant = "solid_glass"` (11) and
  `decoration:blur:solid_glass:*` (refraction, bulb, clarity, aberration, tint, depth, edge_curve, light_angle, specular_intensity,
  specular_power, sheen, saturation, frost, darkening).
- Upstream files: ConfigValues.cpp, Provider.hpp, Factory.cpp, Material.{hpp,cpp}, Kawase.cpp, Shader.{hpp,cpp}, ShaderLoader.hpp, OpenGL.cpp.
- Fork-owned: `src/render/gl/blur/SolidGlass.*`, `src/render/blur/SolidGlass{Field,Shapes}.*`, `solidglassfinish.frag`.
- Droppable: no, the base of everything below. Probe: `decoration:blur:solid_glass:refraction`.

## 2. render: solid_glass on layer surfaces with a background-effect region
- Why: glass per island of a layer's ext-background-effect region (Quickshell cards), blur work limited to the region.
- Upstream files: ElementRenderer.cpp, OpenGL.cpp, Pass.cpp, PassElement.{hpp,cpp}, SurfacePassElement.{hpp,cpp}. Fork-owned: none new.
- Droppable: not on its own (3 and 6 build on the layer path). No option.

## 3. blur: solid_glass layer_alpha_ref
- Why: the glass of a layer fades with the layer's own alpha (cards that fade out). Option `decoration:blur:solid_glass:layer_alpha_ref` (0 = off).
- Upstream files: ConfigValues.cpp, OpenGL.{hpp,cpp}, Shader.{hpp,cpp}, ShaderLoader.{hpp,cpp}, defines.h, surface.frag. Fork-owned: SolidGlass.*.
- Droppable: yes. Probe: `decoration:blur:solid_glass:layer_alpha_ref`.

## 4. tests: cover solid_glass options and shape fitting
- Upstream files: tests/config/BlurVariants.cpp. Fork-owned: tests/render/SolidGlassField.cpp. Droppable with 1.

## 5. render: draw live-blur windows after the others of their pass
- Why: what a live blur shows must not depend on focus / raise order; damage within a live blur's reach redraws it.
  Affects every live-blur variant, not only solid_glass.
- Upstream files: Renderer.cpp, Pass.cpp. Droppable: yes (on its own). No option.

## 6. blur: per-surface solid_glass settings via a "glass" rule
- Why: different glass per surface (thin bend on the bar, full lens on popups) without new global options.
- Layer and window rule effect `glass` (dynamic rule effect, registered in `Render::registerSolidGlassRuleEffects()` before the config loads):
  `hl.layer_rule({ match = { namespace = "qs-bar" }, glass = "depth:0.3 refraction:10" })`.
- String format: `key:value` pairs separated by spaces, commas or semicolons (`=` works instead of `:`). Keys are the
  `decoration:blur:solid_glass:` option names: refraction, bulb, clarity, aberration, depth, edge_curve, light_angle,
  specular_intensity, specular_power, sheen, saturation, frost, darkening (floats, clamped to the option's range) and tint
  (`0xAARRGGBB`). Unset keys use the global value. Unknown keys / bad values: warning in the log, skipped. Last matching rule wins
  (no merging of two rules' strings).
- Probe: `decoration:blur:solid_glass:per_surface` (bool, default true; false ignores every override).
- Upstream files: ConfigManager.cpp (+3: the registration call), ConfigValues.cpp (+2). Fork-owned: `src/render/blur/SolidGlassOverride.*`,
  SolidGlass.cpp (params resolved per draw, reach / kawase passes cover the parsed overrides), tests/render/SolidGlassOverride.cpp.
- Droppable: yes (7 and 8 build on its parser; drop them with it).

## 7. render: group:groupbar:solid_glass for the groupbar tabs
- Why: the group bar pills get their own glass look. Option `group:groupbar:solid_glass` (string, same format as 6, empty = globals);
  applies to the tab glass when `group:groupbar:blur` is on.
- Mechanism: a blur role (`Render::eBlurRole`, `BLUR_ROLE_GROUPBAR`) carried from the rect / texture pass elements into `SBlurShape::role`.
- Probe: `group:groupbar:solid_glass`.
- Upstream files (+18): Provider.hpp (role field + enum), RectPassElement.hpp, TexPassElement.hpp, OpenGL.hpp (one field each),
  GLElementRenderer.cpp, OpenGL.cpp, ElementRenderer.cpp (pass-through), CHyprGroupBarDecoration.cpp (2 lines), ConfigValues.cpp (+2).
  Fork-owned: SolidGlassOverride.cpp (role lookup). Droppable: yes.

## 8. render: solid_glass window-frame glass under borders (branch `feat/window-frame-glass`, separate on purpose)
- Why: a glass rim around windows. Option `decoration:blur:solid_glass:border` (string: empty / "off" = off, "on" = global look,
  or overrides in the format of 6) and window rule effect `glass_border` (same values, wins over the option). solid_glass only.
- Mechanism: the border decoration adds a transparent blurred rect over the outer border box (`BLUR_ROLE_BORDER`) with the window box
  as `blurHole` (excluded from blur work and drawing), so only the ring gets glass; the lens is the outer box's, the ring shows its rim.
- Probe: `decoration:blur:solid_glass:border`.
- Upstream files (+26): CHyprBorderDecoration.cpp (+17, in `draw()`), OpenGL.cpp (+2), OpenGL.hpp, RectPassElement.hpp, Provider.hpp (+1 each),
  GLElementRenderer.cpp, ConfigValues.cpp (+2). Fork-owned: SolidGlassOverride.*. Droppable: yes, independently of everything but 6/7.
- Caveat: the hole is the window's square box, the ring's inner edge is not rounded; border decoration code is touched often upstream.
