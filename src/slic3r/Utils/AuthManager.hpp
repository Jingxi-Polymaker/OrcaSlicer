#ifndef __AUTH_MANAGER_HPP__
#define __AUTH_MANAGER_HPP__

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <thread>

class wxSecretStore;

namespace Slic3r {

constexpr int ORCA_LOOPBACK_PORT = 41172;
constexpr const char* ORCA_LOOPBACK_PATH = "/callback";
constexpr const char* ORCA_TOKEN_PATH = "/auth/v1/token";

class AuthManager {
public:
    struct PkceBundle {
        std::string verifier;
        std::string challenge;
        std::string state;
        std::string redirect;
        int loopback_port = ORCA_LOOPBACK_PORT;
    };

    using SessionHandler = std::function<bool(const std::string&)>;

    explicit AuthManager(std::string backend_url);
    ~AuthManager();

    void set_extra_headers(const std::map<std::string, std::string>& extra);
    void set_config_dir(const std::string& config_dir);
    void set_session_handler(SessionHandler handler);

    const PkceBundle& pkce();
    void regenerate_pkce();

    void persist_refresh_token(const std::string& token);
    bool load_refresh_token(std::string& out_token);
    void clear_refresh_token();

    void try_refresh_async(const std::string& refresh_token);
    bool refresh_session_with_token(const std::string& refresh_token);

private:
    bool http_post_token(const std::string& body, std::string* response_body, unsigned int* http_code);
    void ensure_secret_store();
    void compute_fallback_path();

    std::string backend_url;
    std::map<std::string, std::string> extra_headers;
    std::mutex headers_mutex;
    PkceBundle pkce_bundle;
    std::unique_ptr<wxSecretStore> secret_store;
    std::string refresh_fallback_path;
    SessionHandler session_handler;

    std::thread refresh_thread;
    std::atomic_bool refresh_running{false};
};

} // namespace Slic3r

#endif // __AUTH_MANAGER_HPP__
