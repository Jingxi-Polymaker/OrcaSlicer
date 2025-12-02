# OrcaNetwork Documentation

## Overview

**OrcaNetwork** implements the `INetworkAgent` interface in-process (no external
`bambu_networking` library). It now targets the **Supabase + Orca API Gateway**
stack described in *Orca API Gateway.md*:

- **Auth (Lane A):** direct HTTPS calls to Supabase GoTrue at
  `https://auth.orcaslicer.com/auth/v1/*` using PKCE.
- **Data (Lane B):** application traffic (presets, sync, future marketplace)
  will go through the Cloudflare Worker gateway at `https://api.orcaslicer.com`.
- **Open source & compiled in:** all logic lives in `src/slic3r/Utils/`.
- **Printer operations:** remain stubs for compatibility.

> Legacy note: earlier revisions documented a local Flask backend at
> `http://localhost:8080`. That flow is deprecated; keep it only for ad‑hoc
> developer testing.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     OrcaSlicer GUI                          │
│  ┌────────────┐  ┌────────────────┐  ┌──────────────────┐  │
│  │  GUI_App   │  │ DeviceManager  │  │  UserManager     │  │
│  └─────┬──────┘  └────────┬───────┘  └────────┬─────────┘  │
│        │                  │                    │            │
│        └──────────────────┴────────────────────┘            │
│                           │                                 │
│                           │ (Polymorphic interface)         │
│                           ▼                                 │
│             ┌─────────────────────────────┐                 │
│             │   OrcaNetwork               │                 │
│             │  (src/slic3r/Utils/)        │                 │
│             │                             │                 │
│             │  • User Management          │                 │
│             │  • Settings Sync            │                 │
│             │  • Server Connectivity      │                 │
│             │  • Stub Printer Ops         │                 │
│             └──────────┬──────────────────┘                 │
└────────────────────────┼────────────────────────────────────┘
                         │
                         │ HTTP/REST (libcurl via Http class)
                         ▼
         ┌─────────────────────────────────┐
         │           Supabase Auth         │
         │      https://auth.orcaslicer.com│
         │  • /auth/v1/authorize (PKCE)    │
         │  • /auth/v1/token (PKCE)        │
         │  • /auth/v1/logout              │
         └─────────────────────────────────┘
                        │
                        ▼
         ┌─────────────────────────────────┐
         │      Orca API Gateway           │
         │     https://api.orcaslicer.com  │
         │  • /api/v1/presets (CRUD/sync)  │
         │  • /api/v1/health               │
         │  • future plugin endpoints      │
         └─────────────────────────────────┘
                         │
                         ▼
         ┌─────────────────────────────────┐
         │   JSON File Storage             │
         │   backend/data/                 │
         │                                 │
         │  • users.json                   │
         │  • presets/{user_id}/*.json     │
         └─────────────────────────────────┘
```

## Key Features

### 1. User Management (Supabase PKCE)

**Implemented Methods:**
- `change_user(user_info)` – Accepts Supabase session JSON (or WebView `user_login` message) and sets the active session.
- `set_user_session(...)` – Direct helper invoked by WebView bridge after browser auth completes.
- `user_logout(request)` – Revokes Supabase session (when `request=true`) and clears local/secure storage.
- `is_user_login()`, `get_user_id()`, `get_user_name()`, `get_user_avatar()`, `get_user_nickanme()` – read accessors.
- `build_login_cmd()` – Emits PKCE-capable login command for the embedded web view.
- `build_logout_cmd()` / `build_login_info()` – broadcast logout/login snapshots to the web view.

**Supabase PKCE Login Flow (desktop):**
1) **Generate PKCE + state**: OrcaNetwork creates `code_verifier`, `S256` `code_challenge`, random `state`, and loopback redirect `http://localhost:41172/callback` on startup (`ensure_pkce_material`).
2) **Launch browser via WebView**: `build_login_cmd()` returns JSON:
```json
{
  "action": "login",
  "provider": "orca",
  "backend_url": "https://auth.orcaslicer.com",
  "pkce": {
    "code_challenge": "<S256>",
    "code_challenge_method": "S256",
    "state": "<random>",
    "redirect_uri": "http://localhost:41172/callback",
    "code_verifier": "<kept in app>",
    "loopback_port": 41172
  }
}
```
The JS/UI layer builds the Supabase authorize URL with these fields and opens the system browser.
3) **User authenticates** in the browser; Supabase redirects to the local loopback server with `code` + `state`.
4) **Token exchange**: The WebView helper posts the Supabase token response (includes `access_token`, `refresh_token`, user profile) back into the app as `user_login` JSON; OrcaNetwork parses it in `change_user()` and calls `set_user_session()`.
5) **Secure persistence**: `refresh_token` is saved into OS keyring via `wxSecretStore` when available; otherwise AES-256-GCM encrypted to `supabase_refresh_token.sec` under the user config dir (key derived from machine id).
6) **Silent sign-in / refresh**: On startup, OrcaNetwork loads the stored refresh token and exchanges it at `/auth/v1/token` (grant_type=refresh_token). If successful, it repopulates the session and fires the login callback.
7) **Bearer usage**: All API calls attach `Authorization: Bearer <access_token>` automatically; the token endpoint intentionally omits Authorization to allow refresh.

- Loopback redirect: the listener chooses `ORCA_LOOPBACK_PORT` if set (default 41172) and falls back to 41173/41174 when busy; the callback returns a friendly HTML page and auto-closes the browser tab.

**Session Management Notes:**
- Logout clears memory, deletes secure storage, and optionally POSTs `/auth/v1/logout` with the refresh token.
- PKCE material is regenerated lazily but kept per-process to avoid cross-login mixing.
- Anon key usage is **scoped to auth only**: the `apikey` from `ORCA_BACKEND_ANON_KEY`
  is attached exclusively to Supabase `/auth/v1/*` calls, and is stripped from all data-lane
  traffic to `api.orcaslicer.com`.
- Access tokens are auto-refreshed shortly before expiry and retried once on HTTP 401 with a brief backoff; refresh failures clear the session and notify the UI.
- Set `ORCA_AUTH_ENABLED=0` to temporarily disable the auth lane (skips silent refresh/PKCE auto-login) when debugging.

### 2. Settings Sync

**Implemented Methods:**
- `get_user_presets(map*)` - Get all user cloud presets
- `request_setting_id(name, values, http_code)` - Create new preset
- `put_setting(id, name, values, http_code)` - Update preset
- `delete_setting(id)` - Delete preset
- `get_setting_list(version, progress_fn, cancel_fn)` - Sync presets
- `get_setting_list2(version, check_fn, progress_fn, cancel_fn)` - Enhanced sync

**Preset Data Structure:**
```cpp
// In-memory format: map[type][setting_id] = json_string
std::map<std::string, std::map<std::string, std::string>>* user_presets;

// Example:
user_presets["print"]["uuid-123"] = "{\"name\":\"Fast Print\", ...}";
user_presets["filament"]["uuid-456"] = "{\"name\":\"PLA Basic\", ...}";
```

**Synchronization Flow:**
1. Call `get_setting_list2()` with callbacks
2. Background thread fetches from `/api/v1/presets/sync` (via API Gateway)
3. For each preset, `CheckFn` callback invoked with preset info
4. Progress reported via `ProgressFn` (0-100%)
5. Supports cancellation via `WasCancelledFn`

### 3. Server Connectivity

**Implemented Methods:**
- `connect_server()` - Test connection to backend
- `is_server_connected()` - Check connection status
- `refresh_connection()` - Re-test connection
- `start_subscribe(module)` - Stub (logs only)
- `stop_subscribe(module)` - Stub (logs only)

**Health Check:**
- GET `/api/v1/health` endpoint (served by the API Gateway)
- Sets `is_connected` flag on success
- Invokes `OnServerConnectedFn` callback

### 4. Dummy Printer Operations

All printer-related methods are implemented as **stubs** that:
- Log the operation via Boost.Log
- Return `BAMBU_NETWORK_SUCCESS`
- Do not perform actual operations

Examples:
- `connect_printer()`, `disconnect_printer()`
- `send_message()`, `send_message_to_printer()`
- `start_print()`, `start_local_print()`, etc.
- `bind()`, `unbind()`
- `get_camera_url()`, `get_printer_firmware()`

This ensures compatibility with existing code without implementing full printer functionality.

## Usage

### Starting the Backend Service

```bash
# Navigate to backend directory
cd backend

# Install dependencies (first time only)
pip install -r requirements.txt

# Start the service
python orca_backend.py
```

Service starts on `http://localhost:8080`

**Default Test Users:**
- Username: `test_user`, Password: `password123`
- Username: `admin`, Password: `admin123`

### Using OrcaNetwork in Code

```cpp
#include "OrcaNetwork.hpp"

// Create instance
auto network = new Slic3r::OrcaNetwork("/path/to/logs");

// Configure
network->set_config_dir("/path/to/config");
network->set_country_code("US");
// Backend URL is fixed to the Supabase auth host (https://auth.orcaslicer.com).
// For internal testing you can set ORCA_BACKEND_URL env var before launch.

// Register callbacks
network->set_queue_on_main_fn([](std::function<void()> fn) {
    wxGetApp().CallAfter(fn); // Ensure thread safety
});

network->set_on_user_login_fn([](int online_login, bool login) {
    if (login) {
        std::cout << "User logged in!" << std::endl;
    }
});

network->set_on_server_connected_fn([](int return_code, int reason_code) {
    if (return_code == 0) {
        std::cout << "Server connected!" << std::endl;
    }
});

// Initialize
network->init_log();
network->start();

// Connect to backend
network->connect_server();

// Login
std::string user_info = R"({"username":"test_user","password":"password123"})";
network->change_user(user_info);

// Get presets
std::map<std::string, std::map<std::string, std::string>> presets;
network->get_user_presets(&presets);

// Create new preset
std::map<std::string, std::string> values;
values["layer_height"] = "0.2";
values["infill_density"] = "20%";

unsigned int http_code;
std::string setting_id = network->request_setting_id("My Profile", &values, &http_code);

// Cleanup
delete network;
```

## Configuration

### Backend Hosts

- **Auth host (Lane A):** `https://auth.orcaslicer.com` (Supabase). Requires
  `apikey` = anon key. Used for `/auth/v1/*` and PKCE login/refresh/logout.
- **API Gateway host (Lane B):** `https://api.orcaslicer.com` for presets/sync and
  future cloud features. Do **not** send the anon key; the gateway handles
  credentials server-side.

Current implementation uses a fixed Supabase auth host (`https://auth.orcaslicer.com`)
for both auth and API calls. For internal testing you may override via the
`ORCA_BACKEND_URL` environment variable before launching OrcaSlicer.

### Logging

OrcaNetwork uses Boost.Log with `BOOST_LOG_TRIVIAL`:
- `trace` - HTTP requests/responses
- `info` - Operations and state changes
- `error` - Failures and exceptions

### Thread Safety

- All state mutations protected by `std::mutex`
- Callbacks invoked on main thread via `QueueOnMainFn`
- HTTP operations synchronous (blocking)
- `get_setting_list2()` uses background thread for async sync

## Comparison: OrcaNetwork vs NetworkAgent

| Feature | NetworkAgent | OrcaNetwork |
|---------|--------------|-------------|
| **Source Code** | Proprietary binary | Open source C++ |
| **Loading** | Dynamic library (dll/dylib/so) | Compiled into OrcaSlicer |
| **Backend** | Bambu Cloud (production) | Local Flask service (testing) |
| **User Management** | Full Bambu account | Simple username/password |
| **Settings Sync** | Bambu Cloud presets | JSON file storage |
| **Printer Operations** | Full MQTT/FTP support | Stub implementations |
| **Discovery** | SSDP device discovery | Stub |
| **Print Jobs** | Cloud & local printing | Stub |
| **Version Updates** | Plugin updates | Code updates |
| **Dependencies** | None (self-contained) | Python backend service |

## API Compatibility

OrcaNetwork implements the **exact same public interface** as NetworkAgent:
- Same method signatures
- Same return codes (BAMBU_NETWORK_*)
- Same callback types
- Same data structures (PrintParams, etc.)

This ensures **drop-in compatibility** - existing code works without modification.

## Error Handling

### Error Codes

OrcaNetwork returns standard error codes from `bambu_networking.hpp`:

```cpp
#define BAMBU_NETWORK_SUCCESS                    0
#define BAMBU_NETWORK_ERR_INVALID_HANDLE        -1
#define BAMBU_NETWORK_ERR_CONNECT_FAILED        -2
#define BAMBU_NETWORK_ERR_REQUEST_SETTING_FAILED -7
#define BAMBU_NETWORK_ERR_PUT_SETTING_FAILED    -8
#define BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED -9
#define BAMBU_NETWORK_ERR_DEL_SETTING_FAILED    -10
#define BAMBU_NETWORK_ERR_TIMEOUT               -17
#define BAMBU_NETWORK_ERR_CANCELED              -18
#define BAMBU_NETWORK_ERR_INVALID_RESULT        -19
```

### HTTP Error Callback

```cpp
network->set_on_http_error_fn([](unsigned http_code, std::string http_body) {
    std::cerr << "HTTP Error " << http_code << ": " << http_body << std::endl;
});
```

Invoked automatically on HTTP failures (status >= 400).

## Implementation Details

### HTTP Client

Uses OrcaSlicer's existing `Http` class (wraps libcurl):

```cpp
auto http = Http::get(url)
    .header("Authorization", "Bearer " + token)
    .header("Content-Type", "application/json")
    .timeout_max(30)
    .on_complete([](std::string body, unsigned status) { ... })
    .on_error([](std::string body, std::string error, unsigned status) { ... })
    .perform_sync();
```

### JSON Parsing

Uses Boost.PropertyTree for JSON:

```cpp
namespace pt = boost::property_tree;
std::stringstream ss(json_string);
pt::ptree tree;
pt::read_json(ss, tree);
std::string value = tree.get<std::string>("key");
```

### Callback Threading

All callbacks queued to main thread:

```cpp
void OrcaNetwork::invoke_user_login_callback(int online_login, bool login) {
    BBL::OnUserLoginFn callback;
    BBL::QueueOnMainFn queue_fn;

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_user_login_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, online_login, login]() {
                callback(online_login, login);
            });
        } else {
            callback(online_login, login);
        }
    }
}
```

### Async Operations

`get_setting_list2()` uses detached thread:

```cpp
std::thread([this, bundle_version, chk_fn, pro_fn, cancel_fn]() {
    // Fetch data
    // For each preset:
    //   - Check cancellation
    //   - Invoke chk_fn callback
    //   - Report progress via pro_fn
}).detach();
```

## Limitations

1. **No Real Printer Support** - Printer operations are stubs
2. **Local Backend Only** - Requires separate backend service
3. **Simple Authentication** - No OAuth, password in plaintext
4. **No Real-time Updates** - No MQTT/WebSocket subscriptions
5. **Testing/Development Only** - Not suitable for production

## Future Enhancements

Potential improvements:

1. **WebSocket Support** - Real-time updates without polling
2. **SQLite Storage** - Embedded database instead of JSON files
3. **OAuth2 Integration** - Connect to real cloud services
4. **Printer Simulation** - Mock printer responses for testing
5. **Embedded Backend** - C++ HTTP server instead of Python
6. **Docker Container** - Packaged backend for easy deployment

## Troubleshooting

### Backend Connection Failed

**Problem:** `connect_server()` returns `BAMBU_NETWORK_ERR_CONNECT_FAILED`

**Solutions:**
- Ensure backend service is running: `python backend/orca_backend.py`
- Backend URL is fixed to `https://auth.orcaslicer.com`; override via `ORCA_BACKEND_URL` env for internal testing.
- Verify port not blocked by firewall
- Check backend logs for errors

### Login Failed

**Problem:** `change_user()` returns error, `is_user_login()` returns false

**Solutions:**
- Check username/password correct
- Verify user exists in `backend/data/users.json`
- Check backend `/api/v1/auth/login` endpoint responding
- Review HTTP error callback for details

### Preset Sync Not Working

**Problem:** `get_setting_list2()` doesn't call callbacks

**Solutions:**
- Ensure logged in: `is_user_login()` must be true
- Check `queue_on_main_fn` registered
- Verify backend `/api/v1/presets/sync` endpoint working
- Enable verbose logging to see background thread activity

### Callbacks Not Invoked

**Problem:** Registered callbacks never called

**Solutions:**
- **Must register `queue_on_main_fn`** - Required for thread safety
- Check callback registered before operation
- Verify operation completes successfully
- Use logging in callbacks to confirm invocation

## Testing

### Manual Testing

```bash
# Start backend
cd backend && python orca_backend.py

# In another terminal, test with curl
curl http://localhost:8080/api/v1/health

# Test login
curl -X POST http://localhost:8080/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"test_user","password":"password123"}'

# Get token from response, then test presets
curl http://localhost:8080/api/v1/presets \
  -H "Authorization: Bearer YOUR_TOKEN_HERE"
```

### Unit Testing

Create test cases:

```cpp
#include "OrcaNetwork.hpp"
#include <cassert>

void test_login() {
    OrcaNetwork network("/tmp/logs");
    // Backend URL fixed to Supabase; use ORCA_BACKEND_URL env var for internal testing.

    std::string user_info = R"({"username":"test_user","password":"password123"})";
    int result = network.change_user(user_info);

    assert(result == BAMBU_NETWORK_SUCCESS);
    assert(network.is_user_login() == true);
    assert(!network.get_user_id().empty());
}

void test_presets() {
    OrcaNetwork network("/tmp/logs");
    // ... login first ...

    std::map<std::string, std::map<std::string, std::string>> presets;
    int result = network.get_user_presets(&presets);

    assert(result == BAMBU_NETWORK_SUCCESS);
}
```

## License

Same as OrcaSlicer: AGPL-3.0

## Contributing

To extend OrcaNetwork:

1. **Add Backend Endpoint** - Implement in `backend/orca_backend.py`
2. **Add HTTP Helper** - Create `http_*` method in `OrcaNetwork.cpp` if needed
3. **Implement Method** - Replace stub with real implementation
4. **Update Tests** - Add test cases
5. **Document** - Update this file with new features

## References

- [bambu_network.md](../bambu_network.md) - Original NetworkAgent documentation
- [Backend README](../backend/README.md) - Backend service documentation
- [bambu_networking.hpp](../src/slic3r/Utils/bambu_networking.hpp) - Interface definitions
- [OrcaNetwork.hpp](../src/slic3r/Utils/OrcaNetwork.hpp) - Implementation header
