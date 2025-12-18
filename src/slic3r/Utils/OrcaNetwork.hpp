#ifndef __ORCA_NETWORK_HPP__
#define __ORCA_NETWORK_HPP__

#include "INetworkAgent.hpp"
#include "bambu_networking.hpp"
#include "../../libslic3r/ProjectTask.hpp"
#include "AuthManager.hpp"
#include <string>
#include <map>
#include <mutex>
#include <functional>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Forward declaration
class AppConfig;

// ============================================================================
// Sync Protocol Data Structures (per Orca Cloud Sync Protocol Specification)
// ============================================================================

// Represents an upserted profile from the server during sync pull
struct ProfileUpsert {
    std::string id;
    std::string name;
    nlohmann::json content;
    std::string updated_at;  // ISO 8601 timestamp
    std::string created_at;  // ISO 8601 timestamp
};

// Response from sync pull endpoint
struct SyncPullResponse {
    std::string next_cursor;              // New sync cursor (ISO 8601 timestamp)
    std::vector<ProfileUpsert> upserts;   // Profiles to create/update
    std::vector<std::string> deletes;     // Tombstone IDs to delete locally
};

// Result of a sync push operation
struct SyncPushResult {
    bool success;
    int http_code;                        // 200 = success, 409 = conflict, etc.
    std::string new_updated_at;           // On success: new timestamp for the profile
    ProfileUpsert server_version;         // On 409 conflict: current server version (metadata only, no content)
    bool server_deleted;                  // On 409 conflict: true if record was deleted on server (response is null)
    std::string error_message;
};

// Sync state persisted to JSON config file
// Note: Per-profile timestamps (updated_time) are stored in .info files as-is from server
struct SyncState {
    std::string last_sync_timestamp;  // Global sync cursor (ISO 8601)
};

/**
 * OrcaNetwork - A drop-in replacement for NetworkAgent
 *
 * This class implements the INetworkAgent interface using a simulated
 * backend service instead of the bambu_networking dynamic library.
 *
 * Features:
 * - User authentication and management
 * - Cloud preset storage and synchronization
 * - Server connectivity management
 * - Dummy implementations for printer operations (compatibility)
 *
 * Inheritance:
 * - Derives from INetworkAgent to enable polymorphic usage
 * - All virtual methods are implemented (override)
 */
class OrcaNetwork : public INetworkAgent {
public:
    // Constructor/Destructor
    explicit OrcaNetwork(std::string log_dir);
    ~OrcaNetwork() override;

    // Configuration - call after construction to override default URLs
    // Reads orca_api_url, orca_auth_url, orca_pub_key from AppConfig if set
    void configure_urls(AppConfig* app_config);

    // Lifecycle methods (INetworkAgent interface)
    int init_log() override;
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string folder, std::string filename) override;
    int set_country_code(std::string country_code) override;
    int start() override;

    // Callback registration (INetworkAgent interface)
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_user_login_fn(OnUserLoginFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_server_connected_fn(OnServerConnectedFn fn) override;
    int set_on_http_error_fn(OnHttpErrorFn fn) override;
    int set_get_country_code_fn(GetCountryCodeFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Server connectivity (INetworkAgent interface)
    int connect_server() override;
    bool is_server_connected() override;
    int refresh_connection() override;
    int start_subscribe(std::string module) override;
    int stop_subscribe(std::string module) override;
    int add_subscribe(std::vector<std::string> dev_list) override;
    int del_subscribe(std::vector<std::string> dev_list) override;
    void enable_multi_machine(bool enable) override;

    // User management (INetworkAgent interface)
    int change_user(std::string user_info) override;
    bool is_user_login() override;
    int user_logout(bool request = false) override;

    // OrcaNetwork-specific: WebView login support
    int set_user_session(std::string token, std::string user_id, std::string username,
                        std::string name, std::string nickname, std::string avatar,
                        std::string refresh_token = "");
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override;
    std::string get_user_nickanme() override;
    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;

    // Settings sync (INetworkAgent interface)
    // These methods use values_map["updated_time"] for Optimistic Concurrency Control:
    //   - Input: Pass current Preset::updated_time for version checking (empty for new profiles)
    //   - Output: On success, values_map["updated_time"] is updated with new server timestamp
    //             Caller MUST store this in Preset::updated_time for future sync operations
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) override;
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int get_setting_list(std::string bundle_version, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr) override;
    int get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr) override;
    int delete_setting(std::string setting_id) override;

    // New Sync Protocol (per Orca Cloud Sync Protocol Specification)
    // Pull: GET /api/v1/sync/pull?cursor={timestamp}
    int sync_pull(
        std::function<void(const SyncPullResponse&)> on_success,
        std::function<void(int http_code, const std::string& error)> on_error
    );

    // Push: POST /api/v1/sync/push with optimistic concurrency control
    // name: Profile file name (required per spec)
    // original_updated_at: Pass Preset::updated_time directly for OCC (empty for new profiles)
    // Returns: SyncPushResult with new_updated_at - caller must store it in Preset::updated_time as-is
    SyncPushResult sync_push(
        const std::string& profile_id,
        const std::string& name,
        const nlohmann::json& content,
        const std::string& original_updated_at = ""
    );

    // Sync state management (global cursor only - per-profile timestamps are in .info files)
    void load_sync_state();
    void save_sync_state();
    void clear_sync_state();
    const SyncState& get_sync_state() const { return sync_state; }

    // Extra features (INetworkAgent interface)
    int set_extra_http_header(std::map<std::string, std::string> extra_headers) override;
    std::string get_studio_info_url() override;

    // Printer operations - all stubs (INetworkAgent interface)
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;
    bool start_discovery(bool start, bool sending) override;
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int set_server_callback(OnServerErrFn fn) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    std::string get_bambulab_host() override;
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Print job operations - all stubs (INetworkAgent interface)
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Cloud services - all stubs (INetworkAgent interface)
    int get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body) override;
    int check_user_task_report(int* task_id, bool* printable) override;
    int get_user_print_info(unsigned int* http_code, std::string* http_body) override;
    int get_user_tasks(TaskQueryParams params, std::string* http_body) override;
    int get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body) override;
    int get_task_plate_index(std::string task_id, int* plate_index) override;
    int get_user_info(int* identifier) override;
    int request_bind_ticket(std::string* ticket) override;
    int get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) override;
    int get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) override;
    int query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) override;
    int modify_printer_name(std::string dev_id, std::string dev_name) override;

    // Model mall & publishing - all stubs (INetworkAgent interface)
    int get_camera_url(std::string dev_id, std::function<void(std::string)> callback) override;
    int get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback) override;
    int start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out) override;
    int get_model_publish_url(std::string* url) override;
    int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn) override;
    int get_model_mall_home_url(std::string* url) override;
    int get_model_mall_detail_url(std::string* url, std::string id) override;
    int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body) override;

    // Analytics & tracking - all stubs (INetworkAgent interface)
    int track_enable(bool enable) override;
    int track_remove_files() override;
    int track_event(std::string evt_key, std::string content) override;
    int track_header(std::string header) override;
    int track_update_property(std::string name, std::string value, std::string type = "string") override;
    int track_get_property(std::string name, std::string& value, std::string type = "string") override;
    bool get_track_enable() override { return enable_track; }

    // Ratings & reviews - all stubs (INetworkAgent interface)
    int put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error) override;
    int get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error) override;
    int put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error) override;
    int get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error) override;

    // Miscellaneous - all stubs (INetworkAgent interface)
    int get_mw_user_preference(std::function<void(std::string)> callback) override;
    int get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback) override;
    
    // Version Information (INetworkAgent interface)
    std::string get_version() override;

    // Utility methods (OrcaNetwork-specific, not in interface)
    std::string get_backend_url() const { return api_base_url; }

private:
    // HTTP request helpers
    int http_get(const std::string& path, std::string* response_body, unsigned int* http_code);
    int http_post(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code);
    int http_put(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code);
    int http_delete(const std::string& path, std::string* response_body, unsigned int* http_code);
    int http_post_auth(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code);
    std::map<std::string, std::string> data_headers();
    bool ensure_token_fresh(const std::string& reason);
    bool attempt_refresh_after_unauthorized(const std::string& reason);

    // Callback invocation helpers (thread-safe via queue_on_main)
    void invoke_user_login_callback(int online_login, bool login);
    void invoke_server_connected_callback(int return_code, int reason_code);
    void invoke_http_error_callback(unsigned http_code, const std::string& http_body);

    // JSON helpers
    std::string map_to_json(const std::map<std::string, std::string>& map);
    void json_to_map(const std::string& json, std::map<std::string, std::string>& map);

    // Member variables - configuration
    std::string log_dir;
    std::string config_dir;
    std::string cert_folder;
    std::string cert_filename;
    std::string country_code;
    std::string api_base_url;
    std::string auth_base_url;
    std::map<std::string, std::string> extra_headers;
    std::map<std::string, std::string> auth_headers;

    // Member variables - state
    bool is_connected;
    bool enable_track;
    bool multi_machine_enabled;
    std::string selected_machine;
    std::unique_ptr<AuthManager> auth_manager;

    // Sync state (persisted to {config_dir}/{user_id}/sync_state - plain text file)
    SyncState sync_state;
    std::string sync_state_path;

    // Callbacks
OnMsgArrivedFn on_ssdp_msg_fn;
OnUserLoginFn on_user_login_fn;
OnPrinterConnectedFn on_printer_connected_fn;
OnServerConnectedFn on_server_connected_fn;
OnHttpErrorFn on_http_error_fn;
GetCountryCodeFn get_country_code_fn;
GetSubscribeFailureFn on_subscribe_failure_fn;
OnMessageFn on_message_fn;
OnMessageFn on_user_message_fn;
OnLocalConnectedFn on_local_connect_fn;
OnMessageFn on_local_message_fn;
QueueOnMainFn queue_on_main_fn;
OnServerErrFn on_server_err_fn;

    // Thread safety
    mutable std::recursive_mutex state_mutex;
};

} // namespace Slic3r

#endif // __ORCA_NETWORK_HPP__
