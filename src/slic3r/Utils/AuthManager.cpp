#include "AuthManager.hpp"
#include "Http.hpp"
#include "slic3r/Utils/InstanceID.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
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
    pkce_bundle.redirect = "http://localhost:" + std::to_string(ORCA_LOOPBACK_PORT) + ORCA_LOOPBACK_PATH;
    regenerate_pkce();
    ensure_secret_store();
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
        pkce_bundle.redirect = "http://localhost:" + std::to_string(ORCA_LOOPBACK_PORT) + ORCA_LOOPBACK_PATH;
    }
    BOOST_LOG_TRIVIAL(debug) << "AuthManager: regenerated PKCE bundle";
}

std::string AuthManager::build_login_cmd()
{
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

    ensure_secret_store();
    bool stored = false;
    if (secret_store && secret_store->IsOk()) {
        wxSecretValue secret(wxString::FromUTF8(token.c_str()));
        if (secret_store->Save("OrcaSlicer", "orca_refresh_token", secret)) {
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

        compute_fallback_path();
        wxFileName path(wxString::FromUTF8(refresh_fallback_path.c_str()));
        path.Normalize();
        if (!wxFileName::DirExists(path.GetPath())) {
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }

        std::ofstream ofs(refresh_fallback_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (ofs.good()) {
            ofs << payload;
            ofs.close();
            stored = true;
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
    ensure_secret_store();

    if (secret_store && secret_store->IsOk()) {
        wxString username = "orca_refresh_token";
        wxSecretValue secret;
        if (secret_store->Load("OrcaSlicer", username, secret) && secret.IsOk()) {
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
        if (!key.empty() && aes256gcm_decrypt(payload, key, plain) && !plain.empty()) {
            out_token = plain;
            BOOST_LOG_TRIVIAL(info) << "AuthManager: loaded refresh token from encrypted fallback";
            return true;
        }
    }

    return false;
}

void AuthManager::clear_refresh_token()
{
    ensure_secret_store();
    if (secret_store && secret_store->IsOk()) {
        secret_store->Delete("OrcaSlicer");
    }

    compute_fallback_path();
    if (!refresh_fallback_path.empty() && wxFileExists(wxString::FromUTF8(refresh_fallback_path.c_str()))) {
        wxRemoveFile(wxString::FromUTF8(refresh_fallback_path.c_str()));
    }
}

void AuthManager::try_refresh_async(const std::string& refresh_token)
{
    if (refresh_token.empty()) return;

    bool expected = false;
    if (!refresh_running.compare_exchange_strong(expected, true)) {
        BOOST_LOG_TRIVIAL(debug) << "AuthManager: refresh already running, skip";
        return;
    }

    if (refresh_thread.joinable()) {
        refresh_thread.join();
    }

    refresh_thread = std::thread([this, refresh_token]() {
        if (!refresh_session_with_token(refresh_token)) {
            BOOST_LOG_TRIVIAL(warning) << "AuthManager: refresh_token exchange failed";
        }
        refresh_running.store(false);
    });
}

bool AuthManager::refresh_session_with_token(const std::string& refresh_token)
{
    pt::ptree req;
    req.put("grant_type", "refresh_token");
    req.put("refresh_token", refresh_token);
    std::stringstream body_ss;
    pt::write_json(body_ss, req);

    std::string response;
    unsigned int http_code = 0;
    if (!http_post_token(body_ss.str(), &response, &http_code) || http_code >= 400) {
        BOOST_LOG_TRIVIAL(warning) << "AuthManager: token refresh failed - http_code=" << http_code;
        return false;
    }

    if (session_handler) {
        return session_handler(response);
    }

    return true;
}

bool AuthManager::http_post_token(const std::string& body, std::string* response_body, unsigned int* http_code)
{
    std::map<std::string, std::string> headers_copy;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(headers_mutex);
        url = auth_base_url + ORCA_TOKEN_PATH;
        headers_copy = extra_headers;
    }
    BOOST_LOG_TRIVIAL(trace) << "AuthManager: POST " << url;

    try {
        auto http = Http::post(url);

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
            BOOST_LOG_TRIVIAL(error) << "AuthManager: HTTP error - " << error;
        })
        .timeout_max(30)
        .perform_sync();

        if (response_body) *response_body = resp_body;
        if (http_code) *http_code = status;
        return success;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AuthManager: http_post_token exception - " << e.what();
        if (http_code) *http_code = 0;
        return false;
    }
}

void AuthManager::ensure_secret_store()
{
    if (secret_store && secret_store->IsOk()) return;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        secret_store = std::make_unique<wxSecretStore>(store);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "AuthManager: wxSecretStore unavailable; falling back to encrypted file for refresh token";
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
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session.access_token = token;
        session.refresh_token = refresh_token;
        session.user_id = user_id;
        session.user_name = name.empty() ? username : name;
        session.user_nickname = nickname.empty() ? (!username.empty() ? username : name) : nickname;
        session.user_avatar = avatar;
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
