# Handing a calibration to the human

**You cannot run any calibration.** There is no calibration route on the Remote
API, and every `Plater::calib_*` method starts by calling `new_project()` — which
destroys the current plate and loaded model — behind a modal dialog the API cannot
dismiss. The human runs the test in the GUI; you write the resulting number back.

That is not a limitation to apologise for. It is the only honest division: these
values are *measured*, not derived, and inventing one is worse than admitting it
is inherited.

## What to say, and when

Name the inherited values in the print plan, and again in the handover. For each,
say what it is currently set to, that it came from the vendor profile rather than
this spool, and what would change if it were measured.

Do not hand over all eight tests. Pick the one or two that matter for *this* part:

- Bolt holes or a press fit → the dimensional coupon, then flow ratio
- Visible surfaces, corner bulge, gaps after corners → pressure advance
- Speed-limited or a large nozzle → max volumetric speed
- Layer splitting on a structural part → temperature tower
- Stringing that did not respond to temperature → retraction

## The tests

All live under **Calibration** in the OrcaSlicer menu bar. Each starts a new
project, so tell the user to save their work first.

### Flow ratio → `filament_flow_ratio`

**Calibration → Flow rate.** Two flavours: the classic two-pass patch grid
(run Pass 1, pick the flattest square, then Pass 2 around it), or *Orca YOLO*
(linear, single pass, with a finer "Perfectionist" variant).

Read: the square with the smoothest top surface and no gaps or ridges between
lines. Its label is the modifier.

Write back: multiply the current `filament_flow_ratio` by `(100 + modifier)/100`.
Typical result 0.94-1.00. Note that `print_flow_ratio` multiplies this — leave
that at 1.0 or you will double-apply the correction.

### Pressure advance → `pressure_advance` (+ `enable_pressure_advance`)

**Calibration → Pressure advance.** Three shapes: Line, Pattern, Tower. Pattern is
the most readable on a well-tuned machine; Tower works when the surface is poor.

Read: the band where corners are neither bulged nor gapped. On the pattern, the
row whose corners are sharpest and most uniform.

Write back: `pressure_advance` and set `enable_pressure_advance` to `1` — the
value alone does nothing while the flag is off. Typical: 0.02-0.05 direct drive,
higher on Bowden.

Run this at the acceleration you actually print at; PA and acceleration interact.
`adaptive_pressure_advance_model` needs the test repeated at 3+ speeds per
acceleration and at both the slowest and fastest print accelerations — six prints
minimum. Only suggest it if the user asks for the last few percent.

### Max volumetric speed → `filament_max_volumetric_speed`

**Calibration → Max flowrate.** Prints a spiral-vase tower that ramps speed.

Read: the height where the surface first degrades — under-extrusion, matte
patches, missing material. Convert the height to flow using the on-screen scale.

Write back: the value **below** the failure point, derated ~10-15% for quality
rather than for the point of visible failure. PLA+ blends run notably lower than
plain PLA at the same temperature, which is exactly why it cannot be inherited
from a PLA profile.

This is the value that bounds every speed in the profile. Getting it right is
what makes the breakdown's `clamped` verdicts disappear.

### Temperature → `nozzle_temperature`

**Calibration → Temperature.** A tower in 5 °C steps, 10 mm blocks on a 0.4 nozzle.

Read: the block with the best combination of layer bonding and surface finish —
snap a block off to judge adhesion, look at overhangs and stringing for the upper
bound.

Write back: `nozzle_temperature`, and consider `nozzle_temperature_initial_layer`
5-10 °C above it for adhesion. Stay inside
`[nozzle_temperature_range_low, nozzle_temperature_range_high]`; nothing validates
a single filament against its own range.

Remember the coupling: sustainable flow for PLA is roughly `(T - 195) / 1.2`
mm³/s, PETG `(T - 220) / 1.4`, ABS and ASA `(T - 225) / 1.4`. Dropping temperature
to fix stringing can push the melt below what the profile's speeds demand. For any
other material there is no rule — treat it as judgement, not a gate.

### Retraction → `retraction_length`

**Calibration → Retraction test.** A tower that steps retraction length.

Read: the lowest height at which strings disappear. Lower is better — excess
retraction grinds filament and risks clogs.

Write back: `retraction_length`. Sane ranges are 0.2-2.0 mm direct drive and
2-7 mm Bowden. With PA tuned on Klipper these drop further, to roughly 0.5-1 mm
and 1-3 mm, because PA removes most of the pressure retraction otherwise relieves.

### Input shaping → firmware, not the profile

**Calibration → Input shaping → Frequency, then Damping.**

On Klipper this is the *real* fix for ringing; reducing `outer_wall_acceleration`
is a workaround that costs print time. The result goes into the printer firmware
(`SHAPER_CALIBRATE` or the measured frequency), not into an OrcaSlicer key.

### Cornering and VFA

**Calibration → Cornering** (junction deviation / jerk) and **VFA test** (vertical
fine artefacts). Both are refinement passes. Suggest them only after flow,
temperature and PA are measured — they will not fix a profile that is still
clamping.

## Dimensional coupon — not a menu item

For a fit tolerance there is no built-in test. Have the user print a small coupon
carrying the actual hole, peg or slot dimensions of their part, measure with
calipers, and set `xy_hole_compensation` and `xy_contour_compensation` from the
measured error. Holes and contours need independent values — a nozzle that draws
walls fat usually draws holes small at the same time.

This is normally the highest-value twenty minutes for any part that has to mate
with something, and it is the one case where a coupon beats every setting change.

## After the number comes back

Write it with `set_config`, then `remember` it with enough identifying detail to
survive a keyword search across all scopes:

```
remember(
  note="Creality K2 Plus / 0.4 nozzle / Overture Super PLA+: measured "
       "filament_max_volumetric_speed 14.5 mm3/s from the Max flowrate tower "
       "(degraded above 16, derated 10%). Was inheriting 16 from the vendor profile.",
  scope="machine:Creality K2 Plus/Overture Super PLA+"
)
```

The note text has to identify itself — `consult` returns matching lines from every
scope with no filter, so a note that only makes sense next to its filename is a
note you will misread later.
