# Remote API — High Level Design

## Purpose and scope

OrcaSlicer is a desktop application whose entire model of a print lives in GUI objects: the
`Plater`, the `PartPlateList`, the `PresetBundle`, the background slicing process. Everything a
user does goes through wxWidgets event handlers on one thread. That makes the slicer excellent to
drive by hand and impossible to drive by anything else.

The Remote API is an HTTP and WebSocket server embedded in the running application that gives an
external program the same reach a user has: read and write settings, load models, move and
duplicate objects, choose presets, manage plates, start and cancel a slice, render a plate, fetch
the resulting G-code, and save or open the project. Its intended client is the `orcaslicer-mcp`
server, which exposes those operations to an AI agent, but nothing in the design is specific to
that client.

Two constraints shape the whole subsystem, and most of what follows is a consequence of one of
them:

- **Everything the API touches is GUI-thread-owned.** The server runs on its own thread and may
  not touch a single slicer object directly.
- **The API is an extra writer the rest of the application does not know about.** Upstream code
  is written on the assumption that the GUI is the only thing mutating the model, and it
  routinely relies on validation that happens in dialogs the API never opens.

The API is off by default. When disabled, no socket is opened, no thread is created, and the only
cost to the application is one null `unique_ptr`.

## Threading and the GUI hand-off

The server owns a single `io_context` on one dedicated thread named `remote_api`. Request parsing,
the WebSocket sessions and all socket writes happen there. That thread never reads or writes
slicer state.

Every handler that touches the application funnels through `Controller::run_on_ui`, which wraps
the work in a `UiTask`, posts it with `wxGetApp().CallAfter`, and blocks the calling io thread on
a future. The GUI thread runs the lambda when it next drains its event queue, and the result
travels back through a `std::promise`. Exceptions thrown on the GUI side are captured and
rethrown on the io thread, so a handler can validate with ordinary C++ control flow.

The wait is bounded. If the future is not ready within the timeout, the io thread marks the task
cancelled and throws `ui_timeout`, which `dispatch` maps to `504`. The cancellation flag is the
load-bearing part: the posted lambda checks it before doing anything, so a request that has
already returned `504` does not still apply its side effects when the GUI thread eventually gets
around to it. Without that check, a timed-out configuration write would land minutes later with
no one listening.

The default budget is ten seconds. `POST /model` overrides it to a hundred and twenty, because
reading and tessellating a real-world STEP file, or importing a very large mesh, legitimately
exceeds ten seconds and the load would otherwise complete on the GUI thread after the client had
already been told it failed.

## Mutual exclusion with the auto-backup exporter

OrcaSlicer periodically exports the project to a backup 3MF. That exporter pumps the wx event
queue while it runs, which means an API task posted with `CallAfter` can execute *inside* the
export, between the moments the exporter reads two parts of the model. The result is a backup
file that is internally inconsistent, written from a model that changed underneath it.

The interlock runs in both directions and lives in three statics on the GUI thread:

- `s_backup_in_progress` is set by `MainFrame` around the `export_3mf` call. While it is true, a
  `UiTask` does not run; it parks itself on a deferred queue. When the exporter clears the flag,
  the parked tasks are re-posted rather than run inline, so the export has fully unwound before
  any of them touches the model.
- `s_api_ui_task_depth` counts API tasks currently on the stack. The backup timer checks it and
  skips the export entirely when an API mutation is in flight, re-arming for the next cycle
  instead.
- Each task also calls `backup_defer(3)`, pushing the next scheduled backup at least three seconds
  into the future. A burst of API calls therefore does not race the timer between its individual
  requests.

The deferral is a no-op when auto-backup is disabled, so the whole mechanism costs nothing for a
user who has turned that feature off.

## Validating what the GUI would have validated

A setting written through the user interface passes through a dialog, a field validator and the
preset machinery before it reaches the configuration. A setting written through the API arrives as
JSON. The difference matters more than it first appears, because several consumers index
configuration vectors with unchecked arithmetic whose bound comes from a *different* vector: a
wrong-length write is an out-of-bounds heap access, not a validation failure.

`PUT /config` therefore stages the whole batch, validates it as a unit, and applies it or rejects
it atomically. Values are parsed rather than trusted: a non-numeric entry in a numeric vector is
rejected instead of being stored as zero, and an empty vector is rejected because the slicer reads
it unguarded.

Project-scope keys — the multi-material and purge settings that live in `project_config` rather
than in a preset — get a stricter treatment still. They are governed by an explicit allow-list,
default-deny, with each permitted key carrying the invariant its consumers assume. Upstream keeps
adding project keys; a default-deny list means a new one is inert here until someone has decided
what writing it safely requires.

Some guards exist only because upstream removed its own. The wipe tower position is the standing
example: a render-time clamp was deleted on the grounds that the stored position is already
clamped by the paths that write it, and the enumeration of those paths did not include this API.
The clamp now lives here.

## Plates

The slicer has a single notion of "the current plate": the one the canvas shows, the background
process is pointed at, and the Slice button slices. Routes that predate plate support act on it
implicitly, and they still do, so existing clients keep working. Every such route also accepts an
explicit plate, and honours it the way the GUI would: slicing, rendering, arranging or orienting a
plate first makes it current, exactly as clicking its tab does. Reading a plate's G-code is the one
exception, because each plate keeps its own slice result and reading it needs no switch.

Switching plates retargets the background process, so it is refused while a slice runs rather
than leaving the running print attached to a plate that is no longer current. Plate indices are
the plate list's own, from 0; deleting a plate shifts the ones after it, which is why a delete
reports the new count and the new current plate.

Plate operations reuse the plater's public functions — the ones the toolbar, the plate menu and
the Plate Settings dialog call — rather than reimplementing them. Where those paths end in a
dialog, the API calls what the dialog would have called on confirmation: a rename sets the name
directly, and enabling spiral mode applies the vase-mode object settings the dialog asks about,
because a caller that requested spiral mode has already given that answer. The slicer never
deletes objects with their plate — it moves them onto another plate or off every plate — so
deleting a plate that still holds objects requires the caller to say so explicitly.

## Projects

Saving, opening and replacing a project are the operations most bound up with modal dialogs: a
file chooser, a save-failure message box, a prompt about unsaved changes, a choice between opening
a project and importing its geometry, and a warning about custom G-code in its presets. No API
client can answer a modal dialog, and one left open blocks the GUI thread until the request times
out. Each route therefore takes the path the dialog would have produced and calls the code behind
it: saving writes the 3MF and then does what a successful Save does; opening passes the loader the
choice the drop dialog would have asked for.

Unsaved changes are refused rather than prompted about, and the caller may ask to discard them.
Discarding first is what makes the confirmation paths silent, because they only ask when something
is unsaved. "Unsaved" has two sources — the undo stack, which the close prompt consults, and the
title bar's dirty flag, which also covers edits that take no snapshot, such as a plate rename — and
the API counts both, so a rename is never lost silently.

A save pumps the event queue while it writes, exactly like the periodic auto-backup, so it takes
the same mutual-exclusion gate described below: other API work waits until the file is written.

## Slice state and events

The API does not poll the slicer. `bind_plater_events` subscribes to the plater's slicing events
and maintains a `SliceState` snapshot — state, percentage, message, statistics and warnings —
under a mutex, written on the GUI thread and read by io threads.

Every subscription calls `evt.Skip()`. The plater's own handlers are bound earlier and therefore
run after these; without `Skip()` the API would silently break ordinary slicing for the user
sitting in front of the application.

Each state change is broadcast to WebSocket subscribers as well as recorded, so a client can
follow a slice without polling. Configuration changes are emitted too, debounced through a pending
set so that a preset switch touching many keys produces one event rather than a storm.

Statistics are harvested on the GUI thread at the moment the slice completes, because they are
read from the plate's print object, which no other thread may touch.

## Surviving a GUI recreate

Changing the language or the theme destroys and rebuilds the entire window tree. The old plater's
event bindings die with it.

The server deliberately keeps running across a recreate — `shutdown()` skips the stop when the
application is recreating the GUI — so WebSocket clients are not disconnected by a user changing a
preference. What must be repaired is the subscription: after the new plater exists,
`bind_plater_events` runs again. It is idempotent and tracks which plater it is bound to, so
calling it on every start and every recreate is safe.

## Compilation boundary

`RemoteAPIServer.hpp` pulls in Boost.Asio, Boost.Beast and Boost.Thread. Including that from
`GUI_App.hpp` would leak those headers into every translation unit in the GUI, and on Windows the
Asio-before-`windows.h` ordering requirement would become everyone's problem.

The server and controller are therefore held as `unique_ptr` to forward-declared types.
`GUI_App.cpp` includes the Remote API headers before `GUI_App.hpp` to preserve the include order,
and it is the only file that needs to.

## Configuration and access control

Settings live in `AppConfig` and are edited on a dedicated Preferences page: enabled, port,
whether to bind the LAN, whether to show in-app notifications, and the token. Loading and saving
are GUI-thread-only.

The listener binds `127.0.0.1` unless LAN access is explicitly enabled, in which case it binds all
interfaces. `SO_REUSEADDR` is set so that relaunching the application after a kill can rebind the
port while the previous socket lingers in `TIME_WAIT`.

Every request carries a token in an `X-Api-Token` header. It is generated from OpenSSL's CSPRNG
rather than `std::random_device`, which is not required to be cryptographically secure and on
MinGW is a fixed deterministic sequence — and this token is the only thing guarding the API once
the user opts into LAN binding. Comparison is constant-time: the length is checked separately and
early, because the length is not the secret, and the contents are compared without
short-circuiting so that timing does not reveal how many leading characters were correct.

The WebSocket is the one exception to the header. Browsers cannot set headers on a WebSocket
handshake, so `/api/v1/events` takes the same token as a query parameter instead. The upgrade is
recognised before the HTTP authentication path, and a handshake presenting no valid token simply
falls through to that path and is rejected there.

A failure to bind is not fatal. The server logs the error, leaves itself stopped, and the
Preferences page reports it through `running()`; the application starts normally without an API.

## Where this lives in the tree

| Area | Files |
| --- | --- |
| HTTP/WebSocket server, sessions, broadcast | [RemoteAPIServer.cpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIServer.cpp), [RemoteAPIServer.hpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIServer.hpp) |
| Routing, handlers, GUI marshalling, slice state | [RemoteAPIController.cpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIController.cpp), [RemoteAPIController.hpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIController.hpp) |
| Persisted settings and token generation | [RemoteAPIConfig.cpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.cpp), [RemoteAPIConfig.hpp](../../src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.hpp) |
| Lifecycle, pimpl ownership, recreate handling | [GUI_App.cpp](../../src/slic3r/GUI/GUI_App.cpp), [GUI_App.hpp](../../src/slic3r/GUI/GUI_App.hpp) |
| Auto-backup interlock | [MainFrame.cpp](../../src/slic3r/GUI/MainFrame.cpp), [bbs_3mf.cpp](../../src/libslic3r/Format/bbs_3mf.cpp) |
| Preferences page | [Preferences.cpp](../../src/slic3r/GUI/Preferences.cpp) |
| Offscreen plate and toolpath rendering | [GLCanvas3D.cpp](../../src/slic3r/GUI/GLCanvas3D.cpp), [GCodeViewer.cpp](../../src/slic3r/GUI/GCodeViewer.cpp) |
| Endpoint reference and schema | [docs/remote-api/README.md](../remote-api/README.md), [docs/remote-api/openapi.yaml](../remote-api/openapi.yaml) |
