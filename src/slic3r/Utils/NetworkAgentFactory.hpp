#ifndef __NETWORK_AGENT_FACTORY_HPP__
#define __NETWORK_AGENT_FACTORY_HPP__

#include "INetworkAgent.hpp"
#include "NetworkAgent.hpp"
#include "OrcaNetwork.hpp"
#include "libslic3r/AppConfig.hpp"
#include <memory>
#include <string>

namespace Slic3r {

/**
 * NetworkAgentFactory - Factory for creating network agent instances
 *
 * This factory enables polymorphic creation of network agents based on
 * configuration or runtime decisions. Supports:
 * - NetworkAgent (dynamic library wrapper)
 * - OrcaNetwork (native implementation)
 *
 * Usage:
 *   auto agent = NetworkAgentFactory::create_orca_network(log_dir);
 *   // or
 *   auto agent = NetworkAgentFactory::create_network_agent(log_dir);
 *   // or
 *   auto agent = NetworkAgentFactory::create(log_dir, use_orca);
 */
class NetworkAgentFactory {
public:
    /**
     * Create a network agent based on type flag
     *
     * @param log_dir Directory for log files
     * @param use_orca_network If true, creates OrcaNetwork; otherwise NetworkAgent
     * @return Unique pointer to INetworkAgent implementation
     */
    static std::unique_ptr<INetworkAgent> create(const std::string& log_dir, bool use_orca_network = false) {
        if (use_orca_network) {
            return create_orca_network(log_dir);
        } else {
            return create_network_agent(log_dir);
        }
    }

    /**
     * Create an OrcaNetwork instance
     *
     * @param log_dir Directory for log files
     * @return Unique pointer to OrcaNetwork (as INetworkAgent interface)
     */
    static std::unique_ptr<INetworkAgent> create_orca_network(const std::string& log_dir) {
        return std::make_unique<OrcaNetwork>(log_dir);
    }

    /**
     * Create a NetworkAgent instance
     *
     * Note: This requires the bambu_networking library to be loaded first
     * via NetworkAgent::initialize_network_module()
     *
     * @param log_dir Directory for log files
     * @return Unique pointer to NetworkAgent (as INetworkAgent interface)
     */
    static std::unique_ptr<INetworkAgent> create_network_agent(const std::string& log_dir) {
        return std::make_unique<NetworkAgent>(log_dir);
    }

private:
    // Factory is not instantiable
    NetworkAgentFactory() = delete;
    ~NetworkAgentFactory() = delete;
    NetworkAgentFactory(const NetworkAgentFactory&) = delete;
    NetworkAgentFactory& operator=(const NetworkAgentFactory&) = delete;
};

/**
 * Helper function to create agent from AppConfig
 *
 * Reads the "use_orca_network" setting from app config to determine
 * which implementation to create.
 *
 * Example:
 *   #include "slic3r/GUI/GUI_App.hpp"
 *   auto agent = create_agent_from_config(log_dir, wxGetApp().app_config);
 *
 * @param log_dir Directory for log files
 * @param app_config Application configuration object
 * @return Unique pointer to network agent
 */
inline std::unique_ptr<INetworkAgent> create_agent_from_config(
    const std::string& log_dir,
    AppConfig* app_config)
{
    bool use_orca = true;
    if (app_config) {
        // Try to read use_orca_network setting
        try {
            use_orca = app_config->get("use_orca_network") == "true" ||
                       app_config->get_bool("use_orca_network");
        } catch (...) {
            // Setting doesn't exist or error reading - default to false
            use_orca = true;
        }
    }

    auto agent = NetworkAgentFactory::create(log_dir, use_orca);

    // Configure OrcaNetwork URL overrides from AppConfig if applicable
    if (use_orca && app_config) {
        auto* orca_agent = dynamic_cast<OrcaNetwork*>(agent.get());
        if (orca_agent) {
            orca_agent->configure_urls(app_config);
        }
    }

    return agent;
}

} // namespace Slic3r

#endif // __NETWORK_AGENT_FACTORY_HPP__
