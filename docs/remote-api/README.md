# OrcaSlicer Remote API

An HTTP and WebSocket API embedded in the running slicer, so an external program can read and
change settings, load and manipulate models, choose presets, slice, render the plate and fetch the
resulting G-code.

For why the subsystem is built the way it is, see [the design document](../HLSD/remote-api.md).
For a machine-readable contract, see [openapi.yaml](openapi.yaml).

- **Off by default.** Nothing listens until you enable it.
- **One API version:** `v1`. Every path below is prefixed `/api/v1`.
- **Loopback by default.** LAN binding is a separate opt-in.

## Enabling

Preferences → Remote API.

| Setting | Key | Default | Meaning |
| --- | --- | --- | --- |
| Enable Remote API | `remote_api_enabled` | off | Start the server |
| Port | `remote_api_port` | `13130` | Listening port |
| Allow LAN access | `remote_api_bind_lan` | off | Off binds `127.0.0.1`; on binds `0.0.0.0` |
| Notify on API changes | `remote_api_notify` | on | Show an in-app toast when the API changes something |
| Token | `remote_api_token` | generated | 32 hex characters from OpenSSL's CSPRNG |

The server starts and stops when the Preferences dialog closes. If the port cannot be bound the
application still starts normally without an API, and the Preferences page reports the failure.

## Authentication

Every request carries the token in an `X-Api-Token` header. It is a bare value, not a `Bearer`
scheme.

```bash
curl -H "X-Api-Token: $ORCA_API_TOKEN" http://127.0.0.1:13130/api/v1/status
```

No route is exempt. Authentication runs before routing, so an unknown path without a valid token
returns `401`, not `404`. A missing token and a wrong token are deliberately indistinguishable:
both return `401 {"error": "unauthorized"}`.

The WebSocket is the one exception to the header, because browsers cannot set headers on a
WebSocket handshake. It takes the same token as a query parameter instead.

## Endpoints

24 HTTP operations and one WebSocket.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/status` | Application, project, presets, capabilities |
| GET | `/config` | Read configuration values |
| PUT | `/config` | Write configuration values, atomically |
| POST | `/slice` | Start slicing the current plate |
| GET | `/slice/status` | Slice state, progress, statistics, warnings |
| POST | `/slice/cancel` | Cancel a running slice |
| POST | `/model` | Load a model file from the host filesystem |
| GET | `/presets` | List presets by type |
| PUT | `/preset` | Select a preset |
| POST | `/preset/save` | Save the edited preset under a name |
| POST | `/preset/config` | Read a named preset's configuration |
| DELETE | `/preset` | Delete a user preset |
| GET | `/objects` | List objects on the plate |
| DELETE | `/objects/{id}` | Remove an object |
| POST | `/objects/{id}/transform` | Move, rotate or scale an object |
| POST | `/objects/{id}/duplicate` | Add an instance |
| PUT | `/objects/{id}/config` | Per-object settings, atomically |
| PUT | `/objects/{id}/layer_height` | Adaptive layer height, or reset |
| PUT | `/objects/{id}/height_range` | Per-height-range layer height |
| POST | `/arrange` | Arrange the plate |
| POST | `/orient` | Auto-orient objects |
| GET | `/jobs/status` | Whether arrange/orient is idle |
| GET | `/plate/render` | PNG of the plate or the toolpaths |
| GET | `/gcode` | Raw G-code of the last successful slice |
| WS | `/events` | Push notifications |

Two of these take a request body on a method where some HTTP clients disallow one:
`DELETE /preset` and, by design, `POST /preset/config` — a POST precisely so preset names
containing spaces or `@` need no URL encoding.

### GET /status

```bash
curl -H "X-Api-Token: $T" http://127.0.0.1:13130/api/v1/status
```

```json
{
  "app": "OrcaSlicer", "app_version": "2.5.0-dev", "api_version": "1.0",
  "capabilities": ["status","config","slice","events","model","preset","gcode",
                   "objects","arrange","orient","object_config","slice_breakdown","plate_render"],
  "project": "/path/to/project.3mf",
  "objects": [{"name": "cube", "size_mm": [20.0, 20.0, 20.0]}],
  "presets": {"printer": "…", "print": "…", "filaments": ["…"]},
  "modified": {"print": ["layer_height"], "filament": [], "printer": []},
  "slice_result_valid": true,
  "slicing": false
}
```

`modified` lists the keys dirty in each edited preset.

### GET /config

`keys` is an optional comma-separated list; omit it to get everything.

```bash
curl -H "X-Api-Token: $T" "http://127.0.0.1:13130/api/v1/config?keys=layer_height,wall_loops"
```

```json
{"config": {"layer_height": "0.2", "wall_loops": "2"}}
```

Every value is a **string**, in the canonical serialized form used by `.ini` and `.3mf`. Keys that
do not exist are omitted rather than reported.

> `keys` must be the first and only query parameter. The handler takes everything after `?keys=`
> to the end of the target, so a trailing `&other=…` is glued onto the last key.

### PUT /config

A flat object of key to value. Values may be strings, booleans, integers or floats; booleans
become `"1"` and `"0"`.

```bash
curl -X PUT -H "X-Api-Token: $T" -H "Content-Type: application/json" \
  -d '{"layer_height": 0.24, "wall_loops": 3}' \
  http://127.0.0.1:13130/api/v1/config
```

```json
{"applied": ["layer_height", "wall_loops"], "errors": {}}
```

Keys are routed to the first config that accepts them, in the order print, filament, printer, then
the project config.

**The write is atomic.** If any key fails validation, nothing is applied and the response is `422`
with `applied` empty and a reason per failing key:

```json
{"applied": [], "errors": {"wall_loops": "Invalid value provided for parameter wall_loops"}}
```

Reason strings include `unknown_key`, `not_editable_in_current_config`,
`key_dropped_by_legacy_handler`, `unsupported value type`,
`expected at least one value, got an empty list`, and the text of whatever the strict deserializer
threw.

#### Project-scope keys (multi-material and purge)

Keys in the project config are governed by a default-deny allow-list, because their consumers
index them with unchecked arithmetic whose bound comes from a different vector. Eight are
writable:

| Key | Invariant enforced |
| --- | --- |
| `flush_volumes_matrix` | exactly filaments² × nozzles values, each finite in `[0, 20000]` |
| `flush_multiplier` | one value per nozzle, each finite in `[0, 3]` |
| `flush_multiplier_fast` | one value per nozzle, each finite in `[0, 3]` |
| `filament_colour` | `;`-separated, length fixed to the filament count, each `#RRGGBB` or `#RRGGBBAA` |
| `curr_bed_type` | enum; `Default Plate` rejected |
| `prime_volume_mode` | enum, self-validating |
| `wipe_tower_x` | a single finite number, on the bed, written to the current plate |
| `wipe_tower_y` | a single finite number, on the bed, written to the current plate |

The two wipe tower keys take a **single number**, not a vector: the value is written into the
per-plate vector at the current plate index.

Every other project key is refused with a reason naming why, such as
`values_are_unchecked_extruder_indices` for the filament map keys,
`derived_from_flush_volumes_matrix` for `flush_volumes_vector`, or `not_writable_project_key` as
the fallback.

### POST /slice

```json
{"started": true}
```

`202` when slicing begins. `200` with `{"started": false, "already_valid": true}` when the plate
already holds a valid result. `409 already_slicing`, `422 nothing_to_slice`, or
`422 slice_not_started` when the plate cannot be sliced.

### GET /slice/status

Reads a snapshot without touching the GUI thread, so it never times out.

```json
{
  "state": "done", "percent": 100, "message": "",
  "warnings": [{"level": 3, "message": "A G-code path goes beyond the plate boundaries.",
                "code": "TOOLPATH_OUTSIDE"}],
  "stats": {"estimated_time": "1h2m", "filament_used_mm": 1234.5,
            "filament_used_g": 3.7, "total_cost": 0.08,
            "estimated_time_seconds": 3720.0, "breakdown": {}},
  "breakdown": {}
}
```

`state` is `idle`, `slicing`, `done` or `error`. `stats` appears only after a successful slice.
`breakdown` is emitted both inside `stats` and mirrored at the top level.

The breakdown carries `mode`, `total_time_s` and a `roles` array, each entry with `role`,
`time_s`, `time_pct`, `flow_mm3_s` as `{max, mean}`, and `filament_mm`/`filament_g` where the role
consumed filament. Role tokens are `outer_wall`, `inner_wall`, `sparse_infill`,
`internal_solid_infill`, `top_surface`, `gap_infill`, `bridge`, `overhang_perimeter`,
`bottom_surface`, `ironing`, `internal_bridge`, `skirt`, `brim`, `support`, `support_interface`,
`support_transition`, `wipe_tower`, `custom` and `mixed`.

### POST /slice/cancel

```json
{"cancelled": true}
```

`cancelled` reports whether a slice was actually running.

### POST /model

```json
{"path": "C:/models/part.stl"}
```

A path on the machine running the slicer. Accepts `.stl`, `.obj`, `.3mf`, `.step` and `.stp`; any
embedded configuration in a `.3mf` is ignored.

```json
{"loaded": true, "objects": [{"index": 0, "name": "part", "size_mm": [20.0, 20.0, 20.0]}]}
```

This endpoint allows **120 seconds** instead of the usual 10, because reading and tessellating a
real STEP file legitimately takes longer.

Errors: `400 missing_path`, `404 not_found`, `422 unsupported_format`, `422 load_failed`.

### GET /presets

```json
{"print":    [{"name": "0.20mm Standard", "system": true, "visible": true, "selected": true}],
 "filament": [{"name": "Generic PLA",     "system": true, "visible": true, "selected": true}],
 "printer":  [{"name": "…",               "system": true, "visible": true, "selected": true}]}
```

### PUT /preset

```json
{"type": "print", "name": "0.20mm Standard"}
```

`type` is `print`, `filament` or `printer`. Returns `{"selected": "<name>"}`.

> Unsaved changes in the edited presets are **discarded** first, so the selection cannot block on
> the modal unsaved-changes dialog.

Errors: `400 missing_fields`, `400 unknown_type`, `422 unknown_preset`, `500 tab_unavailable`,
`500 select_cancelled`.

### POST /preset/save

```json
{"type": "print", "name": "My profile", "detach": false}
```

`detach` defaults to `false`. The name is trimmed, must be at most 128 characters, and may not be
empty, `.`, `..`, contain any of `<>[]:/\|?*"`, or contain a control character.

Returns `{"saved": "<name>", "created": true}`, where `created` says whether the preset is new.

Errors: `400 missing_fields`, `400 unknown_type`, `400 invalid_name`, `409 name_reserved`,
`500 tab_unavailable`, `500 save_failed`.

### POST /preset/config

Reads a named preset without selecting it. A POST so that names with spaces or `@` need no
encoding.

```json
{"type": "filament", "name": "Generic PLA @MyPrinter"}
```

```json
{"name": "Generic PLA @MyPrinter", "system": true, "config": {"…": "…"}}
```

Errors: `400 missing_fields`, `400 unknown_type`, `404 unknown_preset`.

### DELETE /preset

Takes a body: `{"type": "print", "name": "My profile"}`. Returns `{"deleted": "<name>"}`.

Errors: `404 unknown_preset`, `409 builtin_preset`, `409 preset_selected` (select another first),
`409 has_children` (other presets inherit from it), `500 delete_failed`.

### GET /objects

```json
{"objects": [{
   "id": 12345, "index": 0, "name": "cube",
   "size_mm": [20.0, 20.0, 20.0], "instances": 1,
   "transform": {"offset": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1]},
   "bbox_min": [0,0,0], "bbox_max": [20,20,20], "on_plate": true,
   "config": {}, "custom_layer_profile": false, "height_ranges": []
 }],
 "count": 1}
```

`id` is the stable identifier the object sub-routes take. `transform` describes **instance 0**
only, and its `rotation` is in **radians**. `on_plate` means the bounding box sits on the bed
within 0.05 mm.

### DELETE /objects/{id}

Returns `{"deleted": true, "id": 12345, "count": 0}`, where `count` is how many objects remain.
`404 unknown_object` otherwise.

### POST /objects/{id}/transform

At least one of three fields, each a 3-element array:

| Field | Units | Semantics |
| --- | --- | --- |
| `translate` | mm | **relative**, added to the current offset |
| `rotate` | degrees | **relative**, added to the current rotation |
| `scale` | factor | **absolute**, replaces the current scale |

```bash
curl -X POST -H "X-Api-Token: $T" -H "Content-Type: application/json" \
  -d '{"translate": [10, 0, 0], "rotate": [0, 0, 45]}' \
  http://127.0.0.1:13130/api/v1/objects/12345/transform
```

Applies to instance 0. The echoed `rotation` is in radians.

Errors: `400 no_transform`, `404 unknown_object`, `404 no_instance`.

### POST /objects/{id}/duplicate

Adds an instance offset by 10 mm in X and Y, sharing the original's scale, rotation and mirror,
and registers it with the plate it lands on.

Returns `{"duplicated": true, "id": 12345, "instances": 2}`. Errors: `404 unknown_object`,
`422 no_instance`.

### PUT /objects/{id}/config

A flat object of per-object settings, validated and applied atomically like `PUT /config`.

```json
{"applied": ["wall_loops"], "errors": {}, "object": "cube"}
```

On failure, `422` with `applied` empty and `errors` populated. Errors: `404 unknown_object`.

### PUT /objects/{id}/layer_height

```json
{"mode": "adaptive", "quality": 0.5}
```

`mode` is `adaptive` or `reset`. `quality` runs 0 to 1 and defaults to `0.5`.

```json
{"id": 12345, "mode": "adaptive", "quality": 0.5, "points": 42,
 "layer_height_min": 0.08, "layer_height_max": 0.28}
```

Errors: `400 missing_mode`, `400 unknown_mode`, `400 quality_out_of_range`, `404 unknown_object`,
`422 profile_failed`.

### PUT /objects/{id}/height_range

Either set a range or clear them all.

```json
{"min_z": 0.0, "max_z": 5.0, "layer_height": 0.12}
```

```json
{"clear": true}
```

`max_z` must exceed `min_z`, `min_z` must be at least 0, and `layer_height` must fall within the
printer preset's limits. A range exactly equal to an existing one updates it rather than
colliding.

Both shapes return the full list after the change:

```json
{"id": 12345, "height_ranges": [{"min_z": 0.0, "max_z": 5.0, "layer_height": 0.12}]}
```

Errors: `400 missing_fields`, `400 bad_range`, `404 unknown_object`,
`422 layer_height_out_of_range` (carrying `min` and `max`), `422 overlaps_existing` (carrying
`existing` as a two-element array).

### POST /arrange and POST /orient

No body. `202 {"started": true}`, `422 {"error": "empty"}` when the plate holds no objects, or
`409 {"error": "job_running"}` when the job worker is busy. Progress is shown in the slicer's own
UI; poll `/jobs/status` for completion.

### GET /jobs/status

`{"idle": true}` — covers arrange and orient, not slicing.

### GET /plate/render

Returns a **PNG**, not JSON.

| Parameter | Values | Default |
| --- | --- | --- |
| `view` | `editor`, `preview` | `editor` |
| `angle` | `iso`, `top`, `front`, `left`, `right`, `rear`, `bottom` | `iso` |
| `frame` | `plate`, `object` | `plate` for `editor`, `object` for `preview` |
| `width` | 64–2048 | `800` |
| `height` | 64–2048 | `600` |

```bash
curl -H "X-Api-Token: $T" \
  "http://127.0.0.1:13130/api/v1/plate/render?view=preview&angle=front&width=1200" \
  -o plate.png
```

Allows 30 seconds. Errors are JSON: `400 bad_param` (naming the parameter and usually its allowed
values), `409 no_slice_result` when asking for `preview` without a valid slice, and
`500 render_failed` with a `reason`.

### GET /gcode

Returns the raw G-code of the last successful slice as `text/plain`, not JSON.

Errors: `409 not_sliced`, `500 gcode_file_missing`.

### WS /events

```
ws://127.0.0.1:13130/api/v1/events?token=<token>
```

Server to client only; inbound frames are read and discarded. Each frame is one JSON object whose
discriminator is **`event`**.

| `event` | Payload |
| --- | --- |
| `project.opened` | `project` — the project path |
| `config.changed` | `tabs` — the coalesced set of `print`, `filament`, `printer`, `sla_print`, `sla_material`, `other` |
| `slice.started` | snapshot: `state` `slicing`, `percent` 0, `message` `starting` |
| `slice.progress` | snapshot with the current `percent` and `message` |
| `slice.done` | snapshot with `percent` 100 and `stats` |
| `slice.error` | snapshot with `state` `error` and the error in `message` |
| `slice.cancelled` | snapshot with `state` `idle` and `message` `cancelled` |

A slice snapshot carries `event`, `state`, `percent`, `message` and, when a slice succeeded,
`stats`. It does not carry `warnings`; read those from `GET /slice/status`.

`config.changed` is debounced by one event-loop turn, so a preset switch touching many keys
produces one event rather than a storm.

## Notes and limits

- **Requests are serialized through the GUI thread.** Most handlers allow 10 seconds and return
  `504 {"error": "ui_timeout"}` on expiry. A timed-out request is cancelled before it runs, so it
  does not apply its effects afterwards. `POST /model` allows 120 seconds and
  `GET /plate/render` 30.
- **Request bodies are capped at 4 MiB**, and sockets time out after 15 seconds of inactivity.
  Connections are not kept alive.
- **Unhandled failures** return `500 {"error": "internal", "detail": "…"}`. A malformed body on a
  route that takes one returns `400 {"error": "invalid_json"}`. A request arriving after the
  server started but before the controller was attached returns `503 {"error": "no_handler"}`.
- **Path matching is exact** up to the query string, so `/api/v1/statuses` does not match
  `/api/v1/status`.
- **Object transforms apply to instance 0.** There is no per-instance addressing.
- **The API does not save your project.** Mutations live in the running session until something
  saves them.
