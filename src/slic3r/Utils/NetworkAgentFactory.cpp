#include "NetworkAgentFactory.hpp"
#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

// ============================================================================
// Registry accessors using Meyer's singleton pattern
// This ensures the registry is initialized before any static registration
// attempts, avoiding the static initialization order fiasco.
// ============================================================================

namespace {

static std::mutex s_registry_mutex;

std::map<std::string, PrinterAgentInfo>& get_printer_agents()
{
    static std::map<std::string, PrinterAgentInfo> agents;
    return agents;
}

std::string& get_default_agent_id()
{
    static std::string default_id;
    return default_id;
}

} // anonymous namespace

bool NetworkAgentFactory::register_printer_agent(const std::string& id,
                                                 const std::string& display_name,
                                                 PrinterAgentFactory factory)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();

    auto result = agents.emplace(id, PrinterAgentInfo(id, display_name, std::move(factory)));

    if (result.second) {
        BOOST_LOG_TRIVIAL(info) << "Registered printer agent: " << id << " (" << display_name << ")";

        // Set as default if it's the first agent registered
        auto& default_id = get_default_agent_id();
        if (default_id.empty()) {
            default_id = id;
        }
        return true;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Printer agent already registered: " << id;
        return false;
    }
}

bool NetworkAgentFactory::is_printer_agent_registered(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    return agents.find(id) != agents.end();
}

const PrinterAgentInfo* NetworkAgentFactory::get_printer_agent_info(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    auto it = agents.find(id);
    return (it != agents.end()) ? &it->second : nullptr;
}

std::vector<PrinterAgentInfo> NetworkAgentFactory::get_registered_printer_agents()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    std::vector<PrinterAgentInfo> result;
    result.reserve(agents.size());

    for (const auto& pair : agents) {
        result.push_back(pair.second);
    }

    return result;
}

std::shared_ptr<IPrinterAgent> NetworkAgentFactory::create_printer_agent_by_id(
    const std::string& id,
    std::shared_ptr<ICloudServiceAgent> cloud_agent,
    const std::string& log_dir)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    auto it = agents.find(id);

    if (it == agents.end()) {
        BOOST_LOG_TRIVIAL(warning) << "Unknown printer agent ID: " << id;
        return nullptr;
    }

    return it->second.factory(cloud_agent, log_dir);
}

std::string NetworkAgentFactory::get_default_printer_agent_id()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    return get_default_agent_id();
}

void NetworkAgentFactory::set_default_printer_agent_id(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();

    if (agents.find(id) != agents.end()) {
        get_default_agent_id() = id;
        BOOST_LOG_TRIVIAL(info) << "Default printer agent set to: " << id;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Cannot set default to unregistered agent: " << id;
    }
}

} // namespace Slic3r
