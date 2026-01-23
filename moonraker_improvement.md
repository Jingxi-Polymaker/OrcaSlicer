---
title: MoonrakerPrinterAgent Improvement Plan
date: 2026-01-22
tags:
  - moonraker
  - improvement
  - plan
status: draft
priority: high
---

# MoonrakerPrinterAgent Improvement Plan

## Overview

This plan proposes enhancements to [[MoonrakerPrinterAgent]] to fully support the existing OrcaSlicer UI components (MonitorPanel, StatusPanel, SendToPrinterDialog) without requiring UI modifications.

**Files to Modify:**
- [[src/slic3r/Utils/MoonrakerPrinterAgent.hpp|MoonrakerPrinterAgent.hpp]]
- [[src/slic3r/Utils/MoonrakerPrinterAgent.cpp|MoonrakerPrinterAgent.cpp]]

**Reference:** [[moonraker_review_report.md]] for previous review findings

---

## Summary of Required Changes

| Category | Status | Priority |
|----------|--------|----------|
| Print Job Operations (upload + start) | ❌ Missing | 🔴 HIGH |
| Print Job Operations (pause/resume/cancel) | ❌ Missing | 🔴 HIGH |
| Extended Status Fields for UI | ⚠️ Partial | 🔴 HIGH |
| Klippy State Check (with new JSON helper) | ❌ Missing | 🟡 MEDIUM |
| WebSocket Reconnection + notify_klippy_disconnected | ❌ Missing | 🟡 MEDIUM |
| Command Handling (temp control, etc.) | ❌ Missing | 🟢 LOW |
| WebSocket server.connection.identify | ❌ Missing | 🟢 LOW |
| WSS (WebSocket Secure) support | ❌ Missing | 🟢 LOW |

---

## Architecture: Moonraker to Bambu Message Mapping

````mermaid
graph LR
    subgraph "Moonraker API"
        MS[print_stats.state]
        MP[virtual_sdcard.progress]
        MT[extruder.temperature]
        MB[heater_bed.temperature]
        MF[fan.speed]
    end

    subgraph "MoonrakerPrinterAgent"
        A[update_status_cache]
        B[build_print_payload_locked]
        C[dispatch_message]
    end

    subgraph "Bambu Message Format"
        BS[gcode_state]
        BP[mc_percent]
        BT[nozzle_temper]
        BB[bed_temper]
        BF[fan_gear]
    end

    subgraph "UI Components"
        UI[StatusPanel]
        MN[MonitorPanel]
    end

    MS --> A
    MP --> A
    MT --> A
    MB --> A
    MF --> A
    A --> B
    B --> C
    C --> UI
    C --> MN
````

---

## Phase 1: Extended Status Fields for UI Support

> [!danger] Priority: HIGH
> Current payload is minimal - many UI fields are missing

### 1.1 Missing MachineObject Fields

The current `build_print_payload_locked()` only populates:
- `gcode_state`, `nozzle_temper`, `bed_temper`, `fan_gear`, `subtask_name`, `mc_percent`, `mc_remaining_time`

**Missing fields that StatusPanel expects:**

| MachineObject Field | UI Usage | Moonraker Source | Implementation Notes |
|---------------------|----------|------------------|----------------------|
| `mc_print_stage` | Print stage display | `print_stats.state` | Map: printing→1, paused→2, complete→3 |
| `mc_print_error_code` | Error display | Leave as 0 | Only set if real HMS mapping defined |
| `print_error` | Error display | Leave as 0 | UI expects numeric HMS codes |
| `home_flag` | Various UI flags | `toolhead.homed_axes` | **WARNING**: Large bitfield, setting only XYZ clears support flags |
| ~~`sdcard_state`~~ | SD card status | **NOT sent** | UI reads from `print.sdcard` or `home_flag` bits 8-9 |
| `nozzle_temp_range` | Temp limit validation | Hardcoded | `[170, 300]` array |
| `bed_temp_range` | Temp limit validation | Hardcoded | `[0, 120]` array |
| `support_bed_leveling` | Feature detection | Object list query | Detect `bed_mesh` or `probe` (not deprecated) |
| `support_send_to_sd` | Feature detection | Always true | Moonraker supports SD |
| `gcode_file` | Current file path | `print_stats.filename` | Full path on printer |

> [!warning] Important Notes from Review
> - **sdcard_state**: Do NOT send in payload. OrcaSlicer reads SD state from `print.sdcard` (see `DevStorage::ParseV1_0`) or from `home_flag` bits 8-9.
> - **virtual_sdcard.is_active**: Indicates active print, NOT SD presence. Mapping this to SD presence will mislead UI.
> - **home_flag**: Is a large bitfield. Setting only XYZ bits (0-2) implicitly clears support flags (bit 3+, e.g., 220V voltage flag at bit 3). This is acceptable if marking those unsupported, but should be explicitly documented.
> - **mc_print_error_code/print_error**: UI expects numeric HMS codes. Setting to 1 will show generic error dialog. Leave at 0 unless defining real mapping.
> - **support_bed_leveling**: NOT deprecated in this codebase. Used by UI flows in `SelectMachine.cpp`. Detect from available objects (`bed_mesh`, `probe`).

### 1.2 Implementation: Enhanced build_print_payload_locked()

```cpp
nlohmann::json MoonrakerPrinterAgent::build_print_payload_locked() const
{
    nlohmann::json payload;
    payload["print"]["command"] = "push_status";
    payload["print"]["msg"] = 0;
    payload["print"]["support_mqtt_alive"] = true;

    // ===== EXISTING CODE =====
    // gcode_state, temperatures, fan, progress, etc...

    // ===== NEW: Print Stage =====
    // Map Moonraker state to Bambu stage numbers
    int mc_print_stage = 0;
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state")) {
        std::string mr_state = status_cache["print_stats"]["state"].get<std::string>();
        if (mr_state == "printing") mc_print_stage = 1;
        else if (mr_state == "paused") mc_print_stage = 2;
        else if (mr_state == "complete") mc_print_stage = 3;
        else if (mr_state == "error") mc_print_stage = 4;
    }
    payload["print"]["mc_print_stage"] = mc_print_stage;

    // ===== NEW: Error Codes =====
    // Leave mc_print_error_code and print_error at 0
    // UI expects numeric HMS codes - setting to 1 shows generic error dialog
    // Only set if real mapping from Moonraker error strings to HMS codes is defined

    // ===== NEW: Home Flag (with warning) =====
    // Map homed axes to bit field: X=bit0, Y=bit1, Z=bit2
    // WARNING: This only sets bits 0-2, clearing support flags (bit 3+)
    // Bit 3 = 220V voltage, bit 4 = auto recovery, etc.
    // This is acceptable for Moonraker (no AMS, different feature set)
    int home_flag = 0;
    if (status_cache.contains("toolhead") && status_cache["toolhead"].contains("homed_axes")) {
        std::string homed = status_cache["toolhead"]["homed_axes"].get<std::string>();
        if (homed.find('X') != std::string::npos) home_flag |= 1;  // bit 0
        if (homed.find('Y') != std::string::npos) home_flag |= 2;  // bit 1
        if (homed.find('Z') != std::string::npos) home_flag |= 4;  // bit 2
    }
    payload["print"]["home_flag"] = home_flag;

    // ===== NEW: Temperature Ranges =====
    // Moonraker doesn't provide this via API - use hardcoded defaults
    payload["print"]["nozzle_temp_range"] = {170, 300};  // Typical Klipper range
    payload["print"]["bed_temp_range"] = {0, 120};        // Typical bed range

    // ===== NEW: Feature Flags =====
    payload["print"]["support_send_to_sd"] = true;
    // Detect bed_leveling support from available objects (bed_mesh or probe)
    // Default to 0 (not supported) if neither object exists
    bool has_bed_leveling = (available_objects.count("bed_mesh") != 0 ||
                             available_objects.count("probe") != 0);
    payload["print"]["support_bed_leveling"] = has_bed_leveling ? 1 : 0;

    // ===== NEW: G-code File Path =====
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename")) {
        payload["print"]["gcode_file"] = status_cache["print_stats"]["filename"];
    }

    payload["t_utc"] = now_ms;
    return payload;
}
```

### 1.3 Additional WebSocket Subscriptions

Add to `subscribe_objects` in `run_status_stream()`:

```cpp
// Add toolhead for homing status
subscribe_objects.insert("toolhead");

// Add display_status for layer info (if available)
if (available_objects.count("display_status") != 0) {
    subscribe_objects.insert("display_status");
}
```

---

## Phase 2: Print Job Operations

> [!danger] Priority: HIGH
> All print methods are currently stubs

### 2.1 File Upload Implementation

Moonraker API: `POST /server/files/upload`

> [!success] Using Http API multipart helpers
> See `src/slic3r/Utils/OctoPrint.cpp` (line 405-408) for reference implementation using `form_add()` and `form_add_file()`.

```cpp
bool MoonrakerPrinterAgent::upload_gcode(
    const std::string& local_path,
    const std::string& filename,
    const std::string& base_url,
    const std::string& api_key,
    OnUpdateStatusFn update_fn,
    WasCancelledFn cancel_fn)
{
    namespace fs = boost::filesystem;

    // Validate file exists
    fs::path source_path(local_path);
    if (!fs::exists(source_path)) {
        BOOST_LOG_TRIVIAL(error) << "File does not exist: " << local_path;
        return false;
    }

    // Check file size
    std::uintmax_t file_size = fs::file_size(source_path);
    if (file_size > 1024 * 1024 * 1024) {  // 1GB limit
        BOOST_LOG_TRIVIAL(error) << "File too large: " << file_size << " bytes";
        return false;
    }

    bool result = true;
    std::string http_error;

    // Use Http::form_add and Http::form_add_file (see OctoPrint.cpp line 405-408)
    auto http = Http::post(join_url(base_url, "/server/files/upload"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.form_add("root", "gcodes")  // Upload to gcodes directory
        .form_add("print", "false")   // Don't auto-start print
        .form_add_file("file", source_path.string(), filename)
        .timeout_connect(10)
        .timeout_max(300)  // 5 minutes for large files
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << "Upload complete: HTTP " << status << " body: " << body;
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "Upload error: " << err << " HTTP " << status;
            http_error = err;
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            // Check for cancellation via WasCancelledFn
            if (cancel_fn && cancel_fn()) {
                cancel = true;
                result = false;
                return;
            }
            // Report progress via OnUpdateStatusFn
            if (update_fn && progress.ultotal > 0) {
                int percent = static_cast<int>((progress.ulnow * 100) / progress.ultotal);
                update_fn(PrintingStageUpload, percent, "Uploading...");
            }
        })
        .perform_sync();

    if (!result) {
        BOOST_LOG_TRIVIAL(error) << "Upload failed: " << http_error;
        return false;
    }

    return true;
}
```

### 2.2 start_local_print Implementation

> [!info] PrintParams::filename clarification
> `params.filename` can be a `.3mf` file path in some flows (see `PrintJob.cpp`). The Moonraker upload must extract the G-code path and use a filename relative to the `gcodes` root directory.
>
> The slicing step (handled before `start_local_print` is called) should export the G-code. Check `params.dst_file` for the actual G-code path, or use the 3MF path if already sliced.

```cpp
int MoonrakerPrinterAgent::start_local_print(
    PrintParams params,
    OnUpdateStatusFn update_fn,
    WasCancelledFn cancel_fn)
{
    if (update_fn) update_fn(PrintingStageCreate, 0, "Preparing...");

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    const std::string base_url = resolve_host(params.dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(params.dev_id, params.password);

    // Determine the G-code file to upload
    // params.filename may be .3mf, params.dst_file contains actual G-code
    std::string gcode_path = params.filename;
    if (!params.dst_file.empty()) {
        gcode_path = params.dst_file;
    }

    // Check if file exists and has .gcode extension
    namespace fs = boost::filesystem;
    fs::path source_path(gcode_path);
    if (!fs::exists(source_path)) {
        BOOST_LOG_TRIVIAL(error) << "G-code file does not exist: " << gcode_path;
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    // Extract filename for upload (relative to gcodes root)
    std::string upload_filename = source_path.filename().string();
    if (!boost::iends_with(upload_filename, ".gcode")) {
        upload_filename += ".gcode";
    }

    // Upload file
    if (update_fn) update_fn(PrintingStageUpload, 0, "Uploading G-code...");
    if (!upload_gcode(gcode_path, upload_filename, base_url, api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    // Start print via WebSocket JSON-RPC
    if (update_fn) update_fn(PrintingStageSending, 0, "Starting print...");

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"] = "printer.print.start";
    request["params"]["filename"] = upload_filename;
    request["id"] = next_jsonrpc_id++;

    std::string response;
    if (!send_jsonrpc_command(base_url, api_key, request, response)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    // Verify print started
    nlohmann::json json = nlohmann::json::parse(response, nullptr, false);
    if (!json.is_discarded() && json.contains("result")) {
        if (update_fn) update_fn(PrintingStageFinished, 100, "Print started");
        return BAMBU_NETWORK_SUCCESS;
    }

    // Check for error
    if (!json.is_discarded() && json.contains("error")) {
        BOOST_LOG_TRIVIAL(error) << "Print start failed: " << json["error"].dump();
    }

    return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
}
```

### 2.3 start_send_gcode_to_sdcard Implementation

Similar to `start_local_print` but doesn't start the print:

```cpp
int MoonrakerPrinterAgent::start_send_gcode_to_sdcard(
    PrintParams params,
    OnUpdateStatusFn update_fn,
    WasCancelledFn cancel_fn,
    OnWaitFn wait_fn)
{
    (void) wait_fn;

    if (update_fn) update_fn(PrintingStageCreate, 0, "Preparing...");

    const std::string base_url = resolve_host(params.dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(params.dev_id, params.password);

    std::string filename = params.filename;
    if (filename.empty()) {
        filename = params.task_name;
    }
    if (!boost::iends_with(filename, ".gcode")) {
        filename += ".gcode";
    }

    // Upload only, don't start print
    if (!upload_gcode(params.filename, filename, base_url, api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    if (update_fn) update_fn(PrintingStageFinished, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}
```

### 2.4 Print Control Operations

Add new methods for pause/resume/cancel:

```cpp
// In MoonrakerPrinterAgent.hpp
int pause_print(const std::string& dev_id);
int resume_print(const std::string& dev_id);
int cancel_print(const std::string& dev_id);

// In MoonrakerPrinterAgent.cpp
int MoonrakerPrinterAgent::pause_print(const std::string& dev_id)
{
    const std::string base_url = resolve_host(dev_id);
    const std::string api_key = resolve_api_key(dev_id, "");

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"] = "printer.print.pause";
    request["id"] = next_jsonrpc_id++;

    std::string response;
    return send_jsonrpc_command(base_url, api_key, request, response)
        ? BAMBU_NETWORK_SUCCESS
        : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}
```

---

## Phase 3: Command Handling Extension

> [!warning] Priority: MEDIUM
> Add support for UI controls

### 3.1 Extend handle_request() for New Commands

Current implementation only handles:
- `info.get_version`
- `system.get_access_code`
- `print.gcode_line`

**Add support for:**

| UI Command | Actual Payload | Moonraker API | Implementation |
|------------|----------------|---------------|----------------|
| Bed temp | `print.temp` | `SET_HEATER_TEMPERATURE` | G-code script |
| Nozzle temp | `print.target_temp` + `print.extruder_index` | `SET_HEATER_TEMPERATURE` | G-code script |
| Pause | `print.pause` | `printer.print.pause` | JSON-RPC |
| Resume | `print.resume` | `printer.print.resume` | JSON-RPC |
| Stop | `print.stop` | `printer.print.cancel` | JSON-RPC |
| Home | `print.home` | `G28` | G-code script |

> [!info] Correct Field Names
> See `DeviceManager.cpp` line 1474: `j["print"]["command"] = "set_bed_temp"; j["print"]["temp"] = temp;`
> See `DeviceManager.cpp` line 1497: `j["print"]["command"] = "set_nozzle_temp"; j["print"]["target_temp"] = temp; j["print"]["extruder_index"] = nozzle_id;`

```cpp
int MoonrakerPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
    auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded()) {
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    // ===== EXISTING: info, system, gcode_line handlers =====

    // ===== NEW: Print control commands =====
    if (json.contains("print") && json["print"].contains("command")) {
        const std::string cmd = json["print"]["command"].get<std::string>();

        if (cmd == "pause") {
            return pause_print(dev_id);
        }
        if (cmd == "resume") {
            return resume_print(dev_id);
        }
        if (cmd == "stop") {
            return cancel_print(dev_id);
        }

        // Bed temperature - UI sends "temp" field
        if (cmd == "set_bed_temp") {
            if (json["print"].contains("temp") && json["print"]["temp"].is_number()) {
                int temp = json["print"]["temp"].get<int>();
                std::string gcode = "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=" + std::to_string(temp);
                send_gcode(dev_id, gcode);
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        // Nozzle temperature - UI sends "target_temp" and "extruder_index" fields
        if (cmd == "set_nozzle_temp") {
            if (json["print"].contains("target_temp") && json["print"]["target_temp"].is_number()) {
                int temp = json["print"]["target_temp"].get<int>();
                int extruder_idx = 0;  // Default to main extruder
                if (json["print"].contains("extruder_index") && json["print"]["extruder_index"].is_number()) {
                    extruder_idx = json["print"]["extruder_index"].get<int>();
                }
                std::string heater = (extruder_idx == 0) ? "extruder" : "extruder" + std::to_string(extruder_idx);
                std::string gcode = "SET_HEATER_TEMPERATURE HEATER=" + heater + " TARGET=" + std::to_string(temp);
                send_gcode(dev_id, gcode);
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        if (cmd == "home") {
            return send_gcode(dev_id, "G28") ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }
    }

    return BAMBU_NETWORK_SUCCESS;
}
```

---

## Phase 4: WebSocket Enhancements

> [!warning] Priority: MEDIUM
> Currently connection dies on any error, missing client identification

### 4.1 Add server.connection.identify (Recommended by Moonraker docs)

> [!info] Server Admin documentation
> Moonraker recommends sending `server.connection.identify` immediately after WebSocket connect for proper client identification.

```cpp
void MoonrakerPrinterAgent::run_status_stream(std::string dev_id, std::string base_url, std::string api_key)
{
    // ... existing connection code ...

    ws.handshake(host_header, endpoint.target);
    ws.text(true);

    // ===== NEW: Send client identification =====
    nlohmann::json identify;
    identify["jsonrpc"] = "2.0";
    identify["method"] = "server.connection.identify";
    identify["params"]["client_name"] = "OrcaSlicer";
    identify["params"]["version"] = MoonrakerPrinterAgent_VERSION;
    identify["params"]["type"] = "agent";
    identify["params"]["url"] = "https://github.com/SoftFever/OrcaSlicer";
    identify["id"] = 0;
    ws.write(net::buffer(identify.dump()));

    // ... rest of existing code ...
}
```

### 4.2 Handle notify_klippy_disconnected

```cpp
void MoonrakerPrinterAgent::handle_ws_message(const std::string& dev_id, const std::string& payload)
{
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid WebSocket message JSON";
        return;
    }

    bool updated = false;

    // Check for subscription response (has "result.status")
    if (json.contains("result") && json["result"].contains("status") &&
        json["result"]["status"].is_object()) {
        update_status_cache(json["result"]["status"]);
        updated = true;
    }

    // Check for status update notifications
    if (json.contains("method") && json["method"].is_string()) {
        const std::string method = json["method"].get<std::string>();
        if (method == "notify_status_update" && json.contains("params") &&
            json["params"].is_array() && !json["params"].empty() &&
            json["params"][0].is_object()) {
            update_status_cache(json["params"][0]);
            updated = true;
        } else if (method == "notify_klippy_ready") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "standby";
            update_status_cache(updates);
            updated = true;
        } else if (method == "notify_klippy_shutdown") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "error";
            update_status_cache(updates);
            updated = true;
        }
        // ===== NEW: Handle Klippy disconnect =====
        else if (method == "notify_klippy_disconnected") {
            // Klippy disconnected - revert to "not ready" state
            // Trigger reconnection by breaking the read loop
            BOOST_LOG_TRIVIAL(warning) << "Klippy disconnected, triggering reconnection";
            updated = true;
            // Force reconnection by not setting updated flag properly
            // The reconnection logic in run_status_stream will handle it
            return;
        }
    }

    if (updated) {
        nlohmann::json message;
        {
            std::lock_guard<std::mutex> lock(payload_mutex);
            message = build_print_payload_locked();
        }

        BOOST_LOG_TRIVIAL(trace) << "Dispatching payload: " << message.dump();
        dispatch_message(dev_id, message.dump());

        const auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        ws_last_emit_ms.store(now_ms);
    }
}
```

### 4.3 Reconnection Logic

```cpp
void MoonrakerPrinterAgent::run_status_stream(std::string dev_id, std::string base_url, std::string api_key)
{
    int retry_count = 0;
    const int max_retries = 10;
    const int base_delay_ms = 1000;

    while (!ws_stop.load() && retry_count < max_retries) {
        try {
            // ... connection code including identify ...

            while (!ws_stop.load()) {
                // ... read loop with timeout handling ...
            }

            // Clean shutdown - reset retry count
            retry_count = 0;

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "WebSocket disconnected: " << e.what();
            if (ws_stop.load()) break;

            // Exponential backoff
            int delay_ms = base_delay_ms * (1 << std::min(retry_count, 5));
            BOOST_LOG_TRIVIAL(info) << "Reconnecting in " << delay_ms << "ms (attempt " << (retry_count + 1) << ")";
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            retry_count++;
        }
    }

    if (retry_count >= max_retries) {
        BOOST_LOG_TRIVIAL(error) << "Max reconnection attempts reached";
        dispatch_local_connect(ConnectStatusLost, dev_id, "max_retries");
    }
}
```

### 4.4 WSS (WebSocket Secure) Support

> [!warning] Still from moonraker_review_report.md
> WSS support is not implemented. Current code rejects secure connections at line 1018.

**Future enhancement (not in this plan):** Add support for `wss://` using Boost.Beast SSL streams.

```cpp
// Template-based approach for both secure and non-secure
// Requires #include <boost/beast/ssl.hpp> and #include <boost/asio/ssl.hpp>
```

This is a larger change requiring SSL certificate handling, deferred to a future update.

---

## Phase 5: Klippy State Check

> [!warning] Priority: MEDIUM
> Connection succeeds without checking if printer is ready

> [!info] New JSON helper required
> Current `fetch_server_info()` only returns a version string. Need a new `fetch_server_info_json()` that returns full JSON for klippy_state check.

### 5.1 New Helper: fetch_server_info_json()

**Add to MoonrakerPrinterAgent.hpp:**
```cpp
bool fetch_server_info_json(const std::string& base_url, const std::string& api_key,
                             nlohmann::json& info, std::string& error) const;
```

**Implementation in MoonrakerPrinterAgent.cpp:**
```cpp
bool MoonrakerPrinterAgent::fetch_server_info_json(
    const std::string& base_url,
    const std::string& api_key,
    nlohmann::json& info,
    std::string& error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(join_url(base_url, "/server/info"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    info = nlohmann::json::parse(response_body, nullptr, false, true);
    if (info.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    return true;
}
```

### 5.2 Enhanced connect_printer()

```cpp
int MoonrakerPrinterAgent::connect_printer(
    std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    // ... existing config resolution ...

    // Check Klippy state before signaling success
    nlohmann::json server_info;
    std::string error;
    if (!fetch_server_info_json(base_url, api_key, server_info, error)) {
        dispatch_local_connect(ConnectStatusFailed, dev_id, "server_info_failed");
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    nlohmann::json result = server_info.contains("result") ? server_info["result"] : server_info;
    std::string klippy_state = result.value("klippy_state", "");

    if (klippy_state == "startup") {
        // Poll until ready (30 second timeout)
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (fetch_server_info_json(base_url, api_key, server_info, error)) {
                result = server_info.contains("result") ? server_info["result"] : server_info;
                klippy_state = result.value("klippy_state", "");
                if (klippy_state == "ready") break;
            }
        }
    }

    if (klippy_state != "ready") {
        std::string state_message = result.value("state_message", "Unknown error");
        BOOST_LOG_TRIVIAL(error) << "Klippy not ready: " << klippy_state << " - " << state_message;
        dispatch_local_connect(ConnectStatusFailed, dev_id, "klippy_not_ready:" + klippy_state);
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    // ... rest of existing connection code ...
}
```

---

## Phase 6: Helper Methods

### 6.1 JSON-RPC Request Helper

```cpp
// In MoonrakerPrinterAgent.hpp
private:
    std::atomic<int> next_jsonrpc_id{1};
    bool send_jsonrpc_command(const std::string& base_url, const std::string& api_key,
                              const nlohmann::json& request, std::string& response) const;

// In MoonrakerPrinterAgent.cpp
bool MoonrakerPrinterAgent::send_jsonrpc_command(
    const std::string& base_url, const std::string& api_key,
    const nlohmann::json& request, std::string& response) const
{
    std::string request_str = request.dump();
    std::string url = join_url(base_url, "/printer/print/start");

    // Actually, Moonraker print commands should go through WebSocket
    // For HTTP-based commands, use /printer/gcode/script endpoint

    bool success = false;
    std::string http_error;

    auto http = Http::post(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(request_str)
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response = body;
                success = true;
            } else {
                http_error = "HTTP " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(error) << "JSON-RPC command failed: " << http_error;
    }

    return success;
}
```

---

## Implementation Order

````mermaid
graph TD
    A[Phase 1: Extended Status Fields] --> B[Phase 5.1: fetch_server_info_json]
    B --> C[Phase 5.2: Klippy State Check]
    C --> D[Phase 2.1: File Upload with Http API]
    D --> E[Phase 2.2: start_local_print]
    E --> F[Phase 2.4: Print Control Operations]
    F --> G[Phase 4.1: server.connection.identify]
    G --> H[Phase 4.2: notify_klippy_disconnected]
    H --> I[Phase 4.3: Reconnection Logic]
    I --> J[Phase 3: Command Handling]
````

> [!tip] Recommended Implementation Sequence
> 1. **Phase 1** - Enables full UI display (high impact, low risk)
> 2. **Phase 5.1 + 5.2** - Klippy state check (connection reliability)
> 3. **Phase 2.1** - File upload with proper Http API (core printing)
> 4. **Phase 2.2** - start_local_print implementation (highest value)
> 5. **Phase 2.4** - Print control (pause/resume/cancel)
> 6. **Phase 4.1-4.3** - WebSocket enhancements (identification, disconnect handling, reconnection)
> 7. **Phase 3** - UI control integration (temp commands)
>
> **Future Work** (not in this plan):
> - WSS (WebSocket Secure) support - Requires Boost.Beast SSL streams

---

## Files to Modify

### src/slic3r/Utils/MoonrakerPrinterAgent.hpp

**Add private members:**
```cpp
private:
    std::atomic<int> next_jsonrpc_id{1};
    std::set<std::string> available_objects;  // Track for feature detection

    // Print control helpers
    int pause_print(const std::string& dev_id);
    int resume_print(const std::string& dev_id);
    int cancel_print(const std::string& dev_id);

    // File upload
    bool upload_gcode(const std::string& local_path, const std::string& filename,
                      const std::string& base_url, const std::string& api_key,
                      OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

    // JSON-RPC helper
    bool send_jsonrpc_command(const std::string& base_url, const std::string& api_key,
                              const nlohmann::json& request, std::string& response) const;

    // Server info (returns JSON, not just version string)
    bool fetch_server_info_json(const std::string& base_url, const std::string& api_key,
                                 nlohmann::json& info, std::string& error) const;
```

### src/slic3r/Utils/MoonrakerPrinterAgent.cpp

**Modify methods:**
- `build_print_payload_locked()` - Add all missing status fields (remove sdcard_state, fix error codes)
- `run_status_stream()` - Add server.connection.identify, reconnection logic
- `connect_printer()` - Add Klippy state check using fetch_server_info_json()
- `handle_request()` - Add pause/resume/stop/temp commands with correct field names
- `handle_ws_message()` - Add notify_klippy_disconnected handling
- `fetch_object_list()` - Store results in `available_objects` member for feature detection

**Implement methods:**
- `upload_gcode()` - New file upload using Http::form_add/form_add_file
- `pause_print()` / `resume_print()` / `cancel_print()` - Print control
- `start_local_print()` - Replace stub with full implementation
- `start_send_gcode_to_sdcard()` - Replace stub with full implementation
- `fetch_server_info_json()` - New helper for getting full server info JSON
- `send_jsonrpc_command()` - Helper for JSON-RPC over HTTP

---

## Testing Plan

### Unit Testing

1. **Status Field Mapping**
   - Verify all MachineObject fields are populated correctly
   - Test state mapping (printing → RUNNING, etc.)
   - Test error code generation

2. **File Upload**
   - Test small files (< 1MB)
   - Test large files (> 100MB)
   - Test cancellation during upload
   - Test error handling (no space, network error)

3. **Print Jobs**
   - Test start_local_print() with valid file
   - Test start_send_gcode_to_sdcard()
   - Test pause/resume/cancel during print

### Integration Testing

1. **UI Integration**
   - Verify StatusPanel displays all fields
   - Verify MonitorPanel shows correct status
   - Verify SendToPrinterDialog completes successfully

2. **Connection Resilience**
   - Test reconnection after network loss
   - Test reconnection after Klipper restart
   - Test Klippy state handling (startup, error, shutdown)

3. **End-to-End**
   - Connect to real Moonraker printer
   - Upload and start a print
   - Monitor progress via UI
   - Pause/resume/cancel print
   - Verify final state

---

## References

- [[Moonraker API Reference|https://github.com/Arksine/moonraker/blob/master/docs/web_api.md]]
- [[moonraker_client_ref]] - Moonraker client implementation guide
- [[orca-printer-communication]] - OrcaSlicer printer communication architecture
- [[moonraker_review_report.md]] - Previous review findings
- [[IPrinterAgent interface]]
- [[src/slic3r/Utils/MoonrakerPrinterAgent.hpp]]
- [[src/slic3r/Utils/MoonrakerPrinterAgent.cpp]]
