Architectural Blueprint for Unified Identity in High-Performance Desktop and Web Ecosystems: A Supabase Integration Strategy for OrcaSlicer

  

## 1. Executive Context and Strategic Alignment

  

The modern landscape of open-source software development is witnessing a paradigm shift where high-performance native applications, such as OrcaSlicer, are no longer viewed as isolated silos of computation but rather as connected nodes within a broader, cloud-enabled ecosystem. The user's directive to integrate Supabase authentication into a C++/wxWidgets desktop environment, while simultaneously laying the groundwork for a future web application, represents a sophisticated architectural challenge. This transition moves the application from a state of local autarky to one of federated identity, requiring a rigorous re-evaluation of security primitives, state management, and cross-platform compatibility.

  

This report serves as a comprehensive design document for implementing a safe, optimized, and future-proof authentication workflow. It eschews the simplified "tutorial" approach in favor of a deep architectural analysis suitable for implementation by senior engineering teams. The core objective is to leverage Supabase’s identity infrastructure—built upon GoTrue and PostgreSQL—to create a unified authentication layer that serves both the immediate needs of the compiled desktop binary and the eventual requirements of a browser-based counterpart.

  

The proposed architecture rests on three pillars: **Security via Proof Key for Code Exchange (PKCE)**, **User Experience via Loopback Interface Redirection**, and **Persistence via OS-Native Secure Storage**. By adhering to these standards, OrcaSlicer will not only secure user data but also establish a scalable foundation where identity and authorization rules defined today in PostgreSQL Row Level Security (RLS) policies will seamlessly govern the future web application without code duplication or security regression.1

  

## 2. Authentication Protocol Architecture

  

### 2.1 The Necessity of PKCE in Public Clients

  

In the domain of OAuth 2.0, applications are categorized based on their ability to maintain the confidentiality of a client secret. OrcaSlicer, distributed as a compiled binary for Windows, macOS, and Linux, falls squarely into the category of a "Public Client." It is axiomatically impossible for a public client to store a `client_secret` securely; any string embedded in the binary can be extracted via reverse engineering or simple string analysis tools. Consequently, the traditional Authorization Code Flow, which relies on a static secret to authenticate the client to the Identity Provider (IdP), is fundamentally insecure for this use case.4

  

The architectural solution mandated for OrcaSlicer is the **Authorization Code Flow with Proof Key for Code Exchange (PKCE)**. PKCE mitigates the threat of authorization code interception—a vector where a malicious application on the user's device captures the temporary code returned by the IdP—by introducing a dynamic, cryptographic secret generated at runtime.

  

The mechanism operates through a two-step verification process that binds the initial authorization request to the subsequent token exchange.

  

1. **The Commitment (Code Challenge):** At the initiation of the login flow, the OrcaSlicer client generates a high-entropy random string known as the `code_verifier`. This verifier is then hashed using SHA-256 and encoded using Base64URL to produce the `code_challenge`. This challenge is sent to Supabase in the initial GET request.4

2. **The Proof (Code Verifier):** Supabase stores the challenge. Upon successful user authentication, Supabase returns an Authorization Code. The client then sends this code _and the original plain-text verifier_ to the token endpoint. Supabase hashes the received verifier; if the result matches the stored challenge, the token is issued.

  

This cryptographic binding ensures that even if an attacker intercepts the Authorization Code, they cannot exchange it for an Access Token because they do not possess the `code_verifier`, which remains resident in the legitimate application's memory.5

  

### 2.2 Rejection of the Implicit Flow

  

Historical implementations of client-side authentication often utilized the Implicit Flow, where access tokens were returned directly in the redirect URL fragment. This approach is now universally deprecated for native applications and strongly discouraged for web applications due to security vulnerabilities, including access token leakage in browser history and referrer headers.

  

Supabase fully supports the PKCE flow, and crucially, the ecosystem is moving towards making PKCE the default even for web clients (via libraries like `@supabase/ssr`). For OrcaSlicer, adopting PKCE is not merely a "desktop" decision but a strategic alignment with the future web application's architecture. By standardizing on PKCE now, the development team ensures that the backend policies and flow logic remain consistent across both platforms, fulfilling the "future-proof" requirement of the project scope.1

  

### 2.3 Protocol Flow Diagram Integration

  

The interaction between the C++ client and Supabase services involves a specific sequence of HTTP operations. Understanding the exact parameter requirements is critical for the C++ network implementation, as there is no official SDK to abstract these details.3

  

| Step | Actor      | Action                                                            | Endpoint / Details                                                                                                                                               |
|------|------------|-------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1    | OrcaSlicer | Generate code_verifier (32 bytes random) & code_challenge (S256). | Internal Cryptography                                                                                                                                            |
| 2    | OrcaSlicer | Start Local TCP Listener (Loopback).                              | 127.0.0.1:{port}                                                                                                                                                 |
| 3    | OrcaSlicer | Launch System Browser.                                            | URL: https://auth.orcaslicer.com/auth/v1/authorize?provider=github&code_challenge=<...>&code_challenge_method=S256&redirect_uri=http://localhost:{port}/callback |
| 4    | User       | Authenticates via IdP (e.g., GitHub, Google).                     | Browser Interaction                                                                                                                                              |
| 5    | Supabase   | Redirects Browser with Auth Code.                                 | http://localhost:{port}/callback?code=AUTH_CODE&state=STATE                                                                                                      |
| 6    | OrcaSlicer | Intercepts Request, extracts code.                                | TCP Socket Read                                                                                                                                                  |
| 7    | OrcaSlicer | Exchange Code for Session.                                        | POST https://auth.orcaslicer.com/auth/v1/token                                                                                                                   |
| 8    | Supabase   | Validates Verifier, Issues JWTs.                                  | Returns access_token, refresh_token                                                                                                                              |

  

This flow demonstrates that the "Simple" requirement is met not by reducing security, but by utilizing standard, robust primitives (HTTP/TCP) that are well-supported in the wxWidgets and C++ ecosystem.

  

## 3. Desktop User Experience and Redirect Strategy

  

A critical differentiator between a web app and a desktop app is the mechanism of returning control to the application after authentication. The user must leave the application context to authenticate in a trusted browser (the system browser), preventing the desktop app from ever seeing the user's password. The challenge lies in smoothly returning the user to OrcaSlicer.

  

### 3.1 The Superiority of Loopback Interface Redirection

  

For desktop operating systems (Windows, macOS, Linux), the **Loopback Interface Redirection** is the gold standard for user experience and reliability. This method involves the desktop application spinning up a lightweight HTTP server on the local machine (localhost) to listen for the redirect from the Identity Provider.11

  

The advantages of this approach over Custom URI Schemes are manifold:

  

1. **Reliability:** It does not rely on OS-level registry keys or protocol handlers, which can be corrupted or blocked by aggressive security software.

2. **User Experience:** The application can serve a custom HTML response to the browser (e.g., "Authentication Successful. You can close this window."), providing immediate visual feedback.

3. **Portability:** It requires no installation privileges (unlike registering a protocol handler on Windows, which often requires Admin rights during setup).

  

#### 3.1.1 Implementation Logistics and Port Selection

  

The implementation within OrcaSlicer requires the use of `wxSocketServer`. A specific design consideration is the port number. RFC 8252 recommends using an ephemeral port (port 0) to avoid conflicts. However, Supabase's redirect URL configuration historically required exact matching, which complicates the use of random ports.13

  

Recent updates to Supabase Auth allow for wildcard redirect URLs (e.g., `http://localhost:3000/*` or `http://localhost:*`), which theoretically supports dynamic ports.14 However, the research indicates that wildcard support can be nuanced, with distinctions between `*` (one segment) and `**` (recursive segments).

  

**Recommendation:** To ensure maximum stability and simplicity (satisfying the user's constraints), OrcaSlicer should initially attempt to bind to a fixed, high-range port (e.g., `41172`). This simplifies the Supabase configuration to a single "Allowed Redirect URL."

  

- **Primary Configuration:** `http://localhost:41172/callback`

- **Conflict Resolution:** If port 41172 is in use, the application can fail gracefully with a message instructing the user to close conflicting applications, or implement a fallback range (41173, 41174) if the project's Supabase configuration is set to allow these specific variants.15

  

### 3.2 Custom URI Schemes: The Fallback and Future-Proofer

  

While Loopback is preferred for the authentication event itself, **Custom URI Schemes** (e.g., `orcaslicer://`) are essential for the "Future Web App" requirement. Deep linking allows the web application to trigger actions in the desktop application, such as "Open Model in Slicer" or "Sync Profile."

  

Therefore, even if Loopback is used for Auth, the architecture should include the registration of a Custom URI Scheme to support the broader ecosystem.

  

#### 3.2.1 Cross-Platform Registration Mechanics

  

Registering a custom scheme requires platform-specific installation steps, usually handled by the installer (InnoSetup for Windows, DMG for macOS, Deb/RPM for Linux).

  

- Windows: The registry must be modified to associate the orcaslicer protocol with the executable path.

HKEY_CLASSES_ROOT

orcaslicer

(Default) = "URL:OrcaSlicer Protocol"

URL Protocol = ""

shell

open

command

(Default) = "C:\Program Files\OrcaSlicer\OrcaSlicer.exe" "%1"

This is typically handled in the `` section of the InnoSetup script.16

- **macOS:** The application's `Info.plist` file must include the `CFBundleURLTypes` key.

```xml

<key>CFBundleURLTypes</key>

<array>

<dict>

<key>CFBundleURLName</key>

<string>com.orcaslicer.url</string>

<key>CFBundleURLSchemes</key>

<array>

<string>orcaslicer</string>

</array>

</dict>

</array>

```

This registers the scheme with LaunchServices upon application install.17

- **Linux:** This is the most fragmented environment. The standard approach is the XDG Desktop Entry specification. A `.desktop` file must be installed in `/usr/share/applications/` or `~/.local/share/applications/` containing `MimeType=x-scheme-handler/orcaslicer;`. Subsequently, the `xdg-mime` database must be updated. This complexity reinforces why Loopback is preferred for the initial Auth implementation on Linux, as it avoids the dependency on correct desktop environment configuration.19

  

### 3.3 Handling Multiple Environments

  

To support "Safe" and "Future-proof" development, the redirect URI logic must account for development versus production environments. The Supabase project likely has different redirect URLs configured for testing (`localhost`) versus the future web app domain (`https://slices.orcaslicer.com`).

  

The C++ application should dynamically select the redirect URI based on its build configuration (Debug vs Release). This prevents development tests from accidentally triggering production flows or vice-versa, ensuring the integrity of the authentication data.13

  

## 4. Cryptographic Primitives and Secure Storage

  

The core of a "Safe" authentication workflow is the handling of the Refresh Token. Unlike the short-lived Access Token, the Refresh Token provides persistent access to the user's account. Storing this token in plain text (e.g., in a JSON config file alongside printer settings) is a critical vulnerability that compromises the user's entire identity identity.22

  

### 4.1 The wxSecretStore Abstraction

  

The design mandates the use of `wxSecretStore`, a component of wxWidgets 3.1.1+ that abstracts the operating system's native secure credential storage APIs. This ensures that the sensitive tokens are encrypted using the user's system login credentials, making them inaccessible to other users on the same machine or to external attackers who might access the file system.24

  

### 4.2 Windows: Credential Manager

  

On Windows, `wxSecretStore` utilizes the Windows Credential Manager. This is a robust, encrypted vault integrated into the OS.

  

- **Security Context:** Credentials are stored in the "Generic Credentials" section.

- **Isolation:** The API ensures that only the user who stored the credential can retrieve it.

- **Portability:** OrcaSlicer is often used as a portable application. Windows Credential Manager supports this well, as the storage is tied to the _User Profile_, not the application installation directory. This means a user can update the portable app folder without losing their login session.26

  

### 4.3 macOS: Keychain Services

  

On macOS, the integration maps to the System Keychain.

  

- **Access Control:** macOS enforces strict access control lists (ACLs). Access to a keychain item is restricted to the application that created it.

- **Code Signing:** It is imperative that the OrcaSlicer binary be code-signed. If the binary is unsigned or if the signature changes (e.g., a nightly build vs. a release build), the OS will prompt the user to explicitly grant permission to access the keychain item ("OrcaSlicer wants to access key..."). The build pipeline must ensure consistent signing to maintain a seamless user experience.27

  

### 4.4 Linux: The Dependency Challenge

  

Linux presents the most significant implementation challenge for this architecture. The standard backend for `wxSecretStore` on Linux is `libsecret`, which communicates with the Secret Service API (provided by GNOME Keyring or KWallet).22

  

**The Problem:** OrcaSlicer is distributed effectively as an AppImage or standalone binary to function across various distributions (Ubuntu, Fedora, Arch, NixOS). However, `libsecret` is a runtime dependency that may not be present on all systems, particularly minimal window managers (i3, Sway) or headless render servers.

  

- **Dependency Hell:** Research indicates that missing `libsecret` libraries prevents the secure store from initializing. Furthermore, different distributions package it differently (e.g., `libsecret-1-0` vs `libsecret-1-dev`), complicating the build process.30

- **Build System Impact:** The `CMakeLists.txt` for OrcaSlicer must be updated to conditionally find and link `libsecret`. On NixOS specifically, the package definition requires explicit overrides to include `libsecret` in the build inputs, otherwise, the build will fail or the feature will be disabled silently.31

  

Architectural Fallback:

  

To be "Safe" yet "Simple," the design must include a graceful degradation strategy for Linux.

  

1. **Attempt:** Initialize `wxSecretStore::GetDefault()`.

2. **Check:** Call `IsOk()`.

3. **Fallback:** If the OS store is unavailable, the application should fall back to a proprietary encryption file (e.g., AES-256 encrypted file in `~/.config/OrcaSlicer/`). While less secure than the OS keyring (as the key management becomes the app's responsibility), it is a necessary compromise to ensure functionality on fragmented Linux ecosystems. The encryption key for this fallback should be derived from machine-specific identifiers (e.g., `/etc/machine-id`) to prevent portability of the token file.22

  

## 5. Network Transport and API Integration

  

The current tree already ships a concrete transport stack that should be reused for Supabase rather than inventing a new client. Two key pieces drive all HTTP I/O today:

  

- **`Http` wrapper (`src/slic3r/Utils/Http.*`)**: A thin layer over libcurl with synchronous `perform_sync`, 30s max timeouts, optional CA configuration, header injection, and completion/error callbacks. Global extra headers can be set once and applied to every request.

- **`OrcaNetwork` (`src/slic3r/Utils/OrcaNetwork.*`)**: An `INetworkAgent` implementation that wires the `Http` helpers into application features (server health check, username/password login, presets CRUD). It defaults to `backend_url = http://localhost:8080` for the in-repo Flask simulator and pushes bearer tokens plus any extra headers into each request.

  

### 5.1 What the code already does

  

1. **Health + connectivity**: `connect_server()` calls `GET /api/v1/health`; success flips `is_connected` and fires `OnServerConnectedFn`. All HTTP helpers propagate non-2xx responses to `on_http_error_fn`.

2. **Auth/session handling**:

- `change_user()` supports two modes: a WebView message (`command=user_login` with `token`, `user_id`, etc.) that is passed to `set_user_session()`, and a traditional username/password POST to `/api/v1/auth/login` parsed via Boost.PropertyTree.

- `session_token` lives in memory and is automatically attached as `Authorization: Bearer <token>` inside every `http_*` helper.

- `user_logout(request=true)` POSTs `/api/v1/auth/logout`, then clears local session state and triggers `OnUserLoginFn`.

3. **Headers and API keys**: `set_extra_http_header()` stores a map that is merged into every request. This is the hook for the Supabase `apikey` header without touching call sites.

4. **Threading**: The HTTP helpers run synchronously, but long-running flows (e.g., `get_setting_list2` sync) wrap the calls in detached `std::thread` blocks and report progress through callbacks to avoid blocking the wx main loop.

5. **TLS**: The wrapper exposes `ca_file(...)` and `Http::tls_system_cert_store()`; we can point Supabase calls at `https://<project>.supabase.co` by setting `backend_url` and CA data instead of adding a new SSL layer.

  

### 5.2 Mapping this to Supabase Auth

  

Reuse the existing surface instead of creating a `NetworkClient` singleton:

  

- **Base URL**: Set `backend_url` to the Supabase REST origin (e.g., `https://<project>.supabase.co`) when the Supabase feature flag is on.

- **Headers**: Call `set_extra_http_header({{"apikey", "<anon_key>"}})` once at startup; `Authorization` will carry the Supabase access token stored in `session_token`.

- **Token ingress**: After the PKCE browser flow returns, populate `session_token` and user fields via `set_user_session(...)`. The WebView-path already accepts a JSON blob with `token`, `user_id`, `username`, `name`, `nickname`, and `avatar`, so the auth controller only needs to translate the Supabase response into that shape.

- **API calls**: Supabase endpoints can plug directly into the existing helpers (`http_post`, `http_get`, etc.). Error bodies already flow to `OnHttpErrorFn` for UI messaging.

- **Background refresh (gap)**: Token rotation is not yet implemented in `OrcaNetwork`. Add a refresh routine that uses `http_post("/auth/v1/token", ...)` inside a worker thread, then atomically updates `session_token` and persists the new refresh token to secure storage (see §4) before swapping it into memory.

  

### 5.3 Practical migration steps

  

1. Initialize libcurl once via `Http::tls_global_init()` at startup (currently implicit) and point CA resolution to the system store or a bundled `cacert.pem`.

2. During feature rollout, gate the Supabase `backend_url` + headers behind a build flag so the existing Flask simulator continues to work for tests.

3. Keep using the callback plumbing that already exists (`OnUserLoginFn`, `OnServerConnectedFn`, `OnHttpErrorFn`) to drive UI state; only the payloads and endpoints change.

4. When PKCE/WebView login lands, prefer the WebView message path to avoid threading new UI state into `change_user()`’s username/password branch.

  

## 6. Future-Proofing: Web Application Synergy

  

The requirement to support a future web application drives several architectural decisions. The goal is to avoid implementing business logic or security rules twice (once in C++, once in JS).

  

### 6.1 The Database as the Source of Truth

  

The architecture treats the Supabase Database (PostgreSQL) as the sole arbiter of permission. The desktop application should not contain logic like if (user.isPremium) { enableFeature() }.

  

Instead, the application should query the database, and the Row Level Security (RLS) policies should determine what data is returned.

  

- **Mechanism:** When the desktop app sends a query `SELECT * FROM printers`, the request includes the User's JWT. Postgres inspects the JWT, extracts the User ID, and filters the rows based on the policy `auth.uid() = owner_id`.2

- **Benefit:** When the web application is built, it will use the same Supabase JS Client. Because the security is in the database (RLS), the web app automatically inherits the same security model. No new backend code is needed to secure the web interface.

  

### 6.2 Shared State Considerations

  

The snippets highlight a complexity: sharing state between desktop and web.38

  

- **Separation:** It is recommended _not_ to try and share the actual session (Access Token) between the desktop and web browser directly. They should be treated as separate sessions (devices).

- **Synchronization:** State synchronization (e.g., "User added a printer on Web, it should appear on Desktop") should be handled via **Supabase Realtime**. The desktop app can subscribe to database changes (`supabase.channel('printers').on(...)`). When the web app inserts a row, the desktop app receives a push notification and updates the UI. This is superior to sharing auth tokens and creates a responsive "multi-device" experience.2

  

## 7. Implementation Roadmap & Code Structure

  

To satisfy the "High-level design" requirement, we outline the recommended class structure for the implementation within the OrcaSlicer codebase.

  

### 7.1 Class: `AuthManager` (Singleton)

  

This is the central controller.

  

- **Dependencies:** `NetworkClient`, `wxSecretStore`.

- **Methods:**

- `Login()`: Orchestrates the flow (Generate PKCE -> Start Server -> Open Browser).

- `Logout()`: Revokes token on server, clears Secure Store.

- `GetAccessToken()`: Returns valid token, handling refresh transparently if expired.

- `IsLoggedIn()`: Boolean check for UI state.

  

### 7.2 Class: `AuthServer` (wxSocketServer)

  

A specialized, short-lived TCP server.

  

- **Logic:**

1. Bind to `127.0.0.1:41172`.

2. `WaitForAccept()` with a timeout (e.g., 5 minutes).

3. On connection, read the request buffer.

4. Parse `GET /callback?code=...`.

5. Validate `state` parameter matches the one generated in `AuthManager`.

6. Send HTTP 200 response with HTML payload ("You may close this tab").

7. Signal `AuthManager` with the code.

8. Self-terminate.

  

### 7.3 Integration into `Slic3r::GUI`

  

- **Initialization:** On app launch (`OnInit`), `AuthManager` attempts to load a session from storage. If successful, it validates connectivity.

- **UI Elements:**

- A new "UserButton" in the top-right of the Plater.

- A "Sign In" modal dialog explaining the benefits (Cloud Sync, Remote Monitoring).

- The "Future Web App" button, which uses the `NetworkClient` to generate a magic link or simply opens the web dashboard, leveraging the shared database backend.

  

## 8. Security Audit and Threat Modeling

  

A safe architecture requires anticipating attack vectors.

  

| Threat               | Vulnerability                            | Mitigation in Design                                                                                  |
|----------------------|------------------------------------------|-------------------------------------------------------------------------------------------------------|
| Code Interception    | Malicious app listens on port 41172.     | PKCE: The attacker lacks the code_verifier, rendering the intercepted code useless.                   |
| Token Theft          | Malware copies config files.             | Secure Storage: Tokens are in OS Vaults, not files. Access requires user login/biometrics.            |
| Man-in-the-Middle    | Network sniffing at public WiFi.         | TLS/SSL: Strict HTTPS enforcement. Optional Certificate Pinning in libcurl.                           |
| Privilege Escalation | Client modifies binary to bypass checks. | RLS: Security logic is server-side (Postgres). Modified client still cannot read data it doesn't own. |


  

## 9. Conclusion

  

The integration of Supabase into OrcaSlicer is a transformative step that bridges the gap between high-performance local computing and cloud-based collaborative workflows. This design prioritizes **Safety** through PKCE and RLS, **Optimization** through efficient C++ networking and Realtime sync, and **Future-Proofing** by adhering to standard protocols (OAuth 2.0) that govern the web.

  

By implementing the **Loopback Flow** for authentication and relying on **OS-Native Secure Storage**, OrcaSlicer avoids the pitfalls of embedded browser insecurities and fragile file-based storage. This architecture not only satisfies the immediate requirement but positions OrcaSlicer as a modern, connected platform ready for its web-based expansion.