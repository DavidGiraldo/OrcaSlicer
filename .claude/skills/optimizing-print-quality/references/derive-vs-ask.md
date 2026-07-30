# What the config answers, and what it only appears to answer

Coverage figures are measured over the profiles shipped in this OrcaSlicer tree:
1009 instantiable machine profiles and 6282 filament profiles, with `inherits`
chains resolved. "Coverage" means the key is set somewhere in the chain — it does
**not** mean the value is meaningful.

The trap this file exists for: **an unset key returns its PrintConfig default,
which is indistinguishable from a deliberate vendor value.** A key can be 98%
"covered" and still tell you nothing.

## Derive — do not ask

| Key | Coverage | Note |
|---|---|---|
| `nozzle_diameter` | 100% | `nozzle_diameter.size()` **is** the extruder count — that is how `Print::validate` reads it |
| `printable_area` / `printable_height` | 100% / 99.9% | 4 points = rectangular bed; more = custom or delta |
| `bed_exclude_area` | 81% | non-zero means a real keep-out zone |
| `gcode_flavor` | 99.9% | klipper 538, marlin 260, marlin2 177, reprapfirmware 32 |
| `single_extruder_multi_material` | 100% explicit | `size()>1 && ==1` → AMS/CFS-style; `size()>1 && ==0` → toolchanger |
| `machine_max_speed_*` / `_acceleration_*` / `_jerk_*` | ~98% | **read `[0]`** — the vector is `[normal, silent]`, not per-extruder |
| `auxiliary_fan` | 93.5% | whether `M106 P2` exists |
| `filament_type` | 95.8% | PLA 2355, PETG 763, ABS 502, TPU 310, ASA 272, PC 222 |
| `filament_max_volumetric_speed` | 99.3% | **the best-tuned filament key** — only 266 profiles sit at the default 2 |
| `filament_flow_ratio` | 98.4% | genuinely differentiated: 0.98, 0.95, 1.0, 0.96, 0.94… |
| `nozzle_temperature` / `_initial_layer` | ~97% | |
| `slow_down_layer_time` | 97.2% | real spread 8 / 4 / 12 / 6 / 2 — trustworthy |
| `fan_min_speed` / `fan_max_speed` | 98.9% | best-populated cooling pair |
| `overhang_fan_threshold` / `_speed` | 95.4% / 95.7% | threshold is an enum string: `0%`…`95%` |
| `filament_soluble` | 94.7% | |
| every print-preset quality key | — | these are the profile's actual content |

**Bed types are derivable, but not from a "supported beds" key — there isn't one.**
A bed type is usable iff the selected filament's per-bed temp key is non-zero:
`supertack_plate_temp`, `cool_plate_temp`, `textured_cool_plate_temp`,
`eng_plate_temp`, `hot_plate_temp`, `textured_plate_temp`. `Print::validate` does
exactly this lookup and hard-errors when the value is 0. Note the check only fires
when `is_BBL_printer() || support_multi_bed_types`.

## Derive with suspicion — verify against a second signal, and say you did

| Key | Coverage | Why it lies |
|---|---|---|
| `temperature_vitrification` | 95.0% | Semantically inconsistent across vendors. Modal value is **45** (1543 profiles) — a heat-creep threshold, not Tg; 100 is the stock default. Use as a bed-temp ceiling at most |
| `filament_shrink` | 53.6% | 3349 profiles are exactly `100%` (= no compensation). Perhaps 14 in the whole tree carry a real value |
| `nozzle_type` | 94.1% | Modal value is `undefine` (321), ahead of hardened_steel (273) and brass (259) |
| `nozzle_hrc` | — | Effectively dead: absent 848, `0` 157, non-zero **4** |
| `filament_is_support` | 63.0% | 319 explicit `1`, but 2323 absent-and-defaulted. Absent ≠ "not support material" |
| `pressure_advance` | 48.7% | A coin flip whether the profile has one at all — and it is the highest-leverage quality knob |
| `machine_max_junction_deviation` | 13.6% | Only applies on `gcfMarlinFirmware` anyway |
| `machine_max_acceleration_travel` | 96.1% | Frequently `0,0`, which means *disabled*, not "no limit found" |
| `filament_retraction_*` | ~94% | Serialized as the literal string **`"nil"`** when unset, meaning "inherit from the printer". Never parse `"nil"` as a number |
| `required_nozzle_HRC` | 63.4% | Usable as a positive signal only: `>= 30` → abrasive (405 profiles). `3` is BBL's "no hardened nozzle needed"; silence proves nothing |

## Ask — the config genuinely cannot answer

1. **Direct drive or Bowden?** `extruder_type` is set on **13.2%** of machine
   profiles and defaults to `Direct Drive`, so 876 of 1009 silently claim direct
   drive. Its tooltip: *"This setting is only used for initial value of manual
   calibration of pressure advance… doesn't influence normal slicing."*
   Sane `retraction_length` is 0.2–2.0 mm direct drive, 2–7 mm Bowden — you cannot
   pick without knowing. Weak hints only: a `retraction_length` above 2 mm
   suggests Bowden, as does `pressure_advance > 0.1`.

2. **Is the printer enclosed?** No key expresses it.
   `support_chamber_temp_control` and `support_air_filtration` both **default to
   `true`**, so absence reads as "yes." They only ever meant "the firmware accepts
   `M141` / `M106 P3`."

3. **What nozzle is actually installed**, when the filament is CF/GF-filled.
   `nozzle_hrc` is dead, `nozzle_type` is often `undefine`, and the abrasion check
   is a level-3 warning that never blocks.

4. **How strong is the part cooling in practice?** No key describes fan strength
   or CFM. Whether it can cool a PLA bridge at 200 mm/s is not in the config.

5. **Has this spool been calibrated?** Profile values are vendor generics.
   Roughly half of filament profiles ship no `pressure_advance` at all.

6. **Filament age and dryness.** Dominates stringing and surface quality, entirely
   absent from config, and no setting fixes wet filament.

7. **Which defect did you actually see?** The breakdown gives flow and time, not a
   diagnosis. "Support marks that are hard to clean" is *fusion* if the marks are
   torn and *sag* if they are scalloped — and the two want opposite changes to
   `support_interface_spacing`.

8. **Load direction, fit tolerance, environment, and time budget.** These decide
   between more walls and reorientation, whether a test coupon is warranted, and
   whether PLA is the wrong material entirely.
