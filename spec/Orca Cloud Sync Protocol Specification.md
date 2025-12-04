
Date: 2025-11-28

Scope: OrcaSlicer Configuration Synchronization & Versioning

Architecture Pattern: Offline-First, Incremental Sync, Optimistic Concurrency Control (Time-based)

## 1. Core Architecture Principles

- **Timestamp as Truth:** The system relies exclusively on high-precision **ISO 8601 String Timestamps** (e.g., `"2025-11-28T14:30:00.123456Z"`) for both synchronization cursors and optimistic locking tokens. 
    
- **Decoupled History:** Synchronization and Version Control are physically separated. The main table only stores the _current_ state to ensure maximum sync performance, while a secondary table stores historical revisions.
    
- **Automatic Pruning:** The system enforces a strict limit of **10 versions per profile** (1 Active + 9 Historical) at the database level using Triggers.
    

---

## 2. Database Schema & Data Logic

### 2.1 Primary Active Table (`profiles`)

Stores only the current, active version of the configuration. This is the table used by the Sync Engine.


```SQL
CREATE TABLE profiles (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    content JSONB NOT NULL,
    updated_at TIMESTAMPTZ DEFAULT NOW(), -- Acts as Sync Cursor & Optimistic Lock
    created_at TIMESTAMPTZ DEFAULT NOW()
);
-- Index for sync queries filtering by user and time
CREATE INDEX idx_profiles_sync ON profiles (user_id, updated_at);
```

### 2.2 History Archive Table (`profile_versions`)

Stores the previous 9 versions. This table is **never** queried during standard Sync operations.


```sql
CREATE TABLE profile_versions (
    version_id BIGSERIAL PRIMARY KEY,
    profile_id UUID NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
    content JSONB NOT NULL,            -- Snapshot of the config
    recorded_at TIMESTAMPTZ NOT NULL,  -- The 'updated_at' of this version
    created_at TIMESTAMPTZ DEFAULT NOW(),
    
    -- Index for fast retrieval and pruning
    CONSTRAINT fk_profile FOREIGN KEY (profile_id) REFERENCES profiles (id)
);
CREATE INDEX idx_versions_lookup ON profile_versions (profile_id, recorded_at DESC);
```

_Note: `ON DELETE CASCADE` ensures that if a user permanently deletes a profile, its history is also wiped._

### 2.3 Tombstone Table (`tombstones`)

Used to propagate deletion events to other devices.


```sql
CREATE TABLE tombstones (
    id BIGSERIAL PRIMARY KEY,
    user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
    entity_id UUID NOT NULL,
    entity_type VARCHAR(50) NOT NULL,
    deleted_at TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX idx_tombstone_sync ON tombstones (user_id, deleted_at);
```

---

## 3. Server-Side Automation (Triggers)

### 3.1 The "Archive & Prune" Trigger

This logic executes **BEFORE UPDATE** on the `profiles` table. It manages the "Top 10" rule automatically.


```sql
CREATE OR REPLACE FUNCTION func_archive_and_prune() RETURNS TRIGGER AS $$
BEGIN
    -- Optimization: Do nothing if content hasn't changed
    IF NEW.content IS NOT DISTINCT FROM OLD.content THEN
        RETURN NEW;
    END IF;

    -- 1. ARCHIVE: Save the OLD version to history
    INSERT INTO profile_versions (profile_id, content, recorded_at)
    VALUES (OLD.id, OLD.content, OLD.updated_at);

    -- 2. PRUNE: Keep only the latest 9 historical records (Total 10 incl. active)
    DELETE FROM profile_versions
    WHERE profile_id = OLD.id
      AND version_id NOT IN (
          SELECT version_id FROM profile_versions
          WHERE profile_id = OLD.id
          ORDER BY recorded_at DESC
          LIMIT 9
      );

    -- 3. UPDATE TIMESTAMP: Ensure new active record has fresh time
    NEW.updated_at := NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
```

### 3.2 The Tombstone Trigger

Executes **AFTER DELETE** on the `profiles` table.


```sql
CREATE OR REPLACE FUNCTION func_create_tombstone() RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO tombstones (user_id, entity_id, entity_type, deleted_at)
    VALUES (OLD.user_id, OLD.id, 'profile', NOW());
    RETURN OLD;
END;
$$ LANGUAGE plpgsql;
```

---

## 4. Client State Management

The client application must maintain two distinct types of timestamp state:

### 4.1 Global Sync Cursor

- **Variable:** `last_sync_timestamp`
    
- **Scope:** One per user account.
    
- **Purpose:** Tracks the high-water mark of the last successful **Pull**.
    
- **Behavior:** When pulling, ask server for changes `> last_sync_timestamp`.
    

### 4.2 Per-Entity Lock Token

- **Variable:** `local_updated_at`
    
- **Scope:** One per profile file.
    
- **Purpose:** Stores the exact timestamp of the file version currently on the local disk.
    
- **Behavior:** 
    - **On Pull:** Updated from the server's `updated_at`.
    - **On Push:** Sent as `original_updated_at` to prove the modification is based on the latest server version. If creating a new file, this token is null.
    

---

## 5. Synchronization Protocol Flow

### 5.1 Phase 1: Pull (Downstream)

**Goal:** Fetch updates and deletions from the cloud.

- **Endpoint:** `GET /api/v1/sync/pull?cursor={last_sync_timestamp}`
    
- **Server Logic:**
    
    - Fetch active records where `updated_at > cursor`.
        
    - Fetch tombstones where `deleted_at > cursor`.
        
    - Calculate `next_cursor` (Max timestamp seen).
        
- **Response:**
    
    ```json
    {
      "next_cursor": "2025-11-28T14:35:00.123456Z",
      "upserts": [
        { "id": "p1", "content": {...}, "updated_at": "..." }
      ],
      "deletes": ["p2", "p3"]
    }
    ```
    
- **Client Action:** Apply changes locally. Update `last_sync_timestamp` to `next_cursor`.
    

### 5.2 Phase 2: Push (Upstream)

**Goal:** Upload local modifications with Optimistic Concurrency Control.

- **Endpoint:** `POST /api/v1/sync/push`
    
- **Payload:**
    
    ```json
    {
      "id": "p1",
      "content": { ... },
      "original_updated_at": "2025-11-28T10:00:00.000000Z" 
      // ^ Optional. 
      // Present = Update existing record (must match DB). 
      // Absent/Null = Insert new record.
    }
    ```
    
- **Server Logic:**

    1. **If `original_updated_at` is provided (Update Flow):**
       - Attempt SQL: `UPDATE profiles SET content = $content, updated_at = NOW() WHERE id = $id AND updated_at = $original_updated_at RETURNING *;`
       - If 0 rows returned:
         - Fetch current record by ID.
         - If record exists: **409 Conflict** (Client is stale). Return server's `current`.
         - If record missing: **409 Conflict** (Record deleted on server). Return `null`.

    2. **If `original_updated_at` is missing (Insert Flow):**
       - Attempt SQL: `INSERT INTO profiles (id, content) VALUES ($id, $content) RETURNING *;`
       - If PK violation (ID exists):
         - **409 Conflict** (ID collision). Return server's `current`.

- **Response Scenarios:**
    
    - **200 OK:** Operation successful. Returns `new_updated_at`. Client updates local state.
        
    - **409 Conflict:** Operation failed.
        - **Stale/Collision:** Returns server's current version (`{ "id": ..., "updated_at": ... }`). Client must merge or overwrite, then retry push using the *new* server timestamp.
        - **Deleted:** Returns `null`. Client must decide to re-create (push without token) or accept deletion.
        

---

## 6. Version Control API (The "Time Machine")

Since history is decoupled, we use separate endpoints for version management.

### 6.1 List History

- **GET** `/api/v1/profiles/{id}/history`
    
- **Response:** Returns metadata only (saves bandwidth).
    
    ```json
    [
      { "version_id": 105, "recorded_at": "2025-11-28T10:00:00Z" },
      { "version_id": 104, "recorded_at": "2025-11-27T09:00:00Z" }
    ]
    ```
    

### 6.2 Get Historical Content

- **GET** `/api/v1/profiles/{id}/history/{version_id}`
    
- **Response:** Returns the full JSON content of that specific version.
    

### 6.3 Rollback

- **Logic:** The client fetches the historical content (via 6.2) and simply performs a standard **Push** (via 5.2) with this content.
    
- **Result:** The server treats this as a new update, automatically archiving the "bad" current version into history and setting the "old" content as the new active version.
    

---

## 7. Maintenance & Cleanup

### 7.1 Tombstone Retention

A `pg_cron` job runs daily to clean up tombstones older than 30 days.

- `DELETE FROM tombstones WHERE deleted_at < NOW() - INTERVAL '30 days';`
    

### 7.2 Full Resync Protocol

If a client sends a cursor older than 30 days, the server responds with `410 Gone`. The client must discard its local sync state (`last_sync_timestamp`) and perform a full re-download.

## 8. Summary of Advantages

1. **High Performance:** Sync queries are lightning-fast because they scan small, active tables, ignoring historical bloat.
    
2. **Zero Maintenance:** PostgreSQL Triggers automatically handle version archiving and pruning (Top 10 limit). No application logic is needed for cleanup.
    
3. **Conflict Safety:** String-based timestamp comparison ensures that no user overwrite occurs silently.
    
4. **Storage Efficient:** History is cleaned up automatically; deleted profiles cascade-delete their history.