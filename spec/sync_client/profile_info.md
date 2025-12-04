# Profile Sync Info States File

## Overview

OrcaSlicer uses a `.info` file to store the sync info of a profile. For each profile, there is a `.info` file alongside the profile (`.json`) file locally. This document describes the `.info` file format and how it is used.

## File Format

```
version
sync_info
user_id
setting_id
base_id
updated_time
```

### Field Definitions

| Field | Description |
|-------|-------------|
| `version` | version of the info file format |
| `sync_info` | Sync status of the profile (see values below) |
| `user_id` | ID of the user in Orca Cloud |
| `setting_id` | UUID assigned by the server (empty for new local presets) |
| `base_id` | ID of the base profile |
| `updated_time` | Server timestamp in ISO 8601 format for optimistic concurrency control. Must be stored as-is from server. Empty for newly created profiles. |

## Sync Info Values

The `sync_info` field acts as a state machine flag indicating what synchronization action is needed. Each preset contains this field (`Preset.hpp`) that works in conjunction with the sync operations.
It has the following values: "create", "update", "delete", "save", "hold"

### `"create"` - New Local Preset, Needs Upload

**Set when:** User creates a brand new preset locally that doesn't exist on the server.

**Code reference:** `CalibrationWizard::save_preset()` in `CalibrationWizard.cpp`
```cpp
new_preset->sync_info = "create";
if (wxGetApp().is_user_login())
    new_preset->user_id = wxGetApp().getAgent()->get_user_id();
```

**Sync action:** Push this preset to the cloud as a new profile.

**After sync:** Cleared to `""` and `setting_id` is assigned from server response.

---

### `"update"` - Modified Local Preset, Needs Upload

**Set when:**
1. User modifies an existing preset that's already synced to the cloud
2. Local version is newer than cloud version (`local updated_time > cloud updated_time`)

**Code reference:** `PresetCollection::load_user_preset()` in `Preset.cpp`
```cpp
if (cloud_update_time < iter->updated_time)
    iter->sync_info = "update";
```

**Sync action:** Push updated content to the cloud using `sync_push()` with optimistic concurrency control.

**After sync:** Cleared to `""` and `local_updated_at` is updated with server's `new_updated_at`.

---

### `"delete"` - Marked for Deletion on Server

**Set when:** User deletes a preset that exists on the server (has a non-empty `setting_id`).

**Code reference:** `CreateFilamentPresetDialog::delete_preset()` in `CreatePresetsDialog.cpp`
```cpp
m_presets.set_sync_info_and_save(need_delete_preset->name,
                                  need_delete_preset->setting_id,
                                  "delete", 0);
wxGetApp().delete_preset_from_cloud(need_delete_preset->setting_id);
```

**Sync action:** Send DELETE request to the cloud API.

**After sync:** Preset is removed from local storage.

---

### `"save"` - Downloaded from Cloud, Needs Local Save

**Set when:** A preset is pulled from the cloud (either new or updated) and needs to be persisted to local disk.

**Code reference:** `PresetCollection::load_user_preset()` in `Preset.cpp`
```cpp
iter->sync_info = "save";   // existing preset updated from cloud
preset.sync_info = "save";  // new preset created from cloud
```

**Sync action:** Handled by `PresetCollection::save_user_presets()` which writes the preset JSON and `.info` file to disk.

**After save:** Cleared to `""` in `PresetCollection::save_user_presets()`
```cpp
if (preset->sync_info != "save") continue;
preset->sync_info.clear();
preset->file = path_for_preset(*preset);
```

---

### `"hold"` - Temporary Skip State

**Set when:** Internal use to temporarily exclude a preset from sync operations (e.g., during concurrent sync).

**Code reference:** `PresetCollection::get_user_presets()` in `Preset.cpp`
```cpp
if (preset.sync_info == "hold") continue;
```

**Sync action:** Skipped entirely - not included in sync push list from `get_user_presets()`.

**Purpose:** Prevents race conditions when multiple sync operations might process the same preset.

---

### `""` (empty) - In Sync, No Action Needed

**Set when:**
1. After successful sync completion
2. When local and cloud timestamps match exactly

**Code reference:** `PresetCollection::load_user_preset()` in `Preset.cpp`
```cpp
else
    iter->sync_info.clear();
```

**Sync action:** Preset is skipped in `get_user_presets()` if it already has a `setting_id`.

**Selection logic:** `PresetCollection::get_user_presets()` in `Preset.cpp`
```cpp
if (!preset.setting_id.empty() && preset.sync_info.empty()) continue;
```

---

## State Transition Diagram

```
                          ┌──────────────────────────────────────┐
                          │           LOCAL OPERATIONS           │
                          └──────────────────────────────────────┘
                                          │
            ┌─────────────────────────────┼─────────────────────────────┐
            │                             │                             │
            ▼                             ▼                             ▼
    ┌───────────────┐           ┌───────────────┐             ┌───────────────┐
    │    "create"   │           │    "update"   │             │    "delete"   │
    │  (new preset) │           │   (modified)  │             │   (removed)   │
    └───────┬───────┘           └───────┬───────┘             └───────┬───────┘
            │                           │                             │
            │ sync_push()               │ sync_push()                 │ DELETE API
            │ (no original_updated_at)  │ (with original_updated_at)  │
            ▼                           ▼                             ▼
    ┌───────────────┐           ┌───────────────┐             ┌───────────────┐
    │      ""       │           │      ""       │             │   (removed    │
    │  (in sync)    │           │  (in sync)    │             │    locally)   │
    └───────────────┘           └───────────────┘             └───────────────┘


                          ┌──────────────────────────────────────┐
                          │           CLOUD OPERATIONS           │
                          └──────────────────────────────────────┘
                                          │
                                          ▼
                                  ┌───────────────┐
                                  │    "save"     │
                                  │ (from cloud)  │
                                  └───────┬───────┘
                                          │
                                          │ save_user_presets()
                                          │ (write to disk)
                                          ▼
                                  ┌───────────────┐
                                  │      ""       │
                                  │  (in sync)    │
                                  └───────────────┘
```

## Decision Logic: Which Presets Need Sync?

The `PresetCollection::get_user_presets()` method in `Preset.cpp` determines which presets need to be pushed:

```cpp
int PresetCollection::get_user_presets(PresetBundle *preset_bundle,
                                        std::vector<Preset> &result_presets)
{
    for (Preset &preset : m_presets) {
        // Skip non-user presets (system, default, project-embedded)
        if (!preset.is_user()) continue;

        // Skip presets with incomplete inheritance
        if (preset.base_id.empty() && preset.inherits() != "") continue;

        // Skip presets that are already in sync (have setting_id, no sync_info)
        if (!preset.setting_id.empty() && preset.sync_info.empty()) continue;

        // Skip presets in hold state
        if (preset.sync_info == "hold") continue;

        // This preset needs sync action
        result_presets.push_back(preset);
    }
}
```

**A preset needs pushing if:**
1. It's a user preset (`is_user() == true`)
2. Has valid inheritance (`base_id` is set OR no `inherits`)
3. Either:
   - Has no `setting_id` (new preset, never synced), OR
   - Has non-empty `sync_info` (modified, deleted, etc.)
4. Is not in `"hold"` state

## Related Files

- `src/libslic3r/Preset.hpp` - Preset class definition with `sync_info` field
- `src/libslic3r/Preset.cpp` - Sync info handling and `get_user_presets()`
- `src/slic3r/Utils/OrcaNetwork.hpp` - Sync protocol data structures
- `src/slic3r/Utils/OrcaNetwork.cpp` - `sync_push()` and `sync_pull()` implementation
- `src/slic3r/GUI/CalibrationWizard.cpp` - Example of setting `"create"` and `"update"`
- `src/slic3r/GUI/CreatePresetsDialog.cpp` - Example of setting `"delete"`