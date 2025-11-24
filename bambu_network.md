# Bambu Networking Architecture

## Overview

The Bambu Networking system is a **plugin-based networking architecture** that provides OrcaSlicer with cloud and local printer communication capabilities. It uses **dynamic library loading** to separate networking implementation from the main application, enabling updates and version compatibility management.

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          OrcaSlicer GUI                             │
│  ┌────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │  GUI_App       │  │  DeviceManager   │  │  MachineObject   │   │
│  └────────┬───────┘  └────────┬─────────┘  └────────┬─────────┘   │
│           │                   │                     │              │
│           │                   │                     │              │
│           └───────────────────┴─────────────────────┘              │
│                               │                                    │
└───────────────────────────────┼────────────────────────────────────┘
                                │
                                │ Creates and manages
                                ▼
        ┌────────────────────────────────────────────┐
        │         NetworkAgent Wrapper               │
        │  (src/slic3r/Utils/NetworkAgent.cpp/hpp)  │
        │                                            │
        │  • Dynamic library loading                 │
        │  • Function pointer management             │
        │  • Type conversions & compatibility        │
        │  • Callback registration                   │
        └─────────────┬──────────────────────────────┘
                      │ dlopen/LoadLibrary
                      │ Function pointers via dlsym/GetProcAddress
                      ▼
        ┌────────────────────────────────────────────┐
        │    bambu_networking Dynamic Library        │
        │        (Platform-specific plugin)          │
        │                                            │
        │  Windows: bambu_networking.dll             │
        │  macOS:   libbambu_networking.dylib        │
        │  Linux:   libbambu_networking.so           │
        │                                            │
        │  Location: {data_dir}/plugins/             │
        │  Backup:   {data_dir}/plugins/backup/      │
        └─────────────┬──────────────────────────────┘
                      │
        ┌─────────────┴──────────────┐
        │                            │
        ▼                            ▼
┌──────────────────┐      ┌──────────────────────┐
│  Bambu Cloud     │      │  Local Printers      │
│  - MQTT Server   │      │  - LAN Connection    │
│  - REST APIs     │      │  - FTP Upload        │
│  - User Auth     │      │  - MQTT Messages     │
│  - Model Mall    │      │  - SSDP Discovery    │
└──────────────────┘      └──────────────────────┘
```

## Core Components

### 1. NetworkAgent Class

**Location:** `src/slic3r/Utils/NetworkAgent.hpp` and `NetworkAgent.cpp`

**Purpose:** Wrapper class that dynamically loads and interfaces with the bambu_networking plugin library.

**Key Responsibilities:**
- Load networking plugin at runtime via `initialize_network_module()`
- Resolve function symbols from the library
- Provide C++ interface to library functions
- Manage agent lifecycle (creation/destruction)
- Handle version compatibility checks
- Support legacy version fallback

**Key Static Methods:**
```cpp
static int initialize_network_module(bool using_backup = false)
static int unload_network_module()
static std::string get_version()
static void* get_network_function(const char* name)
static void* get_bambu_source_entry()  // Loads BambuSource library
```

**Instance Methods:** The class wraps ~80+ functions from the plugin including:
- User management: `is_user_login()`, `change_user()`, `user_logout()`
- Server connectivity: `connect_server()`, `is_server_connected()`
- Printer operations: `connect_printer()`, `disconnect_printer()`
- Print jobs: `start_print()`, `start_local_print()`, etc.
- Settings sync: `get_user_presets()`, `put_setting()`
- And many more...

### 2. bambu_networking.hpp

**Location:** `src/slic3r/Utils/bambu_networking.hpp`

**Purpose:** Defines the interface contract between OrcaSlicer and the networking plugin.

**Key Definitions:**

#### Error Codes
```cpp
#define BAMBU_NETWORK_SUCCESS                    0
#define BAMBU_NETWORK_ERR_INVALID_HANDLE        -1
#define BAMBU_NETWORK_ERR_CONNECT_FAILED        -2
// ... 40+ error codes for different failure scenarios
```

#### Callback Function Types
```cpp
typedef std::function<void(int online_login, bool login)> OnUserLoginFn;
typedef std::function<void(std::string dev_id, std::string msg)> OnMessageFn;
typedef std::function<void(int status, int code, std::string msg)> OnUpdateStatusFn;
typedef std::function<bool()> WasCancelledFn;
// ... more callback types
```

#### Data Structures
```cpp
struct PrintParams {
    std::string dev_id, task_name, project_name;
    std::string filename, config_filename;
    std::string ams_mapping, ams_mapping_info;
    bool task_bed_leveling, task_flow_cali;
    // ... extensive print configuration
};

struct PublishParams;
struct TaskQueryParams;
struct detectResult;
```

#### Enumerations
```cpp
enum SendingPrintJobStage { /* Print job workflow states */ };
enum PublishingStage { /* Model publishing states */ };
enum BindJobStage { /* Printer binding workflow */ };
enum ConnectStatus { /* Connection states */ };
enum class MessageFlag { /* Message encryption flags */ };
```

### 3. Dynamic Library Plugin

**Library Name:** `bambu_networking`

**Versions:**
- Legacy: `01.10.01.01`
- Current: `02.01.01.52`

**Location (Configurable):**
- Primary: `{data_dir}/plugins/bambu_networking.{dll|dylib|so}`
- Backup: `{data_dir}/plugins/backup/bambu_networking.{dll|dylib|so}`

**Loading Strategy:**
1. Check if networking plugin installation enabled (`installed_networking` config)
2. Load library from plugins folder
3. If version mismatch, try backup folder
4. Resolve 80+ function symbols
5. Perform debug/release build consistency check

**Function Naming Convention:**
All exported functions follow pattern: `bambu_network_{operation}`

Examples:
- `bambu_network_create_agent`
- `bambu_network_connect_server`
- `bambu_network_start_print`
- `bambu_network_get_user_id`

## Communication Flows

### User Authentication Flow

```
┌──────────┐                  ┌──────────────┐              ┌─────────────┐
│ GUI_App  │                  │ NetworkAgent │              │ Cloud Server│
└─────┬────┘                  └──────┬───────┘              └──────┬──────┘
      │                              │                             │
      │ 1. User clicks login         │                             │
      ├──────────────────────────────>│                             │
      │                              │                             │
      │                              │ 2. build_login_cmd()        │
      │                              ├─────────────────────────────>│
      │                              │                             │
      │                              │ 3. connect_server()         │
      │                              ├─────────────────────────────>│
      │                              │                             │
      │                              │    4. Server connects       │
      │                              │<─────────────────────────────┤
      │                              │                             │
      │                              │ 5. change_user(user_info)   │
      │                              ├─────────────────────────────>│
      │                              │                             │
      │ 6. OnUserLoginFn callback    │    6. Authentication        │
      │<──────────────────────────────│<─────────────────────────────┤
      │                              │                             │
      │ 7. Update UI state           │                             │
      │                              │                             │
```

### Printer Discovery & Connection Flow

```
┌────────────┐              ┌──────────────┐              ┌────────────────┐
│ MachineObj │              │ NetworkAgent │              │ Local Printer  │
└─────┬──────┘              └──────┬───────┘              └────────┬───────┘
      │                            │                               │
      │ 1. start_discovery(true)   │                               │
      ├────────────────────────────>│                               │
      │                            │                               │
      │                            │ 2. SSDP broadcast             │
      │                            ├───────────────────────────────>│
      │                            │                               │
      │                            │    3. SSDP response           │
      │                            │<───────────────────────────────┤
      │                            │                               │
      │ 4. OnMsgArrivedFn callback │                               │
      │<────────────────────────────│                               │
      │ (device info JSON)         │                               │
      │                            │                               │
      │ 5. connect_printer()       │                               │
      ├────────────────────────────>│                               │
      │   (dev_id, IP, credentials)│                               │
      │                            │  6. MQTT connection           │
      │                            ├───────────────────────────────>│
      │                            │                               │
      │                            │  7. Connection established    │
      │ 8. OnPrinterConnectedFn    │<───────────────────────────────┤
      │<────────────────────────────│                               │
      │                            │                               │
      │                            │  8. Subscribe to topics       │
      │                            ├───────────────────────────────>│
      │                            │                               │
      │                            │  9. Status messages (MQTT)    │
      │ 10. OnMessageFn callback   │<───────────────────────────────┤
      │<────────────────────────────│    (continuous updates)       │
      │                            │                               │
```

### Print Job Submission Flow (Cloud Print)

```
┌─────────┐         ┌──────────────┐        ┌─────────┐        ┌─────────┐
│  Plater │         │ NetworkAgent │        │  Cloud  │        │ Printer │
└────┬────┘         └──────┬───────┘        └────┬────┘        └────┬────┘
     │                     │                     │                  │
     │ start_print()       │                     │                  │
     ├─────────────────────>│                     │                  │
     │ (PrintParams)       │                     │                  │
     │                     │                     │                  │
     │                     │ 1. Request project ID                  │
     │                     ├─────────────────────>│                  │
     │                     │                     │                  │
     │                     │ 2. Upload 3MF to OSS                   │
     │ UpdateStatus:       ├─────────────────────>│                  │
     │ "Uploading"         │                     │                  │
     │<─────────────────────│                     │                  │
     │                     │                     │                  │
     │                     │ 3. Post task to API │                  │
     │                     ├─────────────────────>│                  │
     │                     │                     │                  │
     │                     │                     │ 4. Task created  │
     │                     │                     ├──────────────────>│
     │                     │                     │                  │
     │                     │                     │ 5. Printer ACK   │
     │                     │                     │<──────────────────┤
     │ UpdateStatus:       │ 6. Wait for ACK     │                  │
     │ "Sending"           │<─────────────────────│                  │
     │<─────────────────────│                     │                  │
     │                     │                     │                  │
     │ UpdateStatus:       │ 7. Print started    │                  │
     │ "Finished"          │<─────────────────────┤                  │
     │<─────────────────────│                     │                  │
     │                     │                     │                  │
```

### Local Print Flow

```
┌─────────┐         ┌──────────────┐                ┌─────────────┐
│  Plater │         │ NetworkAgent │                │ LAN Printer │
└────┬────┘         └──────┬───────┘                └──────┬──────┘
     │                     │                               │
     │ start_local_print() │                               │
     ├─────────────────────>│                               │
     │                     │                               │
     │                     │ 1. FTP upload G-code          │
     │ UpdateStatus:       ├───────────────────────────────>│
     │ "Uploading"         │                               │
     │<─────────────────────│                               │
     │                     │                               │
     │                     │ 2. Send print command (MQTT)  │
     │                     ├───────────────────────────────>│
     │                     │                               │
     │                     │ 3. Printer confirms           │
     │ UpdateStatus:       │<───────────────────────────────┤
     │ "Finished"          │                               │
     │<─────────────────────│                               │
     │                     │                               │
```

## Use Cases

### 1. User Account Management
- **Login/Logout:** Authenticate with Bambu Cloud account
- **User Profile:** Get user info (ID, name, avatar, nickname)
- **Session Management:** Track login state, handle token refresh

### 2. Printer Management
- **Discovery:** SSDP-based local network printer discovery
- **Binding:** Bind printers to user account via ticket system
- **Connection:** Establish MQTT connections to printers (LAN/Cloud)
- **Monitoring:** Real-time status updates via MQTT messages

### 3. Print Job Operations

#### Cloud Print (with record)
- Upload 3MF to Bambu Cloud OSS
- Create cloud project and task
- Send to printer via cloud
- Record in cloud history

#### Local Print
- Direct FTP upload to printer
- MQTT command to start print
- No cloud record

#### SD Card Upload
- FTP upload only
- Store on printer SD card
- Manual start from printer UI

### 4. Settings Synchronization
- **Download:** Fetch user-saved presets from cloud
- **Upload:** Save/update custom profiles
- **Sync:** Keep printer/filament/print settings synchronized
- **Delete:** Remove obsolete settings

### 5. Model Mall Integration
- Browse featured designs (`get_design_staffpick`)
- Get model details and URLs
- Publish user models (`start_publish`)
- Rating and reviews

### 6. Analytics & Tracking
- Enable/disable tracking (`track_enable`)
- Track events with custom data (`track_event`)
- Update user properties (`track_update_property`)
- Session analytics

### 7. Certificate Management
- Install device certificates for encrypted communication
- Update certificates when needed
- Check certificate validity

### 8. Multi-Printer Support
- Enable multi-machine mode
- Subscribe to multiple printers
- Route messages to specific devices
- Manage selected printer state

## Key Design Patterns

### 1. **Plugin Architecture**
- **Separation of Concerns:** Networking logic isolated in plugin
- **Updateability:** Plugin can be updated independently
- **Version Management:** Multiple versions can coexist (backup folder)

### 2. **Callback-Based Event System**
- **Asynchronous Operations:** Network operations don't block UI
- **Event Notifications:** Callbacks for login, connection, messages, errors
- **Progress Tracking:** Update callbacks for long operations

### 3. **Function Pointer Indirection**
- **Dynamic Linking:** All plugin functions accessed via pointers
- **Lazy Loading:** Library loaded only when needed
- **Graceful Degradation:** Missing functions can be detected

### 4. **Type Compatibility Layer**
- **Legacy Support:** `PrintParams_Legacy` conversion for old library versions
- **Version Detection:** `use_legacy_network` flag determines which API to use
- **Smooth Migration:** Both versions supported simultaneously

### 5. **Queue to Main Thread**
- **Thread Safety:** Callbacks queued to main GUI thread
- **QueueOnMainFn:** Ensures GUI updates happen on correct thread
- **Race Condition Prevention:** Serializes UI updates

## Version Compatibility

### Version Check Flow
```cpp
bool check_networking_version() {
    std::string version = NetworkAgent::get_version();
    // Check if version compatible with this OrcaSlicer version
    // Format: XX.YY.ZZ.WW
}
```

### Version Comparison
- **Major.Minor.Patch.Build** format
- Minimum required version defined in code
- Incompatible versions rejected
- Fallback to backup plugin if available

### Legacy vs Current
```cpp
bool NetworkAgent::use_legacy_network = true;  // Default to legacy

// After loading, set based on version:
if (version >= "02.00.00.00") {
    use_legacy_network = false;  // Use new API
}
```

## Error Handling

### Error Code Categories

1. **Connection Errors (-1 to -3)**
   - Invalid handle
   - Connect/disconnect failures

2. **Operation Errors (-4 to -13)**
   - Send message failed
   - Bind/unbind failed
   - Settings operations failed

3. **File Errors (-14 to -23)**
   - File not exist
   - File over size
   - MD5 check failed
   - FTP upload failed

4. **Bind Errors (-1010 to -1090)**
   - Socket creation failed
   - Login timeout
   - Ticket exchange failed

5. **Print Job Errors (-2010 to -5010)**
   - Project creation failed
   - OSS upload failed
   - Notification failed
   - Printer wait timeout

### Error Handling Pattern
```cpp
int result = network_agent->some_operation(...);
if (result != BAMBU_NETWORK_SUCCESS) {
    // Log error
    BOOST_LOG_TRIVIAL(error) << "Operation failed: " << result;

    // Show user error message
    show_error_dialog(translate_error_code(result));

    // Update UI state
    update_status_fn(ERROR_STAGE, result, error_msg);
}
```

## Thread Safety

### Main Thread Queue Pattern
```cpp
// Set callback to queue work to main thread
agent->set_queue_on_main_fn([](std::function<void()> fn) {
    wxGetApp().CallAfter(fn);  // wxWidgets main thread dispatch
});

// Plugin uses this for all callbacks:
queue_on_main([callback_data] {
    user_callback(callback_data);  // Safe to update UI
});
```

### Callback Thread Model
- **Network operations:** Execute on background threads in plugin
- **Callbacks:** Always delivered on main thread via queue
- **UI Updates:** Safe to update wxWidgets controls in callbacks

## Configuration

### App Config Settings
```ini
[app]
installed_networking = true       # Enable/disable plugin
sync_system_preset = true         # Sync system presets

[network]
country_code = US                 # For region-specific servers
```

### Directory Structure
```
{data_dir}/
├── plugins/
│   ├── bambu_networking.dll      # Primary plugin
│   ├── BambuSource.dll           # Additional features
│   └── backup/
│       ├── bambu_networking.dll  # Fallback version
│       └── BambuSource.dll
└── logs/
    └── network_*.log             # Network operation logs
```

## Dependencies

### External Libraries (Plugin Dependencies)
- **MQTT Client:** For real-time printer communication
- **HTTP/HTTPS Client:** For REST API calls
- **OpenSSL:** For certificate validation and encryption
- **JSON Parser:** For message and API response handling
- **FTP Client:** For file uploads to printers

### OrcaSlicer Dependencies
- **Boost:** Filesystem, logging, format
- **wxWidgets:** Main thread dispatching
- **Platform APIs:** `dlopen`/`LoadLibrary` for dynamic loading

## Security Considerations

### Certificate Management
- Device-specific certificates for encrypted MQTT
- Certificate installation and validation
- Certificate update mechanism

### Message Encryption
```cpp
enum class MessageFlag {
    MSG_FLAG_NONE = 0,
    MSG_SIGN      = 1 << 0,  // Message signing
    MSG_ENCRYPT   = 1 << 1,  // Message encryption
};
```

### Access Control
- User authentication required for cloud features
- Access codes for printer binding
- SSL/TLS for FTP and MQTT connections

## Performance Considerations

### Asynchronous Operations
- All network I/O is non-blocking
- Cancellation support via `WasCancelledFn`
- Progress callbacks for long operations

### Connection Pooling
- Maintains persistent connections
- Automatic reconnection on connection loss
- Subscription management for efficiency

### Caching
- User settings cached locally
- Printer status cached and updated incrementally
- Reduces API calls

## Debugging & Logging

### Log Initialization
```cpp
agent->init_log();  // Starts logging in plugin
```

### Log Location
Logs written to `{data_dir}/logs/` with network operation details

### Verbose Logging
- All function calls logged with parameters
- Error conditions logged with full context
- Callback invocations logged

## Future Evolution

### Potential Improvements
1. **WebSocket Migration:** Replace MQTT with WebSocket for better firewall compatibility
2. **Offline Mode:** Better offline operation support
3. **P2P Discovery:** Enhanced local network discovery
4. **Incremental Uploads:** Resume interrupted uploads
5. **Background Sync:** Automated settings synchronization

### Extension Points
- Additional callbacks can be added
- New operations can be exposed via new function pointers
- Plugin versioning supports gradual migration

## Summary

The Bambu Networking system provides a **robust, plugin-based architecture** for cloud and local printer communication. Key strengths:

✅ **Modularity:** Clean separation via dynamic library
✅ **Versioning:** Multiple versions supported, graceful upgrades
✅ **Asynchronicity:** Non-blocking operations with progress callbacks
✅ **Thread Safety:** Main thread queuing for UI updates
✅ **Extensibility:** Easy to add new features via function pointers
✅ **Security:** Certificate-based encryption and authentication
✅ **Error Handling:** Comprehensive error codes and recovery

This architecture enables OrcaSlicer to provide sophisticated cloud and local networking features while maintaining stability and allowing independent updates to the networking layer.
