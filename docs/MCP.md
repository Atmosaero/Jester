# Jester MCP

Jester **0.1.0-alpha.1** exposes **44 tools**. Project schema **9** reads schemas **1-9**.

MCP управляет запущенным Jester через его C++/Qt-функции. Сервер встроен в приложение; Python и отдельный процесс для HTTP-подключения не нужны.

## Включение и остановка

1. Запустите `Jester.exe`.
2. Выберите **Automation → MCP connection…** или нажмите **MCP off** в строке состояния.
3. При необходимости задайте порт, затем нажмите **Start**. По умолчанию используется `http://127.0.0.1:18530/mcp`.
4. Скопируйте настройки нужного клиента. **Copy JSON** содержит URL и Authorization; **Copy Codex config** содержит готовую секцию TOML.
5. **Stop** в этом окне или **Automation → Stop MCP server** прекращает приём команд и отключает соединения. Незавершённый жест позирования отменяется.

После перезапуска редактора сервер выключен. Порт и случайный ключ сохраняются в пользовательских QSettings. Сервер принимает соединения только через IPv4 loopback, проверяет Host/Origin и Bearer token. В окне соединения видны число запросов и журнал вызовов, поступивших, пока окно открыто. Аргументы и ключ в журнал не записываются.

## Подключение Codex

Нажмите **Copy Codex config** при работающем сервере и добавьте скопированную секцию в `~/.codex/config.toml`. Конфигурация имеет вид:

```toml
[mcp_servers.jester]
url = "http://127.0.0.1:18530/mcp"
http_headers = { Authorization = "Bearer YOUR_LOCAL_TOKEN" }
tool_timeout_sec = 180
```

Редактор не изменяет настройки Codex автоматически. После добавления сервера обновите подключение в клиенте или начните новую сессию. Для проверки попросите ассистента вызвать `editor_state`, затем `editor_actions`.

Поля конфигурации проверены по [официальному справочнику OpenAI](https://learn.chatgpt.com/docs/config-file/config-reference) и [руководству MCP](https://learn.chatgpt.com/docs/extend/mcp?surface=cli). Копируемая конфигурация содержит локальный ключ доступа: храните её как пользовательскую настройку.

## HTTP и stdio

Встроенный сервер реализует JSON-RPC / Streamable HTTP с JSON-ответами на POST. Поддерживаются MCP **2026-07-28**, **2025-11-25**, **2025-06-18**, **2025-03-26**. Новая версия использует `server/discover`, метаданные каждого запроса, проверку заголовков, `resultType` и приватные ответы с нулевым TTL. Старые версии используют `initialize` и `notifications/initialized`. GET/SSE и protocol sessions не используются; GET возвращает 405. Заявленные возможности — tools и resources. Не заявляются prompts, subscriptions, tasks или sampling.

Для клиентов со stdio и для CLI есть `scripts/jester-mcp.py` (Python 3.9+, только стандартная библиотека). Он передаёт запросы работающему редактору, поддерживает обе версии метаданных и не запускает редактор сам.

Пример конфигурации клиента со stdio:

```json
{
  "mcpServers": {
    "jester": {
      "command": "python",
      "args": ["-X", "utf8", "C:/path/to/Jester/scripts/jester-mcp.py"],
      "env": {"JESTER_MCP_TOKEN": "YOUR_LOCAL_TOKEN"}
    }
  }
}
```

При другом порте добавьте в `args` параметры `--url`, `http://127.0.0.1:PORT/mcp`. В портативной сборке мост лежит в `scripts` рядом с EXE.

CLI также принимает JSON из **Copy JSON**, сохранённый в локальный файл:

```powershell
python -X utf8 scripts/jester-mcp.py --config connection.json --list
python -X utf8 scripts/jester-mcp.py --config connection.json --call editor_state
python -X utf8 scripts/jester-mcp.py --config connection.json --call batch --arguments-file poses.json
```

Для запуска из скрипта можно явно включить сервер и сохранить параметры подключения:

```powershell
.\build\bin\profile\Jester.exe --project-path=. --rhi=dx12 --mcp --mcp-port 0 --mcp-config artifacts/connection.json
```

Порт `0` выбирает свободный порт. Родительский каталог файла должен существовать. По умолчанию, без `--mcp`, сервер выключен. Выданный JSON можно передать CLI через `--config`; из интерфейса по-прежнему доступны Stop и повторный Start.

## Инструменты

Полные схемы аргументов доступны через `tools/list` и в [Editor/src/automation/mcp_tools.json](../Editor/src/automation/mcp_tools.json).

| Tool | Purpose |
|---|---|
| `editor_state` | Read current document, scenes, selection, clip, camera, warnings, physics and optionally all node transforms |
| `scene` | Manage document tabs and files |
| `node_select` | Select one or multiple bones/controllers; mode replace/add/toggle |
| `pose_transform` | Set TRS while preserving omitted components |
| `pose_ik` | Move an end effector with the editor two-bone IK/reach solver |
| `pose_pin` | Set/remove a world-space pin, pin both named feet, or clear all pins |
| `pose_operation` | Apply rest pose, mirror, named-humanoid crouch, or toggle reach assistance |
| `pose_limits` | Author rest-relative cone, directed hinge or swing/twist limits |
| `morph_set` | Set a morph target weight on a mesh node |
| `timeline` | Seek, set clip range/FPS, play/pause/loop, insert/delete/move/copy keys, copy/paste a pose, jump to previous/next key, or fit timeline |
| `history` | Undo or redo completed edits |
| `view` | Control camera, viewport tools and display overlays |
| `contact` | List/add/remove/clear planted foot contact intervals |
| `physics_settings` | Set any world physics settings |
| `physics_body` | Fit a capsule rig or edit a single body mass/radius/COM/tip |
| `physics_preview` | Native MuJoCo background calculation: simulate (legacy external-anchor preview), flight (ballistic COM + conserved angular momentum) or optimize (bounded control shooting with real geom contacts and independent forward replay) |
| `scene_bake` | Bake corrections or an experimental parabolic COM arc to a separate scene, optionally save it |
| `batch` | Atomically apply up to 256 pose/key/contact/physics edits to a staging copy, then commit one Undo entry |
| `screenshot` | Capture the real editor window or viewport including gizmos/overlays |
| `record_frames` | Render a deterministic PNG sequence from the current animation, with one clip-frame per step |
| `editor_actions` | List menu/toolbar actions with stable IDs, labels, shortcuts and enabled/checked state |
| `editor_action` | Trigger an existing QAction by ID or exact objectName |
| `ui_inspect` | Inspect live Qt controls and modal dialogs |
| `ui_set` | Set an existing spinbox, slider, checkbox, line edit or combo box |
| `ui_click` | Click a button or accept/reject a Qt dialog; queued to keep modal interactions responsive |
| `ui_select` | Select a tree/list item by row path (e.g |
| `ui_key` | Send a key sequence to a Qt control, e.g |
| `ui_pointer` | Send real viewport/timeline mouse gestures |
| `motion_correction` | Create and edit ordered keyed additive correction layers, compare original, or bake an independent result. |
| `character_profile` | Assign saved humanoid roles, explicit root/mid/end IK chains, rest-space pole directions and reciprocal mirror pairs |
| `whole_body` | Solve multiple weighted/prioritized hard/soft effectors plus a COM projection goal, preserving bone lengths and directed limits |
| `physics_analyze` | Compile the native model, measure SI mass/COM/inertia/linear and angular momentum, diagnose unsupported rigid transforms, or export readable MJCF |
| `motion_annotations` | Edit context metadata with Undo, search/filter, stable bundle-key attachments, explicit affine retime or trim |
| `motion_context` | Read-only authored notes and nearest events |
| `animation_curves` | Author local scalar position/scale/morph and explicit unwrapped YXZ Euler curves, or SO(3) quaternion Step/SLERP/spherical cubic channels |
| `motion_trajectory` | Read-only evaluator snapshot sampling |
| `retarget_profile` | Review role mapping / calculate bounded humanoid transfer / create independent result document |
| `retarget_preview` | Review role mapping / calculate bounded humanoid transfer / create independent result document |
| `retarget_apply` | Review role mapping / calculate bounded humanoid transfer / create independent result document |
| `analysis_job` | Start snapshot trajectory/compare analysis on a worker thread; poll/cancel and page revision-bound immutable results |
| `motion_compare` | Bounded read-only mapped transform comparison |
| `motion_validate` | Sampled proxies, directed authored joint limits, structured expectations, skinned mesh-pair or self triangle BVH intersections, and closed-volume containment |

В `resources/list` доступны `trickster://editor/state`, `/scene`, `/actions`, `/controls`. `/scene` возвращает полный JSON проекта, включая геометрию и изображения; для обычной работы используйте компактный `editor_state`.

## Семантика редактирования

- Индексы узлов относятся к активной сцене. Уникальное точное имя также принимается. Состояние возвращает единицы сцены; вращения задаются в градусах. Экранные координаты — логические пиксели относительно viewport.
- Обычная правка позы создаёт ключ. `key: false` отключает создание ключа для этой правки. Pins хранятся в ключах: очистка текущей позы не удаляет закрепления из других ключей.
- `batch` выполняет только операции редактирования одной активной сцены. Файлы, переключение вкладок, playback и UI в транзакцию не входят. Каждая операция проверяется до публикации результата.
- Завершённые правки проходят через историю документа. Буфер поз сбрасывается при смене сцены. Незавершённый viewport-жест отменяется перед новой отдельной командой редактирования.
- Закрытие грязной сцены через `scene` требует сохранения или явного `discard: true`. Путь проекта, уже открытого в другой вкладке, защищён от перезаписи через `scene save`.
- `editor_action` и `ui_click` возвращают `queued: true`: действие исполняется на следующем проходе Qt event loop. После него читайте состояние. Модальные окна обслуживаются через `ui_inspect` и `ui_*`; прямые команды редактирования предлагают сначала завершить диалог.
- UI-ID действителен, пока существует соответствующий виджет. `objectName` также принимается, если он однозначен. После закрытия диалога получите свежий список контролов.
- Запись PNG восстанавливает playhead и playback. При изменении документа пользователем запись прекращается, сохраняя его правку. При отключении клиента запись отменяется; уже записанные кадры остаются в каталоге. Существующие кадры защищены от перезаписи по умолчанию.

Пример `poses.json` для встроенного манекена:

```json
{
  "label": "Three poses",
  "operations": [
    {"tool":"timeline","arguments":{"operation":"insert","frame":0}},
    {"tool":"pose_pin","arguments":{"operation":"feet","frame":0}},
    {"tool":"pose_transform","arguments":{"node":"Hips","space":"world","position":[0,0.8,0],"frame":30}},
    {"tool":"pose_ik","arguments":{"node":"LeftHand","target":[0.6,1.6,0.1],"frame":60}}
  ]
}
```

Перед применением примера прочитайте `editor_state`: имена и мировые позиции другого персонажа могут отличаться. Для воспроизводимого превью задайте камеру через `view`, затем вызовите `record_frames`. Кодирование MP4 из PNG остаётся внешним шагом, например через FFmpeg; он не входит в runtime-зависимости Jester.

## Проверки и реализация

`JesterAutomationTests` проверяет протокол, полный цикл редактирования/экспорта, атомарность batch, историю, UI-контролы, диалоги, запись и зарегистрированные QAction. `tests/mcp_client_check.py` дополнительно проверяет стандартный CLI и официальный Python MCP SDK по HTTP/stdio в режимах 2025/2026. SDK используется только в проверках, не поставляется с приложением.

Основные файлы: `src/automation/editor_api.*`, `src/automation/editor_ui_api.cpp`, `src/automation/mcp_server.*`, `src/automation/automation_ui.cpp`. Схемы встроены в Qt resources. Сетевые команды исполняются в UI-потоке; при вложенном запросе во время занятой операции клиент получает busy и может повторить вызов. Запрос ограничен 16 MiB, заголовки 32 KiB, одновременно принимается до 16 TCP-соединений.

Спецификации: [MCP 2026](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning), [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http), [MCP 2025](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).


## Motion editing and background physics

`motion_correction` creates/sets/disables/clears one layer or bakes it into a new scene. `character_profile` saves explicit roles, root/mid/end chains, pole directions and mirror pairs; `suggest` applies only unambiguous name suggestions. `pose_limits` also accepts `kind: hinge|swingTwist`, local `axis`, `minimum`, `maximum`, and `swing`. `contact` accepts `orient`, Euler `orientation`, `fadeIn`, `fadeOut`, and `operation: diagnose`.

Imported source clips retain their timing; normal pose tools automatically create a non-destructive correction on their first keyed edit. Explicit `motion_correction create` lets a client choose dates, fades and `space: local|world` beforehand. A disabled layer is read-only for pose edits. All these edits participate in atomic batches and one Undo, just like existing pose commands.

`physics_settings` accepts `followAnimation`, `stiffness` (Nm/rad), `damping` (Nm·s/rad) and `maxTorque` (Nm). `physics_preview simulate|flight` now returns a background task state immediately. Poll `physics_preview` with `operation: status`; `running` includes progress. `ready` permits `apply`. `cancel` requests cancellation; `clear` removes the preview. Source changes invalidate results (`stale`), including changes during calculation. Repeated polling does not move/apply the result again. `editor_state` remains available while physics is running. There are no MCP protocol-level task objects or SSE sessions; this is ordinary editor task state.

See [Motion workflow](MOTION.md) for evaluation order and physical limitations.


## Documents and keyed layers

`editor_state` includes documentId/documentRevision, ordered layer summaries, activeLayer and compareOriginal. Each tab is a persistent document, not a restored session. Generic Qt UI automation remains a desktop adapter; typed scene edits use the headless EditService with the same transactions/validation as inspector edits.

`motion_correction` adds `select`, `remove`, `duplicate`, `reorder`, `key_insert`, `key_reset`, `key_delete`, `key_move`, `key_copy` and `compare`. Use zero-based `layer`, optional name, and destination `index` for reorder; key operations accept `from`/`frame`. `node` scopes explicit neutral/current key creation to a selected FK/IK chain. `bake` accepts `fps` (1–240) and opens an independent baked document. Original comparison and layer selection do not dirty the document. `clear` removes all layers; `remove` deletes only the selected one. Read the full project resource for keyed offsets and source clips.

`contact` now supports `set` by index and optional `ground=true` for the configured support plane. Existing add/remove/list/clear and orientation/fade fields remain compatible. Editing contacts evaluates the final pose immediately; interval diagnostics report residuals.

`physics_preview status` includes source documentId/revision. Switching tabs does not invalidate a job; changing/closing its source does. A ready result is only applicable from its original document, and apply creates a new document. Polling does not mutate playback/history or copy mesh geometry. The editor never advertises protocol-level MCP Tasks for these ordinary application jobs.


## Native MuJoCo operations

`whole_body` accepts multiple `goals` (node, target in scene units, hard, priority 0–3, weight), optional `comTarget`, `comHeight`, contact reuse, iteration budget and frame. It writes one selected-layer key/Undo and reports actual residuals; COM projection is explicitly static.

`physics_analyze` measures SI mass/COM/inertia, linear/angular momentum, sampled inverse root wrench/required internal torques and contact forces, or returns/exports readable MJCF (`operation: mjcf`, optional path). Boundary acceleration estimates and unsupported transforms are diagnosed.

`physics_preview` adds `operation: optimize` and a `settings` object. Flight settings select endpoint positions or initial velocity, optional end velocity consistency and angular momentum conservation. Optimization settings select iteration/time budgets, pose/contact/smoothness weights and weighted importantFrames. Poll `status.report` for engine/settings/identity, residuals, stop reason and solved/approximate/infeasible status. Ready means the calculation completed; it does not imply physical acceptance. Apply opens a separate document; source changes invalidate it.

`physics_body` adds `set_many` and explicit SI principal inertia, xyzw inertiaRotation, local footHalfSize, estimated marker and per-body stiffness/damping/torque (-1 inherits). `physics_settings` exposes contactTime/contactDamping and legacy-preview externalAnchors. The optimizer never uses world anchors or root actuators.

### Physics reliability settings

`physics_analyze` accepts `operation: "proxy_preview"` (read-only measured preparation) and `operation: "proxy_create"` (open an independent approximate document), with optional `outputFps`. It uses the same implementation as Qt. Failed preparation returns numerical error and a reason; it never clears scale channels in the source document.

Dynamics settings accept `prepareLanding` (default true) in addition to `endAtRest`, `importantFrames`, budgets and `outputFps`; requests persist with the project. Numerical `substeps` accepts 1-512. Report schema 2 uses `measurements` and `validationDomains`; deprecated flat before/after force/moment aliases both refer to common-grid centered inverse values. Preserve method/version/scope when comparing reports. `exportDynamics: not_checked` must not be treated as solved.

## Scoped pose and timeline edits

`pose_transform` and `pose_ik` accept `symmetric: true` to update the assigned opposite FK bone/IK chain in the same Undo transaction. `pose_operation` supports `copy_opposite` with `node`; `mirror` remains a whole-pose flip and reports unpaired side bones. The Qt symmetry toggle is available through `editor_action` (`symmetryAction`). Pair configuration remains in `character_profile`.

`motion_correction` operations `key_move`, `key_copy` and `key_delete` accept `node` and `channels` (Position=1, Rotation=2, Scale=4; default 7), or `nodes` for an explicit controller chain. Scoped edits preserve other bones and morphs, merge disjoint destination components and reject conflicting destinations atomically. Omitting node scope intentionally edits the whole keyed bundle. Imported source channels are immutable. Current saves use schema 9. See [Motion editing](MOTION.md).

`editor_state` includes `symmetryEditing`, `recordingContext` and each node's `mirrorPartner` (-1 if unassigned). The session symmetry mode also applies to MCP single/batch FK/IK edits unless `symmetric` is explicitly provided. Record-disabled layers and source comparison reject keyed pose edits and key insertion/reset as well as retiming.

## Group operations

`node_select` accepts `nodes` and `mode: replace|add|toggle`; `editor_state.selection` contains all selected IDs, while `selected` remains the active bone. `timeline select_track` chooses `node`, `channels` (1/2/4/7) and optional `source` for the actual UI insertion scope, without an Undo or document edit. GUI I/Key/Neutral use that scope; typed `timeline insert` accepts explicit `nodes`, `channels`, `includeChain` or `whole`. An explicit component mask defaults to no IK-chain expansion. Component-row insertion excludes morphs; whole-layer bundles include existing morph tracks.

`timeline` (explicit raw whole-pose keys) and `motion_correction` support `keys_transform`: `keys` is an array of `{frame,node?,nodes?,channels?}`, `delta` shifts all requested times, `copy` retains originals, and `remove` deletes selected scopes. A correction request without node scope refers to the whole layer bundle at that frame. Overlaps are canonicalized; moves first vacate selected components, permitting adjacent selected keys to move together. Destination collisions and clip overflows reject the entire group, including batch calls, with one Undo on success. Imported source channels stay immutable.

`pose_transform.rotationSpace` independently selects local/world Euler rotation; it defaults to local for client compatibility. `space` continues to control position and scale remains local. GUI numeric FK fields now follow the gizmo space, with group manipulation through the viewport. Live symmetry transfers changed components, while explicit `copy_opposite` retains full-pose semantics.


## Motion analysis and retargeting

All motion editing uses the same EditService and evaluator as Qt. Project schema
9 reads schemas 1-9. Read-only operations keep
playback, frame, selection, dirty state, Undo and pending gestures intact.

| Tool | Operations and scope |
|---|---|
| `motion_annotations` | list/search, add/set/remove/duplicate, attach/detach bundle UUIDs, metadata-only retime/trim |
| `motion_context` | Point or interval, bone filters, compact/full detail, roles, contacts, limits, neighboring events, authored expectations; optional `measure` returns separate kinematic results |
| `animation_curves` | list, atomic set/remove, `extract_native`, selected bundle `interpolation`; local TRS, morph, SO(3), Euler YXZ turns, layer offsets and world IK targets/poles/weight |
| `motion_trajectory` | Snapshot sampling, points/offsets/ends, coordinates/units, derivatives, contact/target errors; `offset`/`limit` and `budgetMs` |
| `motion_compare` | Mapped position/orientation/velocity, calibrated swing/twist, cycle closure, time/phase/event alignment and bounded lag search |
| `motion_validate` | Approximate proxies, directed limits before projection, authored expectations, explicit mesh-pair BVH intersections/closed containment and optional self checks; bounded adaptive time refinement |
| `retarget_profile` | Suggested humanoid roles, ambiguity/missing-role report and skeleton fingerprints |
| `retarget_preview` / `retarget_apply` | Small synchronous preview or apply a background `jobId` as an independent document; source is preserved |
| `analysis_job` | start/status/cancel/result; kinds trajectory/compare/validate/retarget, immutable snapshots, progress, budgets and result pagination |

Annotation list reads default to 128 compact notes; use `offset`/`limit` and
`detail: full` (or an explicit `id`) for the complete authored fields.

Start a full trajectory job (JSON arguments to `analysis_job`):

```json
{"operation":"start","kind":"trajectory","settings":{"nodes":[0,4,15,19],"start":0,"end":2,"step":0.008333333333333333,"space":"world","units":"metres","timeBudgetMs":30000}}
```

Poll `{"operation":"status","id":"JOB_ID"}`, then retrieve
`{"operation":"result","id":"JOB_ID","offset":0,"limit":128}`. Keep
following `nextResultOffset`. Validation also exposes `nextProblemOffset` and
stable `problemId` values for the immutable job. A ready job can contain a
`budget-exhausted` partial report: inspect the report's own status.

For retargeting, make the target active and start
`{"operation":"start","kind":"retarget","otherScene":0,"settings":{"fps":30,"rootMotion":"inPlace","ik":true,"contacts":true}}`.
Review the result, then call `retarget_apply` with `sourceScene` and `jobId`.
Only one analysis job/result is retained; starting another replaces that result.
Motion edits invalidate both source and target snapshots. Validation also depends
on context metadata. Text changes preserve trajectory results.

`animation_curves` list returns paged authored channel summaries. Supply an `id`
with `offset`/`limit` for its keys; node/path/layer filter the catalog. Reading one
page never expands an entire large curve. For native graph inspection, the same
list also returns a compact native channel catalog. Add `channel`, `offset` and `limit` to read its original keys.
`extract_native` preserves scalar STEP/LINEAR/CUBICSPLINE mathematics; component
cubic quaternions explicitly become sampled SLERP with
`angleToleranceDegrees` (default 0.02). Source native channels remain unchanged.

```json
{"operation":"interpolation","layer":"LAYER_UUID","selection":[{"frame":12,"node":8,"channels":1}],"interpolation":"Bezier","handles":"AutoClamped"}
```

This authors only compatible selected channel overrides, with one Undo. Scalar
easing skips quaternion channels and reports the skip. Bundle data is retained.
Attaching context uses an explicit pose/layer bundle, not a guessed native key.
Copied bundles receive new IDs; deleted attachments remain marked orphaned.
FPS changes retain context seconds. Trim clips intersecting context and marks
outside notes instead of deleting them. Retime changes context only.

A measured expectation can fail. A proxy overlap is approximate; a surface hit
is geometric evidence; open/invalid containment is uncertain. None is a dynamic
certificate. Calibrated sole targets transferred during retarget remain authored
intentions, including time-varying moving-ground anchors for in-place. Their
actual errors are measured separately. Full workflow and limitations:
[MOTION_ANALYSIS.md](MOTION_ANALYSIS.md).


## Quick-note context

Compact `motion_context` and `motion_annotations` records include note
`description` (up to 256 UTF-16 code units), `textTruncated`, `textLength`,
`nodeNames`, `bindingLabel`, exact `start`/`end`, nearest `startFrame`/`endFrame`,
`timeLabel`, `point`, `missingBindings` and `outsideClip`. Existing IDs, provenance,
tags and expectations remain compatible. Use `detail: "full"` or annotation-list
lookup by `id` for the complete text. `motion_context.id` filters active/neighbor
results; it does not turn inactive notes into active ones.

Display/navigation round seconds to nearest frames with ties toward +infinity.
Point events match that frame bin; intervals remain half-open rounded bins, with
subframe intervals occupying one bin. Moving/stretching note bounds transforms
expected target-key times by the same affine map as retime. Text remains authored
context, never a generated constraint. See [NOTES.md](NOTES.md) for the editor
workflow and data-safety policy.

Scene display and editor preferences use `scene_visuals` and `editor_preferences`; see [Scene settings](SCENE_SETTINGS.md) and [personalization](PERSONALIZATION.md).

`screenshot` and `record_frames` accept `overlays: false` for a native scene capture without viewport annotations/manipulators (default: true). Visual/preferences/language validation failures include a stable `code`, such as `visual.unsupported_parameter`, separately from the English diagnostic in `error`. Language changes do not translate tool enum values, keys, action IDs or error codes.
