# Sync log

What this fork has absorbed from upstream `OrcaSlicer/OrcaSlicer` and from `MaxEllis/OrcaSlicer`,
and what it decided along the way. Newest entry first.

The conventions that govern this file live in [AGENTS.md](AGENTS.md).

## Standing decisions

Settled questions, kept here so they are not argued again on the next sync.

### Taken from MaxEllis

- **The Remote API itself.** The subsystem originates on his `remote-api-port` branch; this fork
  carries it forward and adds its own fixes on top.
- **MCP build identity** — the version string on the splash screen and in the About dialog, the
  `mcp` wordmark on both splash logos, and the `OrcaSlicer MCP` window-title suffix. Safe to take
  because it is display-only: `GET /status` reports the bare upstream `SoftFever_VERSION`, so the
  API contract does not move, and none of the changed strings are translated, so no language
  regresses to English.

### Skipped, and why

- **Removing the stealth-mode gate on the update check.** The application consults
  `get_stealth_mode()` at eight other call sites and every one exists to suppress network traffic.
  The fork's update check polls `api.github.com`, which is network traffic. Stealth mode is the
  user's explicit opt-out and wins over hearing about new releases. The gate stays, with the
  reason in a code comment.
- **Retitling the update dialog to "New version of OrcaSlicer MCP".** The new message id exists in
  none of the twenty-four catalogs, so adopting it would regress that dialog title to English in
  every language.
- **Anything routing attribution or funding to MaxEllis** — `.github/FUNDING.yml`, the Buy Me a
  Coffee buttons in the README. This is David's fork.
- **His README fork banner.** It hardcodes his URLs and states that issues are disabled, which is
  untrue here. This fork's banner is its own.
- **A higher `ORCA_MCP_RELEASE` ordinal.** `version.inc` keeps `"0"`; his higher ordinals would
  make this fork's own first releases look older than they are and suppress them.
- **Pinning the external `orca-test-repo` to a fixed commit.** His branch is based on 2.4.2, whose
  binary cannot pass the CLI suite upstream added on 2026-09-08. This fork rebases onto current
  `origin/main`, so cloning that repository's `HEAD` is exactly right, and a pin would freeze CI
  against a stale suite. `build_orca.yml` is byte-identical to upstream here.
- **Refusing to move an object off a locked plate.** The canvas lets an object be dragged off a
  locked plate; locking only keeps arrange and orient away from it, and the API already honours
  that for both. Refusing the move would make the API stricter than the window.
- **His CI changes for his own build matrix** — the Windows ARM64 CMake pins, `build_release_vs.bat`
  (upstream replaced that script with `build_win.bat`, which already fails on a CMake error) and the
  macOS ad-hoc signing step. This fork's workflows track upstream byte for byte and it ships no macOS
  build; revisit the signing step, guarded by repository and marked, if it ever does.

### Where his work lands

Triage covers `remote-api-port` **and** `remote-api-242`. Since 2026-10 his Remote API fixes land on
`remote-api-242` (and its `fix/*` branches) and only some reach `remote-api-port`, so watching one
branch misses them.

---

## 2026-10-18 — rebase onto upstream `06665d0558`

Previous base `e4142db820` (2026-10-10). **474 upstream commits** absorbed, 165 of them on the
first-parent line (most of the rest is the IDEX/IQEX parallel-printing branch); 58 fork commits
replayed.

### Conflicts

None.

### Dependencies

Only libjpeg-turbo was rebuilt: 3.0.1 → 3.2.0 (`f6cf18fb96`). The CPython recipe change
(`d5403ccc94`) pins the Python that runs CPython's own Windows build scripts and fetches its
externals itself; the CPython version and hash are unchanged, so the existing build stays valid.

### From MaxEllis

Four new commits, all on `remote-api-242` (2026-10-12). None applied as-is; the controller changes
were ported by hand.

- **Taken — host passwords containing `/`** (`35f2590d2e`). `strip_url_userinfo` was identical
  here, so `print_host = http://user:pa/ss@host` came back unredacted from `POST /preset/config`
  and in the change notification. Everything between the scheme and the last `@` is now cut.
- **Taken, adapted — moving an instance to another plate** (`fa3c9ee370`). The plate-list update
  half was already here (2026-09-18). Taken: the position is kept relative to the old plate only
  when the instance really lies on it, since the plate list can lag behind the geometry, and an
  instance whose relative position would fall outside the target is centred on it instead. This
  fork moves through `POST /objects/{id}/transform` with `plate`, not a separate route.
- **Taken, adapted — `/slice/status` follows the current plate** (from `4078e34cac`, his version of
  the plate routes this fork wrote separately). A `POST /slice` that found the plate already
  sliced, or a plate add, duplicate, delete, select or move, left the snapshot on whichever plate
  was sliced last, so a client reading stats after `already_valid` got another plate's. The
  snapshot now describes the current plate (`done` with its stats, or `idle`), with a new
  `slice.plate_changed` event. His refusal to move off a locked plate was not taken; see the
  standing decisions.
- **Skipped** — `ce6db4b62b`, his `ORCA_MCP_RELEASE` bump.

### What the clean rebase was hiding

**A G-code template outside the sensitive-key policy.** The parallel-printing work (`aa14598e32`)
added `imex_mode_gcodes`, a printer key holding G-code injected before `machine_start_gcode`. The
policy matched templates by the `_gcode` suffix, which `_gcodes` does not end in, so with only the
token a client could set it (with `is_imex` and a mode) and have every export run arbitrary G-code.
Keys ending in `_gcodes` are now templates too; it is the only plural template in the tree.

**A new duty after moving instances.** The same commit made `GLCanvas3D::do_move` and its siblings
drop the IDEX/IQEX copy and mirror previews, whose cache key ignores instance transforms. The
transform and duplicate routes now do the same. Display only.

**A fork defect, found while verifying.** `Plater::reslice` reuses a finished print it still holds
by marking the plate valid itself, with no completion event; adding or switching plates marks a
plate stale without changing its print, so this is the normal case after a plate route. The
slice route took the missing event for a refusal and answered `422 slice_not_started` ("cannot be
sliced in its current state") for a plate that was valid a moment later. It now answers
`already_valid`. The reuse branch predates this sync.

Noted, not changed:

- `a338f6ca7d` moved `auto_drop` from the build item to the object's metadata and stopped reading
  the old place; a missing attribute means on. A project saved before it with an object whose
  snapping was turned off opens with snapping on, and the next `changed_object` (any transform or
  object-config route, or the GUI) drops the object to the bed. An upstream compatibility bug that
  hits the GUI as well; none of the projects at hand use it.
- The same commit makes a GUI move bring all instances of an object to one Z, so a per-instance Z
  set through the transform route is undone by the next drag of a sibling.
- Slice progress text is now `Slicing plate N: <step>` (was ` plate N:<step>`), as seen in
  `slice.progress` and the status `message`. Nothing here parses it.
- An object overlapping an IDEX/IQEX zone makes `slice_not_started` report the generic `outside`
  reason, and the parallel-printing advisories (bed temperature, filament type) are raised only by
  the Slice button, so `POST /slice` does not surface them.
- With Focus Mode on, `GET /plate/render?plate=N&frame=object` can frame the previous plate's
  objects, because inactive plates are hidden only on the next frame.
- The plugin host's `export_3mf_copy` (`de5f2cb3b9`) saves without the auto-backup gate, the same
  as the GUI's own Save.

Checked clean: no guard deleted on an API path, no new modal reachable from an API call (the new
prime tower dialog lives in `Tab::on_value_change` only), no signature or key renames.

### Result

RelWithDebInfo builds cleanly. `ctest`: 2319 of 2321 pass. `Hollowed cube should be supported from
the inside` fails as it did in the last entry. `FilamentGroup golden regression` failed only its
throughput ceiling (32.6 s against 30 s, identical score) under a 12-way parallel run and passes
alone. A live run passed every changed path: `imex_mode_gcodes` and `machine_start_gcode` refused,
the `emit_machine_limits_to_gcode` toggle still writable, `already_valid` with the plate's own
stats, the status after adding, selecting and moving between plates, a reused result reported as
`already_valid`, a straddling instance moved onto its target, duplicate, and a genuinely
unsliceable plate still refused with its reason. The password redaction was not exercised live;
it is his function verbatim, which he checked against his MCP's guard on 50,167 inputs.

---

## 2026-10-10 — rebase onto upstream `e4142db820`

Previous base `93b58a2034` (2026-09-25). **768 upstream commits** absorbed; 55 fork commits
replayed.

### Conflicts

Four, none of substance:

- `Plater.hpp`: upstream `c44f2d6324` removed the MakerWorld publish dialog and with it
  `EVT_PUBLISH`, which the fork's declaration line carried as a neighbour. Only the fork's
  `EVT_PROCESS_COMPLETED` declaration was kept.
- `Plater.cpp` and `MainFrame.cpp`: include lists; both sides kept.
- `.gitignore`: both sides' ignore lines kept.

### Dependencies

Rebuilt selectively, with no full deps rebuild: CGAL 5.6.3 → 6.2.1 (`e729dbf142`), OCCT 7.6 → 8.0.1
(`78a4f2867c`, plus the BRepMesh seam patch from `8790b07773`), the new LibDataChannel
(`a5413efc37`, linked by `libslic3r_gui`) and FFMPEG's newer prebuilt zip. GLEW and OpenCSG left the
tree (`35bac4cb69`) and need nothing. `cmake_minimum_required` and the forwarded
`CMAKE_POLICY_VERSION_MINIMUM` went from 3.5 to 3.10, which changes every CMake dependency's
configure command, so anything a rebuilt target depends on (Boost, for CGAL) reconfigures and
relinks too. The OpenSSL, Python and EXPAT recipe changes are Linux, macOS or ARM64 only.

### From MaxEllis

Three new commits on `remote-api-port` and four more that so far exist only on `remote-api-242`
and `fix/remote-api-per-filament-writes-and-objectlist`. None applied as-is to this fork's diverged
controller; the controller changes were ported by hand in one commit:

- **Taken, extended — the sensitive-key policy** (`81b6da6580`, controller half of `bedca3633e`).
  `PUT /config` let anyone with the token change `post_process`, which runs a program on the host —
  on every slice for BBL printers — and `POST /preset/config` returned the printhost credentials
  verbatim. Changing scripts, G-code templates, the printer connection, `printer_model` or
  `filename_format` now needs a Preferences opt-in; credentials are redacted. His key list is
  extended with upstream's Python plugin keys (`slicing_pipeline_plugin`, `plugins`,
  `*_plugin_config_overrides`), which also run code at slice time. Not covered, by design: a
  `.3mf` opened through `POST /project/open` brings its own `post_process`, as the GUI's Open does.
  His `guard.py` second check lives in his MCP server; this fork's MCP has none, so the API is the
  only barrier.
- **Taken — per-filament length check, discard reporting, object-list refresh** (`2b9f39f168`,
  docs from `a1d033928e`). The length check also covers the keys upstream made per-variant in this
  window (`pressure_advance`, the fan speeds, `nozzle_temperature_range_*`, …): a single value
  written to a filament preset that holds one per variant is refused instead of collapsing them.
- **Skipped** — the CI commits (`bedca3633e`'s workflow half, `fbe47d4fa5`, `dd51650350`,
  `a04b6b7bd5`), see the standing decisions; his README banner hunk.

### What the clean rebase was hiding

**A new modal on printer switch and project open.** Upstream `1241dd5521` added
`Plater::normalize_bed_types`, which resets per-plate bed types on single-bed printers and announces
it with a modal `show_info`. `PUT /preset` (printer) and `POST /project/open` both reach it on the GUI
thread, so one API call would block every later one until someone clicked OK. While an API task is
running the reset now happens with a non-modal notification. The same commit disabled per-plate bed
types in the dialog for single-bed printers; `PUT /plates/{i}` now refuses them the same way.

**More modals reachable from `PUT /config`.** `92d30fbc55` added a reset-and-warn modal for
`filament_ironing_spacing`, and the belt-printer work added one for raft or draft shield on a belt.
They join a family that predates this sync: `TabFilament::update` and
`ConfigManipulation::update_print_fff_config` raise more than fifteen `MessageDialog`s from values a
config write can set — volumetric speed, ironing spacing, compensation limits, layer heights, and
Yes/No questions such as prime tower with adaptive layer height. Any of them wedges the API.
Patching the two new ones would not address the class; it needs one mechanism that answers these
dialogs during an API task and reports what they did, which is a design change and is left for its
own commit.

**Two crash reports from the previous base are fixed upstream.** A slice cancel — from the GUI or
the API — left `initial_tool` unset, and the old base's `on_process_completed` evaluated
`filename_format` to name a lifecycle event without a try/catch; the exception escaped the main
loop. `ec0d8c225f` names the event from the model instead. The access violation seen seconds after
such a cancel was the same crash during the resulting shutdown (`~GLCanvas3D` reaching a freed
notification manager, because `is_closing()` is never set on an exception exit).

**The fork's own cancel race.** The cancel route cleared the slicer's `m_is_slicing` while the
"cancelled" completion event was still queued, so a `POST /slice` in between could start a slice
that event then stopped. Fixed in the same pass.

Not fixed, noted: the API's toolpath-outside-the-plate check does not yet mirror the G-code viewer's
belt-printer branch (`69e7cc1544`), so it reports a false positive on belt printers. G-code export
remains one layer at a time (`GCode::process_layers`); this window brings only small constant
speedups to it.

### Result

RelWithDebInfo builds cleanly. `ctest`: 1862 of 1863 pass. The exception,
`Hollowed cube should be supported from the inside` (`sla_print`), fails deterministically here and
passes on upstream's own CI at the same commit (`build_all` run 38022558147, Windows x64 Release).
The fork does not touch that code — its only `libslic3r` edits are in `Format/bbs_3mf` — so the
difference is this machine's build (RelWithDebInfo, locally built dependencies), not the fork; it
is left for a separate look. A live run against the application passed for every route changed
here: the policy refusals and the write-back exemption, credential redaction, the per-filament
length check, `discarded_changes`, a printer switch that no longer blocks, and cancel while idle.
The `bed_type` refusal itself was not exercised, because the printer at hand has several bed types.

---

## 2026-09-25 — rebase onto upstream `93b58a2034`

Previous base `f956914a11` (2026-09-18). **137 upstream commits** absorbed; 49 fork commits
replayed. No dependency needed rebuilding: the `deps/` changes were clang-cl and GCC 15
compatibility, with no `URL`, hash or tag change.

### Conflicts

One, in `.gitignore`: upstream `505a46b280` dropped the `/.test/` rule when `check_profile` moved its
downloads to a per-user cache, and the fork's commit carried that line as context. Upstream's
removal was honoured and the fork's own ignore lines kept.

### From MaxEllis

Nothing new. His two latest commits on `remote-api-port` (2026-09-17) were triaged in the previous
sync.

### What the clean rebase was hiding

**Plate renders lost their GL context guarantee.** Upstream `42009cf385` made both
`render_thumbnail` overloads bind the shown canvas's context first, because thumbnails render
outside `render()` where another library's context — WebKitGTK's, on Linux — can be current. The
fork's `render_plate_thumbnail` and `render_gcode_thumbnail`, which `GET /plate/render` drives from
exactly that situation, did not. With `66b300987b` now building web panels lazily on idle, a
webview context being current when the API call arrives became more likely. Both now bind first.

**Renamed presets passed validation and selected something else.** `24f380963b` moved third-party
filaments into the Orca filament library with `renamed_from`. `PUT /preset` validated the name with
`find_preset`, which follows renames, then selected with `select_preset_by_name`, which does not —
so an old name was accepted and a different preset selected. The handler now selects by the
resolved name.

Two older defects in the same handler surfaced while switching a project to a newly used printer,
and were fixed in the same pass:

- A printer that exists but is not installed was reported as selected while the slicer fell back
  to another, because `select_preset_by_name` only matches visible presets and reports success
  either way. The handler now makes such a printer visible, as the sidebar does, and checks what
  was actually selected.
- Selecting a filament changed only the Tab's edited preset, never the project filament slot that
  slicing reads, so the call succeeded and nothing printed with the new filament. It now assigns
  the slot the way the sidebar dropdown does, with an optional `slot`.

The rest of the 137-commit audit found nothing that breaks an API path. Two notes for later:
`top_solid_infill_flow_ratio` became a per-variant vector (`PUT /config` writes it as one value,
which is correct only on single-nozzle printers — a gap the other variant keys already had), and
frames where only the overlay changed now reuse the cached scene, so an API path that edits the
model without dirtying the canvas no longer gets a free repaint.

---

## 2026-09-18 — rebase onto upstream `f956914a11`

Previous base `4fd7fdb3fa` (2026-08-18). **818 upstream commits** absorbed; 41 fork commits
replayed, 4 added during the sync.

### Conflicts

All four were the same shape: upstream deleted something the fork's commits carried along as
context. In each case the fork's own lines were kept and upstream's deletion was honoured.

| Where | What upstream removed |
| --- | --- |
| `GUI_App.hpp` (twice) | `switch_staff_pick`, which no longer exists anywhere |
| `Plater.cpp` | the `NetworkAgentFactory.hpp` include, unused after the change |
| `libslic3r_version.h.in` | the `GIT_COMMIT_HASH` fallback block, moved to a generated header |

### From MaxEllis

Two new commits on `remote-api-port`, one taken and one skipped. Both are recorded under
[standing decisions](#standing-decisions): the MCP build identity was taken, the CI test-repo pin
was not.

The identity commit needed adapting. Upstream had since moved `GIT_COMMIT_HASH` out of
`libslic3r_version.h.in`, so his version of that block was not reinstated, and comments naming
2.4.2 as the base version were made version-neutral — this fork's base is now `2.5.0-dev`, so the
displayed version reads `2.5.0-dev-mcp.0`.

### What the clean rebase was hiding

Two real defects, neither of which produced a conflict.

**`Plater::load_files` lost its string overload.** Upstream's "Publish 3MF Workflow"
(`3788a19730`) removed `load_files(const std::vector<std::string>&)`, leaving only the
`boost::filesystem::path` form, so `POST /api/v1/model` stopped compiling. The deleted overload
did nothing but wrap each string in a path before delegating, so building the path vector at the
call site is the same call with the same encoding behaviour.

**Instances created or moved through the API were not registered with their plate.** Upstream's
"Register Instance Copies and Moves with Their Plate" (`8d874cdc36`) made plate registration a
call-site contract: `increase_instances` now notifies the plate list for every copy, and the
canvas move path notifies per moved instance. `Plater::changed_object`, which was all the API
called, does not touch that registry, and nothing resyncs it outside the project-load path.

So `POST /objects/{id}/duplicate` left its copy registered on no plate, and
`POST /objects/{id}/transform` could translate an instance onto a different plate without telling
it. The plate's filament list, wipe tower preview and wipe tower position clamp all read that
registry, so a multi-filament copy could land on a plate that then generated no prime tower, and a
saved project listed the instance on no plate at all. The Print side selects instances by
geometry, so slicing still succeeded — the failure was quietly wrong output rather than an error.

This is the canonical shape described in [AGENTS.md](AGENTS.md#sync-duties), inverted: upstream
did not delete a guard, it introduced a duty, and the API was the caller that did not learn about
it.

An audit of the full 818-commit range for the more usual form — a deleted clamp justified by an
enumerated caller list — found no second instance beyond the wipe tower one this fork had already
guarded. Related upstream work in the range strengthened things rather than weakening them: the
plate wipe-tower clamp became brim-aware, and an off-plate tower now throws before G-code export.

### Dependencies

Upstream added three: SLVS, Assimp and FFMPEG. It also introduced `SLIC3R_CAD`, which requires
OpenCASCADE to be rebuilt with its modeling-algorithms module and applies a new patch step that an
existing build's stamps would skip.

Only those four were rebuilt. The rest of the `deps/` diff in this range is clang-cl and toolchain
compatibility work that does not change the artifacts an MSVC build consumes; no dependency
changed its source URL or hash.

### Cleanup

Two earlier fork commits had retyped em-dashes and arrows in upstream comment text they were only
passing through. Restored, so `MainFrame.cpp` and `Plater.cpp` now match upstream byte for byte
outside the fork's own additions.

### Result

Build clean. Test suite: 1224 tests, 5 skipped by design, 1 failure — the known-intermittent
lightning infill test, which passed three of three on re-run and cannot be affected by fork code.
