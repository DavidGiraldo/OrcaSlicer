---
name: optimizing-print-quality
description: Use when preparing a 3D print in OrcaSlicer and print quality matters - dialing in a profile for a specific printer and filament, or diagnosing stringing, under-extrusion, poor overhangs, bad supports, seam defects, warping, layer splitting, or elephant foot.
---

# Optimizing print quality

## Overview

You drive a real slicer holding someone's real profile. The physics you can compute; the *situation* you cannot. A profile tuned for the wrong load direction, or a support fix aimed at fusion when the real defect was sag, is worse than no change — it looks like progress and costs a print.

**Core principle: the plan is the first half of the deliverable.** You produce a written plan and settle the unknowns that would change your approach *before* the first `set_config`. Everything after that is measurement.

## The shape of a run

A run produces these, in order. Each is a real artifact, not a phase you pass through.

1. **Ground truth** — read, never ask, what the config already answers
2. **Print plan** — written out, unknowns settled, before any write
3. **Consulted knowledge** — the shipped principles for this goal
4. **Config changes** — small atomic batches, in dependency order
5. **Two gates** — the physics check, plus what it does not cover
6. **A slice you looked at** — numbers *and* the preview image
7. **Options** — 2-3, quantified with measured time and mass
8. **Notes** — so the next session does not re-ask

## 1. Ground truth

`get_status` then `get_config`. Derive the machine and filament envelope before forming any opinion.

Read machine limits at **index 0**: `machine_max_acceleration_x` = `"8000,8000"` means 8000 in normal mode and 8000 in silent mode. It is **not** per-extruder. Orca itself only ever reads `[0]`.

Three facts the config will confidently lie about. See [references/derive-vs-ask.md](references/derive-vs-ask.md) for the full split with coverage numbers:

- **`extruder_type`** is set on 13% of machine profiles and defaults to `Direct Drive`, so 87% of printers claim direct drive whether they are or not. Its own tooltip says it "doesn't influence normal slicing." Retraction length and PA both hang off this. **Ask.**
- **`support_chamber_temp_control`** defaults to `true`, so absence reads as "enclosed" when it only ever meant "accepts M141." **Ask** whether the printer is enclosed.
- **`temperature_vitrification`** is populated but semantically inconsistent across vendors — its modal value for PLA is 45, a heat-creep number, not Tg. Do not treat it as a softening point.

## 2. The print plan

**Write the plan before the first `set_config`.** It has exactly these parts:

```
Part        — the model file, by path, that the user gave you
Goal        — what the part is for, in their words
Hardware    — printer / nozzle / filament, as derived, naming the presets
Unknowns    — fundamentals that would change the approach, each with its question
Inherited   — values taken on faith from the vendor profile, named, with their source
Approach    — what you intend to change and the reason
```

**Part names a file the user gave you.** If you do not have one, that is the first question — not an invitation to generate a stand-in. Measurements on invented geometry do not transfer, and a stand-in left on the plate is litter the user has to find.

**Hardware checks that the loaded preset matches what they told you.** Someone who says "PLA+" may have the plain-PLA preset selected, and the two differ where it counts — flow ceiling, flow ratio, temperature range. A selected preset is evidence of what is *configured*, not of what is on the spool. When the names disagree, say so and ask; do not quietly tune the wrong material.

To find a preset that is not currently loaded, `list_presets(type=..., include_system=True)` — the default hides several hundred system presets, so the default view can show one filament where hundreds exist. `get_preset_config(type, name)` then reads any of them without selecting it. The shipped profiles are also plain JSON under `resources/profiles/<Vendor>/`, which is the fastest way to compare candidates side by side.

**Unknowns is where the questions live.** The seven fundamentals are purpose, mechanical load and direction, fit, environment, appearance, budget, hardware truth (`consult("interview fundamentals")` for the full protocol). Derive silently every one the user already answered. Ask only those where a different answer produces a *different change*, one or two at a time, conversationally — a list of seven reads as an interrogation and most of it is already knowable.

The test for whether to ask: *would the opposite answer change what I write?* "Is this bolted in shear or in tension?" changes whether the lever is wall count or reorientation — ask it. "What colour is it?" does not.

**Inherited names the values you are trusting without evidence.** Typically `filament_flow_ratio`, `pressure_advance`, `filament_max_volumetric_speed`, and the exact `nozzle_temperature` within the datasheet range. These come from the vendor profile and are not measured on this spool and machine. Say so by name, in the plan, every time. See [references/calibration-handoff.md](references/calibration-handoff.md) for how the user can turn each one into a measured number.

## 3. Consult before deriving

`consult(query="...")` — the parameter is `query`, not `topic`.

Retrieval is a keyword score over 6 **whole files**, so one well-aimed query beats five vague ones. Tokens of 2 characters or fewer are dropped and numbers are mangled: `"0.8mm nozzle"` retrieves nothing useful, `"large-nozzle flow"` retrieves the right file. Query with concept words that appear in a file's `topics`: `strength`, `surface`, `dimensional`, `speed`, `material`, `cooling`, `retraction`, `stringing`, `overhang`, `warping`, `seam`, `under-extrusion`, `elephant-foot`, `first-layer`, `layer-adhesion`, `ringing`, `small-features`, `large-nozzle`.

The `failures/*` files are the diagnostic path: each gives causes ranked most-likely-first and a named test print that confirms it.

## 4. Change in dependency order

Flow ceiling bounds every speed → temperature must sustain that flow → geometry → cosmetics last. Deriving speeds before checking the ceiling means deriving them twice.

`set_config(changes={...})` is **atomic**: one bad key rejects the whole batch. Send small related batches so a rejection tells you which group failed.

**One axis at a time.** When two settings could each explain the same symptom, change one, slice, measure, then the other. Dropping temperature *and* changing wipe behaviour in one step means a fix you cannot attribute and cannot repeat.

**On a multi-slot machine every filament key is a vector**, and writing a scalar replaces the whole thing. On a 4-slot AMS/CFS printer `nozzle_temperature` reads back as `"220,220,220,220"`; writing `"210"` collapses it to one entry and leaves it inconsistent with `nozzle_temperature_initial_layer`. This is not an edge case on such a machine — it is every filament write. Read the current value, count the entries, write the same shape back.

## 5. Gate twice

**`check_profile_physics(changes={...})`** — pass your proposed changes to dry-run them before writing.

Read the `detail` strings, not the `verdict` word. Six of its eight checks emit `warn` on *missing data* rather than staying silent, so `ok` is rare and `warnings` is usually noise about something it could not read.

Watch specifically for `temp_vs_flow` reporting *"insufficient data"* while `filament_type`, `nozzle_temperature` and `filament_max_volumetric_speed` are all clearly populated. On a multi-slot machine it cannot parse the vector values, so the one check that couples melt temperature to demanded flow — the check that matters most to a strength question — silently switches itself off. Do that arithmetic yourself: sustainable flow is roughly `(T-195)/1.2` for PLA, `(T-220)/1.4` for PETG, `(T-225)/1.4` for ABS and ASA.

**Then check what it does not.** It validates flow, melt temperature, layer and line geometry, retraction length, fan ordering, and the first layer. It validates **nothing** about supports, overhangs, bridging, seam, acceleration, shell floors, or per-feature line widths. [references/quality-gate.md](references/quality-gate.md) has those checks with their keys and thresholds, and the client-side pre-validation for the hard errors whose messages never reach you.

One shipped contradiction to resolve in favour of the knowledge: `cooling_sanity` warns below **3 s** of `slow_down_layer_time`, but `physics/cooling.md` sets the floor at **8 s** for small or thin-walled parts. A profile at 4 s passes the gate and still stacks molten layers.

## 6. Slice, then look

```
slice_and_wait(timeout=300)
get_slice_breakdown()
render_plate(view="preview", angle="iso")     # then read the PNG
```

**`view` defaults to `"editor"`, which shows the 3D scene, not toolpaths.** `view="preview"` is the only way to see where support actually landed, how the first layer sits on the plate, and what the seam is doing. If the question involves supports or overhangs and you did not render the preview, you have not looked at the thing you are fixing.

In `get_slice_breakdown`, `prediction_check` is the honesty signal:

- **`clamped`** — the profile demands more flow than the ceiling and Orca is silently throttling. The speed fields are fiction. This is the highest-value finding in the whole tool and it is common in stock profiles.

  When you find it, work out what each feature *actually* runs at: `speed = ceiling / area`, where `area = line_width x layer_height - layer_height^2 x (1 - pi/4)`. Two features with different line widths clamp to different real speeds, so the intended relationship between them can invert — a profile reading a textbook 67% outer/inner ratio can execute 108%, making the one pass you actually see the fastest pass on the part. Nothing in the UI shows this. Slowing down then costs almost no time, because the fast numbers were never real.
- **`anomaly`** — observed exceeds predicted by >10%. Model and reality disagree; often percent-resolved line widths or arc fitting. Investigate before acting on it.
- **`matches`** — *includes observed far below predicted.* There is no under-run verdict. Never read `matches` as "the feature achieves its commanded speed."

Roles outside the eight known names never appear in `prediction_check` at all — `support`, `support_interface`, `overhang_perimeter`, `ironing`, `skirt`, `brim`, `wipe_tower` are listed in `roles` but never checked. Read their `flow_mm3_s.max` yourself.

## 7. Present options, then stop

2-3 concrete options, each quantified with **measured** print time and filament mass from real slices — never adjectives. `compare_settings(key, values)` sweeps one key, slicing once per value; it is the cheapest way to turn a judgement call into a table.

Name every inherited-unverified value again in the handover, and give the calibration procedure for the ones that matter most to this part.

**Stop here. Do not `save_preset`.** Changes live as unsaved overrides; the user decides what becomes permanent. Say clearly that they are unsaved and will be lost on a preset switch.

## 8. Persist what was learned

`remember(note="...", scope="machine:<printer>/<filament>")` — the parameter is `note`.

There is no scope-filtered read: `consult` returns matching note lines from *every* scope. Write notes that identify themselves — printer, filament, nozzle, the number, and how it was measured — so a cross-scope hit is still interpretable.

## Tool signatures worth having right

Guessing these costs a failed call each. Verified against the running server.

| Call | Note |
|---|---|
| `consult(query)` | not `topic` |
| `remember(note, scope)` | not `fact` |
| `set_object_config(object_id, changes)` | not `config` |
| `duplicate_object(object_id)` | not `id` |
| `render_plate(view, angle, width, height)` | `view` defaults to `"editor"` |
| `set_config(changes)` | atomic across the whole dict |
| `compare_settings(key, values, extra)` | one full slice per value |
| `find_config_keys(substring)` | live keys; `search_settings(query)` searches labels and tooltips offline |

## Common mistakes

| Mistake | What it costs |
|---|---|
| Writing config before the plan is agreed | Tuning the wrong axis; the work is unattributable |
| Generating a stand-in model instead of asking for theirs | Measurements that do not transfer, and litter on their plate |
| `render_plate()` with default `view` | You looked at the scene, not the toolpaths — supports unverified |
| Reading `verdict` instead of `detail` | False alarm on missing data, false comfort on unchecked areas |
| Treating `matches` as "speed achieved" | Silent under-run reads as healthy |
| Changing two settings in one axis at once | A fix you cannot attribute or repeat |
| One large `set_config` | A single bad key discards every good change with it |
| Inventing a value for flow ratio or PA | These are measured, not derived. Hand over the test instead |
| Enabling a global flag to make one setting work | Check what else that flag ungates before flipping it |

## When this does not apply

A user who names a specific setting and asks what it does wants `describe_setting`, not a print plan. A user reporting a defect on a print that already happened is diagnosing, not tuning — start at step 3 with the matching `failures/*` file, and the plan's Unknowns section becomes "which defect is this, exactly."
