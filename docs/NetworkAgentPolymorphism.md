# Network Agent Polymorphism Guide

## Overview

The network agent system now uses **polymorphism** to enable seamless switching between NetworkAgent (original) and OrcaNetwork (new implementation) at runtime.

## Architecture

```
┌──────────────────────────────────┐
│       INetworkAgent              │
│     (Pure Virtual Interface)     │
│                                  │
│  • All ~80 methods as pure       │
│    virtual functions             │
│  • Defines the contract          │
└───────────┬──────────────────────┘
            │
            │ implements
     ┌──────┴──────┐
     │             │
     ▼             ▼
┌─────────┐   ┌──────────────┐
│NetworkAgent  │  OrcaNetwork │
│(Original)│   │  (New Impl)  │
│          │   │              │
│ Dynamic  │   │ Native C++   │
│ Library  │   │ + Backend    │
│ Wrapper  │   │ Service      │
└──────────┘   └──────────────┘
```

## Components

### 1. INetworkAgent Interface

**File:** `src/slic3r/Utils/INetworkAgent.hpp`

Pure virtual interface defining ~80 methods that all network agents must implement:

```cpp
class INetworkAgent {
public:
    virtual ~INetworkAgent() = default;

    // Lifecycle
    virtual int init_log() = 0;
    virtual int start() = 0;

    // User management
    virtual bool is_user_login() = 0;
    virtual int change_user(std::string user_info) = 0;

    // Settings sync
    virtual int get_user_presets(...) = 0;

    // ... ~75 more methods
};
```

### 2. OrcaNetwork Implementation

**File:** `src/slic3r/Utils/OrcaNetwork.hpp`

Now derives from INetworkAgent:

```cpp
class OrcaNetwork : public INetworkAgent {
public:
    explicit OrcaNetwork(std::string log_dir);
    ~OrcaNetwork() override;

    // All interface methods implemented with 'override' keyword
    int init_log() override;
    bool is_user_login() override;
    // ... etc
};
```

### 3. NetworkAgentFactory

**File:** `src/slic3r/Utils/NetworkAgentFactory.hpp`

Factory for creating network agents polymorphically:

```cpp
class NetworkAgentFactory {
public:
    // Create based on flag
    static std::unique_ptr<INetworkAgent> create(
        const std::string& log_dir,
        bool use_orca_network = false);

    // Create specific implementations
    static std::unique_ptr<INetworkAgent> create_orca_network(
        const std::string& log_dir);

    static std::unique_ptr<INetworkAgent> create_network_agent(
        const std::string& log_dir);
};

// Helper function
template<typename AppConfigType>
std::unique_ptr<INetworkAgent> create_agent_from_config(
    const std::string& log_dir,
    AppConfigType* app_config);
```

## Usage Examples

### Basic Usage - Direct Creation

```cpp
#include "NetworkAgentFactory.hpp"

// Create OrcaNetwork
auto agent = NetworkAgentFactory::create_orca_network("/path/to/logs");

// Use polymorphically
agent->init_log();
agent->start();
agent->connect_server();

std::string user_info = R"({"username":"test","password":"pass"})";
agent->change_user(user_info);

if (agent->is_user_login()) {
    std::string user_id = agent->get_user_id();
}
```

### Configuration-Based Creation

```cpp
#include "NetworkAgentFactory.hpp"
#include "slic3r/GUI/GUI_App.hpp"

// Create based on AppConfig setting
auto agent = create_agent_from_config(
    data_dir(),
    wxGetApp().app_config
);

// Agent type determined by "use_orca_network" config setting
// No code changes needed when switching implementations
```

### Manual Selection

```cpp
bool use_orca = true;  // Could come from UI, config, command line, etc.

auto agent = NetworkAgentFactory::create(data_dir(), use_orca);

// Same code works with either implementation
agent->set_queue_on_main_fn([](auto fn) { wxGetApp().CallAfter(fn); });
agent->set_on_user_login_fn([](int online, bool login) {
    if (login) std::cout << "Logged in!\n";
});
```

### Integration with Existing Code

#### Before (Direct NetworkAgent)

```cpp
// OLD: Hardcoded to NetworkAgent
NetworkAgent* m_agent = new NetworkAgent(log_dir);
m_agent->init_log();
m_agent->connect_server();
```

#### After (Polymorphic)

```cpp
// NEW: Polymorphic - can be either implementation
INetworkAgent* m_agent = NetworkAgentFactory::create(log_dir, use_orca).release();
m_agent->init_log();
m_agent->connect_server();
```

Same code, works with either implementation!

### Using in Manager Classes

#### DeviceManager Example

```cpp
// DeviceManager.hpp
class DeviceManager {
private:
    INetworkAgent* m_agent{nullptr};  // Was: NetworkAgent*

public:
    void set_agent(INetworkAgent* agent) {  // Was: NetworkAgent*
        m_agent = agent;
    }

    void some_operation() {
        if (m_agent) {
            m_agent->connect_printer(...);
        }
    }
};
```

#### TaskManager Example

```cpp
// TaskManager.hpp
class TaskManager {
private:
    INetworkAgent* m_agent{nullptr};

public:
    void start_print_job(PrintParams params) {
        if (m_agent) {
            m_agent->start_print(params, ...);
        }
    }
};
```

## AppConfig Integration

### Adding the Setting

In initialization code (e.g., `GUI_App.cpp`):

```cpp
// Set default value
app_config->set("use_orca_network", "false");  // Default to NetworkAgent

// Optional: Set backend URL for OrcaNetwork
app_config->set("orca_backend_url", "http://localhost:8080");
```

### Reading the Setting

```cpp
bool use_orca = app_config->get("use_orca_network") == "true";

if (use_orca) {
    auto orca_net = NetworkAgentFactory::create_orca_network(log_dir);

    // Configure OrcaNetwork-specific settings
    std::string backend_url = app_config->get("orca_backend_url");
    if (!backend_url.empty()) {
        dynamic_cast<OrcaNetwork*>(orca_net.get())->set_backend_url(backend_url);
    }

    m_agent = orca_net.release();
} else {
    // Initialize NetworkAgent as before
    NetworkAgent::initialize_network_module();
    m_agent = new NetworkAgent(log_dir);
}
```

### Safer Helper Function

```cpp
INetworkAgent* create_and_configure_agent(
    const std::string& log_dir,
    AppConfig* app_config)
{
    bool use_orca = app_config->get("use_orca_network") == "true";

    if (use_orca) {
        auto agent = std::make_unique<OrcaNetwork>(log_dir);

        // Configure OrcaNetwork-specific settings
        std::string backend_url = app_config->get("orca_backend_url");
        if (!backend_url.empty()) {
            agent->set_backend_url(backend_url);
        }

        return agent.release();
    } else {
        // NetworkAgent setup
        NetworkAgent::initialize_network_module();
        return reinterpret_cast<INetworkAgent*>(new NetworkAgent(log_dir));
    }
}
```

## Preferences UI Example

### Adding Toggle in Preferences

```cpp
// In Preferences.cpp

// Network Backend Selection
auto* network_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Network Backend");

auto* bambu_radio = new wxRadioButton(
    panel, wxID_ANY, "Bambu Network (Production)",
    wxDefaultPosition, wxDefaultSize, wxRB_GROUP
);

auto* orca_radio = new wxRadioButton(
    panel, wxID_ANY, "OrcaNetwork (Testing/Development)"
);

// Set current selection
bool use_orca = app_config->get("use_orca_network") == "true";
bambu_radio->SetValue(!use_orca);
orca_radio->SetValue(use_orca);

// Backend URL field (only for OrcaNetwork)
auto* url_label = new wxStaticText(panel, wxID_ANY, "Backend URL:");
auto* url_text = new wxTextCtrl(
    panel, wxID_ANY,
    app_config->get("orca_backend_url")
);
url_text->Enable(use_orca);

// Event handlers
orca_radio->Bind(wxEVT_RADIOBUTTON, [=](wxCommandEvent&) {
    url_text->Enable(true);
    app_config->set("use_orca_network", "true");
});

bambu_radio->Bind(wxEVT_RADIOBUTTON, [=](wxCommandEvent&) {
    url_text->Enable(false);
    app_config->set("use_orca_network", "false");
});

url_text->Bind(wxEVT_TEXT, [=](wxCommandEvent&) {
    app_config->set("orca_backend_url", url_text->GetValue().ToStdString());
});

network_box->Add(bambu_radio, 0, wxALL, 5);
network_box->Add(orca_radio, 0, wxALL, 5);
network_box->Add(url_label, 0, wxALL | wxLEFT, 20);
network_box->Add(url_text, 0, wxALL | wxLEFT | wxEXPAND, 20);
```

## Migration Guide

### Step 1: Update Type Declarations

**Before:**
```cpp
NetworkAgent* m_agent;
```

**After:**
```cpp
INetworkAgent* m_agent;
```

### Step 2: Update Creation

**Before:**
```cpp
m_agent = new NetworkAgent(log_dir);
```

**After:**
```cpp
m_agent = NetworkAgentFactory::create(log_dir, use_orca).release();
```

### Step 3: No Changes Needed for Usage

All method calls remain the same:
```cpp
m_agent->connect_server();        // Works with both
m_agent->is_user_login();         // Works with both
m_agent->get_user_presets(...);   // Works with both
```

### Step 4: Update Includes

**Before:**
```cpp
#include "NetworkAgent.hpp"
```

**After:**
```cpp
#include "INetworkAgent.hpp"
#include "NetworkAgentFactory.hpp"
```

## Benefits

### 1. Runtime Switching

Switch implementations without recompiling:
```cpp
// Read from config file, command line, environment variable, etc.
bool use_orca = config.get_bool("use_orca_network");
auto agent = NetworkAgentFactory::create(log_dir, use_orca);
```

### 2. Easier Testing

Mock implementations for unit tests:
```cpp
class MockNetworkAgent : public INetworkAgent {
public:
    bool is_user_login() override { return true; }
    int connect_server() override { return 0; }
    // ... mock all other methods
};

// In tests
INetworkAgent* agent = new MockNetworkAgent();
test_component_with_agent(agent);
```

### 3. Future Extensibility

Easy to add new implementations:
```cpp
class CloudProviderNetwork : public INetworkAgent {
    // Implementation for different cloud provider
};

// Add to factory
static std::unique_ptr<INetworkAgent> create_cloud_provider(...) {
    return std::make_unique<CloudProviderNetwork>(...);
}
```

### 4. Clean Dependency Injection

Pass interface to components:
```cpp
class PrintJobManager {
    INetworkAgent* agent;  // Doesn't care which implementation

public:
    PrintJobManager(INetworkAgent* agent) : agent(agent) {}

    void submit_job() {
        agent->start_print(...);  // Works with any implementation
    }
};
```

## Type Safety

### Downcasting (Use Sparingly)

When you need implementation-specific features:

```cpp
INetworkAgent* agent = NetworkAgentFactory::create_orca_network(log_dir);

// Downcast only when needed
if (auto* orca = dynamic_cast<OrcaNetwork*>(agent)) {
    // OrcaNetwork-specific methods
    orca->set_backend_url("http://localhost:9090");
}
```

**Note:** Minimize downcasting. Prefer keeping code generic using only INetworkAgent interface.

### Type Checking

```cpp
void configure_agent(INetworkAgent* agent, AppConfig* config) {
    // Common configuration (all agents)
    agent->set_country_code(config->get_country_code());

    // Implementation-specific configuration
    if (auto* orca = dynamic_cast<OrcaNetwork*>(agent)) {
        orca->set_backend_url(config->get("orca_backend_url"));
    }
    // NetworkAgent doesn't need special config
}
```

## Error Handling

### Null Checking

```cpp
auto agent = NetworkAgentFactory::create(log_dir, use_orca);

if (!agent) {
    BOOST_LOG_TRIVIAL(error) << "Failed to create network agent";
    return ERROR_INITIALIZATION_FAILED;
}

// Safe to use
agent->init_log();
```

### Exception Safety

```cpp
std::unique_ptr<INetworkAgent> agent;

try {
    agent = NetworkAgentFactory::create(log_dir, use_orca);
    agent->init_log();
    agent->start();
} catch (const std::exception& e) {
    BOOST_LOG_TRIVIAL(error) << "Agent initialization failed: " << e.what();
    return ERROR_INITIALIZATION_FAILED;
}
```

## Performance Considerations

### Virtual Function Overhead

- **Impact:** Minimal (~1-2 CPU cycles per call)
- **Reality:** Network operations are I/O bound (milliseconds to seconds)
- **Verdict:** Virtual function overhead is negligible

### Memory Layout

- Interface pointer: 8 bytes (64-bit systems)
- No additional memory overhead from polymorphism
- Smart pointers recommended for automatic cleanup

## Best Practices

### DO ✅

1. **Use interface type in declarations**
   ```cpp
   INetworkAgent* m_agent;
   ```

2. **Use factory for creation**
   ```cpp
   auto agent = NetworkAgentFactory::create(...);
   ```

3. **Use smart pointers when possible**
   ```cpp
   std::unique_ptr<INetworkAgent> agent = NetworkAgentFactory::create(...);
   ```

4. **Keep code generic**
   ```cpp
   void process(INetworkAgent* agent) {
       agent->connect_server();  // No implementation assumptions
   }
   ```

### DON'T ❌

1. **Don't hardcode implementation**
   ```cpp
   // BAD
   OrcaNetwork* agent = new OrcaNetwork(...);
   ```

2. **Don't downcast unnecessarily**
   ```cpp
   // BAD - assumes implementation
   auto* orca = static_cast<OrcaNetwork*>(agent);
   ```

3. **Don't check implementation type in business logic**
   ```cpp
   // BAD
   if (typeid(*agent) == typeid(OrcaNetwork)) { ... }
   ```

4. **Don't leak pointers**
   ```cpp
   // BAD
   INetworkAgent* agent = NetworkAgentFactory::create(...).release();
   // ... might forget to delete
   ```

## Troubleshooting

### "Pure virtual function called"

**Problem:** Interface method not implemented

**Solution:** Ensure all virtual methods have `override` keyword and implementation:
```cpp
class OrcaNetwork : public INetworkAgent {
    int init_log() override { /* implementation */ }
    // ... all other methods
};
```

### Linker Errors

**Problem:** "Undefined reference to vtable"

**Solution:** Ensure all virtual methods are implemented, including destructor:
```cpp
~OrcaNetwork() override { /* implementation */ }
```

### Dynamic Cast Returns nullptr

**Problem:** Downcast fails

**Solution:** Check if agent is correct type before casting:
```cpp
if (auto* orca = dynamic_cast<OrcaNetwork*>(agent)) {
    // Only executes if agent is actually OrcaNetwork
    orca->set_backend_url(...);
}
```

## Summary

The polymorphic network agent system provides:

✅ **Runtime flexibility** - Switch implementations without recompiling
✅ **Clean architecture** - Single interface, multiple implementations
✅ **Future-proof** - Easy to add new network backends
✅ **Testable** - Mock implementations for unit tests
✅ **Type-safe** - Compiler-enforced interface compliance
✅ **Minimal overhead** - Negligible performance impact

Use `INetworkAgent` interface for all declarations, `NetworkAgentFactory` for creation, and enjoy seamless switching between NetworkAgent and OrcaNetwork!
