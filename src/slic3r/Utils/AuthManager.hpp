#ifndef __AUTH_MANAGER_HPP__
#define __AUTH_MANAGER_HPP__

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <optional>
#include <thread>

class wxSecretStore;

namespace Slic3r {

constexpr int ORCA_LOOPBACK_PORT = 41172;
constexpr const char* ORCA_LOOPBACK_PATH = "/callback";
constexpr const char* ORCA_TOKEN_PATH = "/auth/v1/token";

class AuthManager {
public:
    struct SessionInfo {
        std::string access_token;
        std::string refresh_token;
        std::string user_id;
        std::string user_name;
        std::string user_nickname;
        std::string user_avatar;
        std::chrono::system_clock::time_point expires_at{};
        bool logged_in = false;
    };

    struct PkceBundle {
        std::string verifier;
        std::string challenge;
        std::string state;
        std::string redirect;
        int loopback_port = ORCA_LOOPBACK_PORT;
    };

    using SessionHandler = std::function<bool(const std::string&)>;

    explicit AuthManager(std::string auth_base_url);
    ~AuthManager();

    void set_extra_headers(const std::map<std::string, std::string>& extra);
    void set_config_dir(const std::string& config_dir);
    void set_api_base_url(const std::string& api_base_url);
    void set_auth_base_url(const std::string& auth_base_url);
    void set_session_handler(SessionHandler handler);

    const PkceBundle& pkce();
    void regenerate_pkce();

    void persist_refresh_token(const std::string& token);
    bool load_refresh_token(std::string& out_token);
    void clear_refresh_token();

    // Token refresh helpers
    bool refresh_if_expiring(std::chrono::seconds skew, const std::string& reason);
    bool refresh_from_storage(const std::string& reason, bool async = false);
    bool refresh_now(const std::string& refresh_token, const std::string& reason, bool async = false);

    void try_refresh_async(const std::string& refresh_token);
    bool refresh_session_with_token(const std::string& refresh_token);

    // Session state helpers
    bool set_user_session(const std::string& token,
                          const std::string& user_id,
                          const std::string& username,
                          const std::string& name,
                          const std::string& nickname,
                          const std::string& avatar,
                          const std::string& refresh_token = "");
    void clear_session();
    bool is_logged_in() const;
    std::string get_access_token() const;
    std::string get_refresh_token() const;
    std::string get_user_id() const;
    std::string get_user_name() const;
    std::string get_user_avatar() const;
    std::string get_user_nickname() const;

    // UI helpers for constructing login/logout commands
    std::string build_login_cmd();
    std::string build_logout_cmd();
    std::string build_login_info() const;

private:
    bool http_post_token(const std::string& body, std::string* response_body, unsigned int* http_code, const std::string& url = "");
    void update_redirect_uri();
    void compute_fallback_path();
    bool decode_jwt_expiry(const std::string& token, std::chrono::system_clock::time_point& out_tp);
    bool should_refresh_locked(std::chrono::seconds skew) const;

    std::string auth_base_url;
    std::string api_base_url;
    std::map<std::string, std::string> extra_headers;
    std::mutex headers_mutex;
    PkceBundle pkce_bundle;
    std::string refresh_fallback_path;
    SessionHandler session_handler;
    SessionInfo session;
    mutable std::mutex session_mutex;

    std::thread refresh_thread;
    std::atomic_bool refresh_running{false};
};

} // namespace Slic3r

#endif // __AUTH_MANAGER_HPP__
