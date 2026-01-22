#include "MoonrakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <sstream>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr const char* k_no_api_key = "__NO_API_KEY__";

bool is_numeric(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string normalize_base_url(std::string host, const std::string& port)
{
    boost::trim(host);
    if (host.empty()) {
        return "";
    }

    std::string value = host;
    if (is_numeric(port) && value.find("://") == std::string::npos && value.find(':') == std::string::npos) {
        value += ":" + port;
    }

    if (!boost::istarts_with(value, "http://") && !boost::istarts_with(value, "https://")) {
        value = "http://" + value;
    }

    if (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }

    return value;
}

std::string extract_host(const std::string& base_url)
{
    std::string host = base_url;
    auto        pos  = host.find("://");
    if (pos != std::string::npos) {
        host = host.substr(pos + 3);
    }
    pos = host.find('/');
    if (pos != std::string::npos) {
        host = host.substr(0, pos);
    }
    return host;
}

std::string join_url(const std::string& base_url, const std::string& path)
{
    if (base_url.empty()) {
        return "";
    }
    if (path.empty()) {
        return base_url;
    }
    if (base_url.back() == '/' && path.front() == '/') {
        return base_url.substr(0, base_url.size() - 1) + path;
    }
    if (base_url.back() != '/' && path.front() != '/') {
        return base_url + "/" + path;
    }
    return base_url + path;
}

std::string normalize_api_key(const std::string& api_key)
{
    if (api_key.empty() || api_key == k_no_api_key) {
        return "";
    }
    return api_key;
}

struct WsEndpoint
{
    std::string host;
    std::string port;
    std::string target;
    bool        secure = false;
};

bool parse_ws_endpoint(const std::string& base_url, WsEndpoint& endpoint)
{
    if (base_url.empty()) {
        return false;
    }

    std::string url = base_url;
    if (boost::istarts_with(url, "https://")) {
        endpoint.secure = true;
        url             = url.substr(8);
    } else if (boost::istarts_with(url, "http://")) {
        url = url.substr(7);
    }

    auto slash = url.find('/');
    if (slash != std::string::npos) {
        url = url.substr(0, slash);
    }
    if (url.empty()) {
        return false;
    }

    endpoint.host = url;
    endpoint.port = endpoint.secure ? "443" : "80";
    if (auto colon = url.rfind(':'); colon != std::string::npos && url.find(']') == std::string::npos) {
        endpoint.host = url.substr(0, colon);
        endpoint.port = url.substr(colon + 1);
    }

    endpoint.target = "/websocket";
    return !endpoint.host.empty() && !endpoint.port.empty();
}

std::string map_moonraker_state(std::string state)
{
    boost::algorithm::to_lower(state);
    if (state == "printing") {
        return "RUNNING";
    }
    if (state == "paused") {
        return "PAUSE";
    }
    if (state == "complete") {
        return "FINISH";
    }
    if (state == "error" || state == "cancelled") {
        return "FAILED";
    }
    return "IDLE";
}

} // namespace

namespace Slic3r {

const std::string MoonrakerPrinterAgent_VERSION = "1.0.0";

MoonrakerPrinterAgent::MoonrakerPrinterAgent(std::string log_dir) : m_cloud_agent(nullptr)
{
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Constructor - log_dir=" << log_dir;
    (void) log_dir;
}

MoonrakerPrinterAgent::~MoonrakerPrinterAgent()
{
    stop_status_stream();
}

AgentInfo MoonrakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{.id = "moonraker", .name = "Moonraker Printer Agent", .version = MoonrakerPrinterAgent_VERSION, .description = "Klipper/Moonraker printer agent"};
}

void MoonrakerPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    m_cloud_agent = cloud;
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: Cloud agent set";
}

int MoonrakerPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int MoonrakerPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int MoonrakerPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    (void) username;
    (void) use_ssl;
    std::string base_url = normalize_base_url(dev_ip, "");
    std::string api_key  = normalize_api_key(password);

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        if (base_url.empty()) {
            base_url = config.base_url;
        }
        if (api_key.empty()) {
            api_key = normalize_api_key(config.api_key);
        }
    }

    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: connect_printer missing host for dev_id=" << dev_id;
        dispatch_local_connect(ConnectStatusFailed, dev_id, "host_missing");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    if (dev_id.empty()) {
        dev_id = extract_host(base_url);
    }

    {
        std::lock_guard<std::mutex> lock(payload_mutex);
        status_cache = nlohmann::json::object();
    }
    ws_last_emit_ms.store(0);

    store_host(dev_id, base_url, api_key);
    start_status_stream(dev_id, base_url, api_key);

    // Query initial status via HTTP before signaling connection
    nlohmann::json initial_status;
    std::string query_error;
    if (query_printer_status(base_url, api_key, initial_status, query_error)) {
        update_status_cache(initial_status);
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Initial status queried successfully";
    } else {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Initial status query failed: " << query_error;
    }

    dispatch_local_connect(ConnectStatusOk, dev_id, "0");
    dispatch_printer_connected(dev_id);
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: connect_printer - dev_id=" << dev_id << ", dev_ip=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::disconnect_printer()
{
    stop_status_stream();
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::check_cert()
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: check_cert (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: install_device_cert (stub) - dev_id=" << dev_id << ", lan_only=" << lan_only;
}

bool MoonrakerPrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    if (start) {
        announce_printhost_device();
    }
    return true;
}

int MoonrakerPrinterAgent::ping_bind(std::string ping_code)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: ping_bind (stub) - ping_code=" << ping_code;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;

    std::string base_url = normalize_base_url(dev_ip, "");
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    PrinthostConfig config;
    get_printhost_config(config);
    const std::string api_key = normalize_api_key(config.api_key);

    MoonrakerDeviceInfo info;
    std::string         error;
    if (!fetch_device_info(base_url, api_key, info, error)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: bind_detect failed: " << error;
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    detect.dev_id       = info.dev_id.empty() ? dev_ip : info.dev_id;
    detect.dev_name     = info.dev_name.empty() ? "Moonraker Printer" : info.dev_name;
    detect.model_id     = "moonraker";
    detect.version      = info.version;
    detect.connect_type = "lan";
    detect.bind_state   = "free";

    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind(
    std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: bind (stub) - dev_id=" << dev_id;
    (void) dev_ip;
    (void) sec_link;
    (void) timezone;
    (void) improved;
    (void) update_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::unbind(std::string dev_id)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: unbind (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::request_bind_ticket(std::string* ticket)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: request_bind_ticket (stub)";
    if (ticket)
        *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

std::string MoonrakerPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return selected_machine;
}

int MoonrakerPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    selected_machine = dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_print (stub) - task_name=" << params.task_name;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_local_print_with_record (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_send_gcode_to_sdcard (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_local_print (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_sdcard_print (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        on_ssdp_msg_fn = fn;
    }
    // Call announce_printhost_device() outside the lock to avoid deadlock
    // since announce_printhost_device() also acquires state_mutex
    if (fn) {
        announce_printhost_device();
    }
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // Moonraker doesn't have standard filament tracking like Qidi
    // This is a no-op for standard Moonraker installations
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: fetch_filament_info (no-op) - dev_id=" << dev_id;
}

int MoonrakerPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
    {
        BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: handle_request received: " << json_str;
        auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Invalid JSON request";
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    // Handle info commands
    if (json.contains("info") && json["info"].contains("command")) {
        const auto& command = json["info"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_version") {
            return send_version_info(dev_id);
        }
    }

    // Handle system commands
    if (json.contains("system") && json["system"].contains("command")) {
        const auto& command = json["system"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_access_code") {
            return send_access_code(dev_id);
        }
    }

    // Handle print commands
    if (json.contains("print") && json["print"].contains("command")) {
        const auto& command = json["print"]["command"];
        if (!command.is_string()) {
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: print command is not a string";
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }

        const std::string cmd = command.get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Received print command: " << cmd;

        // Handle gcode_line command - this is how G-code commands are sent from OrcaSlicer
        if (cmd == "gcode_line") {
            if (!json["print"].contains("param") || !json["print"]["param"].is_string()) {
                BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: gcode_line missing param value, full json: " << json_str;
                return BAMBU_NETWORK_ERR_INVALID_RESULT;
            }
            std::string gcode = json["print"]["param"].get<std::string>();

            // Extract sequence_id from request if present
            std::string sequence_id;
            if (json["print"].contains("sequence_id") && json["print"]["sequence_id"].is_string()) {
                sequence_id = json["print"]["sequence_id"].get<std::string>();
            }

            nlohmann::json response;
            response["print"]["command"] = "gcode_line";
            if (!sequence_id.empty()) {
                response["print"]["sequence_id"] = sequence_id;
            }
            response["print"]["param"] = gcode;

            if (send_gcode(dev_id, gcode)) {
                response["print"]["result"] = "success";
                dispatch_message(dev_id, response.dump());
                return BAMBU_NETWORK_SUCCESS;
            }
            response["print"]["result"] = "failed";
            dispatch_message(dev_id, response.dump());
            return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        }
    }

    return BAMBU_NETWORK_SUCCESS;
}

bool MoonrakerPrinterAgent::get_printhost_config(PrinthostConfig& config) const
{
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return false;
    }

    auto&      preset      = preset_bundle->printers.get_edited_preset();
    const auto& printer_cfg = preset.config;
    const DynamicPrintConfig* host_cfg = &printer_cfg;
    config.host = host_cfg->opt_string("print_host");
    if (config.host.empty()) {
        if (auto* physical_cfg = preset_bundle->physical_printers.get_selected_printer_config()) {
            if (!physical_cfg->opt_string("print_host").empty()) {
                host_cfg   = physical_cfg;
                config.host = host_cfg->opt_string("print_host");
            }
        }
    }
    if (config.host.empty()) {
        return false;
    }

    config.port       = host_cfg->opt_string("printhost_port");
    config.api_key    = host_cfg->opt_string("printhost_apikey");
    config.base_url   = normalize_base_url(config.host, config.port);

    return !config.base_url.empty();
}

bool MoonrakerPrinterAgent::fetch_device_info(const std::string& base_url,
                                               const std::string& api_key,
                                               MoonrakerDeviceInfo& info,
                                               std::string& error) const
{
    auto fetch_json = [&](const std::string& url, nlohmann::json& out) {
        std::string response_body;
        bool        success = false;
        std::string http_error;

        auto http = Http::get(url);
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

        out = nlohmann::json::parse(response_body, nullptr, false, true);
        if (out.is_discarded()) {
            error = "Invalid JSON response";
            return false;
        }
        return true;
    };

    nlohmann::json json;
    std::string    url = join_url(base_url, "/server/info");
    if (!fetch_json(url, json)) {
        return false;
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    info.dev_name         = result.value("hostname", "Moonraker Printer");
    info.dev_id           = result.value("hostname", "");
    info.version          = result.value("moonraker_version", "");

    return true;
}

bool MoonrakerPrinterAgent::fetch_server_info(const std::string& base_url,
                                               const std::string& api_key,
                                               std::string& version,
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

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    if (result.contains("moonraker_version") && result["moonraker_version"].is_string()) {
        version = result["moonraker_version"].get<std::string>();
    } else if (result.contains("version") && result["version"].is_string()) {
        version = result["version"].get<std::string>();
    }

    return true;
}

bool MoonrakerPrinterAgent::query_printer_status(const std::string& base_url,
                                                   const std::string& api_key,
                                                   nlohmann::json& status,
                                                   std::string& error) const
{
    std::string url = join_url(base_url, "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status_code) {
            if (status_code == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status_code);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status_code) {
            http_error = err;
            if (status_code > 0) {
                http_error += " (HTTP " + std::to_string(status_code) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.contains("result") || !json["result"].contains("status")) {
        error = "Unexpected JSON structure";
        return false;
    }

    status = json["result"]["status"];
    return true;
}

bool MoonrakerPrinterAgent::send_gcode(const std::string& dev_id, const std::string& gcode) const
{
    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode - empty base_url for dev_id=" << dev_id;
        return false;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    nlohmann::json payload;
    payload["script"] = gcode;
    std::string payload_str = payload.dump();

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: send_gcode to " << base_url << " with payload: " << payload_str;

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::post(join_url(base_url, "/printer/gcode/script"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(payload_str)
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status_code) {
            BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: send_gcode response status=" << status_code << " body=" << body;
            if (status_code == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status_code);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status_code) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode error - body=" << body << " err=" << err << " status=" << status_code;
            http_error = err;
            if (status_code > 0) {
                http_error += " (HTTP " + std::to_string(status_code) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode failed: " << http_error;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: sent gcode successfully: " << gcode;
    return true;
}

bool MoonrakerPrinterAgent::fetch_object_list(const std::string& base_url,
                                               const std::string& api_key,
                                               std::set<std::string>& objects,
                                               std::string& error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(join_url(base_url, "/printer/objects/list"));
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

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    if (!result.contains("objects") || !result["objects"].is_array()) {
        error = "Unexpected JSON structure";
        return false;
    }

    objects.clear();
    for (const auto& entry : result["objects"]) {
        if (entry.is_string()) {
            objects.insert(entry.get<std::string>());
        }
    }

    return !objects.empty();
}

int MoonrakerPrinterAgent::send_version_info(const std::string& dev_id)
{
    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    std::string version;
    std::string error;
    if (!fetch_server_info(base_url, api_key, version, error)) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Failed to fetch server info: " << error;
    }
    if (version.empty()) {
        version = "moonraker";
    }

    nlohmann::json payload;
    payload["info"]["command"] = "get_version";
    payload["info"]["result"]  = "success";
    payload["info"]["module"]  = nlohmann::json::array();

    nlohmann::json module;
    module["name"]         = "ota";
    module["sw_ver"]       = version;
    module["product_name"] = "Moonraker";
    payload["info"]["module"].push_back(module);

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::send_access_code(const std::string& dev_id)
{
    nlohmann::json payload;
    payload["system"]["command"]     = "get_access_code";
    payload["system"]["access_code"] = resolve_api_key(dev_id, "");
    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::announce_printhost_device()
{
    PrinthostConfig config;
    if (!get_printhost_config(config)) {
        return;
    }

    const std::string base_url = config.base_url;
    if (base_url.empty()) {
        return;
    }

    OnMsgArrivedFn ssdp_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ssdp_fn = on_ssdp_msg_fn;
        if (!ssdp_fn) {
            return;
        }
        if (ssdp_announced_host == base_url && !ssdp_announced_id.empty()) {
            return;
        }
    }

    const std::string dev_id   = extract_host(base_url);
    const std::string dev_name = "Moonraker Printer";

    if (auto* app_config = GUI::wxGetApp().app_config) {
        const std::string access_code = normalize_api_key(config.api_key).empty() ? k_no_api_key : config.api_key;
        app_config->set_str("access_code", dev_id, access_code);
        app_config->set_str("user_access_code", dev_id, access_code);
    }

    store_host(dev_id, base_url, normalize_api_key(config.api_key));

    nlohmann::json payload;
    payload["dev_name"]     = dev_name;
    payload["dev_id"]       = dev_id;
    payload["dev_ip"]       = extract_host(base_url);
    payload["dev_type"]     = "moonraker";
    payload["dev_signal"]   = "0";
    payload["connect_type"] = "lan";
    payload["bind_state"]   = "free";
    payload["sec_link"]     = "secure";
    payload["ssdp_version"] = "v1";

    ssdp_fn(payload.dump());

    std::lock_guard<std::mutex> lock(state_mutex);
    ssdp_announced_host = base_url;
    ssdp_announced_id   = dev_id;
}

void MoonrakerPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_connect_fn;
        queue_fn = queue_on_main_fn;
    }
    if (!local_fn) {
        return;
    }

    auto dispatch = [state, dev_id, msg, local_fn]() { local_fn(state, dev_id, msg); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void MoonrakerPrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn connected_fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        connected_fn = on_printer_connected_fn;
        queue_fn     = queue_on_main_fn;
    }
    if (!connected_fn) {
        return;
    }

    auto dispatch = [dev_id, connected_fn]() { connected_fn(dev_id); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void MoonrakerPrinterAgent::start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key)
{
    stop_status_stream();
    if (base_url.empty()) {
        return;
    }

    ws_stop.store(false);
    ws_thread = std::thread([this, dev_id, base_url, api_key]() {
        run_status_stream(dev_id, base_url, api_key);
    });
}

void MoonrakerPrinterAgent::stop_status_stream()
{
    ws_stop.store(true);
    if (ws_thread.joinable()) {
        ws_thread.join();
    }
}

void MoonrakerPrinterAgent::run_status_stream(std::string dev_id, std::string base_url, std::string api_key)
{
    WsEndpoint endpoint;
    if (!parse_ws_endpoint(base_url, endpoint)) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket endpoint invalid for base_url=" << base_url;
        return;
    }
    if (endpoint.secure) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket wss not supported for base_url=" << base_url;
        return;
    }

    try {
        net::io_context ioc;
        tcp::resolver   resolver{ioc};
        beast::tcp_stream stream{ioc};

        stream.expires_after(std::chrono::seconds(10));
        auto const results = resolver.resolve(endpoint.host, endpoint.port);
        stream.connect(results);

        websocket::stream<beast::tcp_stream> ws{std::move(stream)};
        ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
            req.set(http::field::user_agent, "OrcaSlicer");
            if (!api_key.empty()) {
                req.set("X-Api-Key", api_key);
            }
        }));

        std::string host_header = endpoint.host;
        if (!endpoint.port.empty() && endpoint.port != "80") {
            host_header += ":" + endpoint.port;
        }
        ws.handshake(host_header, endpoint.target);
        ws.text(true);

        std::set<std::string> subscribe_objects = {"print_stats", "virtual_sdcard"};
        std::set<std::string> available_objects;
        std::string           list_error;
        if (fetch_object_list(base_url, api_key, available_objects, list_error)) {
            std::string objects_str;
            for (const auto& name : available_objects) {
                if (!objects_str.empty()) objects_str += ", ";
                objects_str += name;
            }

            if (available_objects.count("heater_bed") != 0) {
                subscribe_objects.insert("heater_bed");
            }
            // Only subscribe to "fan" if it exists (standard Moonraker API)
            if (available_objects.count("fan") != 0) {
                subscribe_objects.insert("fan");
            } else {
            }

            for (const auto& name : available_objects) {
                if (name == "extruder" || name.rfind("extruder", 0) == 0) {
                    subscribe_objects.insert(name);
                    if (name == "extruder") {
                        break;
                    }
                }
            }
        } else {
            subscribe_objects.insert("extruder");
            subscribe_objects.insert("heater_bed");
            subscribe_objects.insert("fan");  // Try to subscribe to fan as fallback
        }

        nlohmann::json subscribe;
        subscribe["jsonrpc"] = "2.0";
        subscribe["method"]  = "printer.objects.subscribe";
        nlohmann::json objects = nlohmann::json::object();
        for (const auto& name : subscribe_objects) {
            objects[name] = nullptr;
        }
        subscribe["params"]["objects"] = std::move(objects);
        subscribe["id"] = 1;
        ws.write(net::buffer(subscribe.dump()));

        while (!ws_stop.load()) {
            ws.next_layer().expires_after(std::chrono::seconds(2));
            beast::flat_buffer buffer;
            beast::error_code  ec;
            ws.read(buffer, ec);
            if (ec == beast::error::timeout) {
                const auto now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const auto last_ms = ws_last_emit_ms.load();
                if (last_ms == 0 || now_ms - last_ms >= 10000) {
                    nlohmann::json message;
                    {
                        std::lock_guard<std::mutex> lock(payload_mutex);
                        message = build_print_payload_locked();
                    }
                    dispatch_message(dev_id, message.dump());
                    ws_last_emit_ms.store(now_ms);
                }
                continue;
            }
            if (ec == websocket::error::closed) {
                break;
            }
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket read error: " << ec.message();
                break;
            }
            handle_ws_message(dev_id, beast::buffers_to_string(buffer.data()));
        }

        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket exception: " << e.what();
    }
}

void MoonrakerPrinterAgent::handle_ws_message(const std::string& dev_id, const std::string& payload)
{
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Invalid WebSocket message JSON";
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
    }

    if (updated) {
        nlohmann::json message;
        {
            std::lock_guard<std::mutex> lock(payload_mutex);
            message = build_print_payload_locked();
        }

        BOOST_LOG_TRIVIAL(trace) << "MoonrakerPrinterAgent: Dispatching payload: " << message.dump();
        dispatch_message(dev_id, message.dump());

        const auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        ws_last_emit_ms.store(now_ms);
    }
}

void MoonrakerPrinterAgent::update_status_cache(const nlohmann::json& updates)
{
    if (!updates.is_object()) {
        return;
    }

    std::lock_guard<std::mutex> lock(payload_mutex);
    if (!status_cache.is_object()) {
        status_cache = nlohmann::json::object();
    }

    for (const auto& item : updates.items()) {
        if (item.value().is_object()) {
            nlohmann::json& target = status_cache[item.key()];
            if (!target.is_object()) {
                target = nlohmann::json::object();
            }
            for (const auto& field : item.value().items()) {
                target[field.key()] = field.value();
            }
        } else {
            status_cache[item.key()] = item.value();
        }
    }
}

nlohmann::json MoonrakerPrinterAgent::build_print_payload_locked() const
{
    nlohmann::json payload;
    payload["print"]["command"]            = "push_status";
    payload["print"]["msg"]                = 0;
    payload["print"]["support_mqtt_alive"] = true;

    std::string state = "IDLE";
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state") &&
        status_cache["print_stats"]["state"].is_string()) {
        state = map_moonraker_state(status_cache["print_stats"]["state"].get<std::string>());
    }
    payload["print"]["gcode_state"] = state;

    const nlohmann::json* extruder = nullptr;
    if (status_cache.contains("extruder") && status_cache["extruder"].is_object()) {
        extruder = &status_cache["extruder"];
    } else {
        for (const auto& item : status_cache.items()) {
            if (item.value().is_object() && item.key().rfind("extruder", 0) == 0) {
                extruder = &item.value();
                break;
            }
        }
    }

    if (extruder) {
        if (extruder->contains("temperature") && (*extruder)["temperature"].is_number()) {
            payload["print"]["nozzle_temper"] = (*extruder)["temperature"].get<float>();
        }
        if (extruder->contains("target") && (*extruder)["target"].is_number()) {
            payload["print"]["nozzle_target_temper"] = (*extruder)["target"].get<float>();
        }
    }

    if (status_cache.contains("heater_bed") && status_cache["heater_bed"].is_object()) {
        const auto& bed = status_cache["heater_bed"];
        if (bed.contains("temperature") && bed["temperature"].is_number()) {
            payload["print"]["bed_temper"] = bed["temperature"].get<float>();
        }
        if (bed.contains("target") && bed["target"].is_number()) {
            payload["print"]["bed_target_temper"] = bed["target"].get<float>();
        }
    }

    // Handle fan speed - only if Moonraker provides "fan" object (standard API)
    if (status_cache.contains("fan") && status_cache["fan"].is_object() && !status_cache["fan"].empty()) {
        const auto& fan = status_cache["fan"];
        if (fan.contains("speed") && fan["speed"].is_number()) {
            double speed = fan["speed"].get<double>();
            int    pwm   = 0;
            if (speed <= 1.0) {
                pwm = static_cast<int>(speed * 255.0 + 0.5);
            } else {
                pwm = static_cast<int>(speed + 0.5);
            }
            pwm = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        } else if (fan.contains("power") && fan["power"].is_number()) {
            double power = fan["power"].get<double>();
            int pwm = static_cast<int>(power * 255.0 + 0.5);
            pwm = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        }
    }
    // If "fan" object doesn't exist, don't include fan_gear in payload

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename") &&
        status_cache["print_stats"]["filename"].is_string()) {
        payload["print"]["subtask_name"] = status_cache["print_stats"]["filename"].get<std::string>();
    }

    int mc_percent = -1;
    if (status_cache.contains("virtual_sdcard") &&
        status_cache["virtual_sdcard"].contains("progress") &&
        status_cache["virtual_sdcard"]["progress"].is_number()) {
        const double progress = status_cache["virtual_sdcard"]["progress"].get<double>();
        if (progress >= 0.0) {
            mc_percent = std::clamp(static_cast<int>(progress * 100.0 + 0.5), 0, 100);
        }
    }
    if (mc_percent >= 0) {
        payload["print"]["mc_percent"] = mc_percent;
    }

    if (status_cache.contains("print_stats") &&
        status_cache["print_stats"].contains("total_duration") &&
        status_cache["print_stats"].contains("print_duration") &&
        status_cache["print_stats"]["total_duration"].is_number() &&
        status_cache["print_stats"]["print_duration"].is_number()) {
        const double total   = status_cache["print_stats"]["total_duration"].get<double>();
        const double elapsed = status_cache["print_stats"]["print_duration"].get<double>();
        if (total > 0.0 && elapsed >= 0.0) {
            const auto remaining_minutes = std::max(0, static_cast<int>((total - elapsed) / 60.0));
            payload["print"]["mc_remaining_time"] = remaining_minutes;
        }
    }

    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    payload["t_utc"] = now_ms;

    BOOST_LOG_TRIVIAL(trace) << "MoonrakerPrinterAgent: Built payload with gcode_state=" << state;

    return payload;
}

std::string MoonrakerPrinterAgent::resolve_host(const std::string& dev_id) const
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto                        it = host_by_device.find(dev_id);
        if (it != host_by_device.end()) {
            return it->second;
        }
    }

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        return config.base_url;
    }

    return "";
}

std::string MoonrakerPrinterAgent::resolve_api_key(const std::string& dev_id, const std::string& fallback) const
{
    std::string api_key = normalize_api_key(fallback);
    if (!api_key.empty()) {
        return api_key;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto                        it = api_key_by_device.find(dev_id);
        if (it != api_key_by_device.end() && !it->second.empty()) {
            return it->second;
        }
    }

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        return normalize_api_key(config.api_key);
    }

    return "";
}

void MoonrakerPrinterAgent::store_host(const std::string& dev_id, const std::string& host, const std::string& api_key)
{
    if (host.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    host_by_device[dev_id] = host;
    if (!api_key.empty()) {
        api_key_by_device[dev_id] = api_key;
    }
}

void MoonrakerPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_message_fn;
        cloud_fn = on_message_fn;
        queue_fn = queue_on_main_fn;
    }

    auto dispatch = [dev_id, payload, local_fn, cloud_fn]() {
        if (local_fn) {
            local_fn(dev_id, payload);
            return;
        }
        if (cloud_fn) {
            cloud_fn(dev_id, payload);
        }
    };

    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

} // namespace Slic3r
