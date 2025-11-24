#ifndef __ORCA_NETWORK_HPP__
#define __ORCA_NETWORK_HPP__

#include "INetworkAgent.hpp"
#include "bambu_networking.hpp"
#include "../../libslic3r/ProjectTask.hpp"
#include <string>
#include <map>
#include <mutex>
#include <functional>

namespace Slic3r {

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

    // Lifecycle methods (INetworkAgent interface)
    int init_log() override;
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string folder, std::string filename) override;
    int set_country_code(std::string country_code) override;
    int start() override;

    // Callback registration (INetworkAgent interface)
    int set_on_ssdp_msg_fn(BBL::OnMsgArrivedFn fn) override;
    int set_on_user_login_fn(BBL::OnUserLoginFn fn) override;
    int set_on_printer_connected_fn(BBL::OnPrinterConnectedFn fn) override;
    int set_on_server_connected_fn(BBL::OnServerConnectedFn fn) override;
    int set_on_http_error_fn(BBL::OnHttpErrorFn fn) override;
    int set_get_country_code_fn(BBL::GetCountryCodeFn fn) override;
    int set_on_subscribe_failure_fn(BBL::GetSubscribeFailureFn fn) override;
    int set_on_message_fn(BBL::OnMessageFn fn) override;
    int set_on_user_message_fn(BBL::OnMessageFn fn) override;
    int set_on_local_connect_fn(BBL::OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(BBL::OnMessageFn fn) override;
    int set_queue_on_main_fn(BBL::QueueOnMainFn fn) override;

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
                        std::string name, std::string nickname, std::string avatar);
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override;
    std::string get_user_nickanme() override;
    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;

    // Settings sync (INetworkAgent interface)
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) override;
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int get_setting_list(std::string bundle_version, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) override;
    int get_setting_list2(std::string bundle_version, BBL::CheckFn chk_fn, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) override;
    int delete_setting(std::string setting_id) override;

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
    int bind_detect(std::string dev_ip, std::string sec_link, BBL::detectResult& detect) override;
    int set_server_callback(BBL::OnServerErrFn fn) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, BBL::OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    std::string get_bambulab_host() override;
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Print job operations - all stubs (INetworkAgent interface)
    int start_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_local_print_with_record(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_local_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) override;
    int start_sdcard_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) override;

    // Cloud services - all stubs (INetworkAgent interface)
    int get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body) override;
    int check_user_task_report(int* task_id, bool* printable) override;
    int get_user_print_info(unsigned int* http_code, std::string* http_body) override;
    int get_user_tasks(BBL::TaskQueryParams params, std::string* http_body) override;
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
    int start_publish(BBL::PublishParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, std::string* out) override;
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
    std::string get_backend_url() const { return backend_url; }
    void set_backend_url(const std::string& url) { backend_url = url; }

private:
    // HTTP request helpers
    int http_get(const std::string& path, std::string* response_body, unsigned int* http_code);
    int http_post(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code);
    int http_put(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code);
    int http_delete(const std::string& path, std::string* response_body, unsigned int* http_code);

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
    std::string backend_url;
    std::map<std::string, std::string> extra_headers;

    // Member variables - state
    bool is_connected;
    bool is_logged_in;
    bool enable_track;
    bool multi_machine_enabled;
    std::string session_token;
    std::string user_id;
    std::string user_name;
    std::string user_avatar;
    std::string user_nickname;
    std::string selected_machine;

    // Callbacks
    BBL::OnMsgArrivedFn on_ssdp_msg_fn;
    BBL::OnUserLoginFn on_user_login_fn;
    BBL::OnPrinterConnectedFn on_printer_connected_fn;
    BBL::OnServerConnectedFn on_server_connected_fn;
    BBL::OnHttpErrorFn on_http_error_fn;
    BBL::GetCountryCodeFn get_country_code_fn;
    BBL::GetSubscribeFailureFn on_subscribe_failure_fn;
    BBL::OnMessageFn on_message_fn;
    BBL::OnMessageFn on_user_message_fn;
    BBL::OnLocalConnectedFn on_local_connect_fn;
    BBL::OnMessageFn on_local_message_fn;
    BBL::QueueOnMainFn queue_on_main_fn;
    BBL::OnServerErrFn on_server_err_fn;

    // Thread safety
    std::recursive_mutex state_mutex;
};

} // namespace Slic3r

#endif // __ORCA_NETWORK_HPP__
