#include "OrcaNetwork.hpp"
#include "Http.hpp"
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <wx/filename.h>
#include <sstream>
#include <cstdlib>

namespace pt = boost::property_tree;

namespace Slic3r {

namespace {
constexpr const char* ORCA_DEFAULT_BACKEND_URL = "https://auth.orcaslicer.com";
constexpr const char* ORCA_HEALTH_PATH = "/auth/v1/health";
constexpr const char* ORCA_LOGOUT_PATH = "/auth/v1/logout";
constexpr const char* ENV_ORCA_BACKEND_URL = "ORCA_DEFAULT_BACKEND_URL";
constexpr const char* ENV_ORCA_BACKEND_ANON_KEY = "ORCA_BACKEND_ANON_KEY";
constexpr const char* ENV_BACKEND_OVERRIDE = "ORCA_BACKEND_URL";
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

OrcaNetwork::OrcaNetwork(std::string log_dir)
    : log_dir(log_dir)
    , backend_url(ORCA_DEFAULT_BACKEND_URL)
    , is_connected(false)
    , is_logged_in(false)
    , enable_track(false)
    , multi_machine_enabled(false)
{
    const char* override_url = std::getenv(ENV_BACKEND_OVERRIDE);
    const char* orca_backend_url = std::getenv(ENV_ORCA_BACKEND_URL);
    if (override_url && *override_url) {
        backend_url = override_url;
    } else if (orca_backend_url && *orca_backend_url) {
        backend_url = orca_backend_url;
    }

    if (const char* anon_key = std::getenv(ENV_ORCA_BACKEND_ANON_KEY)) {
        if (*anon_key != '\0') {
            extra_headers["apikey"] = anon_key;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: ORCA_BACKEND_ANON_KEY is empty; Orca cloud requests may fail";
        }
    } else {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: ORCA_BACKEND_ANON_KEY not set; Orca cloud requests may fail";
    }

    auth_manager = std::make_unique<AuthManager>(backend_url);
    auth_manager->set_extra_headers(extra_headers);
    auth_manager->set_session_handler([this](const std::string& payload) {
        return this->change_user(payload) == BAMBU_NETWORK_SUCCESS;
    });

    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Constructor - log_dir=" << log_dir
                            << ", backend_url=" << backend_url;
}

OrcaNetwork::~OrcaNetwork()
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Destructor";

    // Logout if logged in
    if (is_logged_in) {
        user_logout(true);
    }
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
    }

    // Attempt silent sign-in using stored refresh token
    std::string stored_refresh;
    if (auth_manager && auth_manager->load_refresh_token(stored_refresh)) {
        auth_manager->try_refresh_async(stored_refresh);
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
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: connect_server() - url=" << backend_url;

    std::string response;
    unsigned int http_code = 0;

    int result = http_get(ORCA_HEALTH_PATH, &response, &http_code);

    if (!(result == BAMBU_NETWORK_SUCCESS && http_code == 200)) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: Orca cloud health check failed (http_code=" << http_code
                                   << "), falling back to legacy /api/v1/health";
        result = http_get("/api/v1/health", &response, &http_code);
    }

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

    // Set session data directly (already authenticated via WebView)
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        session_token = token;
        this->refresh_token = refresh_token;
        this->user_id = user_id;
        this->user_name = name;
        this->user_nickname = nickname;
        this->user_avatar = avatar;
        is_logged_in = true;
    }

    // Persist refresh token securely for silent re-auth
    if (auth_manager) {
        auth_manager->persist_refresh_token(refresh_token);
    }

    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Auth login successful - user_id=" << user_id;

    // Invoke callback
    invoke_user_login_callback(1, true);

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::change_user(std::string user_info)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: change_user - user_info=" << user_info;

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

            if (token.empty() || user_id.empty()) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: WebView login - token or user_id empty";
                invoke_user_login_callback(0, false);
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }

            return set_user_session(token, user_id, username, name, nickname, avatar, refresh_token);
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
            return set_user_session(access_token, user_id, username, name, nickname, avatar, refresh_token);
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
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return is_logged_in;
}

int OrcaNetwork::user_logout(bool request)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: user_logout - request=" << request;

    // Check if we need to send logout request (with proper locking)
    bool should_send_request = false;
    std::string refresh_copy;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        should_send_request = request && !session_token.empty();
        refresh_copy = refresh_token;
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

        int result = http_post(ORCA_LOGOUT_PATH, body_ss.str(), &response, &http_code);
        if (result != BAMBU_NETWORK_SUCCESS || http_code >= 400) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaNetwork: Orca cloud logout request failed - http_code=" << http_code;
        }
    }

    // Clear session
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        session_token.clear();
        refresh_token.clear();
        user_id.clear();
        user_name.clear();
        user_nickname.clear();
        user_avatar.clear();
        is_logged_in = false;
    }

    if (auth_manager) {
        auth_manager->clear_refresh_token();
    }

    // Invoke callback
    invoke_user_login_callback(0, false);

    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_user_id()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return user_id;
}

std::string OrcaNetwork::get_user_name()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return user_name;
}

std::string OrcaNetwork::get_user_avatar()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return user_avatar;
}

std::string OrcaNetwork::get_user_nickanme()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return user_nickname;
}

std::string OrcaNetwork::build_login_cmd()
{
    if (!auth_manager) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: auth_manager is null in build_login_cmd (fatal)";
        return "{}";
    }
    auth_manager->regenerate_pkce();
    AuthManager::PkceBundle pkce_bundle;
    pkce_bundle = auth_manager->pkce();

    pt::ptree tree;
    tree.put("action", "login");
    tree.put("provider", "orca");
    tree.put("backend_url", backend_url);

    pt::ptree pkce_node;
    pkce_node.put("code_challenge", pkce_bundle.challenge);
    pkce_node.put("code_challenge_method", "S256");
    pkce_node.put("state", pkce_bundle.state);
    pkce_node.put("redirect_uri", pkce_bundle.redirect);
    pkce_node.put("code_verifier", pkce_bundle.verifier); // kept in-process; used by embedded login helper
    pkce_node.put("loopback_port", pkce_bundle.loopback_port);
    tree.add_child("pkce", pkce_node);

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

std::string OrcaNetwork::build_logout_cmd()
{
    pt::ptree tree;
    tree.put("action", "logout");
    tree.put("provider", "orca");

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

std::string OrcaNetwork::build_login_info()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);

    pt::ptree tree;
    tree.put("user_id", user_id);
    tree.put("user_name", user_name);
    tree.put("nickname", user_nickname);
    tree.put("avatar", user_avatar);
    tree.put("logged_in", is_logged_in);
    // Do not expose tokens to WebView to avoid leaking credentials to remote content.
    tree.put("access_token", "");
    tree.put("refresh_token", "");
    tree.put("backend_url", backend_url);

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
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

    std::string response;
    unsigned int http_code = 0;

    int result = http_get("/api/v1/presets", &response, &http_code);

    if (result != BAMBU_NETWORK_SUCCESS || http_code != 200) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_user_presets failed - http_code=" << http_code;
        return BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
    }

    try {
        // Parse response
        std::stringstream ss(response);
        pt::ptree tree;
        pt::read_json(ss, tree);

        bool success = tree.get<bool>("success", false);
        if (!success) {
            return BAMBU_NETWORK_ERR_GET_SETTING_LIST_FAILED;
        }

        // Parse presets: map[type][setting_id] = json_string
        pt::ptree presets_tree = tree.get_child("presets");
        for (const auto& type_pair : presets_tree) {
            std::string type = type_pair.first;

            for (const auto& preset_pair : type_pair.second) {
                std::string setting_id = preset_pair.first;

                // Convert preset ptree to JSON string
                std::stringstream preset_ss;
                pt::write_json(preset_ss, preset_pair.second);

                (*user_presets)[type][setting_id] = preset_ss.str();
            }
        }

        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Retrieved " << user_presets->size() << " preset types";
        return BAMBU_NETWORK_SUCCESS;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_user_presets parse error - " << e.what();
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }
}

std::string OrcaNetwork::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: request_setting_id - name=" << name;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        if (http_code) *http_code = 401;
        return "";
    }

    try {
        // Build request
        pt::ptree req;
        req.put("name", name);
        req.put("type", "print"); // Default type

        // Add values
        if (values_map && !values_map->empty()) {
            pt::ptree values_tree;
            for (const auto& pair : *values_map) {
                values_tree.put(pair.first, pair.second);
            }
            req.add_child("values", values_tree);
        }

        std::stringstream req_ss;
        pt::write_json(req_ss, req);

        std::string response;
        unsigned int code = 0;

        int result = http_post("/api/v1/presets", req_ss.str(), &response, &code);
        if (http_code) *http_code = code;

        if (result == BAMBU_NETWORK_SUCCESS && code == 201) {
            // Parse response
            std::stringstream resp_ss(response);
            pt::ptree resp_tree;
            pt::read_json(resp_ss, resp_tree);

            std::string setting_id = resp_tree.get<std::string>("setting_id", "");
            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Created preset - setting_id=" << setting_id;
            return setting_id;
        }

        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: request_setting_id failed - http_code=" << code;
        return "";

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: request_setting_id exception - " << e.what();
        if (http_code) *http_code = 500;
        return "";
    }
}

int OrcaNetwork::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: put_setting - setting_id=" << setting_id << ", name=" << name;

    if (!is_user_login()) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: Not logged in";
        if (http_code) *http_code = 401;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    try {
        // Build request
        pt::ptree req;
        req.put("name", name);

        // Add values
        if (values_map && !values_map->empty()) {
            pt::ptree values_tree;
            for (const auto& pair : *values_map) {
                values_tree.put(pair.first, pair.second);
            }
            req.add_child("values", values_tree);
        }

        std::stringstream req_ss;
        pt::write_json(req_ss, req);

        std::string response;
        unsigned int code = 0;

        std::string path = "/api/v1/presets/" + setting_id;
        int result = http_put(path, req_ss.str(), &response, &code);
        if (http_code) *http_code = code;

        if (result == BAMBU_NETWORK_SUCCESS && code == 200) {
            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Updated preset successfully";
            return BAMBU_NETWORK_SUCCESS;
        }

        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: put_setting failed - http_code=" << code;
        return BAMBU_NETWORK_ERR_PUT_SETTING_FAILED;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: put_setting exception - " << e.what();
        if (http_code) *http_code = 500;
        return BAMBU_NETWORK_ERR_PUT_SETTING_FAILED;
    }
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
            std::string response;
            unsigned int http_code = 0;

            std::string path = "/api/v1/presets/sync?bundle_version=" + bundle_version;
            int result = http_get(path, &response, &http_code);

            if (result != BAMBU_NETWORK_SUCCESS || http_code != 200) {
                BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: get_setting_list2 failed - http_code=" << http_code;
                return;
            }

            // Parse response
            std::stringstream ss(response);
            pt::ptree tree;
            pt::read_json(ss, tree);

            bool success = tree.get<bool>("success", false);
            if (!success) {
                return;
            }

            int total = tree.get<int>("total", 0);
            pt::ptree presets_tree = tree.get_child("presets");

            int index = 0;
            for (const auto& preset_pair : presets_tree) {
                // Check cancellation
                if (cancel_fn && cancel_fn()) {
                    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 cancelled";
                    break;
                }

                // Convert preset to map for CheckFn
                if (chk_fn) {
                    std::map<std::string, std::string> preset_info;
                    for (const auto& field : preset_pair.second) {
                        preset_info[field.first] = field.second.get_value<std::string>();
                    }

                    // Invoke check function
                    if (queue_on_main_fn) {
                        queue_on_main_fn([chk_fn, preset_info]() {
                            chk_fn(preset_info);
                        });
                    } else {
                        chk_fn(preset_info);
                    }
                }

                // Report progress
                if (pro_fn) {
                    int progress = total > 0 ? (index * 100 / total) : 100;

                    if (queue_on_main_fn) {
                        queue_on_main_fn([pro_fn, progress]() {
                            pro_fn(progress);
                        });
                    } else {
                        pro_fn(progress);
                    }
                }

                index++;
            }

            // Final progress
            if (pro_fn) {
                if (queue_on_main_fn) {
                    queue_on_main_fn([pro_fn]() {
                        pro_fn(100);
                    });
                } else {
                    pro_fn(100);
                }
            }

            BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_setting_list2 completed - processed " << index << " presets";

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

    std::string path = "/api/v1/presets/" + setting_id;
    int result = http_delete(path, &response, &http_code);

    if (result == BAMBU_NETWORK_SUCCESS && http_code == 200) {
        BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: Deleted preset successfully";
        return BAMBU_NETWORK_SUCCESS;
    }

    BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: delete_setting failed - http_code=" << http_code;
    return BAMBU_NETWORK_ERR_DEL_SETTING_FAILED;
}

// ============================================================================
// Extra Features
// ============================================================================

int OrcaNetwork::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: set_extra_http_header - count=" << extra_headers.size();
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto existing_api_key_it = this->extra_headers.find("apikey");
    const std::string existing_api_key = existing_api_key_it != this->extra_headers.end() ? existing_api_key_it->second : "";
    this->extra_headers = extra_headers;
    if (!existing_api_key.empty() && this->extra_headers.find("apikey") == this->extra_headers.end()) {
        this->extra_headers["apikey"] = existing_api_key;
    }
    if (auth_manager) {
        auth_manager->set_extra_headers(this->extra_headers);
    }
    return BAMBU_NETWORK_SUCCESS;
}

std::string OrcaNetwork::get_studio_info_url()
{
    return backend_url + "/api/v1/studio/info";
}

// ============================================================================
// HTTP Request Helpers
// ============================================================================

int OrcaNetwork::http_get(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = backend_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: GET " << url;

    try {
        auto http = Http::get(url);

        // Add authorization header if logged in
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (!session_token.empty()) {
                http.header("Authorization", "Bearer " + session_token);
            }

            // Add extra headers
            for (const auto& pair : extra_headers) {
                http.header(pair.first, pair.second);
            }
        }

        bool success = false;
        unsigned int status = 0;
        std::string body;

        http.on_complete([&](std::string resp_body, unsigned resp_status) {
            success = true;
            status = resp_status;
            body = resp_body;
        })
        .on_error([&](std::string resp_body, std::string error, unsigned resp_status) {
            success = false;
            status = resp_status;
            body = resp_body;
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
        })
        .timeout_max(30) // 30 second timeout
        .perform_sync();

        if (response_body) *response_body = body;
        if (http_code) *http_code = status;

        if (success && status >= 200 && status < 300) {
            return BAMBU_NETWORK_SUCCESS;
        } else {
            invoke_http_error_callback(status, body);
            return BAMBU_NETWORK_ERR_CONNECT_FAILED;
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_get exception - " << e.what();
        if (http_code) *http_code = 0;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
}

int OrcaNetwork::http_post(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = backend_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: POST " << url;

    try {
        auto http = Http::post(url);

        // Add headers
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (!session_token.empty() && path != ORCA_TOKEN_PATH) {
                http.header("Authorization", "Bearer " + session_token);
            }

            for (const auto& pair : extra_headers) {
                http.header(pair.first, pair.second);
            }
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
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
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
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_post exception - " << e.what();
        if (http_code) *http_code = 0;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
}

int OrcaNetwork::http_put(const std::string& path, const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::string url = backend_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: PUT " << url;

    try {
        auto http = Http::put(url);

        // Add headers
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (!session_token.empty()) {
                http.header("Authorization", "Bearer " + session_token);
            }

            for (const auto& pair : extra_headers) {
                http.header(pair.first, pair.second);
            }
        }

        http.header("Content-Type", "application/json");
        http.set_post_body(body); // Note: set_post_body works for PUT too

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
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
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
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_put exception - " << e.what();
        if (http_code) *http_code = 0;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
}

int OrcaNetwork::http_delete(const std::string& path, std::string* response_body, unsigned int* http_code)
{
    std::string url = backend_url + path;
    BOOST_LOG_TRIVIAL(trace) << "OrcaNetwork: DELETE " << url;

    try {
        auto http = Http::del(url);

        // Add headers
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex);
            if (!session_token.empty()) {
                http.header("Authorization", "Bearer " + session_token);
            }

            for (const auto& pair : extra_headers) {
                http.header(pair.first, pair.second);
            }
        }

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
            BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: HTTP error - " << error;
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
        BOOST_LOG_TRIVIAL(error) << "OrcaNetwork: http_delete exception - " << e.what();
        if (http_code) *http_code = 0;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
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
    return backend_url;
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
    if (url) *url = backend_url + "/publish";
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
    if (url) *url = backend_url + "/mall";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaNetwork::get_model_mall_detail_url(std::string* url, std::string id)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaNetwork: get_model_mall_detail_url (stub) - id=" << id;
    if (url) *url = backend_url + "/mall/" + id;
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
