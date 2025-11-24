#ifndef __NETWORK_Agent_HPP__
#define __NETWORK_Agent_HPP__

#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "INetworkAgent.hpp"

using namespace BBL;

namespace Slic3r {
typedef bool (*func_check_debug_consistent)(bool is_debug);
typedef std::string (*func_get_version)(void);
typedef void* (*func_create_agent)(std::string log_dir);
typedef int (*func_destroy_agent)(void *agent);
typedef int (*func_init_log)(void *agent);
typedef int (*func_set_config_dir)(void *agent, std::string config_dir);
typedef int (*func_set_cert_file)(void *agent, std::string folder, std::string filename);
typedef int (*func_set_country_code)(void *agent, std::string country_code);
typedef int (*func_start)(void *agent);
typedef int (*func_set_on_ssdp_msg_fn)(void *agent, OnMsgArrivedFn fn);
typedef int (*func_set_on_user_login_fn)(void *agent, OnUserLoginFn fn);
typedef int (*func_set_on_printer_connected_fn)(void *agent, OnPrinterConnectedFn fn);
typedef int (*func_set_on_server_connected_fn)(void *agent, OnServerConnectedFn fn);
typedef int (*func_set_on_http_error_fn)(void *agent, OnHttpErrorFn fn);
typedef int (*func_set_get_country_code_fn)(void *agent, GetCountryCodeFn fn);
typedef int (*func_set_on_subscribe_failure_fn)(void *agent, GetSubscribeFailureFn fn);
typedef int (*func_set_on_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_on_user_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_on_local_connect_fn)(void *agent, OnLocalConnectedFn fn);
typedef int (*func_set_on_local_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_queue_on_main_fn)(void *agent, QueueOnMainFn fn);
typedef int (*func_connect_server)(void *agent);
typedef bool (*func_is_server_connected)(void *agent);
typedef int (*func_refresh_connection)(void *agent);
typedef int (*func_start_subscribe)(void *agent, std::string module);
typedef int (*func_stop_subscribe)(void *agent, std::string module);
typedef int (*func_add_subscribe)(void *agent, std::vector<std::string> dev_list);
typedef int (*func_del_subscribe)(void *agent, std::vector<std::string> dev_list);
typedef void (*func_enable_multi_machine)(void *agent, bool enable);
typedef int (*func_send_message)(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_connect_printer)(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
typedef int (*func_disconnect_printer)(void *agent);
typedef int (*func_send_message_to_printer)(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_check_cert)(void* agent);
typedef void (*func_install_device_cert)(void* agent, std::string dev_id, bool lan_only);
typedef bool (*func_start_discovery)(void *agent, bool start, bool sending);
typedef int (*func_change_user)(void *agent, std::string user_info);
typedef bool (*func_is_user_login)(void *agent);
typedef int (*func_user_logout)(void *agent, bool request);
typedef std::string (*func_get_user_id)(void *agent);
typedef std::string (*func_get_user_name)(void *agent);
typedef std::string (*func_get_user_avatar)(void *agent);
typedef std::string (*func_get_user_nickanme)(void *agent);
typedef std::string (*func_build_login_cmd)(void *agent);
typedef std::string (*func_build_logout_cmd)(void *agent);
typedef std::string (*func_build_login_info)(void *agent);
typedef int (*func_ping_bind)(void *agent, std::string ping_code);
typedef int (*func_bind_detect)(void *agent, std::string dev_ip, std::string sec_link, detectResult& detect);
typedef int (*func_set_server_callback)(void *agent, OnServerErrFn fn);
typedef int (*func_bind)(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
typedef int (*func_unbind)(void *agent, std::string dev_id);
typedef std::string (*func_get_bambulab_host)(void *agent);
typedef std::string (*func_get_user_selected_machine)(void *agent);
typedef int (*func_set_user_selected_machine)(void *agent, std::string dev_id);
typedef int (*func_start_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_get_user_presets)(void *agent, std::map<std::string, std::map<std::string, std::string>>* user_presets);
typedef std::string (*func_request_setting_id)(void *agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_put_setting)(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_get_setting_list)(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn);
typedef int (*func_get_setting_list2)(void *agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn);
typedef int (*func_delete_setting)(void *agent, std::string setting_id);
typedef std::string (*func_get_studio_info_url)(void *agent);
typedef int (*func_set_extra_http_header)(void *agent, std::map<std::string, std::string> extra_headers);
typedef int (*func_get_my_message)(void *agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body);
typedef int (*func_check_user_task_report)(void *agent, int* task_id, bool* printable);
typedef int (*func_get_user_print_info)(void *agent, unsigned int* http_code, std::string* http_body);
typedef int (*func_get_user_tasks)(void *agent, TaskQueryParams params, std::string* http_body);
typedef int (*func_get_printer_firmware)(void *agent, std::string dev_id, unsigned* http_code, std::string* http_body);
typedef int (*func_get_task_plate_index)(void *agent, std::string task_id, int* plate_index);
typedef int (*func_get_user_info)(void *agent, int* identifier);
typedef int (*func_request_bind_ticket)(void *agent, std::string* ticket);
typedef int (*func_get_subtask_info)(void *agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string *http_body);
typedef int (*func_get_slice_info)(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json);
typedef int (*func_query_bind_status)(void *agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body);
typedef int (*func_modify_printer_name)(void *agent, std::string dev_id, std::string dev_name);
typedef int (*func_get_camera_url)(void *agent, std::string dev_id, std::function<void(std::string)> callback);
typedef int (*func_get_design_staffpick)(void *agent, int offset, int limit, std::function<void(std::string)> callback);
typedef int (*func_start_pubilsh)(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out);
typedef int (*func_get_model_publish_url)(void *agent, std::string* url);
typedef int (*func_get_subtask)(void *agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn);
typedef int (*func_get_model_mall_home_url)(void *agent, std::string* url);
typedef int (*func_get_model_mall_detail_url)(void *agent, std::string* url, std::string id);
typedef int (*func_get_my_profile)(void *agent, std::string token, unsigned int *http_code, std::string *http_body);
typedef int (*func_track_enable)(void *agent, bool enable);
typedef int (*func_track_remove_files)(void *agent);
typedef int (*func_track_event)(void *agent, std::string evt_key, std::string content);
typedef int (*func_track_header)(void *agent, std::string header);
typedef int (*func_track_update_property)(void *agent, std::string name, std::string value, std::string type);
typedef int (*func_track_get_property)(void *agent, std::string name, std::string& value, std::string type);
typedef int (*func_put_model_mall_rating_url)(
    void *agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error);
typedef int (*func_get_oss_config)(void *agent, std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error);
typedef int (*func_put_rating_picture_oss)(
    void *agent, std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error);
typedef int (*func_get_model_mall_rating_result)(void *agent, int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error);

typedef int (*func_get_mw_user_preference)(void *agent, std::function<void(std::string)> callback);
typedef int (*func_get_mw_user_4ulist)(void *agent, int seed, int limit, std::function<void(std::string)> callback);


//the NetworkAgent class
class NetworkAgent : public INetworkAgent
{

public:
    static std::string get_libpath_in_current_directory(std::string library_name);
    static int initialize_network_module(bool using_backup = false);
    static int unload_network_module();
#if defined(_MSC_VER) || defined(_WIN32)
    static HMODULE get_bambu_source_entry();
#else
    static void* get_bambu_source_entry();
#endif
    static std::string get_library_version();
    static void* get_network_function(const char* name);
    static bool use_legacy_network;
    NetworkAgent(std::string log_dir);
    ~NetworkAgent() override;

    int init_log() override;
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string folder, std::string filename) override;
    int set_country_code(std::string country_code) override;
    int start() override;
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
    int connect_server() override;
    bool is_server_connected() override;
    int refresh_connection() override;
    int start_subscribe(std::string module) override;
    int stop_subscribe(std::string module) override;
    int add_subscribe(std::vector<std::string> dev_list) override;
    int del_subscribe(std::vector<std::string> dev_list) override;
    void enable_multi_machine(bool enable) override;
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;
    bool start_discovery(bool start, bool sending) override;
    int change_user(std::string user_info) override;
    bool is_user_login() override;
    int  user_logout(bool request = false) override;
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override;
    std::string get_user_nickanme() override;
    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, BBL::detectResult& detect) override;
    int set_server_callback(BBL::OnServerErrFn fn) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, BBL::OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    std::string get_bambulab_host() override;
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;
    int start_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_local_print_with_record(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, BBL::OnWaitFn wait_fn) override;
    int start_local_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) override;
    int start_sdcard_print(BBL::PrintParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn) override;
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) override;
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int get_setting_list(std::string bundle_version, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) override;
    int get_setting_list2(std::string bundle_version, BBL::CheckFn chk_fn, BBL::ProgressFn pro_fn = nullptr, BBL::WasCancelledFn cancel_fn = nullptr) override;
    int delete_setting(std::string setting_id) override;
    std::string get_studio_info_url() override;
    int set_extra_http_header(std::map<std::string, std::string> extra_headers) override;
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
    int get_camera_url(std::string dev_id, std::function<void(std::string)> callback) override;
    int get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback) override;
    int start_publish(BBL::PublishParams params, BBL::OnUpdateStatusFn update_fn, BBL::WasCancelledFn cancel_fn, std::string* out) override;
    int get_model_publish_url(std::string* url) override;
    int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn) override;
    int get_model_mall_home_url(std::string* url) override;
    int get_model_mall_detail_url(std::string* url, std::string id) override;
    int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body) override;
    int track_enable(bool enable) override;
    int track_remove_files() override;
    int track_event(std::string evt_key, std::string content) override;
    int track_header(std::string header) override;
    int track_update_property(std::string name, std::string value, std::string type = "string") override;
    int track_get_property(std::string name, std::string& value, std::string type = "string") override;
    int put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error) override;
    int get_oss_config(std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error) override;
    int put_rating_picture_oss(std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error) override;
    int get_model_mall_rating_result(int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error) override;
    bool get_track_enable() override { return enable_track; }

    int get_mw_user_preference(std::function<void(std::string)> callback) override;
    int get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback) override;
    std::string get_version() override;
    void *get_network_agent() { return network_agent; }

private:
    bool enable_track = false;
    void*                   network_agent { nullptr };
    
    // Helper to get version (shared by static and instance methods)
    static std::string get_version_impl();

    static func_check_debug_consistent         check_debug_consistent_ptr;
    static func_get_version                    get_version_ptr;
    static func_create_agent                   create_agent_ptr;
    static func_destroy_agent                  destroy_agent_ptr;
    static func_init_log                       init_log_ptr;
    static func_set_config_dir                 set_config_dir_ptr;
    static func_set_cert_file                  set_cert_file_ptr;
    static func_set_country_code               set_country_code_ptr;
    static func_start                          start_ptr;
    static func_set_on_ssdp_msg_fn             set_on_ssdp_msg_fn_ptr;
    static func_set_on_user_login_fn           set_on_user_login_fn_ptr;
    static func_set_on_printer_connected_fn    set_on_printer_connected_fn_ptr;
    static func_set_on_server_connected_fn     set_on_server_connected_fn_ptr;
    static func_set_on_http_error_fn           set_on_http_error_fn_ptr;
    static func_set_get_country_code_fn        set_get_country_code_fn_ptr;
    static func_set_on_subscribe_failure_fn    set_on_subscribe_failure_fn_ptr;
    static func_set_on_message_fn              set_on_message_fn_ptr;
    static func_set_on_user_message_fn         set_on_user_message_fn_ptr;
    static func_set_on_local_connect_fn        set_on_local_connect_fn_ptr;
    static func_set_on_local_message_fn        set_on_local_message_fn_ptr;
    static func_set_queue_on_main_fn           set_queue_on_main_fn_ptr;
    static func_connect_server                 connect_server_ptr;
    static func_is_server_connected            is_server_connected_ptr;
    static func_refresh_connection             refresh_connection_ptr;
    static func_start_subscribe                start_subscribe_ptr;
    static func_stop_subscribe                 stop_subscribe_ptr;
    static func_add_subscribe                  add_subscribe_ptr;
    static func_del_subscribe                  del_subscribe_ptr;
    static func_enable_multi_machine           enable_multi_machine_ptr;
    static func_send_message                   send_message_ptr;
    static func_connect_printer                connect_printer_ptr;
    static func_disconnect_printer             disconnect_printer_ptr;
    static func_send_message_to_printer        send_message_to_printer_ptr;
    static func_check_cert                     check_cert_ptr;
    static func_install_device_cert            install_device_cert_ptr;
    static func_start_discovery                start_discovery_ptr;
    static func_change_user                    change_user_ptr;
    static func_is_user_login                  is_user_login_ptr;
    static func_user_logout                    user_logout_ptr;
    static func_get_user_id                    get_user_id_ptr;
    static func_get_user_name                  get_user_name_ptr;
    static func_get_user_avatar                get_user_avatar_ptr;
    static func_get_user_nickanme              get_user_nickanme_ptr;
    static func_build_login_cmd                build_login_cmd_ptr;
    static func_build_logout_cmd               build_logout_cmd_ptr;
    static func_build_login_info               build_login_info_ptr;
    static func_ping_bind                      ping_bind_ptr;
    static func_bind_detect                    bind_detect_ptr;
    static func_set_server_callback            set_server_callback_ptr;
    static func_bind                           bind_ptr;
    static func_unbind                         unbind_ptr;
    static func_get_bambulab_host              get_bambulab_host_ptr;
    static func_get_user_selected_machine      get_user_selected_machine_ptr;
    static func_set_user_selected_machine      set_user_selected_machine_ptr;
    static func_start_print                    start_print_ptr;
    static func_start_local_print_with_record  start_local_print_with_record_ptr;
    static func_start_send_gcode_to_sdcard     start_send_gcode_to_sdcard_ptr;
    static func_start_local_print              start_local_print_ptr;
    static func_start_sdcard_print             start_sdcard_print_ptr;
    static func_get_user_presets               get_user_presets_ptr;
    static func_request_setting_id             request_setting_id_ptr;
    static func_put_setting                    put_setting_ptr;
    static func_get_setting_list               get_setting_list_ptr;
    static func_get_setting_list2              get_setting_list2_ptr;
    static func_delete_setting                 delete_setting_ptr;
    static func_get_studio_info_url            get_studio_info_url_ptr;
    static func_set_extra_http_header          set_extra_http_header_ptr;
    static func_get_my_message                 get_my_message_ptr;
    static func_check_user_task_report         check_user_task_report_ptr;
    static func_get_user_print_info            get_user_print_info_ptr;
    static func_get_user_tasks                 get_user_tasks_ptr;
    static func_get_printer_firmware           get_printer_firmware_ptr;
    static func_get_task_plate_index           get_task_plate_index_ptr;
    static func_get_user_info                  get_user_info_ptr;
    static func_request_bind_ticket            request_bind_ticket_ptr;
    static func_get_subtask_info               get_subtask_info_ptr;
    static func_get_slice_info                 get_slice_info_ptr;
    static func_query_bind_status              query_bind_status_ptr;
    static func_modify_printer_name            modify_printer_name_ptr;
    static func_get_camera_url                 get_camera_url_ptr;
    static func_get_design_staffpick           get_design_staffpick_ptr;
    static func_start_pubilsh                  start_publish_ptr;
    static func_get_model_publish_url          get_model_publish_url_ptr;
    static func_get_subtask                    get_subtask_ptr;
    static func_get_model_mall_home_url        get_model_mall_home_url_ptr;
    static func_get_model_mall_detail_url      get_model_mall_detail_url_ptr;
    static func_get_my_profile                 get_my_profile_ptr;
    static func_track_enable                   track_enable_ptr;
    static func_track_remove_files             track_remove_files_ptr;
    static func_track_event                    track_event_ptr;
    static func_track_header                   track_header_ptr;
    static func_track_update_property          track_update_property_ptr;
    static func_track_get_property             track_get_property_ptr;
    static func_put_model_mall_rating_url      put_model_mall_rating_url_ptr;
    static func_get_oss_config                 get_oss_config_ptr;
    static func_put_rating_picture_oss         put_rating_picture_oss_ptr;
    static func_get_model_mall_rating_result   get_model_mall_rating_result_ptr;

    static func_get_mw_user_preference get_mw_user_preference_ptr;
    static func_get_mw_user_4ulist     get_mw_user_4ulist_ptr;
};

}

#endif

