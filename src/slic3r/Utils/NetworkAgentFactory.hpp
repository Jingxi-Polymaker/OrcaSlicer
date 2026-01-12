#ifndef __NETWORK_AGENT_FACTORY_HPP__
#define __NETWORK_AGENT_FACTORY_HPP__

#include "ICloudServiceAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgent.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "OrcaPrinterAgent.hpp"
#include "BBLCloudServiceAgent.hpp"
#include "BBLPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "libslic3r/AppConfig.hpp"
#include <memory>
#include <string>

namespace Slic3r {

/**
 * AgentProvider - Specifies which implementation to use for each agent type.
 *
 * - Orca: Native Orca implementations (OrcaCloudServiceAgent, OrcaPrinterAgent)
 * - BBL: BBL DLL wrapper implementations (BBLCloudServiceAgent, BBLPrinterAgent)
 */
enum class AgentProvider {
    Orca,
    BBL
};

/**
 * AgentConfiguration - Configuration for per-agent provider selection.
 *
 * Allows mixing providers, e.g., OrcaCloud + BBLPrinter for
 * using Orca login/cloud with BBL printer connectivity.
 *
 * Note: Auth is now part of ICloudServiceAgent, so there's no separate auth_provider.
 */
struct AgentConfiguration {
    AgentProvider cloud_provider = AgentProvider::Orca;
    AgentProvider printer_provider = AgentProvider::Orca;
};

/**
 * NetworkAgentFactory - Factory for creating network agent instances
 *
 * This factory enables polymorphic creation of network agents based on
 * configuration or runtime decisions. Supports:
 * - NetworkAgent (dynamic library wrapper / facade)
 * - Per-agent creation for mixed Orca/BBL modes
 *
 * Note: Auth functionality is part of ICloudServiceAgent. The cloud service agent
 * handles both authentication and cloud services.
 *
 * Usage:
 *   // Legacy creation (single implementation)
 *   auto agent = NetworkAgentFactory::create_pure_orca(log_dir);
 *
 *   // Per-agent creation (mixed mode)
 *   auto cloud = NetworkAgentFactory::create_cloud_agent(AgentProvider::Orca, log_dir);
 *   auto printer = NetworkAgentFactory::create_printer_agent(AgentProvider::BBL, cloud, log_dir);
 *   auto agent = NetworkAgentFactory::create_from_agents(cloud, printer);
 *
 *   // Configured creation (from AgentConfiguration)
 *   AgentConfiguration config;
 *   config.printer_provider = AgentProvider::BBL;
 *   auto agent = NetworkAgentFactory::create_configured(log_dir, config);
 */
class NetworkAgentFactory {
public:
    // ========================================================================
    // Per-Agent Factory Methods
    // ========================================================================

    /**
     * Create a cloud service agent based on provider type
     *
     * The cloud agent now includes authentication functionality (merged from IAuthAgent).
     *
     * @param provider Which implementation to use (Orca or BBL)
     * @param log_dir Directory for log files
     * @return Shared pointer to ICloudServiceAgent implementation
     */
    static std::shared_ptr<ICloudServiceAgent> create_cloud_agent(
        AgentProvider provider,
        const std::string& log_dir)
    {
        switch (provider) {
            case AgentProvider::Orca:
                return std::make_shared<OrcaCloudServiceAgent>(log_dir);
            case AgentProvider::BBL: {
                auto& plugin = BBLNetworkPlugin::instance();
                if (!plugin.is_loaded()) {
                    return nullptr;
                }
                if (!plugin.has_agent()) {
                    plugin.create_agent(log_dir);
                }
                if (!plugin.has_agent()) {
                    return nullptr;
                }
                return std::make_shared<BBLCloudServiceAgent>();
            }
            default:
                return nullptr;
        }
    }

    /**
     * Create a printer agent based on provider type
     *
     * @param provider Which implementation to use (Orca or BBL)
     * @param cloud_agent Cloud agent for token access (optional for Orca stubs)
     * @param log_dir Directory for log files
     * @return Shared pointer to IPrinterAgent implementation
     */
    static std::shared_ptr<IPrinterAgent> create_printer_agent(
        AgentProvider provider,
        std::shared_ptr<ICloudServiceAgent> cloud_agent,
        const std::string& log_dir)
    {
        switch (provider) {
            case AgentProvider::Orca: {
                auto agent = std::make_shared<OrcaPrinterAgent>(log_dir);
                if (cloud_agent) {
                    agent->set_cloud_agent(cloud_agent);
                }
                return agent;
            }
            case AgentProvider::BBL: {
                auto& plugin = BBLNetworkPlugin::instance();
                if (!plugin.is_loaded() || !plugin.has_agent()) {
                    return nullptr;
                }
                auto agent = std::make_shared<BBLPrinterAgent>();
                if (cloud_agent) {
                    agent->set_cloud_agent(cloud_agent);
                }
                return agent;
            }
            default:
                return nullptr;
        }
    }

    /**
     * Create a NetworkAgent from pre-created sub-agents
     *
     * @param cloud_agent Cloud service agent (required, includes auth)
     * @param printer_agent Printer agent (required)
     * @return Unique pointer to NetworkAgent facade
     */
    static std::unique_ptr<NetworkAgent> create_from_agents(
        std::shared_ptr<ICloudServiceAgent> cloud_agent,
        std::shared_ptr<IPrinterAgent> printer_agent)
    {
        return std::make_unique<NetworkAgent>(
            std::move(cloud_agent),
            std::move(printer_agent)
        );
    }

    /**
     * Create a fully configured NetworkAgent based on AgentConfiguration
     *
     * @param log_dir Directory for log files
     * @param config Configuration specifying which provider to use for each agent
     * @return Unique pointer to NetworkAgent with configured sub-agents
     */
    static std::unique_ptr<NetworkAgent> create_configured(
        const std::string& log_dir,
        const AgentConfiguration& config)
    {
        // Create cloud agent first (includes auth, needed by printer agent)
        auto cloud_agent = create_cloud_agent(config.cloud_provider, log_dir);

        // Create printer agent with cloud agent dependency
        auto printer_agent = create_printer_agent(config.printer_provider, cloud_agent, log_dir);

        return create_from_agents(cloud_agent, printer_agent);
    }

    /**
     * Create a fully configured NetworkAgent with all BBL sub-agents
     *
     * Convenience method that creates a NetworkAgent with BBL cloud and printer.
     * Requires BBL DLL to be loaded via BBLNetworkPlugin::instance().initialize().
     *
     * @param log_dir Directory for log files
     * @return Unique pointer to NetworkAgent with BBL sub-agents, or nullptr if DLL not loaded
     */
    static std::unique_ptr<NetworkAgent> create_pure_bbl(const std::string& log_dir) {
        auto& plugin = BBLNetworkPlugin::instance();
        if (!plugin.is_loaded()) {
            return nullptr;
        }

        AgentConfiguration config;
        config.cloud_provider = AgentProvider::BBL;
        config.printer_provider = AgentProvider::BBL;
        return create_configured(log_dir, config);
    }

    // ========================================================================
    // Legacy Factory Methods (backward compatibility)
    // ========================================================================

    /**
     * Create a network agent based on type flag
     *
     * @param log_dir Directory for log files
     * @param use_orca_network If true, creates OrcaNetwork; otherwise NetworkAgent
     * @return Unique pointer to NetworkAgent implementation
     */
    static std::unique_ptr<NetworkAgent> create(const std::string& log_dir, bool use_orca_network = false) {
        if (use_orca_network) {
            // OrcaNetwork is not compatible with NetworkAgent - use create_network_agent instead
            return create_network_agent(log_dir);
        } else {
            return create_network_agent(log_dir);
        }
    }

    /**
     * Create a NetworkAgent instance
     *
     * Note: This requires the bambu_networking library to be loaded first
     * via NetworkAgent::initialize_network_module()
     *
     * @param log_dir Directory for log files
     * @return Unique pointer to NetworkAgent
     */
    static std::unique_ptr<NetworkAgent> create_network_agent(const std::string& log_dir) {
        return std::make_unique<NetworkAgent>(log_dir);
    }

    /**
     * Create a pure Orca agent (using all Orca sub-agents)
     *
     * Convenience method that creates a NetworkAgent with Orca cloud and printer.
     * Equivalent to create_configured with all providers set to Orca.
     *
     * @param log_dir Directory for log files
     * @return Unique pointer to NetworkAgent with Orca sub-agents
     */
    static std::unique_ptr<NetworkAgent> create_pure_orca(const std::string& log_dir) {
        AgentConfiguration config;
        config.cloud_provider = AgentProvider::Orca;
        config.printer_provider = AgentProvider::Orca;
        return create_configured(log_dir, config);
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
inline std::unique_ptr<NetworkAgent> create_agent_from_config(
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
            // Setting doesn't exist or error reading - default to true
            use_orca = true;
        }
    }

    if (use_orca) {
        // Use the new pure Orca agent with sub-agent composition
        auto agent = NetworkAgentFactory::create_pure_orca(log_dir);

        // Configure OrcaCloudServiceAgent URL overrides from AppConfig if applicable
        if (agent && app_config) {
            auto cloud_agent = agent->get_cloud_agent();
            auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(cloud_agent.get());
            if (orca_cloud) {
                orca_cloud->configure_urls(app_config);
            }
        }

        return agent;
    } else {
        return NetworkAgentFactory::create_network_agent(log_dir);
    }
}

} // namespace Slic3r

#endif // __NETWORK_AGENT_FACTORY_HPP__
