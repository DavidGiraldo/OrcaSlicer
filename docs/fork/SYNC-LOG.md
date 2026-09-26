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
