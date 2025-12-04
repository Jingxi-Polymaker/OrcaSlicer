#include "OrcaNetwork.hpp"
#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/filesystem.hpp>
#include <wx/filename.h>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <thread>

namespace pt = boost::property_tree;

namespace Slic3r {

namespace {
// Default production URLs
constexpr const char* ORCA_DEFAULT_API_URL = "https://api.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_AUTH_URL = "https://auth.orcaslicer.com";
constexpr const char* ORCA_DEFAULT_PUB_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImttYXVqanhlcXJxdW5nb25jcXp2Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTk3ODk4NzAsImV4cCI6MjA3NTM2NTg3MH0.-ChNHK2t0Fbsi8opS2nFse7zxJKpPtvYWqG15sbE908";

// API endpoints
constexpr const char* ORCA_HEALTH_PATH = "/api/v1/health";
constexpr const char* ORCA_LOGOUT_PATH = "/auth/v1/logout";

// Sync Protocol Endpoints (per Orca Cloud Sync Protocol Specification)
constexpr const char* ORCA_SYNC_PULL_PATH = "/api/v1/sync/pull";
constexpr const char* ORCA_SYNC_PUSH_PATH = "/api/v1/sync/push";
constexpr const char* ORCA_PROFILES_PATH = "/api/v1/profiles";
constexpr const char* ORCA_SYNC_STATE_FILE = "sync_state";

// AppConfig keys for URL overrides (developer use)
constexpr const char* CONFIG_ORCA_API_URL = "orca_api_url";
constexpr const char* CONFIG_ORCA_AUTH_URL = "orca_auth_url";
constexpr const char* CONFIG_ORCA_PUB_KEY = "orca_pub_key";

const std::chrono::seconds TOKEN_REFRESH_SKEW{90};

std::map<std::string, std::string> strip_apikey(const std::map<std::string, std::string>& headers)
{
    std::map<std::string, std::string> sanitized;
    for (const auto& pair : headers) {
        std::string key = pair.first;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "apikey") continue;
        sanitized.insert(pair);
    }
    return sanitized;
}
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

OrcaNetwork::OrcaNetwork(std::string log_dir)
    : log_dir(log_dir)
    , api_base_url(ORCA_DEFAULT_API_URL)
    , auth_base_url(ORCA_DEFAULT_AUTH_URL)
    , is_connected(false)
    , enable_track(false)
    , multi_machine_enabled(false)
{
    // Set default pub key - can be overridden via configure_urls()
    auth_headers["apikey"] = ORCA_DEFAULT_PUB_KEY;

    auth_manager = std::make_unique<AuthManager>(auth_base_url);
    auth_manager->set_api_base_url(api_base_url);
    auth_manager->set_extra_headers(auth_headers);
    auth_manager->set_session_handler([this](const std::string& payload) {
        return this->change_user(payload) == BAMBU_NETWORK_SUCCESS;
    });

    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Constructor - log_dir=" << log_dir
                            << ", api_base_url=" << api_base_url
                            << ", auth_base_url=" << auth_base_url;
}

void OrcaNetwork::configure_urls(AppConfig* app_config)
{
    if (!app_config) {
        BOOST_LOG_TRIVIAL(debug) << "OrcaNetwork: configure_urls called with null config, using defaults";
        return;
    }

    bool urls_changed = false;

    // Read API URL override from AppConfig
    std::string api_url = app_config->get(CONFIG_ORCA_API_URL);
    if (!api_url.empty()) {
        api_base_url = api_url;
        urls_changed = true;
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Using custom API URL from config: " << api_base_url;
    }

    // Read Auth URL override from AppConfig
    std::string auth_url = app_config->get(CONFIG_ORCA_AUTH_URL);
    if (!auth_url.empty()) {
        auth_base_url = auth_url;
        urls_changed = true;
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Using custom Auth URL from config: " << auth_base_url;
    }

    // Read Pub Key override from AppConfig
    std::string pub_key = app_config->get(CONFIG_ORCA_PUB_KEY);
    if (!pub_key.empty()) {
        auth_headers["apikey"] = pub_key;
        urls_changed = true;
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Using custom Pub Key from config";
    }

    // Update AuthManager if URLs changed
    if (urls_changed && auth_manager) {
        auth_manager->set_api_base_url(api_base_url);
        auth_manager->set_auth_base_url(auth_base_url);
        auth_manager->set_extra_headers(auth_headers);
    }
}

OrcaNetwork::~OrcaNetwork()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Destructor";
    // Do not force logout here so refresh tokens persist across application restarts.
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

int OrcaNetwork::init_log()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: init_log()";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_config_dir(std::string config_dir)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_config_dir - " << config_dir;
    this->config_dir = config_dir;

    if (auth_manager) {
        auth_manager->set_config_dir(config_dir);
    }

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_cert_file(std::string folder, std::string filename)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_cert_file - folder=" << folder << ", filename=" << filename;
    this->cert_folder = folder;
    this->cert_filename = filename;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_country_code(std::string country_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_country_code - " << country_code;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    this->country_code = country_code;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start()";
    // Initialize TLS using system certificates so HTTPS Orca cloud calls work out of the box.
    if (auto err = Http::tls_global_init(); !err.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: tls_global_init warning - " << err;
    }
    if (auto err = Http::tls_system_cert_store(); !err.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: tls_system_cert_store warning - " << err;
    }

    if (auth_manager) {
        auth_manager->regenerate_pkce();
        // Attempt silent sign-in using stored refresh token
        std::string stored_refresh;
        if (auth_manager->load_refresh_token(stored_refresh)) {
            auth_manager->try_refresh_async(stored_refresh);
        }
    }
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Callback Registration
// ============================================================================

int OrcaNetwork::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_ssdp_msg_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_user_login_fn(OnUserLoginFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_user_login_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_server_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_http_error_fn(OnHttpErrorFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_http_error_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_get_country_code_fn(GetCountryCodeFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    get_country_code_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Server Connectivity
// ============================================================================

int OrcaNetwork::connect_server()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: connect_server() - url=" << api_base_url;

    std::string response;
    unsigned int http_code = 0;

    int result = http_get(ORCA_HEALTH_PATH, &response, &http_code);

    if (result == BAMBU_NETWORK_SUCCESS && http_code == 200) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        is_connected = true;
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Server connected successfully";

        // Invoke callback on main thread
        invoke_server_connected_callback(0, 0);

        return BAMBU_NETWORK_SUCCESS;
    } else {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        is_connected = false;
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Server connection failed - http_code=" << http_code;

        // Invoke callback with error
        invoke_server_connected_callback(-1, http_code);

        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
}

bool OrcaNetwork::is_server_connected()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return is_connected;
}

int OrcaNetwork::refresh_connection()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: refresh_connection()";
    return connect_server();
}

int OrcaNetwork::start_subscribe(std::string module)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_subscribe - module=" << module;
    // Stub implementation
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::stop_subscribe(std::string module)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: stop_subscribe - module=" << module;
    // Stub implementation
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::add_subscribe(std::vector<std::string> dev_list)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: add_subscribe - count=" << dev_list.size();
    // Stub implementation
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::del_subscribe(std::vector<std::string> dev_list)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: del_subscribe - count=" << dev_list.size();
    // Stub implementation
    return BAMBU_NETWORK_SUCCESS;
}

void OrcaNetwork::enable_multi_machine(bool enable)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: enable_multi_machine - " << enable;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    multi_machine_enabled = enable;
}

// ============================================================================
// User Management
// ============================================================================

int OrcaNetwork::set_user_session(std::string token, std::string user_id, std::string username,
                                  std::string name, std::string nickname, std::string avatar,
                                  std::string refresh_token)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_user_session - user_id=" << user_id << ", username=" << username;

    if (!auth_manager) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: auth_manager is null in set_user_session";
        invoke_user_login_callback(0, false);
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    auth_manager->set_user_session(token, user_id, username, name, nickname, avatar, refresh_token);
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Auth login successful - user_id=" << user_id;

    // Initialize per-user sync state path: config_dir/user_id/sync_state
    if (!config_dir.empty() && !user_id.empty()) {
        std::string user_dir = config_dir + "/" + user_id;
        // Create user directory if it doesn't exist
        boost::filesystem::path user_path(user_dir);
        if (!boost::filesystem::exists(user_path)) {
            boost::filesystem::create_directories(user_path);
        }
        sync_state_path = user_dir + "/" + ORCA_SYNC_STATE_FILE;
        load_sync_state();
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Initialized sync state path - " << sync_state_path;
    }

    // Invoke callback
    invoke_user_login_callback(1, true);

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::change_user(std::string user_info)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: change_user invoked";

    try {
        // Parse user_info JSON
        std::stringstream ss(user_info);
        pt::ptree tree;
        pt::read_json(ss, tree);

        auto read_str = [](const pt::ptree& node, const std::string& path) {
            return node.get<std::string>(path, "");
        };

        // Check if this is a WebView login message (PKCE flow completion).
        std::string command = tree.get<std::string>("command", "");
        if (command == "user_login") {
            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Detected WebView login message";

            auto data_opt = tree.get_child_optional("data");
            if (!data_opt) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: WebView login payload missing data field";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            pt::ptree data = *data_opt;
            std::string token = read_str(data, "token");
            std::string user_id = read_str(data, "user_id");
            std::string username = read_str(data, "username");
            std::string name = read_str(data, "name");
            std::string nickname = read_str(data, "nickname");
            std::string avatar = read_str(data, "avatar");
            std::string refresh_token = read_str(data, "refresh_token");
            std::string state = read_str(data, "state");

            if (auth_manager) {
                const auto expected_state = auth_manager->pkce().state;
                if (!expected_state.empty() && state != expected_state) {
                    BOOST_LOG_TRIVIAL(warning) << "[auth] event=login result=failure reason=state_mismatch";
                    invoke_user_login_callback(0, false);
                    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
                }
            }

            if (token.empty() || user_id.empty()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: WebView login - token or user_id empty";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            auto rc = set_user_session(token, user_id, username, name, nickname, avatar, refresh_token);
            BOOST_LOG_TRIVIAL(info) << "[auth] event=login result=" << (rc == BAMBU_NETWORK_SUCCESS ? "success" : "failure")
                                    << " source=webview user_id=" << user_id;
            return rc;
        }

        // Orca cloud session payload (default flow). Accept either data.* or top-level keys.
        const pt::ptree* session_node = nullptr;
        auto data_opt = tree.get_child_optional("data");
        if (data_opt) {
            if (data_opt->get_child_optional("session")) {
                session_node = &data_opt->get_child("session");
            } else if (data_opt->get_optional<std::string>("access_token") ||
                       data_opt->get_optional<std::string>("token")) {
                session_node = &*data_opt;
            }
        }
        if (!session_node) {
            if (tree.get_child_optional("session")) {
                session_node = &tree.get_child("session");
            } else if (tree.get_optional<std::string>("access_token") ||
                       tree.get_optional<std::string>("token")) {
                session_node = &tree;
            }
        }

        if (session_node) {
            std::string access_token = read_str(*session_node, "access_token");
            if (access_token.empty()) {
                access_token = read_str(*session_node, "token");
            }
            std::string refresh_token = read_str(*session_node, "refresh_token");
            std::string user_id = read_str(*session_node, "user.id");
            std::string email = read_str(*session_node, "user.email");
            std::string full_name = read_str(*session_node, "user.user_metadata.full_name");
            std::string preferred_username = read_str(*session_node, "user.user_metadata.preferred_username");
            std::string avatar = read_str(*session_node, "user.user_metadata.avatar_url");
            std::string username = !preferred_username.empty() ? preferred_username : email;
            std::string name = !full_name.empty() ? full_name : (!preferred_username.empty() ? preferred_username : email);
            std::string nickname = !preferred_username.empty() ? preferred_username : username;
            if (nickname.empty()) nickname = name;
            if (nickname.empty()) nickname = email;

            if (access_token.empty() || user_id.empty()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Orca cloud login payload missing access_token or user.id";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Orca cloud login successful - user_id=" << user_id;
            auto rc = set_user_session(access_token, user_id, username, name, nickname, avatar, refresh_token);
            BOOST_LOG_TRIVIAL(info) << "[auth] event=login result=" << (rc == BAMBU_NETWORK_SUCCESS ? "success" : "failure")
                                    << " source=session user_id=" << user_id;
            return rc;
        }

        // Legacy username/password flow is no longer the default; instruct callers to use Orca cloud PKCE.
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: Username/password login is disabled. Use the Orca cloud PKCE flow.";
        invoke_user_login_callback(0, false);
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: change_user exception - " << e.what();
        invoke_user_login_callback(0, false);
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }
}

bool OrcaNetwork::is_user_login()
{
    if (!auth_manager) return false;
    return auth_manager->is_logged_in();
}

int OrcaNetwork::user_logout(bool request)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: user_logout - request=" << request;

    // Check if we need to send logout request (with proper locking)
    bool should_send_request = false;
    std::string refresh_copy;
    if (auth_manager) {
        std::string token = auth_manager->get_access_token();
        refresh_copy = auth_manager->get_refresh_token();
        should_send_request = request && !token.empty();
    }

    if (should_send_request) {
        // Send logout request to backend
        std::string response;
        unsigned int http_code = 0;
        pt::ptree logout_req;
        if (!refresh_copy.empty()) {
            logout_req.put("refresh_token", refresh_copy);
        }
        std::stringstream body_ss;
        if (!logout_req.empty()) {
            pt::write_json(body_ss, logout_req);
        } else {
            body_ss << "{}";
        }

        int result = http_post_auth(ORCA_LOGOUT_PATH, body_ss.str(), &response, &http_code);
        if (result != BAMBU_NETWORK_SUCCESS || http_code >= 400) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: Orca cloud logout request failed - http_code=" << http_code;
        }
    }

    // Clear session
    if (auth_manager) {
        auth_manager->clear_session();
    }

    // Clear per-user sync state
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        sync_state = SyncState{};
        sync_state_path.clear();
    }

    // Invoke callback
    invoke_user_login_callback(0, false);

    BOOST_LOG_TRIVIAL(info) << "[auth] event=logout result=success request=" << request;

    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_user_id()
{
    if (!auth_manager) return "";
    return auth_manager->get_user_id();
}

std::string OrcaNetwork::get_user_name()
{
    if (!auth_manager) return "";
    return auth_manager->get_user_name();
}

std::string OrcaNetwork::get_user_avatar()
{
    if (!auth_manager) return "";
    return auth_manager->get_user_avatar();
}

std::string OrcaNetwork::get_user_nickanme()
{
    if (!auth_manager) return "";
    return auth_manager->get_user_nickname();
}

std::string OrcaNetwork::build_login_cmd()
{
    if (!auth_manager) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: auth_manager is null in build_login_cmd (fatal)";
        return "{}";
    }

    // When already signed in, emit the homepage payload so the web UI
    // can flip to the logged-in state without re-opening the login flow.
    if (auth_manager->is_logged_in()) {
        pt::ptree cmd;
        cmd.put("command", "studio_userlogin");

        pt::ptree data;
        std::string display_name = auth_manager->get_user_nickname();
        if (display_name.empty()) {
            display_name = auth_manager->get_user_name();
        }
        data.put("name", display_name);
        data.put("avatar", auth_manager->get_user_avatar());
        cmd.add_child("data", data);

        std::stringstream ss;
        pt::write_json(ss, cmd, false);
        return ss.str();
    }

    // Build login configuration JSON for WebView
    // WebView handles provider selection (password, Google, Apple, GitHub) internally
    const auto& pkce = auth_manager->pkce();

    pt::ptree cmd;
    cmd.put("action", "login_config");
    cmd.put("backend_url", auth_base_url);

    // Include API key for direct Supabase calls from JavaScript
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        auto it = auth_headers.find("apikey");
        if (it != auth_headers.end()) {
            cmd.put("apikey", it->second);
        }
    }

    // PKCE parameters for OAuth flows
    pt::ptree pkce_node;
    pkce_node.put("code_challenge", pkce.challenge);
    pkce_node.put("code_challenge_method", "S256");
    pkce_node.put("state", pkce.state);
    pkce_node.put("redirect_uri", pkce.redirect);
    pkce_node.put("code_verifier", pkce.verifier);
    pkce_node.put("loopback_port", pkce.loopback_port);

    cmd.add_child("pkce", pkce_node);

    std::stringstream ss;
    pt::write_json(ss, cmd, false);
    return ss.str();
}

std::string OrcaNetwork::build_logout_cmd()
{
    pt::ptree cmd;
    cmd.put("command", "studio_useroffline");
    cmd.put("action", "logout");
    cmd.put("provider", "orca");

    std::stringstream ss;
    pt::write_json(ss, cmd, false);
    return ss.str();
}

std::string OrcaNetwork::build_login_info()
{
    if (!auth_manager) return "{}";
    return auth_manager->build_login_info();
}

// ============================================================================
// Settings Sync
// ============================================================================

int OrcaNetwork::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_user_presets()";

    if (!user_presets) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Save current sync state in case we need to restore on failure
    SyncState saved_state;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        saved_state = sync_state;
    }

    // Clear sync state in memory only (to get full list of profiles)
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        sync_state = SyncState{};
    }

    int result_code = BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;

    sync_pull(
        [&](const SyncPullResponse& response) {
            // Convert sync response to "map[type][setting_id] = json_string"
            for (const auto& upsert : response.upserts) {
                std::string type = "print"; // Default type
                if (upsert.content.contains("type")) {
                    type = upsert.content["type"].get<std::string>();
                }

                (*user_presets)[type][upsert.id] = upsert.content.dump();
            }
            result_code = BAMBU_NETWORK_SUCCESS;
            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Retrieved " << user_presets->size() << " preset types";
        },
        [&](int http_code, const std::string& error) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_user_presets failed - http_code=" << http_code << ", error=" << error;
            result_code = BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
        }
    );

    // If pull failed, restore the previous sync state to preserve conflict protection
    if (result_code != BAMBU_NETWORK_SUCCESS) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        sync_state = saved_state;
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Restored previous sync state after failed pull";
    }

    return result_code;
}

std::string OrcaNetwork::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: request_setting_id - name=" << name;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        if (http_code) *http_code = 401;
        return "";
    }

    // Generate a new UUID for the profile
    boost::uuids::random_generator generator;
    boost::uuids::uuid uuid = generator();
    std::string profile_id = boost::uuids::to_string(uuid);

    // Build content JSON
    nlohmann::json content;
    content["name"] = name;
    content["type"] = "print"; // Default type

    if (values_map && !values_map->empty()) {
        for (const auto& pair : *values_map) {
            // Skip updated_time - it's metadata, not content
            if (pair.first == "updated_time") continue;
            content[pair.first] = pair.second;
        }
    }

    // Use sync_push to create the profile (no original_updated_at for new profiles per spec)
    SyncPushResult result = sync_push(profile_id, content);

    if (http_code) *http_code = result.http_code;

    if (result.success) {
        // Return new_updated_at via values_map so caller can store it in Preset::updated_time
        if (values_map && !result.new_updated_at.empty()) {
            (*values_map)["updated_time"] = result.new_updated_at;
        }
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Created preset - setting_id=" << profile_id
                                << ", new_updated_at=" << result.new_updated_at;
        return profile_id;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: request_setting_id failed - " << result.error_message;
    return "";
}

int OrcaNetwork::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: put_setting - setting_id=" << setting_id << ", name=" << name;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        if (http_code) *http_code = 401;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Extract original_updated_at for Optimistic Concurrency Control (per spec section 5.2)
    // If present, server will verify version before update. If absent, treated as insert.
    std::string original_updated_at;
    if (values_map) {
        auto it = values_map->find("updated_time");
        if (it != values_map->end()) {
            original_updated_at = it->second;
        }
    }

    // Build content JSON
    nlohmann::json content;
    content["name"] = name;

    if (values_map && !values_map->empty()) {
        for (const auto& pair : *values_map) {
            // Skip updated_time - it's used for OCC, not as content
            if (pair.first == "updated_time") continue;
            content[pair.first] = pair.second;
        }
    }

    // Use sync_push to update the profile with OCC
    SyncPushResult result = sync_push(setting_id, content, original_updated_at);

    if (http_code) *http_code = result.http_code;

    if (result.success) {
        // Return new_updated_at via values_map so caller can store it in Preset::updated_time
        if (values_map && !result.new_updated_at.empty()) {
            (*values_map)["updated_time"] = result.new_updated_at;
        }
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Updated preset successfully - new_updated_at=" << result.new_updated_at;
        return BAMBU_NETWORK_SUCCESS;
    }

    // Handle conflict (409) - server has newer version
    if (result.http_code == 409) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: put_setting conflict - server has newer version";
        // Return server's current updated_at so caller can update local state and retry
        if (values_map && !result.server_version.updated_at.empty()) {
            (*values_map)["updated_time"] = result.server_version.updated_at;
        }
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: put_setting failed - " << result.error_message;
    return BAMBU_NETWORK_ERR_PUT_SETTING_FAILED;
}

int OrcaNetwork::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list - bundle_version=" << bundle_version;
    // Simple synchronous version
    return get_setting_list2(bundle_version, nullptr, pro_fn, cancel_fn);
}

int OrcaNetwork::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 - bundle_version=" << bundle_version;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Launch background thread for async operation
    std::thread([this, bundle_version, chk_fn, pro_fn, cancel_fn]() {
        try {
            // Use sync_pull to get changes since last sync
            sync_pull(
                [this, chk_fn, pro_fn, cancel_fn](const SyncPullResponse& response) {
                    int total = static_cast<int>(response.upserts.size() + response.deletes.size());
                    int index = 0;

                    // Process upserts
                    for (const auto& upsert : response.upserts) {
                        // Check cancellation
                        if (cancel_fn && cancel_fn()) {
                            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 cancelled";
                            return;
                        }

                        // Convert profile to map for CheckFn
                        if (chk_fn) {
                            std::map<std::string, std::string> preset_info;
                            preset_info["setting_id"] = upsert.id;
                            preset_info["updated_at"] = upsert.updated_at;

                            // Flatten JSON content to map
                            if (upsert.content.is_object()) {
                                for (auto& [key, value] : upsert.content.items()) {
                                    if (value.is_string()) {
                                        preset_info[key] = value.get<std::string>();
                                    } else {
                                        preset_info[key] = value.dump();
                                    }
                                }
                            }

                            // Invoke check function on main thread
                            QueueOnMainFn queue_fn;
                            {
                                std::lock_guard<std::recursive_mutex> lock(state_mutex);
                                queue_fn = queue_on_main_fn;
                            }

                            if (queue_fn) {
                                queue_fn([chk_fn, preset_info]() {
                                    chk_fn(preset_info);
                                });
                            } else {
                                chk_fn(preset_info);
                            }
                        }

                        // Report progress
                        if (pro_fn) {
                            int progress = total > 0 ? (index * 100 / total) : 100;

                            QueueOnMainFn queue_fn;
                            {
                                std::lock_guard<std::recursive_mutex> lock(state_mutex);
                                queue_fn = queue_on_main_fn;
                            }

                            if (queue_fn) {
                                queue_fn([pro_fn, progress]() {
                                    pro_fn(progress);
                                });
                            } else {
                                pro_fn(progress);
                            }
                        }

                        index++;
                    }

                    // Process deletes (tombstones)
                    for (const auto& deleted_id : response.deletes) {
                        if (cancel_fn && cancel_fn()) {
                            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 cancelled";
                            return;
                        }

                        // Notify via CheckFn with a special marker for deletion
                        if (chk_fn) {
                            std::map<std::string, std::string> preset_info;
                            preset_info["setting_id"] = deleted_id;
                            preset_info["deleted"] = "true";

                            QueueOnMainFn queue_fn;
                            {
                                std::lock_guard<std::recursive_mutex> lock(state_mutex);
                                queue_fn = queue_on_main_fn;
                            }

                            if (queue_fn) {
                                queue_fn([chk_fn, preset_info]() {
                                    chk_fn(preset_info);
                                });
                            } else {
                                chk_fn(preset_info);
                            }
                        }

                        index++;
                    }

                    // Final progress
                    if (pro_fn) {
                        QueueOnMainFn queue_fn;
                        {
                            std::lock_guard<std::recursive_mutex> lock(state_mutex);
                            queue_fn = queue_on_main_fn;
                        }

                        if (queue_fn) {
                            queue_fn([pro_fn]() {
                                pro_fn(100);
                            });
                        } else {
                            pro_fn(100);
                        }
                    }

                    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 completed - upserts="
                                            << response.upserts.size() << ", deletes=" << response.deletes.size();
                },
                [](int http_code, const std::string& error) {
                    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_setting_list2 sync_pull failed - http_code="
                                             << http_code << ", error=" << error;
                }
            );

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_setting_list2 exception - " << e.what();
        }
    }).detach();

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::delete_setting(std::string setting_id)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: delete_setting - setting_id=" << setting_id;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    std::string response;
    unsigned int http_code = 0;

    // Use new profiles endpoint for deletion
    std::string path = std::string(ORCA_PROFILES_PATH) + "/" + setting_id;
    int result = http_delete(path, &response, &http_code);

    if (result == BAMBU_NETWORK_SUCCESS && (http_code == 200 || http_code == 204)) {
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Deleted preset successfully";
        // Note: Preset's .info file will be deleted when the preset file is removed
        return BAMBU_NETWORK_SUCCESS;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: delete_setting failed - http_code=" << http_code;
    return BAMBU_NETWORK_ERR_DEL_SETTING_FAILED;
}

// ============================================================================
// Sync State Persistence
// ============================================================================

void OrcaNetwork::load_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    if (sync_state_path.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: sync_state_path is empty, cannot load sync state";
        return;
    }

    try {
        std::ifstream file(sync_state_path);
        if (!file.is_open()) {
            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: No sync state file found, starting fresh";
            return;
        }

        // Read global cursor
        std::getline(file, sync_state.last_sync_timestamp);

        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Loaded sync state - cursor=" << sync_state.last_sync_timestamp;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Failed to load sync state - " << e.what();
        // Reset to clean state on error
        sync_state = SyncState{};
    }
}

void OrcaNetwork::save_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    if (sync_state_path.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: sync_state_path is empty, cannot save sync state";
        return;
    }

    try {
        // Write to temp file first, then rename for atomic write
        std::string temp_path = sync_state_path + ".tmp";
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Failed to open sync state file for writing";
            return;
        }

        // Write global cursor
        file << sync_state.last_sync_timestamp;
        file.close();

        std::error_code ec = rename_file(temp_path, sync_state_path);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Failed to rename sync state file - " << ec.message();
            std::remove(temp_path.c_str());
            return;
        }

        BOOST_LOG_TRIVIAL(debug) << "OrcaNetwork: Saved sync state - cursor=" << sync_state.last_sync_timestamp;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Failed to save sync state - " << e.what();
    }
}

void OrcaNetwork::clear_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    sync_state = SyncState{};
    save_sync_state();
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Cleared sync state";
}

// ============================================================================
// New Sync Protocol Implementation
// ============================================================================

int OrcaNetwork::sync_pull(
    std::function<void(const SyncPullResponse&)> on_success,
    std::function<void(int http_code, const std::string& error)> on_error)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: sync_pull()";

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        if (on_error) on_error(401, "Not logged in");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Build pull URL with cursor
    std::string cursor;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        cursor = sync_state.last_sync_timestamp;
    }

    std::string path = std::string(ORCA_SYNC_PULL_PATH);
    if (!cursor.empty()) {
        path += "?cursor=" + cursor;
    }

    std::string response;
    unsigned int http_code = 0;

    int result = http_get(path, &response, &http_code);

    // Handle 410 Gone - cursor too old, need full resync
    if (http_code == 410) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: sync_pull returned 410 Gone - cursor too old, triggering full resync";
        clear_sync_state();
        // Retry without cursor
        path = std::string(ORCA_SYNC_PULL_PATH);
        result = http_get(path, &response, &http_code);
    }

    if (result != BAMBU_NETWORK_SUCCESS || (http_code != 200 && http_code != 304)) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_pull failed - http_code=" << http_code;
        if (on_error) on_error(http_code, response);
        return BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
    }

    // 304 Not Modified - no changes
    if (http_code == 304) {
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: sync_pull - no changes (304)";
        if (on_success) {
            SyncPullResponse empty_response;
            on_success(empty_response);
        }
        return BAMBU_NETWORK_SUCCESS;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);

        SyncPullResponse pull_response;
        pull_response.next_cursor = j.value("next_cursor", "");

        // Parse upserts
        if (j.contains("upserts") && j["upserts"].is_array()) {
            for (const auto& item : j["upserts"]) {
                ProfileUpsert upsert;
                upsert.id = item.value("id", "");
                upsert.updated_at = item.value("updated_at", "");
                if (item.contains("content")) {
                    upsert.content = item["content"];
                }
                pull_response.upserts.push_back(std::move(upsert));
            }
        }

        // Parse deletes (tombstones)
        if (j.contains("deletes") && j["deletes"].is_array()) {
            for (const auto& item : j["deletes"]) {
                if (item.is_string()) {
                    pull_response.deletes.push_back(item.get<std::string>());
                }
            }
        }

        // Update global sync cursor
        // Note: Per-profile timestamps are stored in .info files as Preset::updated_time
        // The caller should store upsert.updated_at in Preset::updated_time as-is
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (!pull_response.next_cursor.empty()) {
                sync_state.last_sync_timestamp = pull_response.next_cursor;
            }
            save_sync_state();
        }

        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: sync_pull completed - upserts=" << pull_response.upserts.size()
                                << ", deletes=" << pull_response.deletes.size()
                                << ", next_cursor=" << pull_response.next_cursor;

        if (on_success) on_success(pull_response);
        return BAMBU_NETWORK_SUCCESS;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_pull parse error - " << e.what();
        if (on_error) on_error(500, std::string("Parse error: ") + e.what());
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }
}

SyncPushResult OrcaNetwork::sync_push(
    const std::string& profile_id,
    const nlohmann::json& content,
    const std::string& original_updated_at)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: sync_push - profile_id=" << profile_id
                            << ", original_updated_at=" << (original_updated_at.empty() ? "(new)" : original_updated_at);

    SyncPushResult result;
    result.success = false;
    result.http_code = 0;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        result.http_code = 401;
        result.error_message = "Not logged in";
        return result;
    }

    // original_updated_at is passed from Preset::updated_time directly for optimistic concurrency control

    try {
        // Build request body per spec
        nlohmann::json request_body;
        request_body["id"] = profile_id;
        request_body["content"] = content;
        if (!original_updated_at.empty()) {
            request_body["original_updated_at"] = original_updated_at;
        }

        std::string response;
        unsigned int http_code = 0;

        int net_result = http_post(ORCA_SYNC_PUSH_PATH, request_body.dump(), &response, &http_code);
        result.http_code = http_code;

        if (net_result == BAMBU_NETWORK_SUCCESS && http_code == 200) {
            // Success - parse new timestamp from server
            try {
                nlohmann::json resp_json = nlohmann::json::parse(response);
                result.new_updated_at = resp_json.value("new_updated_at", resp_json.value("updated_at", ""));

                if (!result.new_updated_at.empty()) {
                    // Caller must store result.new_updated_at in Preset::updated_time as-is
                    result.success = true;
                    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: sync_push successful - new_updated_at=" << result.new_updated_at;
                } else {
                    // Server returned 200 but no timestamp - cannot maintain optimistic concurrency
                    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_push failed - server returned 200 but no timestamp in response";
                    result.success = false;
                    result.error_message = "Server response missing required timestamp";
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_push failed - response parse error: " << e.what();
                result.success = false;
                result.error_message = std::string("Response parse error: ") + e.what();
            }
            return result;
        }

        if (http_code == 409) {
            // Conflict - parse server version
            BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: sync_push conflict (409) - server has newer version";
            result.success = false;
            result.error_message = "Conflict: server has newer version";

            try {
                nlohmann::json resp_json = nlohmann::json::parse(response);
                result.server_version.id = resp_json.value("id", profile_id);
                result.server_version.updated_at = resp_json.value("updated_at", "");
                if (resp_json.contains("content")) {
                    result.server_version.content = resp_json["content"];
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_push conflict response parse error - " << e.what();
            }
            return result;
        }

        // Other error
        result.error_message = "HTTP error: " + std::to_string(http_code);
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_push failed - http_code=" << http_code;
        return result;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: sync_push exception - " << e.what();
        result.error_message = std::string("Exception: ") + e.what();
        return result;
    }
}

// ============================================================================
// Extra Features
// ============================================================================

int OrcaNetwork::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_extra_http_header - count=" << extra_headers.size();
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    this->extra_headers.clear();

    for (const auto& pair : extra_headers) {
        std::string key_lower = pair.first;
        std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);
        if (key_lower == "apikey") {
            auth_headers["apikey"] = pair.second;
            continue;
        }
        this->extra_headers.insert(pair);
    }

    if (auth_manager) {
        auth_manager->set_extra_headers(auth_headers);
    }
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_studio_info_url()
{
    return api_base_url + "/v1/studio/info";
}

// ============================================================================
// HTTP Request Helpers
// ============================================================================

std::map<std::string, std::string> OrcaNetwork::data_headers()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return strip_apikey(extra_headers);
}

bool OrcaNetwork::ensure_token_fresh(const std::string& reason)
{
    if (!auth_manager) return true;
    return auth_manager->refresh_if_expiring(TOKEN_REFRESH_SKEW, reason);
}

bool OrcaNetwork::attempt_refresh_after_unauthorized(const std::string& reason)
{
    if (!auth_manager) return false;
    if (auth_manager->refresh_from_storage(reason, false)) return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (auth_manager->refresh_from_storage(reason + "_retry", false)) return true;

    BOOST_LOG_TRIVIAL(warning) << "[auth] event=refresh result=failure source=" << reason << " action=logout";
    auth_manager->clear_session();
    invoke_user_login_callback(0, false);
    return false;
}

int OrcaNetwork::http_get(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: GET " << url;
    const bool disable_cache = path.find("/sync") != std::string::npos;

    ensure_token_fresh(std::string("http_get_pre") + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::get(url);

            std::string token;
            if (auth_manager) {
                token = auth_manager->get_access_token();
            }

            auto headers_copy = data_headers();

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }
            for (const auto& pair : headers_copy) {
                http.header(pair.first, pair.second);
            }
            if (disable_cache) {
                http.header("Cache-Control", "no-store");
            }

            http.on_complete([&](std::string resp_body, unsigned resp_status) {
                result.success = true;
                result.status = resp_status;
                result.body = resp_body;
            })
            .on_error([&](std::string resp_body, std::string error, unsigned resp_status) {
                result.success = false;
                result.status = resp_status;
                result.body = resp_body;
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
            })
            .timeout_max(30) // 30 second timeout
            .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_get exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_get_401")) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (res.success && res.status >= 200 && res.status < 300) {
        return BAMBU_NETWORK_SUCCESS;
    }

    invoke_http_error_callback(res.status, res.body);
    return BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaNetwork::http_post(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: POST " << url;

    ensure_token_fresh(std::string("http_post_pre") + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::post(url);

            std::string token;
            if (auth_manager && path != ORCA_TOKEN_PATH) {
                token = auth_manager->get_access_token();
            }

            auto headers_copy = data_headers();

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            for (const auto& pair : headers_copy) {
                http.header(pair.first, pair.second);
            }

            http.header("Content-Type", "application/json");
            http.set_post_body(body);

            http.on_complete([&](std::string b, unsigned resp_status) {
                result.success = true;
                result.status = resp_status;
                result.body = b;
            })
            .on_error([&](std::string b, std::string error, unsigned resp_status) {
                result.success = false;
                result.status = resp_status;
                result.body = b;
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_post exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_post_401")) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (res.success && res.status >= 200 && res.status < 300) {
        return BAMBU_NETWORK_SUCCESS;
    }

    invoke_http_error_callback(res.status, res.body);
    return BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaNetwork::http_post_auth(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = auth_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: POST (auth) " << url;

    try {
        auto http = Http::post(url);

        std::string token;
        if (auth_manager && path != ORCA_TOKEN_PATH) {
            token = auth_manager->get_access_token();
        }

        std::map<std::string, std::string> headers_copy;
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            headers_copy = auth_headers;
        }

        if (!token.empty()) {
            http.header("Authorization", "Bearer " + token);
        }

        for (const auto& pair : headers_copy) {
            http.header(pair.first, pair.second);
        }

        http.header("Content-Type", "application/json");
        http.set_post_body(body);

        bool success = false;
        unsigned int status = 0;
        std::string resp_body;

        http.on_complete([&](std::string body, unsigned resp_status) {
            success = true;
            status = resp_status;
            resp_body = body;
        })
        .on_error([&](std::string body, std::string error, unsigned resp_status) {
            success = false;
            status = resp_status;
            resp_body = body;
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP auth error - " << error;
        })
        .timeout_max(30)
        .perform_sync();

        if (response_body) *response_body = resp_body;
        if (http_code) *http_code = status;

        if (success && status >= 200 && status < 300) {
            return BAMBU_NETWORK_SUCCESS;
        } else {
            invoke_http_error_callback(status, resp_body);
            return BAMBU_NETWORK_ERR_CONNECT_FAILED;
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_post_auth exception - " << e.what();
        if (http_code) *http_code = 0;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
}

int OrcaNetwork::http_put(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: PUT " << url;

    ensure_token_fresh(std::string("http_put_pre") + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::put(url);

            std::string token;
            if (auth_manager) {
                token = auth_manager->get_access_token();
            }

            auto headers_copy = data_headers();

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            for (const auto& pair : headers_copy) {
                http.header(pair.first, pair.second);
            }

            http.header("Content-Type", "application/json");
            http.set_post_body(body); // Note: set_post_body works for PUT too

            http.on_complete([&](std::string b, unsigned resp_status) {
                result.success = true;
                result.status = resp_status;
                result.body = b;
            })
            .on_error([&](std::string b, std::string error, unsigned resp_status) {
                result.success = false;
                result.status = resp_status;
                result.body = b;
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_put exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_put_401")) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (res.success && res.status >= 200 && res.status < 300) {
        return BAMBU_NETWORK_SUCCESS;
    }

    invoke_http_error_callback(res.status, res.body);
    return BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

int OrcaNetwork::http_delete(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = api_base_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: DELETE " << url;

    ensure_token_fresh(std::string("http_delete_pre") + path);

    struct HttpResult {
        bool success{false};
        unsigned int status{0};
        std::string body;
    };

    auto perform = [&]() {
        HttpResult result;
        try {
            auto http = Http::del(url);

            std::string token;
            if (auth_manager) {
                token = auth_manager->get_access_token();
            }

            auto headers_copy = data_headers();

            if (!token.empty()) {
                http.header("Authorization", "Bearer " + token);
            }

            for (const auto& pair : headers_copy) {
                http.header(pair.first, pair.second);
            }

            http.on_complete([&](std::string b, unsigned resp_status) {
                result.success = true;
                result.status = resp_status;
                result.body = b;
            })
            .on_error([&](std::string b, std::string error, unsigned resp_status) {
                result.success = false;
                result.status = resp_status;
                result.body = b;
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_delete exception - " << e.what();
        }
        return result;
    };

    HttpResult res = perform();
    if (res.status == 401 && attempt_refresh_after_unauthorized("http_delete_401")) {
        res = perform();
    }

    if (response_body) *response_body = res.body;
    if (http_code) *http_code = res.status;

    if (res.success && res.status >= 200 && res.status < 300) {
        return BAMBU_NETWORK_SUCCESS;
    }

    invoke_http_error_callback(res.status, res.body);
    return BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

// ============================================================================
// Callback Invocation Helpers
// ============================================================================

void OrcaNetwork::invoke_user_login_callback(int online_login, bool login)
{
OnUserLoginFn callback;
QueueOnMainFn queue_fn;

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

void OrcaNetwork::invoke_server_connected_callback(int return_code, int reason_code)
{
OnServerConnectedFn callback;
QueueOnMainFn queue_fn;

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_server_connected_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, return_code, reason_code]() {
                callback(return_code, reason_code);
            });
        } else {
            callback(return_code, reason_code);
        }
    }
}

void OrcaNetwork::invoke_http_error_callback(unsigned http_code, const std::string& http_body)
{
OnHttpErrorFn callback;
QueueOnMainFn queue_fn;

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        callback = on_http_error_fn;
        queue_fn = queue_on_main_fn;
    }

    if (callback) {
        if (queue_fn) {
            queue_fn([callback, http_code, http_body]() {
                callback(http_code, http_body);
            });
        } else {
            callback(http_code, http_body);
        }
    }
}

// ============================================================================
// Dummy Printer Operations (Stubs)
// ============================================================================

int OrcaNetwork::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: send_message (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: connect_printer (stub) - dev_id=" << dev_id << ", ip=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::disconnect_printer()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: disconnect_printer (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: send_message_to_printer (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::check_cert()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: check_cert (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

void OrcaNetwork::install_device_cert(std::string dev_id, bool lan_only)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: install_device_cert (stub) - dev_id=" << dev_id;
}

bool OrcaNetwork::start_discovery(bool start, bool sending)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_discovery (stub) - start=" << start;
    return true;
}

int OrcaNetwork::ping_bind(std::string ping_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: ping_bind (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: bind_detect (stub) - dev_ip=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: bind (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::unbind(std::string dev_id)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: unbind (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_bambulab_host()
{
    return api_base_url;
}

std::string OrcaNetwork::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return selected_machine;
}

int OrcaNetwork::set_user_selected_machine(std::string dev_id)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_user_selected_machine - dev_id=" << dev_id;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    selected_machine = dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_print (stub) - dev_id=" << params.dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_local_print_with_record (stub) - dev_id=" << params.dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_send_gcode_to_sdcard (stub) - dev_id=" << params.dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_local_print (stub) - dev_id=" << params.dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_sdcard_print (stub) - dev_id=" << params.dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_my_message (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::check_user_task_report(int* task_id, bool* printable)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: check_user_task_report (stub)";
    if (task_id) *task_id = 0;
    if (printable) *printable = false;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_user_print_info (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_user_tasks (stub)";
    if (http_body) *http_body = "[]";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_printer_firmware (stub) - dev_id=" << dev_id;
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_task_plate_index(std::string task_id, int* plate_index)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_task_plate_index (stub) - task_id=" << task_id;
    if (plate_index) *plate_index = 0;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_user_info(int* identifier)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_user_info (stub)";
    if (identifier) *identifier = 0;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::request_bind_ticket(std::string* ticket)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: request_bind_ticket (stub)";
    if (ticket) *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_subtask_info (stub) - subtask_id=" << subtask_id;
    if (task_json) *task_json = "{}";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_slice_info (stub) - project_id=" << project_id;
    if (slice_json) *slice_json = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: query_bind_status (stub) - count=" << query_list.size();
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::modify_printer_name(std::string dev_id, std::string dev_name)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: modify_printer_name (stub) - dev_id=" << dev_id << ", name=" << dev_name;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_camera_url (stub) - dev_id=" << dev_id;
    if (callback) {
        callback("");
    }
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_design_staffpick (stub)";
    if (callback) {
        callback("[]");
    }
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: start_publish (stub)";
    if (out) *out = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_model_publish_url(std::string* url)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_model_publish_url (stub)";
    if (url) *url = api_base_url + "/v1/publish";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_subtask (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_model_mall_home_url(std::string* url)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_model_mall_home_url (stub)";
    if (url) *url = api_base_url + "/v1/mall";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_model_mall_detail_url(std::string* url, std::string id)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_model_mall_detail_url (stub) - id=" << id;
    if (url) *url = api_base_url + "/v1/mall/" + id;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_my_profile (stub)";
    if (http_code) *http_code = 200;
    if (http_body) *http_body = "{}";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_enable(bool enable)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_enable - " << enable;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    enable_track = enable;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_remove_files()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_remove_files (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_event(std::string evt_key, std::string content)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_event (stub) - key=" << evt_key;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_header(std::string header)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_header (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_update_property(std::string name, std::string value, std::string type)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_update_property (stub) - name=" << name;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::track_get_property(std::string name, std::string& value, std::string type)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: track_get_property (stub) - name=" << name;
    value = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: put_model_mall_rating (stub) - design_id=" << design_id;
    http_code = 200;
    http_error = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_oss_config (stub)";
    config = "{}";
    http_code = 200;
    http_error = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: put_rating_picture_oss (stub) - model_id=" << model_id;
    http_code = 200;
    http_error = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_model_mall_rating_result (stub) - job_id=" << job_id;
    rating_result = "{}";
    http_code = 200;
    http_error = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_mw_user_preference(std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_mw_user_preference (stub)";
    if (callback) {
        callback("{}");
    }
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_mw_user_4ulist (stub) - seed=" << seed << ", limit=" << limit;
    if (callback) {
        callback("[]");
    }
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_version()
{
    // Return version identifier for OrcaNetwork implementation
    // This allows version tracking even when bambu_networking library is not loaded
    return "orca_network";
}

} // namespace Slic3r
