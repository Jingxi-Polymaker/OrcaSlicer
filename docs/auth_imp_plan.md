# OrcaCloud Auth Implementation Plan

## Goals
- Deliver first-class Supabase PKCE login on desktop (wxWidgets) with clear Lane A vs Lane B separation.
- Keep tokens secure (OS keyring first, encrypted fallback) and refreshed automatically.
- Avoid leaking the anon key to data APIs; keep auth and data base URLs independent.

## Milestones & Tasks
1) **Endpoint separation**
   - Keep `auth_base_url` for Supabase GoTrue (`https://auth.orcaslicer.com`), `api_base_url` for data (`https://api.orcaslicer.com`).
   - Block `apikey` propagation to Lane B requests; only attach to `/auth/v1/*`.
2) **PKCE + loopback flow hardening**
   - Add port fallback (41172 -> 41173, etc.) and environment-aware redirect URI (dev/staging/prod).
   - Validate `state` on return; reject mismatches.
   - Serve a friendly HTML completion page on loopback callback.
3) **Session lifecycle**
   - Refresh-on-expiry: on 401 or pre‑expiry timer, call `/auth/v1/token` with stored refresh token.
   - Retry/backoff strategy and UI notification on refresh failure.
   - Ensure logout posts `/auth/v1/logout` with refresh_token when present.
4) **Secure storage**
   - Keep wxSecretStore as primary; encrypted file fallback remains.
   - On refresh-token rotation, overwrite prior secrets atomically.
   - Add integrity check for fallback file (HMAC with derived key).
5) **Configuration & feature flag**
   - Respect `ORCA_AUTH_URL`, `ORCA_API_URL`, `ORCA_BACKEND_ANON_KEY`; add `ORCA_LOOPBACK_PORT` override for dev.
   - Gate new auth path behind a runtime flag until fully verified.
6) **Telemetry & logging**
   - Redact tokens from logs; keep request IDs and http codes.
   - Emit structured auth events (login success/fail, refresh, logout).
7) **Testing**
   - Unit: PKCE generation (challenge/verifier), state validation, refresh handling.
   - Integration: end-to-end login via loopback on macOS/Win/Linux; refresh after forced expiry; logout clears secrets.
   - Regression: ensure legacy username/password path remains disabled by default.

## **Rules**
- Always refer to the design doc in the `spec` directory when working on the OrcaCloudprotocol

## Deliverables
- Updated C++ (`AuthManager`, `OrcaNetwork`) with the above behaviors.
- Minimal QA checklist for manual verification per platform.
- Updated docs (OrcaNetwork.md) to reflect PKCE-only, Supabase-based auth flow.
