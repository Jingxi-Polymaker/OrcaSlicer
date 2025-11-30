# OrcaCloud Sync Implementation Plan

## Goals
- Replace legacy preset CRUD/sync with the timestamped pull/push protocol defined in `spec/Orca Cloud Sync Protocol Specification.md`.
- Operate against Orca API Gateway (`https://api.orcaslicer.com/api/v1/...`) with authenticated requests only (no anon key).
- Maintain offline-first behavior with deterministic conflict resolution and tombstone handling.

## Milestones & Tasks
1) **Endpoint alignment**
   - Swap preset endpoints to gateway paths: `/api/v1/sync/pull`, `/api/v1/sync/push`, `/api/v1/health`.
   - Keep health check fallback minimal; remove `/v1/presets*` legacy calls once migration is gated.
2) **Client state model**
   - Store per-user `last_sync_timestamp` and per-entity `local_updated_at` tokens.
   - Persist sync state under config dir; clear on 410 (full resync).
3) **Pull flow**
   - Implement `get_setting_list2` to call `/sync/pull?cursor=...`, apply upserts/deletes, update cursor, support cancellation/progress callbacks.
4) **Push flow**
   - Add `push` helper that sends modified presets with `original_updated_at`; handle 200 (apply new timestamp) vs 409 (surface server version for merge UI).
5) **Tombstones & deletion**
   - Use returned tombstones to remove local presets; emit callbacks for UI updates.
6) **Conflict & merge UX hooks**
   - Provide callback payloads with server copy on 409; leave merge policy to caller.
7) **Background execution**
   - Run pull/push in worker threads; ensure callbacks are marshalled via `queue_on_main_fn`.
8) **Error handling & retry**
   - Map HTTP errors to `on_http_error_fn`; exponential backoff on transient failures; stop on auth errors and request re-login.
9) **Migration strategy**
   - Feature flag new sync; offer one-time migration to convert legacy preset storage to new format and seed cursor.
10) **Testing**
   - Unit: timestamp parsing, cursor persistence, tombstone application.
   - Integration: pull/push happy path, 409 conflict path, 410 full-resync path, cancellation mid-sync.

## Deliverables
- Updated `OrcaNetwork` sync methods using the new endpoints/protocol.
- Local cache/state persistence utilities.
- QA checklist covering pull, push, delete, conflict, and full-resync scenarios.
