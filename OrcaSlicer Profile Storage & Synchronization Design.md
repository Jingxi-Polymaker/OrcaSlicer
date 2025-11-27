
Version: 1.0

Scope: User Profile Sync, Configuration Bundles, and Asset Storage

## 1. Executive Summary

This document outlines the architectural decisions for synchronizing user configuration profiles (printers, filaments, processes) across multiple devices. The solution prioritizes **data integrity**, **sync speed**, and **cost efficiency**.

We adopt a **Hybrid Storage Strategy**: lightweight configuration data resides in **Supabase (PostgreSQL)** for fast transactional sync, while heavy binary assets reside in **Cloudflare R2** for cost-effective storage. All business traffic is orchestrated by the **Orca API Gateway** (Lane B).

## 2. Synchronization Strategy

### 2.1 Core Philosophy: "Trust No Client"

To prevent data corruption from inconsistent client system clocks, the system relies strictly on **Server-Side Timestamps**.

- **Source of Truth:** The database (PostgreSQL) timestamp is the only valid version controller.
    
- **Client Logic:** Clients store a `last_sync_timestamp` which is opaque to them; they receive it from the server and send it back during the next sync.
    

### 2.2 Sync Protocol: Incremental Cursor

Instead of comparing file-by-file, we use a cursor-based approach for efficiency.

- **Push (Upload):**
    
    1. Client sends modified JSONs to the Gateway.
        
    2. Server writes to DB using `NOW()` (Server Time).
        
    3. Server returns the new `server_time` to the Client.
        
    4. Client updates its local `last_sync_timestamp`.
        
- **Pull (Download):**
    
    1. Client requests updates: `GET /sync/pull?since={last_sync_timestamp}`.
        
    2. Server queries DB: `SELECT * FROM profiles WHERE updated_at > {since}`.
        
    3. Server returns **only changed items** (including deletions) and the new `server_time`.
        

### 2.3 Handling Deletions

- **Soft Deletes:** We do not physically delete rows immediately.
    
- **Mechanism:** Records are marked with `is_deleted = true` and a new `updated_at`.
    
- **Sync:** Clients pull these "deleted" records and execute the file deletion locally.
    

### 2.4 Conflict Resolution & Safety

- **Strategy:** **"Server Wins, but Backup Local"**.
    
- **Scenario:** If a client pulls a file that conflicts with a local dirty modification:
    
    - The server version overwrites the standard file (e.g., `Voron.json`).
        
    - The local conflict is renamed (e.g., `Voron.bak`) to prevent data loss.
        
- **Full Sync Fallback:** If the client loses its `last_sync_timestamp`, it requests a full fetch (`since=0`) and merges utilizing file hashes to identify changes.
    

## 3. Hybrid Storage Architecture

We separate data based on its nature to optimize for cost and performance.

### 3.1 Configuration Data (Supabase JSONB)

- **Content:** Printer settings, Process configs, Filament presets (JSON text).
    
- **Storage:** **Supabase Database** (`jsonb` column).
    
- **Rationale:**
    
    - **Atomic Bundling:** A "Profile Bundle" (10+ files) updates in a single transaction.
        
    - **Query Speed:** 1 SQL query retrieves all 10 changed files instantly.
        
    - **Searchability:** Allows querying specific settings inside the JSON.
        

### 3.2 Binary Assets (Cloudflare R2)

- **Content:** Images, 3MF Models, Plugin ZIPs.
    
- **Storage:** **Cloudflare R2** (Object Storage).
    
- **Rationale:**
    
    - **Zero Egress Fees:** Massive cost savings for high-bandwidth downloads.
        
    - **Scalability:** Handles large files efficiently without bloating the database.
        

## 4. Network & Gateway Role (Lane B)

All sync traffic is routed through the **Orca API Gateway** (`api.orcaslicer.com`).

### 4.1 Gateway Responsibilities

- **Authentication:** Validates User JWTs at the gateway layer before the request reaches Supabase.
    
- **Protocol Translation:** Converts HTTP sync requests into optimized SQL queries or R2 fetches.
    
- **Infrastructure Abstraction:** The client does not know whether data comes from DB or R2; it only talks to the Orca API Gateway.
    

### 4.2 Caching Policy

- **Sync Endpoints (`/sync/pull`):** **CACHE DISABLED**.
    
    - Must use `Cache-Control: no-store` to ensure real-time consistency and data privacy.
        
- **Public Assets (Plugins/Images):** **CACHE ENABLED**.
    
    - Aggressive caching enabled for read-heavy public resources.
        

## 5. Access Control (ACL)

### 5.1 The "Smart Gateway" Pattern

Storage buckets (R2) are private. Direct public access is disabled.

1. **Request:** User asks the Orca API Gateway for a file.
    
2. **Check:** Gateway queries Supabase to verify permissions (Owner? Public? Shared?).
    
3. **Serve:**
    
    - If Allowed: Gateway streams the file from R2 or generates a Presigned URL.
        
    - If Denied: Gateway returns `403 Forbidden`.
        

### 5.2 Sharing Logic

- **Bundles:** Users can group multiple JSONs into a "Bundle".
    
- **Visibility:** Defined in the Database (Private / Public / Shared with User X).
    
- **Partial Sharing:** Since JSONs are in the DB, users can share specific subsets of settings without complex file manipulation.
- 
### 6.0 Data Lifecycle & Cleanup

- **Slim Tombstones:** Content payload is nulled upon soft-delete to reclaim storage.
    
- **Sync Horizon:** Soft-deleted records are permanently purged after 180 days via Cron.
    
- **Stale Client Handling:** Clients requesting syncs older than 180 days receive a `410 Gone` response, triggering a self-healing Full Sync protocol.