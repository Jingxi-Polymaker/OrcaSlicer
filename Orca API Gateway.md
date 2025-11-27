
# Architectural Blueprint: The OrcaSlicer Cloud Ecosystem

Version: 1.0 (Orca API Gateway Revision)

Target Scale: 1 Million+ Active Users

Primary Stack: C++ (Client), Cloudflare Workers (Gateway), Supabase (Auth/DB)

## 1. Executive Summary

This document outlines the architecture for transforming OrcaSlicer from a standalone desktop application into a connected cloud ecosystem. To support a projected user base of 1 million active users while maintaining strict security and keeping operational costs low, we have adopted an **"Orca API Gateway" Architecture**.

This strategy decouples the desktop binary from the backend database. Instead of direct connections, traffic is routed through a high-performance API Gateway layer. This ensures that the desktop application remains stable even as backend schemas evolve ("Future-Proof"), and the database is protected from massive traffic spikes ("Scale-Safe").

## 2. High-Level Architecture: The "Two-Lane" Strategy

To optimize for both security and performance, network traffic is split into two distinct lanes based on the nature of the request.
### Lane A: Authentication (The "Handshake")

- **Purpose:** User Login, Sign-up, and Token Refresh.
    
- **Flow:** `OrcaSlicer Client` $\rightarrow$ `Supabase Auth`
    
- **Domain:** `auth.orcaslicer.com`
    
- **Rationale:** Authentication is a standardized, low-volume, high-security operation. We utilize Supabase’s native PKCE handling directly to avoid re-implementing complex cryptographic flows in our own infrastructure.
    

### Lane B: Data & Synchronization (The "Traffic")

- **Purpose:** Profile Sync, Plugin Store, Printer Settings, Telemetry.
    
- **Flow:** `OrcaSlicer Client` $\rightarrow$ `Cloudflare Worker (API Gateway)` $\rightarrow$ `Supabase DB`
    
- **Domain:** `api.orcaslicer.com`
    
- **Rationale:** This represents 99% of application traffic. Routing this through Cloudflare Workers allows us to cache data close to the user, shield the database from connection exhaustion, and version the API without forcing users to update the desktop binary.
    

## 3. Component Design Details

### 3.1 The Client (OrcaSlicer C++)

The desktop application acts as a "Public Client" and assumes an untrusted environment.

- **Security Standard:** Uses **PKCE** (Proof Key for Code Exchange) to prevent code interception.
    
- **Token Storage:** Strictly uses OS-native secure vaults via `wxSecretStore` (Windows Credential Manager, macOS Keychain, Linux Secret Service).
    
- **User Experience:** Implements **Loopback Interface Redirection**. The app spins up a temporary local TCP listener to capture the auth callback, ensuring a seamless "Click to Login" experience without manual copy-pasting.
    

### 3.2 The API Gateway (Cloudflare Workers)

This layer acts as the "Smart Receptionist" for the ecosystem.

- **Technology:** **Hono Framework** running on Cloudflare Workers.
    
- **Responsibility 1: API Gateway Validation.** Validates the JWT signature using the standard HS256 secret. Invalid requests are rejected at the API Gateway (e.g., in Singapore or London) before they ever reach the database.
    
- **Responsibility 2: Aggressive Caching.** Read-heavy data (User Profiles, Plugin Lists) is cached in the Cloudflare data center. A request for a "Trending Plugin" is served in <50ms without touching the database.
    
- **Responsibility 3: Schema Shielding.** Transforms database responses into the JSON format expected by older desktop clients, preventing crashes when the database schema changes.
    

### 3.3 The Backend Core (Supabase)

Supabase acts as the "Source of Truth" and the "Policy Enforcer."

- **Database:** PostgreSQL.
    
- **Security Model:** **Row Level Security (RLS)**. Access control logic is defined in SQL policies, not in application code.
    
- **Storage:** Supabase Storage (S3-compatible) for hosting large assets like plugin ZIP files and printer definition bundles.
    

## 4. Feature Spotlight: The Plugin Marketplace

The architecture is designed to handle complex business logic, such as a Plugin Store, by leveraging the strengths of both layers.

|**Feature**|**Implementation Layer**|**Logic**|
|---|---|---|
|**Browsing Plugins**|**Cloudflare (API Gateway)**|**Cached Read.** The list of public plugins is cached globally. 1M users browsing creates near-zero DB load.|
|**Publishing/Deleting**|**Supabase (DB)**|**RLS Policy.** `auth.uid() = owner_id`. The database ensures only the author can modify their plugin.|
|**Access Control**|**Supabase (DB)**|**RLS Policy.** `is_public = true` OR `owner_id = auth.uid()`. Private plugins are invisible to others at the SQL level.|
|**Downloading**|**Supabase Storage**|**Signed URLs.** The Gateway validates the user's license/subscription and redirects them to a secure download URL.|

## 5. Security & Threat Model

### 5.1 Attack Vector: DDoS via Desktop Clients

- **Risk:** A bug in v1.2 causes 500,000 clients to retry connection simultaneously.
    
- **Mitigation:** Cloudflare Rate Limiting absorbs the traffic. The "Orca API Gateway" returns cached responses or 429 Errors. The Postgres database remains unaffected.
    

### 5.2 Attack Vector: Token Theft

- **Risk:** Malware attempts to read the `session.json` config file.
    
- **Mitigation:** There is no config file. Tokens are stored in the OS Keychain (`wxSecretStore`), which requires user system login to access.
    

### 5.3 Attack Vector: Unauthorized Data Access

- **Risk:** A user modifies the binary to request data for `user_id: 999`.
    
- **Mitigation:** The request passes the Gateway but is stopped by Postgres RLS. The query `SELECT * FROM profiles` automatically limits results to the requesting user's ID.
    

## 6. Implementation Roadmap

1. **Phase 1: Foundation (Lane A)**
    
    - Implement `AuthManager` in C++ with PKCE.
        
    - Set up `auth.orcaslicer.com` pointing to Supabase.
        
    - Verify login flow and secure token storage (`wxSecretStore`).
        
2. **Phase 2: The Facade (Lane B)**
    
    - Deploy Cloudflare Worker with Hono.
        
    - Implement JWT Validation Middleware.
        
    - Create the first "Profile Sync" endpoint with caching.
        
3. **Phase 3: Migration & Expansion**
    
    - Update C++ `NetworkClient` to point to `api.orcaslicer.com`.
        
    - Define RLS policies for User Profiles.
        
    - Begin development of the Plugin Store database schema.
        

## 7. Conclusion

By placing a Cloudflare Worker "Facade" in front of Supabase, OrcaSlicer achieves the resilience required for a mass-market desktop application. We delegate **Identity** to Supabase (via direct flow) to ensure security compliance, while we delegate **Traffic Management** to Cloudflare to ensure scalability. This architecture allows the platform to grow to 1 million users and beyond without exponential database costs or fragility.

## 8. Gateway Contract (High-Level)

Principles that stay true even as the client is refactored:

- **Lane boundaries:**
  - Lane A (Auth): `https://auth.orcaslicer.com/auth/v1/*`. Direct to Supabase with PKCE. Public auth configuration (e.g., anon/public key) is handled within the auth lane only.
  - Lane B (Data): `https://api.orcaslicer.com/v1/*`. No Supabase secrets should originate from the client; identity comes from the user JWT.

- **Identity policy (pass-through):** Gateway validates the user JWT at the edge and forwards the same JWT to Supabase so RLS (`auth.uid()`) remains the single source of truth. Service-role keys are never used on user-data paths.

- **API versioning & compatibility:** All data routes are versioned in the path (`/v1/...`). The gateway is responsible for response shaping to keep older desktop builds working across schema changes; breaking changes must not crash legacy clients.

- **Caching classes:**
  - Public cache (e.g., plugin catalog) – short TTL (≈60s–5m), shared.
  - User-scoped cache (e.g., profile/presets reads) – keyed by user id + app version (+ locale if relevant); invalidate on successful mutations; no stale served immediately after writes.
  - Non-cacheable – mutations and sensitive reads.

- **Rate limiting:** Per-user and per-IP limits; 429 with `Retry-After`. Auth lane can be tighter to protect Supabase.

- **Resilience:** Stale-while-revalidate only for cacheable reads and time-bounded; writes fail closed if Supabase is unavailable. Declare single- vs multi-region assumptions.

- **Large payloads:** Use signed URLs for uploads/downloads (presets, plugins, 3MFs); gateway issues presigned URLs and does not proxy bulk data.

- **Observability:** Every request carries `X-Request-ID`; gateway generates one if absent and propagates it downstream. Structured error bodies; core metrics include auth failure rate, cache hit rate, and p95 latency.

- **Rollout/migration:** Staged cutover from legacy direct Supabase calls to the gateway (canary %, metrics, rollback toggle). Keep legacy endpoints available until adoption is verified.
