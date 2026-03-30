# Dual Cloud Login on Homepage

## Context

Currently the OrcaSlicer homepage (`resources/web/homepage/index.html`) has a single login area that logs into whichever cloud is configured via `use_orca_cloud`. We need to:
1. Make the main login button **always** login into OrcaCloud
2. Add a **collapsible section** for Bambu Cloud login
3. Support **simultaneous** login to both clouds with independent status

The C++ backend has the underlying cloud agents (`OrcaCloudServiceAgent` and `BBLCloudServiceAgent`) but the homepage message routing is not yet wired for dual sessions (see Prerequisites below). This plan focuses on the frontend changes (HTML/CSS/JS) and defines the message contract the frontend expects from the backend.

## Prerequisites (backend — already done / separate task)

The C++ backend has the underlying cloud agents (`OrcaCloudServiceAgent` and `BBLCloudServiceAgent`) but the homepage message routing is **not yet wired** for dual sessions. Today, `homepage_login_or_register` routes through the singleton `m_agent` (`GUI_App.cpp:4618`), `request_login()` uses that same agent (`GUI_App.cpp:4474`), and the login dialog gets its URL from it (`WebUserLoginDialog.cpp:82`). No handlers exist yet for `get_bambu_login_info`, `homepage_bambu_*`, or `studio_bambu_*` commands.

**Required backend work (separate task, must be done before the frontend is fully functional):**
- The backend will handle `homepage_bambu_login_or_register` by opening the Bambu login dialog (not reusing the current `request_login()` singleton path).
- The backend will handle `homepage_bambu_logout` with a **Bambu-only** logout path that does NOT invoke `request_user_logout()`'s global cleanup (presets, device manager, sync). Today's `request_user_logout()` at `GUI_App.cpp:4548` clears presets, device manager user info, and sync state — that must remain Orca-only.
- The backend will emit `studio_bambu_userlogin` / `studio_bambu_useroffline` responses from the Bambu session independently of Orca status.

## Key Architectural Constraints

- **Two independent login sources**: The homepage must consume Orca status and Bambu status as completely separate data paths. They are fetched, rendered, and updated independently.
- **Provider-specific commands only**: Every login/logout command sent from the homepage must be explicitly provider-scoped (e.g. `homepage_bambu_login_or_register`, not a reuse of `homepage_login_or_register`). The existing Orca commands (`homepage_login_or_register`, `homepage_logout`) must never trigger Bambu-side effects and vice versa.
- **No cross-provider side effects**: Orca logout/cleanup flows (preset removal, device manager cleanup) must not affect Bambu session state. Bambu logout must not reset Orca login.
- **Plugin-missing degradation**: If the BBL network plugin is unavailable, the Bambu section remains visible but degrades — see Step 1 and Step 4e for full behavior spec.

## Design Direction

Match the existing OrcaSlicer design system exactly:
- **Colors**: Teal `#009688` (light) / `#00675B` (dark) from `global.css` CSS variables
- **Buttons**: Existing `ButtonStyleRegular`, `ButtonStyleAlert`, `ButtonStyleConfirm` + `ButtonTypeWindow` classes
- **Dark mode**: Overrides in separate `dark.css`
- **Typography**: System UI font stack already in `home.css`
- **Aesthetic**: A quiet dual-account rail — Orca as the primary identity surface, Bambu as a secondary utility section that expands on demand. Cardless, consistent with current sidebar styling (262px wide). Minimal motion: fast height/opacity reveal for the Bambu section only.

---

## Files to Modify

| File | Changes |
|------|---------|
| `resources/web/homepage/index.html` | Restructure `#LoginArea` with Orca + Bambu sections |
| `resources/web/homepage/js/home.js` | New Bambu login functions, collapsible toggle, updated HandleStudio |
| `resources/web/homepage/css/home.css` | Styles for Bambu collapsible section |
| `resources/web/homepage/css/dark.css` | Dark mode overrides for new elements |
| `resources/web/data/text.js` | New translation entry for "Bambu Cloud" label |

---

## Step 1: Restructure HTML (`index.html`)

Replace the current `#LoginArea` (lines 24-42) with two independent sections:

```
#LoginArea (change from fixed 180px to auto height)
  |
  +-- #OrcaLoginSection (keeps existing logo + login/logout behavior)
  |     +-- #OrcaLogin1 (logged out: logo + "login/register" button)
  |     +-- #OrcaLogin2 (logged in: avatar + name + "log out" button)
  |
  +-- #BambuCloudSection (new collapsible section)
  |     +-- #BambuCloudHeader (clickable: chevron + "Bambu Cloud" label + status dot)
  |     +-- #BambuCloudBody (collapsible, hidden by default)
  |           +-- #BambuLogin1 (logged out: login button)
  |           +-- #BambuLogin2 (logged in: avatar + name + logout button)
  |
  +-- #NoPluginTip (MOVED: now inside #BambuCloudBody, not #LoginArea)
```

**Key HTML details:**
- Rename `#Login1` → `#OrcaLogin1`, `#Login2` → `#OrcaLogin2` (update all JS references **and CSS selectors** — see Step 2)
- `#BambuCloudHeader` has `onClick="ToggleBambuSection()"`
- Chevron is a CSS-rotated SVG arrow (inline, small)
- Status dot: small `<span>` circle, green when logged in, gray when out
- `#BambuLogin1`: compact — just a "Login" button (reuse `ButtonStyleRegular ButtonTypeWindow`)
- `#BambuLogin2`: compact — small avatar (40px) + username + "Log out" button
- The Bambu section has a subtle top border separator
- **`#NoPluginTip` relocation**: Move from `#LoginArea` into `#BambuCloudBody`. Currently it's an absolute overlay covering the entire `#LoginArea` (`home.css:104`), which would block Orca login. Since the plugin tip is about the Bambu network plugin, it must only affect the Bambu section. Change its positioning from absolute-fill to a normal flow element inside the Bambu body.

## Step 2: Add CSS (`home.css`)

```css
/* --- Orca Login Selector Migration --- */
/* Rename selectors to match new HTML IDs */
#OrcaLogin1       → copy rules from old #Login1 (home.css:93-102)
#OrcaLogin2       → copy rules from old #Login2 (home.css:135-142, critical: display:none default)
/* Delete old #Login1 and #Login2 rules */

/* --- LoginArea --- */
#LoginArea        → change height: 180px to min-height: 180px (auto grow)

#OrcaLoginSection → wraps renamed #OrcaLogin1/#OrcaLogin2 (centered column)

#BambuCloudSection {
  border-top: 1px solid;  /* inherits border-color from * rule */
  width: 262px;
}

#BambuCloudHeader {
  height: 40px;
  display: flex;
  align-items: center;
  padding: 0 20px;
  cursor: pointer;
  font-size: 13px;
  color: #A8A8A8;
  gap: 8px;
}
#BambuCloudHeader:hover { color: inherit; }

.bambu-chevron {
  width: 12px; height: 12px;
  transition: transform 0.2s ease;
  transform: rotate(0deg);    /* collapsed: points right */
}
.bambu-chevron.expanded {
  transform: rotate(90deg);   /* expanded: points down */
}

.bambu-status-dot {
  width: 8px; height: 8px;
  border-radius: 50%;
  background-color: #A8A8A8;  /* gray = offline */
  margin-left: auto;
}
.bambu-status-dot.online {
  background-color: #4CAF50;  /* green = logged in */
}

#BambuCloudBody {
  max-height: 0;
  overflow: hidden;
  transition: max-height 0.25s ease;
  display: flex;
  flex-direction: column;
  align-items: center;
}
#BambuCloudBody.expanded {
  max-height: 150px;
}

/* --- NoPluginTip relocation --- */
/* Was: position:absolute covering all of #LoginArea (home.css:104-116) */
/* Now: normal flow inside #BambuCloudBody, scoped to Bambu only */
#NoPluginTip {
  position: static;        /* was: absolute top:0 left:0 bottom:0 right:0 */
  display: none;           /* shown via JS when plugin missing */
  flex-direction: column;
  padding: 5px 10px;
  z-index: auto;           /* was: 1 */
}

#BambuLogin1 { padding: 10px 0; }
#BambuLogin2 {
  display: none;
  flex-direction: column;
  align-items: center;
  padding: 8px 0;
  width: 262px;
}
#BambuAvatarIcon { height: 40px; border-radius: 50%; }
#BambuUserName {
  white-space: nowrap;
  text-overflow: ellipsis;
  overflow: hidden;
  width: 80%;
  text-align: center;
  font-size: 13px;
}
```

## Step 3: Add dark mode overrides (`dark.css`)

```css
#BambuCloudHeader       { color: #818183; }
#BambuCloudHeader:hover { color: #efeff0; }
.bambu-chevron svg path { stroke: currentColor; }
```

(Borders, backgrounds inherit from existing dark.css `*` rule and global variables.)

## Step 4: Update JavaScript (`home.js`)

### 4a. Rename existing login functions

| Old | New | Notes |
|-----|-----|-------|
| `SetLoginInfo(avatar, name)` | `SetOrcaLoginInfo(avatar, name)` | Shows `#OrcaLogin2`, hides `#OrcaLogin1` |
| `SetUserOffline()` | `SetOrcaUserOffline()` | Shows `#OrcaLogin1`, hides `#OrcaLogin2` |

Update all internal references (in `HandleStudio`, `OnInit` callbacks, etc.).

`OnLoginOrRegister()` and `OnLogOut()` keep sending `homepage_login_or_register` / `homepage_logout`. These are Orca-only commands. The backend must route them exclusively to the Orca cloud agent (today they route through the singleton `m_agent` — backend work needed). **These must never be reused for Bambu** — Bambu gets its own distinct commands below.

### 4b. Add new Bambu functions

```javascript
// --- Bambu Cloud Section ---
var bambuSectionExpanded = false;

function ToggleBambuSection() {
  bambuSectionExpanded = !bambuSectionExpanded;
  var body = document.getElementById('BambuCloudBody');
  var chevron = document.querySelector('.bambu-chevron');
  if (bambuSectionExpanded) {
    body.classList.add('expanded');
    chevron.classList.add('expanded');
  } else {
    body.classList.remove('expanded');
    chevron.classList.remove('expanded');
  }
}

function SetBambuLoginInfo(strAvatar, strName) {
  $("#BambuLogin1").hide();
  $("#BambuUserName").text(strName);
  if (strAvatar && strAvatar.trim() !== '') {
    $("#BambuAvatarIcon").prop("src", strAvatar);
  }
  $("#BambuLogin2").show();
  $("#BambuLogin2").css("display", "flex");
  $(".bambu-status-dot").addClass("online");
}

function SetBambuUserOffline() {
  $("#BambuAvatarIcon").prop("src", "img/c.jpg");
  $("#BambuUserName").text('');
  $("#BambuLogin2").hide();
  $("#BambuLogin1").show();
  $(".bambu-status-dot").removeClass("online");
}

function OnBambuLoginOrRegister() {
  var tSend = {};
  tSend['sequence_id'] = Math.round(new Date() / 1000);
  tSend['command'] = "homepage_bambu_login_or_register";
  SendWXMessage(JSON.stringify(tSend));
}

function OnBambuLogOut() {
  var tSend = {};
  tSend['sequence_id'] = Math.round(new Date() / 1000);
  tSend['command'] = "homepage_bambu_logout";
  SendWXMessage(JSON.stringify(tSend));
}

function SendMsg_GetBambuLoginInfo() {
  var tSend = {};
  tSend['sequence_id'] = Math.round(new Date() / 1000);
  tSend['command'] = "get_bambu_login_info";
  SendWXMessage(JSON.stringify(tSend));
}
```

### 4c. Update `HandleStudio` dispatcher

Add two new cases:
```javascript
} else if (strCmd == "studio_bambu_userlogin") {
  SetBambuLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"]);
} else if (strCmd == "studio_bambu_useroffline") {
  SetBambuUserOffline();
}
```

Update existing cases:
```javascript
// "studio_userlogin"  → call SetOrcaLoginInfo (renamed)
// "studio_useroffline" → call SetOrcaUserOffline (renamed)
```

### 4d. Update `OnInit`

Add `SendMsg_GetBambuLoginInfo()` after `SendMsg_GetLoginInfo()`.

### 4e. Plugin-missing behavior (`network_plugin_installtip` handler)

The existing handler at `home.js:99` shows/hides `#NoPluginTip` when `network_plugin_installtip` arrives with `show=1`. With `#NoPluginTip` now inside `#BambuCloudBody`, the revised behavior must:

1. **Auto-expand the Bambu section** when `show=1` so the tip is visible (don't leave it hidden inside a collapsed body).
2. **Hide `#BambuLogin1`** (the login button) since plugin is missing — login can't work.
3. **Show `#NoPluginTip`** inside the Bambu body with `display:flex`.
4. When `show=0` (plugin installed), reverse: hide `#NoPluginTip`, restore `#BambuLogin1`.
5. **Do NOT disable `OnBambuLoginOrRegister()`** — instead just hide the button; the tip's "click here to install" link serves as the action.

Updated handler:
```javascript
} else if (strCmd == "network_plugin_installtip") {
  let nShow = pVal["show"] * 1;
  if (nShow == 1) {
    // Auto-expand Bambu section to show the tip
    if (!bambuSectionExpanded) ToggleBambuSection();
    $("#BambuLogin1").hide();
    $("#NoPluginTip").show();
    $("#NoPluginTip").css("display", "flex");
  } else {
    $("#NoPluginTip").hide();
    // Only restore login button if not already logged in
    if ($("#BambuLogin2").is(":hidden")) {
      $("#BambuLogin1").show();
    }
  }
}
```

**CSS note**: Remove `max-height:150px` constraint from `#BambuCloudBody.expanded` — use a larger value (e.g. `200px`) or `max-height:none` to ensure the plugin tip + any login UI fits without clipping.

## Step 5: Add translation entries (`text.js`)

Add a new TID (e.g. `orca6`) for "Bambu Cloud" section header label. **Must be added to all 15 locale blocks** in `resources/web/data/text.js` (en, ca_ES, es_ES, it_IT, de_DE, cs_CZ, fr_FR, zh_CN, zh_TW, ru_RU, ko_KR, tr_TR, pl_PL, pt_BR, lt_LT). The `TranslatePage()` function at line 1689 skips missing keys silently (falls back to HTML text), so missing translations won't crash but will show English in non-English locales — which is acceptable for initial implementation but should be completed for all locales.

---

## Message Protocol

The homepage consumes two independent login sources. Every command is explicitly provider-scoped — no ambiguous reuse.

### Orca commands (existing shape — must become Orca-only via backend work)
| Direction | Command | Purpose |
|-----------|---------|---------|
| JS → C++ | `get_login_info` | Request Orca login status on init |
| JS → C++ | `homepage_login_or_register` | Open Orca login flow |
| JS → C++ | `homepage_logout` | Orca logout only (must not affect Bambu) |
| C++ → JS | `studio_userlogin` | Orca user logged in `{avatar, name}` |
| C++ → JS | `studio_useroffline` | Orca user logged out |

### Bambu commands (new — Bambu-only)
| Direction | Command | Purpose |
|-----------|---------|---------|
| JS → C++ | `get_bambu_login_info` | Request Bambu login status on init |
| JS → C++ | `homepage_bambu_login_or_register` | Open Bambu login flow |
| JS → C++ | `homepage_bambu_logout` | Bambu logout only (must not affect Orca) |
| C++ → JS | `studio_bambu_userlogin` | Bambu user logged in `{avatar, name}` |
| C++ → JS | `studio_bambu_useroffline` | Bambu user logged out |

---

## Verification

### Homepage UI
1. **Orca out, Bambu out**: Orca primary CTA shown, Bambu collapsed section available
2. **Orca in only**: Orca status visible, Bambu remains collapsed/logged out
3. **Bambu in only**: Orca CTA visible, Bambu section shows logged-in status after expand
4. **Both in**: Both provider statuses visible independently, no replacement of one by the other
5. **Collapsible toggle**: Click "Bambu Cloud" header — body expands/collapses with animation, chevron rotates
6. **Status dot**: Green when Bambu logged in, gray when logged out
7. **Dark mode**: All new elements have proper contrast with `dark.css` active
8. **Sidebar height**: LeftBoard accommodates expanded Bambu section without layout breakage

### Independence
9. **Orca logout**: Call `SetOrcaUserOffline()` — Bambu stays logged in
10. **Bambu logout**: Call `SetBambuUserOffline()` — Orca stays logged in
11. **No cross-contamination**: Verify `OnLogOut()` sends only `homepage_logout`, `OnBambuLogOut()` sends only `homepage_bambu_logout`

### Degradation
12. **Plugin missing**: When `network_plugin_installtip` show=1: Bambu section auto-expands, `#BambuLogin1` is hidden, `#NoPluginTip` is visible inside the Bambu body with the install link

### Regression
13. **Recent projects**: Existing recent-file actions still work unchanged
14. **Existing Orca flow**: `homepage_login_or_register` / `homepage_logout` commands unchanged in shape
