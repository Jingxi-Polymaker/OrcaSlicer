# C++ Backend: Multi-Cloud Provider Support

## Context

The homepage frontend already has fully-landed Orca and Bambu login sections — HTML (`index.html:43-67`), JS handlers (`home.js` `HandleStudio` for `studio_bambu_userlogin`/`studio_bambu_useroffline`), and send functions (`SendMsg_GetBambuLoginInfo`, `OnBambuLoginOrRegister`, `OnBambuLogOut`). The C++ backend does NOT yet handle any of these Bambu commands. This plan wires the backend to the existing frontend surface.

Key changes:
1. Removes `use_orca_cloud` with correct migration (preserving legacy Bambu users)
2. Adds `cloud_providers` config (semicolon-delimited, default "orca", can include "bambu")
3. Creates a sidecar `m_bambu_cloud_agent` for independent Bambu login/logout with NO Orca side effects
4. Sends `cloud_providers_info` on page load; default-hides `#BambuCloudSection` in CSS

**Ownership caveat**: `m_bambu_cloud_agent` (`shared_ptr<BBLCloudServiceAgent>`) is a **facade over shared plugin state** — every method delegates to `BBLNetworkPlugin::instance().get_agent()`. It does NOT own an independent backend instance. The `shared_ptr` controls only the wrapper lifetime, not the underlying DLL agent (owned by the `BBLNetworkPlugin` singleton, destroyed at process exit). This matters for lifecycle reasoning: `reset()` must happen before `unload_network_module()`.

---

## Phase 0: AppConfig — `cloud_providers` Setting

### `src/libslic3r/AppConfig.hpp`
- Add `#define SETTING_CLOUD_PROVIDERS "cloud_providers"` near other `SETTING_*` defines
- Add 5 methods: `get_cloud_providers()`, `set_cloud_providers()`, `has_cloud_provider()`, `add_cloud_provider()`, `remove_cloud_provider()`

### `src/libslic3r/AppConfig.cpp`

**Migration in `set_defaults()` (~line 102)** — must preserve legacy Bambu users:
```cpp
// Migrate use_orca_cloud -> cloud_providers
if (!get("use_orca_cloud").empty()) {
    bool was_orca = get_bool("use_orca_cloud");
    if (!was_orca) {
        // Legacy Bambu-only user: give them both providers
        set(SETTING_CLOUD_PROVIDERS, "orca;bambu");
    }
    erase("app", "use_orca_cloud");
    m_dirty = true;
}
// Default for new installs
if (get(SETTING_CLOUD_PROVIDERS).empty()) {
    set(SETTING_CLOUD_PROVIDERS, "orca");
}
```

When `use_orca_cloud=false` (or absent with `installed_networking=true`), the old code used BBL as the primary cloud agent. These users relied on Bambu Cloud login. Migrating them to `orca;bambu` ensures they keep Bambu access while gaining the new Orca-primary architecture.

**Implement 5 methods** (semicolon-delimited pattern, same as `get_skipped_network_versions()` at line 1504):
- `get_cloud_providers()` always ensures "orca" is present
- `remove_cloud_provider()` refuses to remove "orca"

---

## Phase 1: Remove `use_orca_cloud` References

### `src/slic3r/Utils/NetworkAgentFactory.cpp` (lines 152-181)
- Replace `create_agent_from_config()` to always create `OrcaCloudServiceAgent`
- Remove `use_orca_cloud` read (line 158), provider branching (lines 162-164), conditional `configure_urls` (line 173)
- Always call `configure_urls(app_config)` on the Orca agent

### `src/slic3r/Utils/NetworkAgentFactory.hpp` (lines 167-168)
- Update doc comment to reflect Orca-always-primary

### `src/slic3r/GUI/GUI_App.cpp` (lines 3467-3468)
- Change `if (should_load_networking_plugin && !m_networking_need_update && app_config->get_bool("use_orca_cloud"))` to `if (should_load_networking_plugin && !m_networking_need_update)`

---

## Phase 2: Store Bambu Cloud Agent in GUI_App

### `src/slic3r/GUI/GUI_App.hpp`
- Add `#include "slic3r/Utils/BBLCloudServiceAgent.hpp"`
- Add member: `std::shared_ptr<BBLCloudServiceAgent> m_bambu_cloud_agent;` (near `m_agent`, ~line 295)
- Add 6 public methods: `get_bambu_cloud_agent()`, `get_bambu_login_info()`, `request_bambu_login()`, `request_bambu_logout()`, `handle_bambu_script_message()`, `ShowBambuUserLogin()`

### `src/slic3r/GUI/GUI_App.cpp` (lines 3473-3481, BBL init block)
- Keep the existing BBL DLL init (needed for BBLPrinterAgent LAN operations regardless of cloud_providers config)
- After init, conditionally create the facade: `if (app_config->has_cloud_provider("bambu")) m_bambu_cloud_agent = std::make_shared<BBLCloudServiceAgent>();`
- Note: the DLL agent is already initialized by the preceding `bbl.set_config_dir()/init_log()/start()` calls via the singleton — the new `shared_ptr` is just a convenience wrapper

---

## Phase 3: Route Bambu Commands in `handle_web_request()`

### `src/slic3r/GUI/GUI_App.cpp` (after line 4627, after `homepage_logout` handler)
Add 3 handlers matching the commands already sent by `home.js`:
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

## Phase 4: Implement 5 Bambu Login Methods

### `src/slic3r/GUI/GUI_App.cpp`

**`get_bambu_login_info()`** (after `get_login_info()`, ~line 4510):
- If `m_bambu_cloud_agent->is_user_login()`: send `studio_bambu_userlogin` with name/avatar via `run_script()`
- Else: send `studio_bambu_useroffline`
- These command names match what `home.js:HandleStudio` already dispatches

**`request_bambu_login()`** (after `request_login()`, ~line 4481):
- Guard on `m_bambu_cloud_agent != nullptr`
- Call `ShowBambuUserLogin()`, then optionally `get_bambu_login_info()`

**`request_bambu_logout()`** (after `request_user_logout()`, ~line 4568):
- **Intentionally minimal** — calls only `m_bambu_cloud_agent->user_logout(true)` then `get_bambu_login_info()`
- NO preset cleanup, NO device manager changes, NO sync stop (critical isolation)

**`handle_bambu_script_message()`** (after `handle_script_message()`, ~line 4799):
- Parses JSON, on `user_login` command: calls `m_bambu_cloud_agent->change_user()` then `get_bambu_login_info()`
- Does NOT call `request_user_login()` (which triggers EVT_USER_LOGIN -> preset loading, device manager, sync)

**`ShowBambuUserLogin()`** (after `ShowUserLogin()`, ~line 4255):
- Creates `ZUserLogin dlg(m_bambu_cloud_agent)` (agent-parameterized constructor) and shows modal

---

## Phase 5: Refactor ZUserLogin for Agent Injection

### `src/slic3r/GUI/WebUserLoginDialog.hpp`
- Add `#include "slic3r/Utils/ICloudServiceAgent.hpp"`
- Add constructor: `explicit ZUserLogin(std::shared_ptr<ICloudServiceAgent> cloud_agent)`
- Add members: `std::shared_ptr<ICloudServiceAgent> m_cloud_agent`, `bool m_is_bambu_login{false}`

### `src/slic3r/GUI/WebUserLoginDialog.cpp`

**Default constructor** (line 43):
- At line 48, after getting `agent`, add: `if (agent) m_cloud_agent = agent->get_cloud_agent();`
- At line 82, change `agent->get_cloud_login_url(...)` to `m_cloud_agent->get_cloud_login_url(...)`

**New constructor** `ZUserLogin(shared_ptr<ICloudServiceAgent>)`:
- Sets `m_cloud_agent` and `m_is_bambu_login = true`
- Uses `m_cloud_agent->get_cloud_login_url()` for URL
- Same webview setup as default constructor (from line 86 onward)

**`OnDocumentLoaded()`** (line 219):
- Change `agent->get_cloud_service_host()` to `m_cloud_agent->get_cloud_service_host()` so Bambu URL matches correctly

**`OnScriptMessage()`** (line 264):
- Line 273: change `agent && strCmd == "get_login_cmd" && agent->get_cloud_agent()` to `m_cloud_agent && strCmd == "get_login_cmd"`
- Line 276: change `agent->build_login_cmd()` to `m_cloud_agent->build_login_cmd()`
- Lines 322-333: branch `user_login` handler on `m_is_bambu_login`:
  - If bambu: `wxGetApp().handle_bambu_script_message(msg)` (no Orca side effects)
  - If orca: `wxGetApp().handle_script_message(msg)` (existing flow)

---

## Phase 6: Frontend Visibility — Config-Driven, First-Load Hide

The `#BambuCloudSection` is currently **visible by default** (`home.css:119` has no `display: none`). Without an explicit hide, users with `cloud_providers=orca` will see a non-functional Bambu section. This phase is mandatory.

**Product decision**: visibility is solely determined by whether "bambu" is in `cloud_providers`. If "bambu" is configured but the BBL plugin is missing, the section is still shown — the existing `#NoPluginTip` element (`index.html:65-67`) and `network_plugin_installtip` handler already guide the user to install the plugin.

### `resources/web/homepage/css/home.css` (line 119)
Add `display: none` to `#BambuCloudSection`:
```css
#BambuCloudSection {
    display: none;  /* shown by cloud_providers_info from backend */
    border-top: 1px solid;
    width: 262px;
}
```

### `src/slic3r/GUI/WebViewDialog.hpp`
- Add declaration: `void SendCloudProvidersInfo();`

### `src/slic3r/GUI/WebViewDialog.cpp`
- Implement `SendCloudProvidersInfo()`: reads `app_config->get_cloud_providers()`, sends provider list to JS — **no runtime plugin check**, purely config-driven:
  ```json
  {"command": "cloud_providers_info", "providers": ["orca", "bambu"]}
  ```
- Call `SendCloudProvidersInfo()` from `OnNavigationComplete()` (line 600, after `ShowNetpluginTip()`)

### `resources/web/homepage/js/home.js`
- Add `cloud_providers_info` handler in `HandleStudio()`:
  - If "bambu" is in the providers array: `$("#BambuCloudSection").show()`
  - Otherwise: keep hidden (default CSS)
- The existing `network_plugin_installtip` handler continues to work independently — it shows `#NoPluginTip` and hides `#BambuLogin1` when the plugin is missing, regardless of `cloud_providers`

---

## Phase 7: Shutdown Cleanup

### `src/slic3r/GUI/GUI_App.cpp`

**OnExit** (before `delete m_agent` at line 2618):
```cpp
m_bambu_cloud_agent.reset();
```

**`hot_reload_network_plugin()`** (before `delete m_agent` at line 1792):
```cpp
m_bambu_cloud_agent.reset();
```
After reload succeeds (~line 1808), recreate if configured:
```cpp
if (app_config->has_cloud_provider("bambu")) {
    auto& plugin = BBLNetworkPlugin::instance();
    if (plugin.is_loaded() && plugin.has_agent())
        m_bambu_cloud_agent = std::make_shared<BBLCloudServiceAgent>();
}
```

Critical: `m_bambu_cloud_agent` must be reset BEFORE `NetworkAgent::unload_network_module()` since the wrapper delegates to DLL function pointers that become dangling after unload.

---

## Side-Effect Isolation

| Action | Presets | DeviceManager | Sync | Orca Agent | Bambu Agent |
|--------|---------|---------------|------|------------|-------------|
| Orca login | Load | Init | Start | `change_user` | No effect |
| Orca logout | Clean | Clean | Stop | `user_logout` | No effect |
| Bambu login | No effect | No effect | No effect | No effect | `change_user` |
| Bambu logout | No effect | No effect | No effect | No effect | `user_logout` |

Note: BBL DLL callbacks (`set_on_user_login_fn`, `set_on_server_connected_fn`) are registered globally on the singleton plugin agent. The Bambu sidecar does NOT register its own callbacks — it uses synchronous polling (`is_user_login()`, `get_user_name()`) rather than relying on callback-driven state changes. This avoids interference with any callbacks the Orca-side `BBLPrinterAgent` may have registered.

---

## Files Changed

| File | Changes |
|------|---------|
| `src/libslic3r/AppConfig.hpp` | Add `SETTING_CLOUD_PROVIDERS`, 5 new methods |
| `src/libslic3r/AppConfig.cpp` | Migration (preserve legacy Bambu users), default, 5 methods |
| `src/slic3r/Utils/NetworkAgentFactory.cpp` | Always create Orca, remove `use_orca_cloud` |
| `src/slic3r/Utils/NetworkAgentFactory.hpp` | Update doc comment |
| `src/slic3r/GUI/GUI_App.hpp` | Add `m_bambu_cloud_agent`, 6 methods |
| `src/slic3r/GUI/GUI_App.cpp` | Remove guard, store BBL agent, 3 handlers, 5 methods, shutdown |
| `src/slic3r/GUI/WebUserLoginDialog.hpp` | Agent constructor, 2 new members |
| `src/slic3r/GUI/WebUserLoginDialog.cpp` | New constructor, refactor 3 methods |
| `src/slic3r/GUI/WebViewDialog.hpp` | Add `SendCloudProvidersInfo()` |
| `src/slic3r/GUI/WebViewDialog.cpp` | Implement + call from `OnNavigationComplete()` |
| `resources/web/homepage/css/home.css` | Add `display: none` to `#BambuCloudSection` |
| `resources/web/homepage/js/home.js` | Add `cloud_providers_info` handler |

---

## Verification

### Build
1. `cmake --build build --config RelWithDebInfo --target all` — clean compile

### Functional
2. **Orca login**: existing flow unchanged — presets load, device manager inits, sync starts
3. **Bambu login** (config=`orca;bambu`): Bambu dialog opens, `studio_bambu_userlogin` sent, NO preset/sync side effects
4. **Bambu logout**: `studio_bambu_useroffline` sent, NO cleanup
5. **Independence**: Orca logout doesn't affect Bambu; Bambu logout doesn't affect Orca
6. **Host check**: `ZUserLogin::OnDocumentLoaded()` uses `m_cloud_agent->get_cloud_service_host()` so Bambu URL matches

### Visibility
7. **Page load**: `cloud_providers_info` correctly shows/hides `#BambuCloudSection`
8. **Config "orca" only**: `BambuCloudSection` stays hidden (CSS default + no backend show signal)
9. **Config "orca;bambu"**: `BambuCloudSection` visible regardless of plugin state
10. **Config "orca;bambu" + plugin missing**: section visible, `#NoPluginTip` shown inside it, login button hidden (existing `network_plugin_installtip` handler)

### Migration
11. **Legacy `use_orca_cloud=false`**: after upgrade, config reads `cloud_providers=orca;bambu`, Bambu section visible, user can still log into Bambu
12. **Legacy `use_orca_cloud=true`**: after upgrade, config reads `cloud_providers=orca` (default), `use_orca_cloud` key erased
13. **Fresh install**: config reads `cloud_providers=orca`, no legacy key present

### Persistence & Lifecycle
14. **Dual login across page reload**: both `get_login_info` and `get_bambu_login_info` return correct logged-in state after homepage navigation
15. **Dual login across app restart**: BBL DLL persists Bambu session, OrcaCloudServiceAgent persists Orca session via token storage — both should restore on next launch
16. **Shutdown**: `m_bambu_cloud_agent.reset()` before `unload_network_module()` — no dangling DLL calls
17. **Hot reload** (`hot_reload_network_plugin()`): sidecar reset before unload, recreated after reload if configured
