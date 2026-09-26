# Fork conventions

This file covers what is specific to this fork. Upstream's [AGENTS.md](../../AGENTS.md) still
applies in full and is not repeated here.

## What this fork is

A personal fork of OrcaSlicer carrying an embedded Remote HTTP/WebSocket API under
`src/slic3r/GUI/RemoteAPI/`, driven by the `orcaslicer-mcp` server. It tracks two sources:

| Remote | Repository | Role |
| --- | --- | --- |
| `origin` | `OrcaSlicer/OrcaSlicer` | upstream; the fork rebases onto its `main` |
| `maxellis` | `MaxEllis/OrcaSlicer` | the fork the Remote API came from; `remote-api-port` is the counterpart branch |
| `fork` | `DavidGiraldo/OrcaSlicer` | push target |

The history is a linear rebase onto upstream, never a merge. Syncing means rebasing onto
`origin/main` and then triaging `maxellis/remote-api-port` commit by commit.

## Annotating edits to inherited files

Any change to a file that came from upstream carries a comment beginning `orca-mcp:` that says
what was changed and why. Use the comment syntax of the file: `// orca-mcp:` in C++,
`# orca-mcp:` in YAML.

This is what makes fork divergence greppable. `grep -rn "orca-mcp:" src/ .github/` is the
inventory of everything this fork changes in code it does not own. Files the fork owns outright —
everything under `src/slic3r/GUI/RemoteAPI/` — do not carry the marker, because there is nothing
to distinguish.

Prefer guarding over deleting. An inherited workflow that should not run here gets a
`github.repository` condition rather than being removed, so upstream's edits to it keep merging
cleanly.

Never change an inherited file gratuitously. Reformatting, retyping punctuation or rewrapping a
comment the fork is only passing through creates a permanent hunk in every diff against upstream
and a conflict on every rebase, for no benefit.

## Documentation duties

The Remote API documentation is a three-layer structure, and each layer has a different rule:

| Layer | File | Rule |
| --- | --- | --- |
| Design | [docs/HLSD/remote-api.md](../HLSD/remote-api.md) | The design as it stands. Follows upstream's HLSD conventions: no history, no status, no phases. Update when a change invalidates it. |
| Reference | [docs/remote-api/README.md](../remote-api/README.md) and [openapi.yaml](../remote-api/openapi.yaml) | The observable contract. **Any commit that adds, removes or changes a route, a parameter, a response field or an error string updates both in the same commit.** |
| Provenance | [docs/fork/SYNC-LOG.md](SYNC-LOG.md) | What arrived from where, and what was decided. One entry per sync. |

The reference rule is enforced mechanically by the `fork-docs` workflow, which fails a change to
`src/slic3r/GUI/RemoteAPI/**` that does not touch the documentation. If a change genuinely does
not alter the contract, say so in the commit message and the reviewer can override.

The design document deliberately excludes the route list. Endpoints change often and the design
does not; keeping them apart is what stops the design document from rotting.

## Sync duties

Every sync adds an entry to [SYNC-LOG.md](SYNC-LOG.md) recording what upstream brought, what was
taken or skipped from MaxEllis and why, and anything that broke. The standing decisions section is
the part that saves the most time later: it is the list of things already argued out, so they are
not re-litigated on the next sync.

A textually clean rebase is not a safe one. Upstream sometimes deletes a defensive clamp or check,
justifying it with a comment that enumerates the callers it knows about — and the Remote API is a
caller upstream does not know about. That produces a conflict-free rebase and a silent behaviour
regression that exists only where the two changes meet. Every sync should hunt for that shape
specifically, and the result of the hunt belongs in the log entry whether or not it found
anything.

## Verifying a sync

Build and test per upstream's AGENTS.md. Two fork-specific notes:

- A sync that pulls in months of upstream work usually leaves the prebuilt dependencies stale.
  That surfaces as a **CMake configure error**, not a build error, so nothing compiles and the
  exit code can still look benign. Read the output rather than the exit code. Rebuild only the
  dependencies that are genuinely new or differently configured, not all of them.
- The test `Smoothing multiline lightning infill keeps its outlines connected` fails
  intermittently on a strict inequality. It is never a fork regression: it lives in the
  `fff_print` suite, which links `libslic3r` and `test_common` only, never `libslic3r_gui`, where
  all Remote API code lives. Re-run it before investigating.
- No test exercises the Remote API. `slic3rutils_tests` does link `libslic3r_gui`, but the
  controller needs a running `GUI_App` and `Plater`, so route behaviour is verified live against
  the running application over HTTP, not in `ctest`.
