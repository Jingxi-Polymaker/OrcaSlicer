#include "AuthManager.hpp"
#include "Http.hpp"
#include "slic3r/Utils/InstanceID.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/secretstore.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace pt = boost::property_tree;

namespace Slic3r {
namespace {

constexpr const char* SECRET_STORE_SERVICE = "OrcaSlicer/Auth";
constexpr const char* SECRET_STORE_USER    = "orca_refresh_token";

std::string base64url_encode(const std::vector<unsigned char>& data)
{
    std::string out;
    out.resize(boost::beast::detail::base64::encoded_size(data.size()));
    out.resize(boost::beast::detail::base64::encode(out.data(), data.data(), data.size()));

    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    return out;
}

bool base64url_decode(const std::string& input, std::vector<unsigned char>& out)
{
    std::string padded = input;
    while (padded.size() % 4 != 0) padded.push_back('=');
    std::string normalized = padded;
    std::replace(normalized.begin(), normalized.end(), '-', '+');
    std::replace(normalized.begin(), normalized.end(), '_', '/');

    out.resize(boost::beast::detail::base64::decoded_size(normalized.size()));
    auto res = boost::beast::detail::base64::decode(out.data(), normalized.data(), normalized.size());
    if (!res.second) return false;
    out.resize(res.first);
    return true;
}

std::vector<unsigned char> random_bytes(size_t len)
{
    std::vector<unsigned char> bytes(len);
    if (RAND_bytes(bytes.data(), static_cast<int>(len)) != 1) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));
    }
    return bytes;
}

std::string generate_code_verifier()
{
    constexpr int PKCE_VERIFIER_BYTES = 32;
    auto bytes = random_bytes(PKCE_VERIFIER_BYTES);
    return base64url_encode(bytes);
}

std::string generate_state_token()
{
    auto bytes = random_bytes(16);
    std::stringstream ss;
    for (auto b : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

std::string sha256_base64url(const std::string& input)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    std::vector<unsigned char> hash_vec(hash, hash + sizeof(hash));
    return base64url_encode(hash_vec);
}

std::string machine_identifier()
{
    if (auto* cfg = Slic3r::GUI::wxGetApp().app_config) {
        const auto iid = Slic3r::instance_id::ensure(*cfg);
        if (!iid.empty()) return iid;
    }

#if defined(__linux__)
    std::ifstream f("/etc/machine-id");
    std::string id;
    if (f.good()) {
        std::getline(f, id);
    }
    if (!id.empty()) return id;
#elif defined(_WIN32)
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(buffer, &size)) {
        return std::string(buffer, size);
    }
#elif defined(__APPLE__)
    char uuid_str[128] = {0};
    size_t len = sizeof(uuid_str);
    if (sysctlbyname("kern.uuid", uuid_str, &len, nullptr, 0) == 0 && len > 0) {
        return std::string(uuid_str, len - 1);
    }
#endif
    return wxGetUserId().ToStdString() + "@" + wxGetHostName().ToStdString();
}

std::vector<unsigned char> sha256_bytes(const std::string& input)
{
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH, 0);
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), out.data());
    return out;
}

std::string hmac_sha256_hex(const std::string& data, const std::vector<unsigned char>& key)
{
    unsigned int len = 0;
    unsigned char result[EVP_MAX_MD_SIZE];
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), result, &len) == nullptr) {
        return {};
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(result[i]);
    }
    return oss.str();
}

bool is_port_available(int port)
{
    if (port <= 0 || port > 65535) return false;

    using boost::asio::ip::tcp;
    boost::asio::io_context ctx;
    boost::system::error_code ec;

    tcp::acceptor acceptor(ctx);
    tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));

    acceptor.open(endpoint.protocol(), ec);
    if (ec) return false;
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) return false;
    acceptor.bind(endpoint, ec);
    if (ec) return false;
    acceptor.close(ec);
    return true;
}

int choose_loopback_port()
{
    int base_port = ORCA_LOOPBACK_PORT;

    if (const char* env_port = std::getenv("ORCA_LOOPBACK_PORT")) {
        try {
            int parsed = std::stoi(env_port);
            if (parsed > 0 && parsed <= 65535) {
                base_port = parsed;
            }
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: invalid ORCA_LOOPBACK_PORT value, falling back to default";
        }
    }

    std::vector<int> candidates = {base_port, base_port + 1, base_port + 2};
    for (int port : candidates) {
        if (is_port_available(port)) return port;
    }

    // None of the ports were free; stick with the base port to preserve backward compatibility.
    return base_port;
}

bool aes256gcm_encrypt(const std::string& plaintext, const std::vector<unsigned char>& key, std::string& out_b64)
{
    const int iv_len = 12;
    auto iv = random_bytes(iv_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = true;
    int len = 0;
    std::vector<unsigned char> ciphertext(plaintext.size());
    std::vector<unsigned char> tag(16);

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, nullptr) != 1) ok = false;
    if (ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) ok = false;
    if (ok && EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                                reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()) != 1) ok = false;
    int ciphertext_len = len;
    if (ok && EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) ok = false;
    ciphertext_len += len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) ok = false;

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return false;
    ciphertext.resize(ciphertext_len);

    std::vector<unsigned char> payload;
    payload.reserve(iv.size() + tag.size() + ciphertext.size());
    payload.insert(payload.end(), iv.begin(), iv.end());
    payload.insert(payload.end(), tag.begin(), tag.end());
    payload.insert(payload.end(), ciphertext.begin(), ciphertext.end());

    out_b64 = base64url_encode(payload);
    return true;
}

bool aes256gcm_decrypt(const std::string& b64_payload, const std::vector<unsigned char>& key, std::string& plaintext)
{
    std::vector<unsigned char> payload;
    if (!base64url_decode(b64_payload, payload)) return false;
    if (payload.size() < 12 + 16) return false;

    const size_t iv_len = 12;
    const size_t tag_len = 16;
    std::vector<unsigned char> iv(payload.begin(), payload.begin() + iv_len);
    std::vector<unsigned char> tag(payload.begin() + iv_len, payload.begin() + iv_len + tag_len);
    std::vector<unsigned char> ciphertext(payload.begin() + iv_len + tag_len, payload.end());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = true;
    int len = 0;
    std::vector<unsigned char> plain(ciphertext.size());

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, nullptr) != 1) ok = false;
    if (ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) ok = false;
    if (ok && EVP_DecryptUpdate(ctx, plain.data(), &len, ciphertext.data(), ciphertext.size()) != 1) ok = false;
    int plain_len = len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1) ok = false;
    if (ok && EVP_DecryptFinal_ex(ctx, plain.data() + len, &len) != 1) ok = false;
    plain_len += len;

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return false;
    plain.resize(plain_len);
    plaintext.assign(reinterpret_cast<char*>(plain.data()), plain.size());
    return true;
}
} // namespace

AuthManager::AuthManager(std::string auth_base_url)
    : auth_base_url(std::move(auth_base_url))
{
    pkce_bundle.loopback_port = choose_loopback_port();
    update_redirect_uri();
    regenerate_pkce();
    compute_fallback_path();
}

AuthManager::~AuthManager()
{
    if (refresh_thread.joinable()) {
        refresh_thread.join();
    }
}

void AuthManager::set_extra_headers(const std::map<std::string, std::string>& extra)
{
    std::lock_guard<std::mutex> lock(headers_mutex);
    extra_headers = extra;
}

void AuthManager::set_config_dir(const std::string& config_dir)
{
    wxFileName fallback(wxString::FromUTF8(config_dir.c_str()), "orca_refresh_token.sec");
    fallback.Normalize();
    refresh_fallback_path = fallback.GetFullPath().ToStdString();
}

void AuthManager::set_api_base_url(const std::string& api_base)
{
    api_base_url = api_base;
}

void AuthManager::set_auth_base_url(const std::string& auth_base)
{
    auth_base_url = auth_base;
}

void AuthManager::set_session_handler(SessionHandler handler)
{
    session_handler = std::move(handler);
}

const AuthManager::PkceBundle& AuthManager::pkce()
{
    if (pkce_bundle.verifier.empty() || pkce_bundle.challenge.empty() || pkce_bundle.state.empty()) {
        regenerate_pkce();
    }
    return pkce_bundle;
}

void AuthManager::regenerate_pkce()
{
    pkce_bundle.verifier = generate_code_verifier();
    pkce_bundle.challenge = sha256_base64url(pkce_bundle.verifier);
    pkce_bundle.state = generate_state_token();
    if (pkce_bundle.redirect.empty()) {
        pkce_bundle.redirect = "http://localhost:" + std::to_string(pkce_bundle.loopback_port) + ORCA_LOOPBACK_PATH;
    }
    BOOST_LOG_TRIVIAL(debug) << "AuthManager: regenerated PKCE bundle";
}

std::string AuthManager::build_login_cmd()
{
    update_redirect_uri();
    regenerate_pkce();
    const auto bundle = pkce();

    pt::ptree tree;
    tree.put("action", "login");
    tree.put("provider", "orca");
    tree.put("backend_url", auth_base_url);

    pt::ptree pkce_node;
    pkce_node.put("code_challenge", bundle.challenge);
    pkce_node.put("code_challenge_method", "S256");
    pkce_node.put("state", bundle.state);
    pkce_node.put("redirect_uri", bundle.redirect);
    pkce_node.put("code_verifier", bundle.verifier); // kept in-process; used by embedded login helper
    pkce_node.put("loopback_port", bundle.loopback_port);
    tree.add_child("pkce", pkce_node);

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

void AuthManager::update_redirect_uri()
{
    int selected_port = choose_loopback_port();
    if (selected_port != pkce_bundle.loopback_port) {
        BOOST_LOG_TRIVIAL(info) << "AuthManager: loopback port changed to " << selected_port;
    }
    pkce_bundle.loopback_port = selected_port;
    pkce_bundle.redirect = "http://localhost:" + std::to_string(selected_port) + ORCA_LOOPBACK_PATH;
}

std::string AuthManager::build_logout_cmd()
{
    pt::ptree tree;
    tree.put("action", "logout");
    tree.put("provider", "orca");

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

void AuthManager::persist_refresh_token(const std::string& token)
{
    if (token.empty()) {
        clear_refresh_token();
        return;
    }

    bool stored = false;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        wxSecretValue secret(wxString::FromUTF8(token.c_str()));
        if (store.Save(SECRET_STORE_SERVICE, SECRET_STORE_USER, secret)) {
            stored = true;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: wxSecretStore save failed, will attempt encrypted-file fallback";
        }
    }

    if (!stored) {
        auto key = sha256_bytes(machine_identifier());
        if (key.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: cannot derive key for refresh-token fallback storage";
            return;
        }

        std::string payload;
        if (!aes256gcm_encrypt(token, key, payload)) {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: failed to encrypt refresh token for fallback storage";
            return;
        }

        std::string signed_payload = payload;
        if (auto mac = hmac_sha256_hex(payload, key); !mac.empty()) {
            signed_payload = "v2:" + mac + ":" + payload;
        }

        compute_fallback_path();
        wxFileName path(wxString::FromUTF8(refresh_fallback_path.c_str()));
        path.Normalize();
        if (!wxFileName::DirExists(path.GetPath())) {
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }

        const std::string tmp_path = refresh_fallback_path + ".tmp";
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (ofs.good()) {
            ofs << signed_payload;
            ofs.flush();
            ofs.close();

            if (wxRenameFile(wxString::FromUTF8(tmp_path.c_str()), wxString::FromUTF8(refresh_fallback_path.c_str()), true)) {
                stored = true;
            } else {
                wxRemoveFile(wxString::FromUTF8(tmp_path.c_str()));
                BOOST_LOG_TRIVIAL(warning) << "AuthManager: failed to atomically replace refresh-token fallback file";
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: cannot open fallback refresh-token path for write - " << refresh_fallback_path;
        }
    }

    if (stored) {
        BOOST_LOG_TRIVIAL(info) << "AuthManager: refresh token persisted securely";
    }
}

bool AuthManager::load_refresh_token(std::string& out_token)
{
    out_token.clear();
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        wxString username;
        wxSecretValue secret;
        if (store.Load(SECRET_STORE_SERVICE, username, secret) && secret.IsOk()) {
            out_token.assign(static_cast<const char*>(secret.GetData()), secret.GetSize());
            if (!out_token.empty()) {
                BOOST_LOG_TRIVIAL(info) << "AuthManager: loaded refresh token from wxSecretStore";
                return true;
            }
        }
    }

    compute_fallback_path();
    if (wxFileExists(wxString::FromUTF8(refresh_fallback_path.c_str()))) {
        std::ifstream ifs(refresh_fallback_path, std::ios::binary);
        std::string payload((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        auto key = sha256_bytes(machine_identifier());
        std::string plain;
        if (!key.empty()) {
            std::string encoded_payload = payload;
            bool integrity_ok = true;

            if (payload.rfind("v2:", 0) == 0) {
                auto delim = payload.find(':', 3);
                if (delim == std::string::npos) {
                    integrity_ok = false;
                } else {
                    std::string stored_hmac = payload.substr(3, delim - 3);
                    std::string lower_stored = stored_hmac;
                    std::transform(lower_stored.begin(), lower_stored.end(), lower_stored.begin(), ::tolower);
                    encoded_payload = payload.substr(delim + 1);

                    std::string computed_hmac = hmac_sha256_hex(encoded_payload, key);
                    std::transform(computed_hmac.begin(), computed_hmac.end(), computed_hmac.begin(), ::tolower);
                    if (computed_hmac.empty() || computed_hmac != lower_stored) {
                        integrity_ok = false;
                        BOOST_LOG_TRIVIAL(warning) << "AuthManager: refresh token integrity check failed (HMAC mismatch)";
                    }
                }
            }

            if (integrity_ok && aes256gcm_decrypt(encoded_payload, key, plain) && !plain.empty()) {
                out_token = plain;
                BOOST_LOG_TRIVIAL(info) << "AuthManager: loaded refresh token from encrypted fallback";

                // Upgrade legacy payloads to signed format for next run
                if (payload.rfind("v2:", 0) != 0) {
                    persist_refresh_token(out_token);
                }
                return true;
            }
        }
    }

    return false;
}

void AuthManager::clear_refresh_token()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        store.Delete(SECRET_STORE_SERVICE);
    }

    compute_fallback_path();
    if (!refresh_fallback_path.empty() && wxFileExists(wxString::FromUTF8(refresh_fallback_path.c_str()))) {
        wxRemoveFile(wxString::FromUTF8(refresh_fallback_path.c_str()));
    }
}

bool AuthManager::should_refresh_locked(std::chrono::seconds skew) const
{
    if (!session.logged_in) return false;
    if (session.expires_at.time_since_epoch().count() == 0) return true; // unknown expiry, err on refresh

    auto now = std::chrono::system_clock::now();
    return (session.expires_at - now) <= skew;
}

bool AuthManager::decode_jwt_expiry(const std::string& token, std::chrono::system_clock::time_point& out_tp)
{
    out_tp = {};
    if (token.empty()) return false;

    auto first = token.find('.');
    auto second = token.find('.', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;

    std::string payload_b64 = token.substr(first + 1, second - first - 1);
    std::vector<unsigned char> payload_bytes;
    if (!base64url_decode(payload_b64, payload_bytes)) return false;

    std::string payload_str(payload_bytes.begin(), payload_bytes.end());
    try {
        pt::ptree payload;
        std::stringstream ss(payload_str);
        pt::read_json(ss, payload);
        auto exp_opt = payload.get_optional<long long>("exp");
        if (exp_opt) {
            out_tp = std::chrono::system_clock::time_point{std::chrono::seconds(*exp_opt)};
            return true;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "AuthManager: failed to decode JWT exp - " << e.what();
    }
    return false;
}

bool AuthManager::refresh_now(const std::string& refresh_token, const std::string& reason, bool async)
{
    if (refresh_token.empty()) return false;

    bool expected = false;
    if (!refresh_running.compare_exchange_strong(expected, true)) {
        BOOST_LOG_TRIVIAL(debug) << "AuthManager: refresh already running, skip (reason=" << reason << ")";
        return false;
    }

    auto worker = [this, refresh_token, reason]() {
        const std::string req_id = generate_state_token();
        BOOST_LOG_TRIVIAL(info) << "[auth] event=refresh_start source=" << reason << " rid=" << req_id;
        bool ok = refresh_session_with_token(refresh_token);
        if (ok) {
            BOOST_LOG_TRIVIAL(info) << "[auth] event=refresh_complete result=success source=" << reason << " rid=" << req_id;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[auth] event=refresh_complete result=failure source=" << reason << " rid=" << req_id;
        }
        refresh_running.store(false);
        return ok;
    };

    if (async) {
        if (refresh_thread.joinable()) {
            refresh_thread.join();
        }
        refresh_thread = std::thread([worker]() { worker(); });
        return true;
    }

    return worker();
}

bool AuthManager::refresh_from_storage(const std::string& reason, bool async)
{
    std::string refresh_token = get_refresh_token();
    if (refresh_token.empty()) {
        load_refresh_token(refresh_token);
    }
    if (refresh_token.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "AuthManager: no refresh token available for refresh (reason=" << reason << ")";
        return false;
    }

    return refresh_now(refresh_token, reason, async);
}

bool AuthManager::refresh_if_expiring(std::chrono::seconds skew, const std::string& reason)
{
    bool needs_refresh = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        needs_refresh = should_refresh_locked(skew);
    }

    if (!needs_refresh) return true;

    // simple backoff: try immediately, then once more after 750ms on failure
    if (refresh_from_storage(reason, false)) return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    return refresh_from_storage(reason + "_retry", false);
}

void AuthManager::try_refresh_async(const std::string& refresh_token)
{
    refresh_now(refresh_token, "async_refresh", true);
}

bool AuthManager::refresh_session_with_token(const std::string& refresh_token)
{
    std::string body = "{\"refresh_token\":\"" + refresh_token + "\"}";

    std::string url = auth_base_url + ORCA_TOKEN_PATH + "?grant_type=refresh_token";
    BOOST_LOG_TRIVIAL(debug) << "AuthManager: refresh request - token_length=" << refresh_token.size() << ", url=" << url;

    std::string  response;
    unsigned int http_code = 0;
    if (!http_post_token(body, &response, &http_code, url) || http_code >= 400) {
        std::string truncated_response = response.size() > 200 ? response.substr(0, 200) + "..." : response;
        BOOST_LOG_TRIVIAL(warning) << "AuthManager: token refresh failed - http_code=" << http_code
                                   << ", response_body=" << truncated_response;
        return false;
    }

    if (session_handler) {
        return session_handler(response);
    }

    return true;
}

bool AuthManager::http_post_token(const std::string& body, std::string* response_body, unsigned int* http_code, const std::string& custom_url)
{
    std::map<std::string, std::string> headers_copy;
    std::string                        url;
    {
        std::lock_guard<std::mutex> lock(headers_mutex);
        url          = custom_url.empty() ? (auth_base_url + ORCA_TOKEN_PATH) : custom_url;
        headers_copy = extra_headers;
    }
    BOOST_LOG_TRIVIAL(trace) << "AuthManager: POST " << url;

    // Verify apikey header is present
    bool has_apikey = false;
    for (const auto& pair : headers_copy) {
        if (pair.first == "apikey")
            has_apikey = true;
    }
    if (!has_apikey) {
        BOOST_LOG_TRIVIAL(warning) << "AuthManager: http_post_token - apikey header MISSING! Token request will likely fail.";
    }

    try {
        auto http = Http::post(url);

        for (const auto& pair : headers_copy) {
            http.header(pair.first, pair.second);
        }

        // Ensure no stale Authorization header is sent (e.g. from global headers)
        http.remove_header("Authorization");

        // Force Content-Type to application/json (remove first to avoid duplicates)
        http.remove_header("Content-Type");
        http.header("Content-Type", "application/json");
        http.set_post_body(body);

        bool         success = false;
        unsigned int status  = 0;
        std::string  resp_body;

        http.on_complete([&](std::string body, unsigned resp_status) {
                success   = true;
                status    = resp_status;
                resp_body = body;
            })
            .on_error([&](std::string body, std::string error, unsigned resp_status) {
                success   = false;
                status    = resp_status;
                resp_body = body;
                BOOST_LOG_TRIVIAL(error) << "AuthManager: HTTP error - " << error;
            })
            .timeout_max(30)
            .perform_sync();

        if (response_body)
            *response_body = resp_body;
        if (http_code)
            *http_code = status;
        return success;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AuthManager: http_post_token exception - " << e.what();
        if (http_code)
            *http_code = 0;
        return false;
    }
}

void AuthManager::compute_fallback_path()
{
    if (!refresh_fallback_path.empty()) return;
    wxFileName fallback(wxStandardPaths::Get().GetUserDataDir(), "orca_refresh_token.sec");
    fallback.Normalize();
    refresh_fallback_path = fallback.GetFullPath().ToStdString();
}

bool AuthManager::set_user_session(const std::string& token,
                                   const std::string& user_id,
                                   const std::string& username,
                                   const std::string& name,
                                   const std::string& nickname,
                                   const std::string& avatar,
                                   const std::string& refresh_token)
{
    std::chrono::system_clock::time_point exp_tp{};
    decode_jwt_expiry(token, exp_tp);

    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session.access_token = token;
        session.refresh_token = refresh_token;
        session.user_id = user_id;
        session.user_name = name.empty() ? username : name;
        session.user_nickname = nickname.empty() ? (!username.empty() ? username : name) : nickname;
        session.user_avatar = avatar;
        session.expires_at = exp_tp;
        session.logged_in = true;
    }

    if (!refresh_token.empty()) {
        persist_refresh_token(refresh_token);
    }
    BOOST_LOG_TRIVIAL(info) << "AuthManager: set_user_session - user_id=" << user_id << ", username=" << username;
    return true;
}

void AuthManager::clear_session()
{
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session = SessionInfo{};
    }
    clear_refresh_token();
}

bool AuthManager::is_logged_in() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.logged_in;
}

std::string AuthManager::get_access_token() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.access_token;
}

std::string AuthManager::get_refresh_token() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.refresh_token;
}

std::string AuthManager::get_user_id() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_id;
}

std::string AuthManager::get_user_name() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_name;
}

std::string AuthManager::get_user_avatar() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_avatar;
}

std::string AuthManager::get_user_nickname() const
{
    std::lock_guard<std::mutex> lock(session_mutex);
    return session.user_nickname;
}

std::string AuthManager::build_login_info() const
{
    pt::ptree tree;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        tree.put("user_id", session.user_id);
        tree.put("user_name", session.user_name);
        tree.put("nickname", session.user_nickname);
        tree.put("avatar", session.user_avatar);
        tree.put("logged_in", session.logged_in);
    }
    // Do not expose tokens to the WebView.
    tree.put("access_token", "");
    tree.put("refresh_token", "");
    tree.put("backend_url", api_base_url);
    tree.put("auth_url", auth_base_url);

    std::stringstream ss;
    pt::write_json(ss, tree);
    return ss.str();
}

} // namespace Slic3r
