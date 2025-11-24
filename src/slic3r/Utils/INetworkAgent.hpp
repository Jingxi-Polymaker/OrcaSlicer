#ifndef __I_NETWORK_AGENT_HPP__
#define __I_NETWORK_AGENT_HPP__

#include "bambu_networking.hpp"
#include "../../libslic3r/ProjectTask.hpp"
#include <string>
#include <map>
#include <vector>
#include <functional>


using namespace BBL;

namespace Slic3r {

/**
 * INetworkAgent - Pure virtual interface for network agent implementations.
 *
 * Both NetworkAgent (the wrapper around Bambu Lab's proprietary binary) and
 * OrcaNetwork (the fully open-source implementation that talks to the mock
 * backend documented in docs/OrcaNetwork.md) implement this contract so that
 * the rest of OrcaSlicer can switch between them without touching call sites.
 *
 * === Typical workflow ===
 * 1. Create an instance via NetworkAgentFactory or directly construct OrcaNetwork.
 * 2. Call the lifecycle setters: `set_config_dir()`, `init_log()`, `set_cert_file()`.
 * 3. Register all callbacks (including `set_queue_on_main_fn`) via `init_networking_callbacks`.
 * 4. Set `set_country_code()` and call `start()`.
 * 5. Establish the server connection (`connect_server`) and perform login (`change_user`).
 * 6. Use the sections below (settings sync, printer operations, analytics, ...)
 *    depending on the feature being exercised.
 *
 * Every method returns a value from bambu_networking.hpp unless it is declared
 * to return another type. That keeps compatibility with both the proprietary
 * SDK and the in-tree OrcaNetwork shim.
 */
class INetworkAgent {
public:
    virtual ~INetworkAgent() = default;

    // ========================================================================
    // Lifecycle Methods
    // ========================================================================
    /**
     * Initialize the logging backend for the concrete agent.
     * 
     * **Call order**: Invoke after `set_config_dir()` so the logging system
     * knows where to write files. Typically called during application startup
     * before `start()` to capture all networking/discovery traces.
     * 
     * **Implementation notes**:
     * - NetworkAgent (proprietary DLL): Uses the `g_log_folder` variable
     * - OrcaNetwork: Returns success immediately (uses BOOST_LOG_TRIVIAL)
     *
     * @return `BAMBU_NETWORK_SUCCESS` on success or one of the error codes
     *         defined in bambu_networking.hpp when logging could not be opened.
     */
    virtual int init_log() = 0;
    /**
     * Provide the writable configuration directory the agent should use.
     * The directory stores cached presets, authentication state, device
     * certificates and other long-lived artifacts. Invoke prior to `start()`
     * so subsequent operations know where to write. The directory will be
     * created if it doesn't exist when needed.
     *
     * @param config_dir Absolute/relative path to the configuration directory.
     */
    virtual int set_config_dir(std::string config_dir) = 0;
    /**
     * Register the client certificate file that proves the studio identity.
     * Implementations typically need both a folder (for platform specific
     * trust-store logic) and the filename; both are passed separately to keep
     * compatibility with the legacy NetworkAgent loader.
     *
     * @param folder  Directory containing the certificate/CA copy.
     * @param filename PEM or DER filename located under `folder`.
     */
    virtual int set_cert_file(std::string folder, std::string filename) = 0;
    /**
     * Tell the agent which two-letter ISO country code should be used when
     * selecting backend hosts, localized endpoints and telemetry policy.
     * Must be set before connecting so that `connect_server()` and the queue
     * fallback know which hostnames to assemble.
     */
    virtual int set_country_code(std::string country_code) = 0;
    /**
     * Start internal worker threads, initialize discovery timers and perform
     * any expensive setup the agent requires after configuration is supplied.
     * This method does not establish a connection itself; pair it with
     * `connect_server()` and login afterwards.
     */
    virtual int start() = 0;

    // ========================================================================
    // Callback Registration
    // ========================================================================
    /**
     * Register the handler that consumes SSDP discovery packets.
     * The callback is invoked whenever a printer announces itself on the LAN,
     * giving higher level components a chance to display "available" devices
     * even before authentication succeeds.
     */
    virtual int set_on_ssdp_msg_fn(BBL::OnMsgArrivedFn fn) = 0;
    /**
     * Register the login status callback.
     * Called after `change_user()` finishes or whenever the session expires;
     * `fn` receives the login type (online/offline) and the boolean outcome.
     * Invoke prior to initiating any login flow so UI state stays in sync.
     */
    virtual int set_on_user_login_fn(BBL::OnUserLoginFn fn) = 0;
    /**
     * Register the hook that reports printer MQTT connections.
     * The callback receives the raw MQTT topic so the caller can subscribe to
     * additional device-specific channels.
     */
    virtual int set_on_printer_connected_fn(BBL::OnPrinterConnectedFn fn) = 0;
    /**
     * Register the hook that signals when the agent connects to or disconnects
     * from the cloud backend. This is typically used to toggle the UI "online"
     * indicator and re-run auto-sync logic when the link drops.
     */
    virtual int set_on_server_connected_fn(BBL::OnServerConnectedFn fn) = 0;
    /**
     * Register a callback that fires when an HTTP request returns >= 400.
     * Implementations call this helper before surfacing failures back to the
     * GUI so that telemetry dialogs or toast notifications can reuse the exact
     * HTTP status code and body.
     */
    virtual int set_on_http_error_fn(BBL::OnHttpErrorFn fn) = 0;
    /**
     * Provide the getter used whenever the agent needs the current country
     * code but the GUI is the authoritative source (for example, after the
     * user toggles the region inside preferences). If this callback is set it
     * takes precedence over the value stored via `set_country_code`.
     */
    virtual int set_get_country_code_fn(BBL::GetCountryCodeFn fn) = 0;
    /**
     * Register a callback that is invoked when subscribing to a remote module
     * (MQTT topic) fails. Passing this allows the GUI to surface detailed
     * reasons and optionally retry with updated credentials.
     */
    virtual int set_on_subscribe_failure_fn(BBL::GetSubscribeFailureFn fn) = 0;
    /**
     * Register the handler for cloud device messages (MQTT / WebSocket).
     * The provided function is called with the `dev_id` and JSON payload
     * whenever the agent receives a message from Bambu cloud infrastructure.
     */
    virtual int set_on_message_fn(BBL::OnMessageFn fn) = 0;
    /**
     * Register the handler for user-scoped messaging channels.
     * This is usually bound to notifications coming from `user/{id}` topics
     * instead of specific device topics.
     */
    virtual int set_on_user_message_fn(BBL::OnMessageFn fn) = 0;
    /**
     * Register callback that notifies when a LAN printer accepted or rejected
     * a direct socket/MQTT connection attempt. The hook receives status codes
     * paired with device identifiers so the GUI can update each tile.
     */
    virtual int set_on_local_connect_fn(BBL::OnLocalConnectedFn fn) = 0;
    /**
     * Register handler for raw LAN MQTT/JSON payloads.
     * Unlike `set_on_message_fn`, this path bypasses the cloud relays and
     * surfaces packets received over the direct local socket.
     */
    virtual int set_on_local_message_fn(BBL::OnMessageFn fn) = 0;
    /**
     * Provide the helper that schedules callbacks on the GUI / main thread.
     * All other callbacks registered above are invoked via this trampoline to
     * avoid touching UI state from worker threads. If no queue is set, the
     * agent calls listeners immediately on the worker thread.
     */
    virtual int set_queue_on_main_fn(BBL::QueueOnMainFn fn) = 0;

    // ========================================================================
    // Server Connectivity
    // ========================================================================
    /**
     * Perform a blocking health check against the configured backend host.
     * Successful calls flip the internal `is_server_connected()` flag and
     * trigger `OnServerConnectedFn`. Typical usage is right after `start()`
     * and whenever the UI suspects the token expired.
     */
    virtual int connect_server() = 0;
    /**
     * Report the last known result of `connect_server()`.
     * This is an inexpensive getter used by status bars or by watchdog timers
     * that want to avoid redundant health checks.
     */
    virtual bool is_server_connected() = 0;
    /**
     * Force the agent to re-check the server state even if it was already
     * connected. Most implementations simply call `connect_server()` again
     * but they may also clear caches or DNS pins before retrying.
     */
    virtual int refresh_connection() = 0;
    /**
     * Subscribe to a logical module (MQTT topic such as "printer" or "user").
     * Call this after connecting so that streaming status updates start flowing
     * through `set_on_message_fn`.
     *
     * @param module Identifier defined by the backend (e.g. "job", "chat").
     */
    virtual int start_subscribe(std::string module) = 0;
    /**
     * Stop listening to a formerly subscribed module/topic.
     * Useful when the user disables a feature and the GUI wants to reduce the
     * volume of push events processed on the main thread.
     */
    virtual int stop_subscribe(std::string module) = 0;
    /**
     * Subscribe to the push streams for specific device identifiers.
     * Pass every `dev_id` that should stream telemetry to the desktop client.
     */
    virtual int add_subscribe(std::vector<std::string> dev_list) = 0;
    /**
     * Remove device-level subscriptions that are no longer relevant (for
     * example when a printer is deleted or belongs to a different account).
     */
    virtual int del_subscribe(std::vector<std::string> dev_list) = 0;
    /**
     * Enable or disable "multi-machine" mode where the agent keeps multiple
     * simultaneous device sessions alive. Some flows expect single-device mode
     * for backwards compatibility; call this toggle before starting discovery.
     */
    virtual void enable_multi_machine(bool enable) = 0;

    // ========================================================================
    // User Management
    // ========================================================================
    /**
     * Authenticate the user with the backend using the provided JSON payload.
     * The payload can be in one of these formats:
     * 
     * 1. **Traditional login**: 
     *    `{"username": "...", "password": "..."}`
     * 
     * 2. **WebView/OAuth login**: 
     *    `{"command": "user_login", "data": {"token": "...", "user_id": "...", ...}}`
     *    Or: `{"data": {"token": "...", "refresh_token": "...", "user": {...}}}`
     * 
     * **Implementation status**:
     * - NetworkAgent: Handles all formats (proprietary implementation)
     * - OrcaNetwork: Handles formats 1 & 2; OAuth format support incomplete
     * 
     * On completion, the registered `OnUserLoginFn` callback is invoked with
     * the login result (online_login type, success boolean).
     *
     * @param user_info JSON string containing authentication data.
     * @return BAMBU_NETWORK_SUCCESS or error code on failure.
     */
    virtual int change_user(std::string user_info) = 0;
    /**
     * Check whether the agent currently holds a valid authenticated session.
     * Use this helper to drive UI affordances or to decide whether to call
     * `change_user()` or `user_logout()` next.
     */
    virtual bool is_user_login() = 0;
    /**
     * Terminate the current session and optionally inform the backend.
     * Passing `request = true` forces the implementation to issue a remote
     * logout call; the default only clears local credentials.
     */
    virtual int user_logout(bool request = false) = 0;
    /**
     * Return the backend-generated user id for the current login session.
     * This string is often used to build MQTT topics (`user/{id}`).
     */
    virtual std::string get_user_id() = 0;
    /**
     * Return the printable display name attached to the current session.
     */
    virtual std::string get_user_name() = 0;
    /**
     * Return a URL or file path pointing to the user's avatar image so the
     * GUI can show the profile picture inside the title bar.
     */
    virtual std::string get_user_avatar() = 0;
    /**
     * Return the nickname (friendly alias) recorded for the current user.
     */
    virtual std::string get_user_nickanme() = 0;
    /**
     * Build a JSON command describing the login request that should be issued
     * if the agent is operating through a WebView or delegated authenticator.
     * This is mostly used by the original proprietary agent to drive the
     * embedded web login flow.
     */
    virtual std::string build_login_cmd() = 0;
    /**
     * Build a JSON command describing the logout request so that UI code can
     * publish it to the browser/embedded web view when the user signs out.
     */
    virtual std::string build_logout_cmd() = 0;
    /**
     * Return a snapshot (JSON) of the active session, including tokens,
     * username and avatar. Used by device-managers that need to persist the
     * state or share it with the browser component.
     */
    virtual std::string build_login_info() = 0;

    // ========================================================================
    // Settings Synchronization
    // ========================================================================
    /**
     * Fetch all presets owned by the logged-in user and store them inside the
     * map-of-maps that mirrors the back-end data model:
     * `user_presets[type][setting_id] = serialized_json`.
     *
     * @param user_presets Non-null pointer populated on success.
     */
    virtual int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) = 0;
    /**
     * Request that the server allocate a new preset identifier.
     * Provide the friendly name and the serialized setting values; the method
     * returns the ID string and optionally exposes the HTTP status code.
     */
    virtual std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) = 0;
    /**
     * Update or create a preset with a known `setting_id`.
     * Uploads the metadata (`name`) and serialized values, reporting both the
     * HTTP response code and a BBL error code to indicate client-side issues.
     */
    virtual int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) = 0;
    /**
     * Trigger a bulk download of the user's presets for the supplied bundle
     * version (the slicer build). Progress is reported through the optional
     * `ProgressFn`, while `WasCancelledFn` lets the caller abort mid-sync.
     */
    virtual int get_setting_list(std::string bundle_version, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) = 0;
    /**
     * Enhanced preset synchronization that provides per-item validation via the
     * `CheckFn` callback while still supporting progress and cancellation.
     * The `CheckFn` receives each preset's metadata as a map and returns a boolean
     * indicating whether that preset should be synchronized (true) or skipped (false).
     * Use this variant when the caller needs to filter presets based on sync state
     * or other criteria before they are committed locally.
     */
    virtual int get_setting_list2(std::string bundle_version, BBL::CheckFn chk_fn, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) = 0;
    /**
     * Delete the remote preset identified by `setting_id`.
     * Successful removal should be reflected locally to keep caches in sync.
     */
    virtual int delete_setting(std::string setting_id) = 0;

    // ========================================================================
    // Extra Features
    // ========================================================================
    /**
     * Provide additional HTTP headers appended to every outgoing REST call.
     * This is typically used to inject feature flags or partner identifiers.
     * Pass an empty map to clear previously registered headers.
     */
    virtual int set_extra_http_header(std::map<std::string, std::string> extra_headers) = 0;
    /**
     * Return the absolute URL pointing to the online "studio info" page that
     * the desktop client should open when the user requests help/about.
     */
    virtual std::string get_studio_info_url() = 0;

    // ========================================================================
    // Printer Operations
    // ========================================================================
    /**
     * Publish a JSON command to a printer through the cloud relay.
     * Use this for remote printers when LAN connectivity is not available.
     *
     * @param dev_id   Target printer identifier.
     * @param json_str Serialized command payload (MQTT compatible).
     * @param qos      MQTT QoS level requested.
     * @param flag     Extra metadata understood by the backend (e.g. retain).
     */
    virtual int send_message(std::string dev_id, std::string json_str, int qos, int flag) = 0;
    /**
     * Establish (or re-establish) a direct connection to a LAN printer using
     * its IP address and credentials.
     *
     * @param dev_id   Printer identifier, usually matches the serial number.
     * @param dev_ip   IPv4/IPv6 address discovered via SSDP or manual entry.
     * @param username Digest authentication username provided by the printer.
     * @param password Password/token used for LAN-only operations.
     * @param use_ssl  Indicates if TLS should be used for the socket.
     */
    virtual int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) = 0;
    /**
     * Tear down the active LAN printer connection, closing sockets and
     * stopping LAN status polling.
     */
    virtual int disconnect_printer() = 0;
    /**
     * Send a JSON command to a LAN printer (bypassing the cloud).
     * Mirrors `send_message` but assumes the device is reachable locally.
     */
    virtual int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) = 0;
    /**
     * Validate that the current user certificates are still trusted by the
     * printer. Implementations often probe the filesystem or perform a quick
     * HTTPS handshake and return a BBL error if the cert is missing/expired.
     */
    virtual int check_cert() = 0;
    /**
     * Install or refresh the device certificate the agent will use when
     * establishing LAN TLS connections.
     *
     * @param dev_id   Printer receiving the cert.
     * @param lan_only True when the certificate is only needed for LAN mode.
     */
    virtual void install_device_cert(std::string dev_id, bool lan_only) = 0;
    /**
     * Start or stop SSDP discovery broadcasts and responses.
     *
     * @param start   True to enable SSDP listening, false to stop.
     * @param sending True if the host should also emit its own heartbeat.
     *
     * @return True if the requested state change was applied.
     */
    virtual bool start_discovery(bool start, bool sending) = 0;
    /**
     * Send a lightweight ping to the binding endpoint to check if the printer
     * with the supplied code is ready for account binding.
     */
    virtual int ping_bind(std::string ping_code) = 0;
    /**
     * Perform the printer binding detection/handshake sequence on a LAN
     * printer prior to the actual bind step. The detectResult structure is
     * filled with the information retrieved from the printer.
     *
     * @param dev_ip   Printer IP to probe.
     * @param sec_link Secondary link token displayed on the printer.
     * @param detect   Output structure describing the binding capabilities.
     */
    virtual int bind_detect(std::string dev_ip, std::string sec_link, BBL::detectResult& detect) = 0;
    /**
     * Register a callback invoked whenever the backend reports fatal HTTP
     * errors (server side). Unlike `set_on_http_error_fn`, this exposes the
     * failing URL and the status for logging/telemetry.
     */
    virtual int set_server_callback(BBL::OnServerErrFn fn) = 0;
    /**
     * Execute the multi-stage printer binding workflow:
     * 1. Connect to the printer via LAN.
     * 2. Exchange tickets with the cloud (using `sec_link` and timezone).
     * 3. Stream progress updates through `update_fn`.
     *
     * @param improved Indicates whether to use the improved binding protocol.
     */
    virtual int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, BBL::OnUpdateStatusFn update_fn) = 0;
    /**
     * Remove the association between the current account and the given printer.
     * On success, local caches should be updated to drop state for `dev_id`.
     */
    virtual int unbind(std::string dev_id) = 0;
    /**
     * Return the base hostname for cloud API calls (varies by region).
     * Helpful for diagnostics and when building browser URLs.
     */
    virtual std::string get_bambulab_host() = 0;
    /**
     * Return which printer (if any) the UI marked as "selected". The agent
     * persists this selection because the original implementation stored it
     * inside the cloud session.
     */
    virtual std::string get_user_selected_machine() = 0;
    /**
     * Update the persisted "selected machine" identifier so that subsequent
     * runs of the slicer highlight the same printer by default.
     */
    virtual int set_user_selected_machine(std::string dev_id) = 0;

    // ========================================================================
    // Print Job Operations
    // ========================================================================
    /**
     * Start a fully managed cloud print.
     * The agent performs project creation, uploads, waits for firmware ACKs
     * and finally commands the printer to start. Progress and stage changes
     * are reported through `update_fn`, cancellation through `cancel_fn`.
     * The `wait_fn` callback is invoked when the agent needs to wait for a
     * condition (e.g., printer ready, firmware acknowledgment); it returns
     * true to continue waiting or false to abort.
     */
    virtual int start_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) = 0;
    /**
     * Start a local print that also uploads a record of the job to the cloud
     * (used by Bambu Studio's "Local Print with History" feature).
     */
    virtual int start_local_print_with_record(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) = 0;
    /**
     * Upload a gcode file to the printer's SD card but do not start it.
     * The helper handles chunked FTP uploads and progress callbacks.
     */
    virtual int start_send_gcode_to_sdcard(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) = 0;
    /**
     * Start a LAN-only print with files that already exist locally.
     * There is no cloud project involved, making it the fastest path when the
     * PC and printer share the same network.
     */
    virtual int start_local_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) = 0;
    /**
     * Start a print directly from the printer's SD card.
     * The agent only sends a command referencing a file already uploaded via
     * `start_send_gcode_to_sdcard`.
     */
    virtual int start_sdcard_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) = 0;

    // ========================================================================
    // Cloud Services
    // ========================================================================
    /**
     * Retrieve inbox / notification messages for the active user.
     *
     * @param type      Message type filter (backend-defined constant).
     * @param after     Message id or timestamp cursor for pagination.
     * @param limit     Max number of messages to request.
     * @param http_code Receives the HTTP response code.
     * @param http_body Receives the raw JSON body.
     */
    virtual int get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body) = 0;
    /**
     * Check whether the user has pending task reports and whether they are
     * printable. Returns the task identifier via the output pointer.
     */
    virtual int check_user_task_report(int* task_id, bool* printable) = 0;
    /**
     * Fetch aggregated print statistics for the user (hours printed, etc).
     */
    virtual int get_user_print_info(unsigned int* http_code, std::string* http_body) = 0;
    /**
     * Query the user's tasks/prints using the structured parameters defined in
     * `BBL::TaskQueryParams`. The response JSON is stored in `http_body`.
     */
    virtual int get_user_tasks(BBL::TaskQueryParams params, std::string* http_body) = 0;
    /**
     * Fetch firmware information for a printer, including latest available
     * version. Mainly used by the updater UI.
     */
    virtual int get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body) = 0;
    /**
     * For multi-plate projects, return which plate index is associated with
     * the supplied cloud task id. Useful when resuming prints.
     */
    virtual int get_task_plate_index(std::string task_id, int* plate_index) = 0;
    /**
     * Retrieve extended user profile info such as identifiers used by other
     * backend services. The identifier pointer is filled on success.
     */
    virtual int get_user_info(int* identifier) = 0;
    /**
     * Ask the server for a one-time bind ticket. The ticket is later supplied
     * to a printer during the bind flow to prove the user initiated it.
     */
    virtual int request_bind_ticket(std::string* ticket) = 0;
    /**
     * Fetch information about a subtask (individual stage of a print job).
     * The method can return both structured JSON via `task_json` and the raw
     * HTTP body which may include additional metadata.
     */
    virtual int get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) = 0;
    /**
     * Retrieve slicing job info for a specific project/profile/plate combo.
     * Used to inspect cloud-sliced jobs before starting them.
     */
    virtual int get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) = 0;
    /**
     * Query the binding status for multiple devices simultaneously.
     * `query_list` contains dev ids or tickets, and the response body includes
     * each entry's status.
     */
    virtual int query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) = 0;
    /**
     * Update the friendly printer name stored in the cloud profile so that all
     * clients show the same label.
     */
    virtual int modify_printer_name(std::string dev_id, std::string dev_name) = 0;

    // ========================================================================
    // Model Mall & Publishing
    // ========================================================================
    /**
     * Request the live camera streaming URL for the specified printer and hand
     * it back through `callback`. Completion may be async depending on the
     * underlying implementation.
     */
    virtual int get_camera_url(std::string dev_id, std::function<void(std::string)> callback) = 0;
    /**
     * Fetch the staff-picked designs from the model mall (marketplace).
     * Offset/limit control pagination; the resulting JSON page is delivered
     * via `callback`.
     */
    virtual int get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback) = 0;
    /**
     * Run the multi-stage publishing workflow for uploading a model to the
     * marketplace. `update_fn` reports status (e.g. uploading, waiting for
     * review) while `cancel_fn` allows the caller to abort the long-running job.
     * `out` optionally receives the new listing id/URL on success.
     */
    virtual int start_publish(BBL::PublishParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, std::string* out) = 0;
    /**
     * Retrieve the base URL that the GUI should open when the user wants to
     * publish models via the website.
     */
    virtual int get_model_publish_url(std::string* url) = 0;
    /**
     * Fetch additional information about a publishing subtask (e.g. which file
     * needs attention). `getsub_fn` is invoked with the data when ready.
     */
    virtual int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn) = 0;
    /**
     * Return the entry page for the model mall, often a localized marketing
     * site embedded inside a WebView.
     */
    virtual int get_model_mall_home_url(std::string* url) = 0;
    /**
     * Build the detail page URL for a specific model id so that the GUI can
     * open it in the system browser or an embedded panel.
     */
    virtual int get_model_mall_detail_url(std::string* url, std::string id) = 0;
    /**
     * Retrieve the logged-in user's model mall profile (followers, uploads...).
     * The caller can override the token if needed (used by WebView login flow).
     */
    virtual int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body) = 0;

    // ========================================================================
    // Analytics & Tracking
    // ========================================================================
    /**
     * Globally enable/disable the telemetry subsystem. Call this whenever the
     * user toggles the privacy preference.
     */
    virtual int track_enable(bool enable) = 0;
    /**
     * Delete any telemetry files stored on disk. Invoked when the user opts
     * out so residual logs are not uploaded later.
     */
    virtual int track_remove_files() = 0;
    /**
     * Report a custom analytics event.
     *
     * @param evt_key Unique event identifier.
     * @param content JSON or key/value payload describing the event.
     */
    virtual int track_event(std::string evt_key, std::string content) = 0;
    /**
     * Set headers/common fields that should be attached to every telemetry
     * envelope (e.g. app version, locale).
     */
    virtual int track_header(std::string header) = 0;
    /**
     * Update a user property exposed to the analytics backend. Properties can
     * be typed; string is the default.
     */
    virtual int track_update_property(std::string name, std::string value, std::string type = "string") = 0;
    /**
     * Read the cached value of a tracked user property. Some analytics SDKs
     * allow consumers to query what will be sent to the backend.
     */
    virtual int track_get_property(std::string name, std::string& value, std::string type = "string") = 0;
    /**
     * Convenience getter mirroring the last value passed to `track_enable`.
     */
    virtual bool get_track_enable() = 0;

    // ========================================================================
    // Ratings & Reviews
    // ========================================================================
    /**
     * Submit a review for a marketplace design, optionally attaching image
     * URLs. Provides both HTTP code and detailed error text so the GUI can
     * surface moderation failures.
     */
    virtual int put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error) = 0;
    /**
     * Retrieve the Object Storage Service (OSS) configuration required for
     * uploading review images. The configuration may vary by country.
     */
    virtual int get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error) = 0;
    /**
     * Upload rating images to OSS using the configuration returned above.
     * On success, `pic_oss_path` contains the remote path returned by OSS.
     */
    virtual int put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error) = 0;
    /**
     * Poll the backend for the asynchronous rating result (job id returned by
     * `put_model_mall_rating`). Once the job succeeds the response includes
     * the final moderation state.
     */
    virtual int get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error) = 0;

    // ========================================================================
    // Miscellaneous
    // ========================================================================
    /**
     * Fetch MakerWorld (mw) user preferences and invoke the callback with the
     * JSON payload once the HTTP request completes.
     */
    virtual int get_mw_user_preference(std::function<void(std::string)> callback) = 0;
    /**
     * Retrieve the MakerWorld "For You" list (personalized suggestions) using
     * the provided random seed and page size. Results are delivered via the
     * callback as raw JSON.
     */
    virtual int get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback) = 0;
    
    // ========================================================================
    // Version Information
    // ========================================================================
    /**
     * Return the semantic version of the loaded networking implementation.
     * The proprietary agent exposes the DLL version while OrcaNetwork reports
     * its compiled-in string; used to show diagnostics in About > Network tab.
     */
    virtual std::string get_version() = 0;
};

} // namespace Slic3r

#endif // __I_NETWORK_AGENT_HPP__
