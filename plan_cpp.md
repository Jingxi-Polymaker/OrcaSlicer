# C++ Backend Changes for Dual Cloud Login

## Context

The homepage frontend (plan.md) expects the C++ backend to handle Bambu-specific login commands independently from Orca commands. Currently, a single `m_agent` routes all login traffic to whichever cloud is configured via `use_orca_cloud`. We need a second, independent Bambu cloud agent for login/logout that does NOT trigger Orca-side effects (preset loading, device manager cleanup, sync).

## Prerequisite: Remove `use_orca_cloud` — Orca Is Always Primary

The `use_orca_cloud` config flag is obsolete. The main cloud service is always OrcaCloud; Bambu Cloud becomes a secondary login managed via the new sidecar agent.

### NetworkAgentFactory.cpp (src/slic3r/Utils/NetworkAgentFactory.cpp:152-181)

- Remove the `use_orca_cloud` config read and provider branching
- Always create `OrcaCloudServiceAgent` as the cloud agent:
```cpp
std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config)
{
    if (!app_config)
        return std::make_unique<NetworkAgent>(nullptr, nullptr);

    // Primary cloud agent is always Orca
    std::shared_ptr<ICloudServiceAgent> cloud_agent;
    if (app_config->get_bool("installed_networking") || true) {  // always create
        cloud_agent = NetworkAgentFactory::create_cloud_agent(CloudAgentProvider::Orca, log_dir);
    }

    auto agent = std::make_unique<NetworkAgent>(std::move(cloud_agent), nullptr);
    if (agent) {
        auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(agent->get_cloud_agent().get());
        if (orca_cloud) {
            orca_cloud->configure_urls(app_config);
        }
    }
    return agent;
}
```

### GUI_App.cpp — Remove `use_orca_cloud` guard on BBL init (~line 3467-3468)

The BBL DLL init block currently checks `app_config->get_bool("use_orca_cloud")`. Since the flag is removed, change the condition to always init the BBL DLL when the networking plugin should be loaded:
```cpp
if (should_load_networking_plugin && !m_networking_need_update) {
```

### Sweep: Remove all `use_orca_cloud` references

Only 3 source files reference `use_orca_cloud`:
- `src/slic3r/Utils/NetworkAgentFactory.cpp` — lines 158, 162, 163, 173 (factory logic)
- `src/slic3r/Utils/NetworkAgentFactory.hpp` — lines 167-168 (doc comment)
- `src/slic3r/GUI/GUI_App.cpp` — line 3468 (BBL DLL init guard)

| File | Changes |
|------|---------|
| `src/slic3r/Utils/NetworkAgentFactory.cpp` | Always create Orca cloud agent, remove `use_orca_cloud` branching |
| `src/slic3r/Utils/NetworkAgentFactory.hpp` | Update doc comment |
| `src/slic3r/GUI/GUI_App.cpp` | Remove `use_orca_cloud` guard on BBL DLL init |

## Ownership Model

`BBLCloudServiceAgent` is a stateless wrapper — every method delegates to `BBLNetworkPlugin::instance().get_agent()`. The real DLL agent is owned by `BBLNetworkPlugin` (created via `create_agent()`, destroyed via `destroy_agent()`). The `m_bambu_cloud_agent` shared_ptr we store is just a convenience accessor; it does not own or extend the lifetime of the underlying DLL agent.

**Shutdown**: `NetworkAgent::unload_network_module()` calls `BBLNetworkPlugin::unload()` which frees the DLL library but does NOT destroy the agent handle. Actual agent destruction happens in `BBLNetworkPlugin::~BBLNetworkPlugin()` which calls `destroy_agent()` then `unload()`. Since `BBLNetworkPlugin` is a singleton destroyed at process exit, the DLL agent lifetime extends to program termination. Our `m_bambu_cloud_agent.reset()` only releases the wrapper — it must be called before the DLL is unloaded to avoid dangling delegation calls.

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/slic3r/Utils/NetworkAgentFactory.cpp` | Always create Orca cloud agent, remove `use_orca_cloud` |
| `src/slic3r/Utils/NetworkAgentFactory.hpp` | Update doc comment |
| `src/slic3r/GUI/GUI_App.hpp` | Add `m_bambu_cloud_agent` member, new method declarations |
| `src/slic3r/GUI/GUI_App.cpp` | Remove `use_orca_cloud` guard, store BBL agent at init, add 3 command handlers, implement 5 new methods |
| `src/slic3r/GUI/WebUserLoginDialog.hpp` | Add agent-parameterized constructor, `m_cloud_agent` + `m_is_bambu_login` members |
| `src/slic3r/GUI/WebUserLoginDialog.cpp` | Add second constructor, refactor ALL agent-dependent methods to use `m_cloud_agent` |
| `src/slic3r/GUI/WebViewDialog.cpp` | Send `bambu_cloud_available` message on page load |

---

## Step 0: Frontend Visibility of BambuCloudSection

The `#BambuCloudSection` in the frontend must be hidden when `installed_networking` is false (no BBL DLL plugin). In that case `m_bambu_cloud_agent` is null and no Bambu commands work.

**Approach**: Add a new C++ → JS message `bambu_cloud_available` sent during page init (from `WebViewPanel::OnNavigationComplete` or alongside `ShowNetpluginTip`):

```cpp
// In WebViewPanel, after page loads:
json msg;
msg["command"] = "bambu_cloud_available";
msg["available"] = (wxGetApp().get_bambu_cloud_agent() != nullptr) ? 1 : 0;
wxString strJS = wxString::Format("window.postMessage(%s)", msg.dump());
RunScript(strJS);
```

**Frontend handler** (already covered in plan.md — add to `HandleStudio`):
- When `available=0`: hide `#BambuCloudSection` entirely (`display:none`)
- When `available=1`: show `#BambuCloudSection` (default collapsed state)

This replaces the need for the frontend to guess from `network_plugin_installtip` — the backend explicitly tells the frontend whether Bambu Cloud login is available.

---

## Step 1: Store Bambu Cloud Agent in GUI_App

### GUI_App.hpp (~line 295, near `m_agent`)

Add member:
```cpp
std::shared_ptr<BBLCloudServiceAgent> m_bambu_cloud_agent;
```

Add public methods (~line 353, near `getAgent()`):
```cpp
std::shared_ptr<BBLCloudServiceAgent> get_bambu_cloud_agent() { return m_bambu_cloud_agent; }
void get_bambu_login_info();
void request_bambu_login(bool show_user_info = false);
void request_bambu_logout();
void handle_bambu_script_message(std::string msg);
void ShowBambuUserLogin(bool show = true);
```

Add include at top:
```cpp
#include "slic3r/Utils/BBLCloudServiceAgent.hpp"
```

### GUI_App.cpp (~line 3473-3479, existing BBL init block)

Change the stack-local `BBLCloudServiceAgent bbl` to store in `m_bambu_cloud_agent`:

```cpp
if (plugin.has_agent()) {
    m_bambu_cloud_agent = std::make_shared<BBLCloudServiceAgent>();
    m_bambu_cloud_agent->set_config_dir(data_directory);
    m_bambu_cloud_agent->init_log();
    m_bambu_cloud_agent->set_cert_file(resources_dir() + "/cert", "slicer_base64.cer");
    m_bambu_cloud_agent->set_country_code(app_config->get_country_code());
    m_bambu_cloud_agent->start();
}
```

Note: `m_bambu_cloud_agent` is a convenience accessor only. The underlying DLL agent lifetime is managed by `BBLNetworkPlugin::instance()` (destroyed in its destructor at process exit).

---

## Step 2: Route New Commands in handle_web_request()

### GUI_App.cpp (~line 4627, after `homepage_logout` handler)

Add three new command handlers:
```cpp
else if (command_str.compare("get_bambu_login_info") == 0) {
    CallAfter([this] { get_bambu_login_info(); });
}
else if (command_str.compare("homepage_bambu_login_or_register") == 0) {
    CallAfter([this] { this->request_bambu_login(true); });
}
else if (command_str.compare("homepage_bambu_logout") == 0) {
    CallAfter([this] { wxGetApp().request_bambu_logout(); });
}
```

---

## Step 3: Implement Bambu Login Methods

### get_bambu_login_info() — GUI_App.cpp (after get_login_info(), ~line 4510)

Sends `studio_bambu_userlogin` or `studio_bambu_useroffline` to WebView. Cannot reuse `build_login_cmd()`/`build_logout_cmd()` because those emit `studio_userlogin`/`studio_useroffline` which would conflict with Orca's commands.

```cpp
void GUI_App::get_bambu_login_info()
{
    if (!m_bambu_cloud_agent) return;

    if (m_bambu_cloud_agent->is_user_login()) {
        std::string name = m_bambu_cloud_agent->get_user_nickname();
        if (name.empty()) name = m_bambu_cloud_agent->get_user_name();
        std::string avatar = m_bambu_cloud_agent->get_user_avatar();

        json cmd;
        cmd["command"] = "studio_bambu_userlogin";
        cmd["data"]["name"] = name;
        cmd["data"]["avatar"] = avatar;

        wxString strJS = wxString::Format("window.postMessage(%s)", cmd.dump());
        run_script(strJS);
    } else {
        json cmd;
        cmd["command"] = "studio_bambu_useroffline";

        wxString strJS = wxString::Format("window.postMessage(%s)", cmd.dump());
        run_script(strJS);
    }
}
```

### request_bambu_login() — GUI_App.cpp (after request_login(), ~line 4481)

```cpp
void GUI_App::request_bambu_login(bool show_user_info)
{
    if (!m_bambu_cloud_agent) return;
    ShowBambuUserLogin();
    if (show_user_info) {
        get_bambu_login_info();
    }
}
```

### request_bambu_logout() — GUI_App.cpp (after request_user_logout(), ~line 4568)

**Intentionally minimal** — no preset cleanup, no device manager changes, no sync stop:

```cpp
void GUI_App::request_bambu_logout()
{
    if (m_bambu_cloud_agent && m_bambu_cloud_agent->is_user_login()) {
        m_bambu_cloud_agent->user_logout(true);
        get_bambu_login_info();  // sends studio_bambu_useroffline
    }
}
```

### handle_bambu_script_message() — GUI_App.cpp (after handle_script_message(), ~line 4799)

Processes login completion from the Bambu login dialog. Does NOT call `request_user_login()` (which triggers preset loading, device manager, sync):

```cpp
void GUI_App::handle_bambu_script_message(std::string msg)
{
    try {
        json j = json::parse(msg);
        if (j.contains("command")) {
            wxString cmd = j["command"];
            if (cmd == "user_login") {
                if (m_bambu_cloud_agent) {
                    m_bambu_cloud_agent->change_user(j.dump());
                    if (m_bambu_cloud_agent->is_user_login()) {
                        get_bambu_login_info();  // sends studio_bambu_userlogin
                    }
                }
            }
        }
    } catch (...) { ; }
}
```

### ShowBambuUserLogin() — GUI_App.cpp (after ShowUserLogin(), ~line 4255)

```cpp
void GUI_App::ShowBambuUserLogin(bool show)
{
    if (!m_bambu_cloud_agent) return;
    if (show) {
        try {
            ZUserLogin dlg(m_bambu_cloud_agent);
            dlg.ShowModal();
        } catch (std::exception&) { ; }
    }
}
```

---

## Step 4: Add Agent-Parameterized Constructor to ZUserLogin

### WebUserLoginDialog.hpp

Add to class declaration:
```cpp
class ZUserLogin : public wxDialog
{
public:
    ZUserLogin();
    explicit ZUserLogin(std::shared_ptr<ICloudServiceAgent> cloud_agent);  // NEW
    virtual ~ZUserLogin();
    // ... existing ...
private:
    // ... existing ...
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;  // NEW
    bool m_is_bambu_login{false};                        // NEW
};
```

Add include:
```cpp
#include "slic3r/Utils/ICloudServiceAgent.hpp"
```

### WebUserLoginDialog.cpp

**New constructor** — nearly identical to default but uses `m_cloud_agent` directly:

```cpp
ZUserLogin::ZUserLogin(std::shared_ptr<ICloudServiceAgent> cloud_agent)
    : wxDialog((wxWindow*)(wxGetApp().mainframe), wxID_ANY, "OrcaSlicer")
    , m_cloud_agent(cloud_agent)
    , m_is_bambu_login(true)
{
    // Same setup as default constructor, but use m_cloud_agent->get_cloud_login_url()
    // instead of agent->get_cloud_login_url()
    // ... (see implementation details below)
}
```

**Refactor default constructor** to also set `m_cloud_agent`:
- At line 48: after `NetworkAgent* agent = wxGetApp().getAgent();`, add:
  ```cpp
  if (agent) m_cloud_agent = agent->get_cloud_agent();
  ```
- At line 82: change `agent->get_cloud_login_url(strlang.ToStdString())` to `m_cloud_agent->get_cloud_login_url(strlang.ToStdString())`

**Refactor OnDocumentLoaded** (~line 219):

Currently uses `wxGetApp().getAgent()->get_cloud_service_host()` which returns the Orca host. When the dialog is opened for Bambu login, the loaded URL is a Bambu URL, so the host check fails and `m_networkOk` stays false, potentially triggering the error-page timer.

Change:
```cpp
NetworkAgent* agent = wxGetApp().getAgent();
std::string strHost = agent->get_cloud_service_host();
```
to:
```cpp
std::string strHost;
if (m_cloud_agent) {
    strHost = m_cloud_agent->get_cloud_service_host();
} else {
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent) strHost = agent->get_cloud_service_host();
}
```

**Modify OnScriptMessage** (~line 264):

At line 273, change:
```cpp
NetworkAgent* agent = wxGetApp().getAgent();
if (agent && strCmd == "get_login_cmd" && agent->get_cloud_agent()) {
    std::string login_cmd = agent->build_login_cmd();
```
to:
```cpp
if (m_cloud_agent && strCmd == "get_login_cmd") {
    std::string login_cmd = m_cloud_agent->build_login_cmd();
```

At line 322-333, change `user_login` handler to branch:
```cpp
if (strCmd == "user_login") {
    j["data"]["autotest_token"] = m_AutotestToken;
    std::string message_json = j.dump();
    bool is_bambu = m_is_bambu_login;
    EndModal(wxID_OK);
    wxTheApp->CallAfter([message_json, is_bambu]() {
        if (is_bambu) {
            wxGetApp().handle_bambu_script_message(message_json);
        } else {
            wxGetApp().handle_script_message(message_json);
        }
    });
}
```

---

## Step 5: Cleanup on Shutdown

In GUI_App.cpp, wherever `m_agent` is cleaned up (OnExit or destructor), add:
```cpp
m_bambu_cloud_agent.reset();
```

This releases the stateless `BBLCloudServiceAgent` wrapper only. The underlying DLL agent is owned by `BBLNetworkPlugin` and destroyed in `~BBLNetworkPlugin()` at process exit. Important: `m_bambu_cloud_agent` must be reset **before** `NetworkAgent::unload_network_module()` is called, since `unload()` frees the DLL library — any calls through the wrapper after that would crash.

---

## Thread Safety Notes

- `m_bambu_cloud_agent` is set once during init, read on UI thread via `CallAfter` — no mutex needed
- BBL DLL singleton handles its own thread safety for login/logout
- All `run_script()` calls happen on main thread (wxWidgets requirement)
- Login dialog is modal on main thread, same as Orca login

## What Does NOT Change

- `m_agent` always wraps `OrcaCloudServiceAgent` (enforced by factory change in Prerequisite)
- `on_user_login` / `on_user_login_handle` — NOT triggered by Bambu login
- Preset loading/unloading — Orca-only
- Device manager — Orca-only
- Sync thread — Orca-only

## Verification

1. **Build**: `cmake --build build --config RelWithDebInfo --target all`
2. **Orca login**: Existing flow unchanged — `homepage_login_or_register` → Orca dialog → presets load
3. **Bambu login**: `homepage_bambu_login_or_register` → Bambu dialog → `studio_bambu_userlogin` sent to JS, no preset side effects
4. **Bambu logout**: `homepage_bambu_logout` → `studio_bambu_useroffline` sent to JS, no preset/sync cleanup
5. **Independence**: Orca logout does not affect Bambu session and vice versa
6. **Init**: Both `get_login_info` and `get_bambu_login_info` return correct status on page load
7. **Plugin missing**: `m_bambu_cloud_agent` is null when BBL plugin unavailable — all Bambu handlers gracefully no-op
8. **Bambu login dialog host check**: `OnDocumentLoaded()` uses `m_cloud_agent->get_cloud_service_host()` so it correctly matches the Bambu URL
