# The checks `check_profile_physics` does not do

`check_profile_physics` runs eight checks: flow ceiling, temperature-vs-flow,
layer-height ratio, line-width ratio, retraction length, fan ordering, first-layer
height, first-layer temperature. Everything below is outside it. A profile can
return `verdict: "ok"` and still be wrong in every way this file describes.

Two things about its output before you rely on it:

- **Read `detail`, not `verdict`.** Six of the eight checks emit `warn` when data
  is *missing* rather than staying silent, so `ok` is rare and `warnings` is often
  noise about a key it could not parse.
- **It reads the first element of any vector** and strips a trailing `%` without
  converting it. A `line_width` of `"105%"` is evaluated as 105.

---

## Client-side pre-validation — the errors whose messages never reach you

A slice that fails `Print::validate()` returns a **generic 422**
(`{"error": "slice_not_started"}`); the specific message goes to the GUI
notification manager and is lost. Check these yourself before writing, because
the slicer will not tell you which one you tripped.

| Rule | Hard error when |
|---|---|
| Layer height vs nozzle | `layer_height > nozzle_diameter` |
| First layer vs nozzle | `initial_layer_print_height > nozzle_diameter` |
| Line width floor | any `*_line_width <= layer_height` → "Line width too small" |
| Line width ceiling | any `*_line_width > nozzle_diameter * 5` → "Line width too large" |
| Bridge width | `bridge_line_width > nozzle_diameter`; or `<= layer_height` unless both `thick_bridges` and `thick_internal_bridges` are on |
| Bed compatibility | selected `curr_bed_type`'s temp key is 0 for a used filament |
| Prime tower | requires `use_relative_e_distances`; and all objects sharing `layer_height`, `initial_layer_print_height`, `raft_layers` |
| Organic tree | `tree_support_tip_diameter < extrusion_width`, or `tree_support_branch_diameter_organic < 2x extrusion_width`, or branch < tip |
| Multi-filament temp | each used filament's `nozzle_temperature` must sit inside every other's `[nozzle_temperature_range_low, _high]` |

`Print::validate()`'s **warnings** never reach the API at all. These are silent:
jerk and acceleration exceeding `machine_max_*[0]`, mismatched `filament_shrink`,
and `precise_outer_wall` being ignored — which happens for the **outer-inner** and
**inner-outer-inner** wall sequences, i.e. everything except `wall_sequence ==
InnerOuter`. Check them against the config yourself.

Two things nothing validates anywhere:

- **Speed against `machine_max_speed_x/y`** — the check is commented out in
  `Print.cpp`. `outer_wall_speed = 2000` is accepted silently.
- **`filament_max_volumetric_speed`** — never validated, only *clamped* at G-code
  generation. An over-ambitious speed produces a slower print and no diagnostic.
  This is what the breakdown's `clamped` verdict catches after the fact.

---

## Supports

Nothing in the gate touches supports. This is usually the largest single source of
visible damage on a finished part.

**Diagnose the mark before changing anything.** Torn or fused witness marks and
scalloped or dimpled ones want opposite changes:

| Symptom | Cause | Direction |
|---|---|---|
| Support welded to the model, tears the surface | interface too close or too dense | raise `support_top_z_distance` toward one layer height; raise `support_interface_spacing`; lower `support_interface_flow_ratio` |
| Sagging, drooping, scalloped underside | interface too sparse or too far | lower `support_interface_spacing`; add `support_interface_top_layers`; **lower** the z distance |

Keys that matter, roughly in order of leverage:

- `support_type` — `tree(auto)` conforms to organic and oblique surfaces far
  better than grid and leaves smaller witness marks
- `support_style` — `snug` reduces scarring versus `grid`; `organic` for tree
- `support_top_z_distance` / `support_bottom_z_distance` — the removal-vs-sag
  trade, in layer-height units
- `support_object_xy_distance` — a full nozzle width keeps supports off vertical
  walls; too small and they weld
- `support_interface_top_layers` / `support_interface_spacing` — the surface the
  part actually rests on
- `support_threshold_angle` — below roughly 55° from vertical most geometry needs
  no support at all
- `support_on_build_plate_only`, `support_critical_regions_only` — cheapest way to
  stop supporting things that do not need it

**`support_interface_flow_ratio` silently does nothing unless
`set_other_flow_ratios` is enabled.** The GUI greys the field; through the API you
can write a value that never reaches the G-code. Before flipping that flag, read
the other ratios it ungates (`first_layer_flow_ratio`, `outer_wall_flow_ratio`,
`inner_wall_flow_ratio`, `overhang_flow_ratio`, `sparse_infill_flow_ratio`,
`internal_solid_infill_flow_ratio`, `gap_fill_flow_ratio`, `support_flow_ratio`) —
if any is not 1.0, enabling it changes more than you intended.

Verify support placement with `render_plate(view="preview")`. Support is visually
distinct there. It is the only way to see where it actually landed.

## Stringing

The gate checks `retraction_length` and nothing else in this axis.

Ranked by likelihood, from the shipped knowledge:

1. **Melt too hot** — drop `nozzle_temperature` in 5-10 °C steps. Usually the
   cause, and it is checked by nothing
2. `retraction_length` +0.2-0.5 mm per step; `retraction_speed` toward 30-45 mm/s.
   Sane ranges: **0.2-2.0 mm direct drive, 2-7 mm Bowden** — above 3 mm on direct
   drive is a smell
3. `wipe` enabled, with `wipe_distance`. `retract_before_wipe` at 100% means the
   retraction completes before the wipe, so the wipe drags a depressurized nozzle
   instead of scraping the bead
4. `travel_speed` too slow — the nozzle spends longer over open air
5. Wet filament. No setting fixes this; drying the spool is the only fix, and it
   should be the first suspicion when nothing else responds

`z_hop` does not reduce ooze. It stops existing ooze being smeared into the part —
a complement to retraction tuning, never a substitute.

There is **no coasting setting** in OrcaSlicer. Do not go looking for one.

## Overhangs and bridging

- `overhang_fan_speed` with `enable_overhang_bridge_fan` is usually the single
  highest-leverage fix, and the gate never looks at it
- `overhang_1_4_speed` … `overhang_4_4_speed` are percentages of
  `outer_wall_speed`; `0` means "use wall speed"
- `bridge_speed`, `bridge_flow`, `internal_bridge_speed` (a ratio over
  `bridge_speed`, default 150%)
- Melt too hot makes overhangs droop; `outer_wall_speed` too high does the same
- Past roughly 55-60° from vertical, no amount of tuning substitutes for support

Note the gate models `bridge_speed` and `gap_infill_speed` using the **generic**
`line_width`, not a bridge-specific one, so its bridge flow figure is approximate.

## Cooling

`cooling_sanity` only checks that `fan_min_speed <= fan_max_speed` and warns below
**3 s** of `slow_down_layer_time`. The shipped knowledge puts the floor at **8 s**
for small or thin-walled parts, and small-feature prints want it well above that.
Resolve in favour of 8.

Heat load scales with deposited *volume*, so layer height and feature size drive
cooling need as much as speed does. ABS, ASA, PC and PA want low fan — often near
zero — except on isolated overhangs and bridges, where `overhang_fan_speed`
applies as a feature-triggered override independent of the layer-time ramp.

## Surface finish, seam, and motion fidelity

Nothing here is gated at all.

- `outer_wall_speed` conventionally sits at **60-70% of `inner_wall_speed`**. When
  the flow ceiling clamps both to the same real speed, the outer wall gets no
  quality benefit at all — this is common and invisible without the breakdown
- `outer_wall_acceleration` is the ringing lever. On Klipper, input shaping is the
  real fix; reducing acceleration is a workaround
- `seam_position` places the mark, it does not remove it. `aligned` or `back` to
  hide it; `wipe` and tuned retraction to shrink it
- `top_surface_line_width` slightly narrower than `line_width` overlaps more per
  pass; `top_surface_speed` well below infill speed

## Strength floors

For a load-bearing part, treat **3 `wall_loops`, 4 `top_shell_layers`, 4
`bottom_shell_layers`** as the functional minimum. Walls carry load; infill mostly
resists crushing — going from 2 to 3 walls adds more continuous load-bearing
material than +15 percentage points of infill density.

`sparse_infill_pattern` matters for direction. Directional and weak in Z:
rectilinear, grid, triangles, tri-hexagon, honeycomb, lightning, concentric.
Isotropic and 3D-continuous: cubic, adaptivecubic, quartercubic, gyroid,
3dhoneycomb, crosshatch, tpmsd.

A part loaded **across** the layer stack is limited by interlayer adhesion, which
responds to hotter and thinner layers — or far better, to reorienting the part so
the load runs in-plane. Reorientation is usually the larger effect and no setting
substitutes for it.

## Dimensional accuracy

`xy_hole_compensation` and `xy_contour_compensation` are independent: a nozzle
that draws outer walls slightly fat often draws holes slightly small at the same
time, and one uniform offset cannot correct both directions.

`elefant_foot_compensation` acts only on the bottom of the part. Do not reach for
it to fix general over-sizing higher up.

For a real tolerance, the only reliable path is a **test coupon** with the target
hole and peg dimensions, measured with calipers. Do not invent a compensation
value from a formula.
