#include "NetworkAgentFactory.hpp"
#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

// Static member initialization
std::mutex NetworkAgentFactory::s_registry_mutex;
std::map<std::string, PrinterAgentInfo> NetworkAgentFactory::s_printer_agents;
std::string NetworkAgentFactory::s_default_agent_id;

void NetworkAgentFactory::register_printer_agent(const std::string& id,
                                                 const std::string& display_name,
                                                 PrinterAgentFactory factory)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);

    auto result = s_printer_agents.emplace(id, PrinterAgentInfo(id, display_name, std::move(factory)));

    if (result.second) {
        BOOST_LOG_TRIVIAL(info) << "Registered printer agent: " << id << " (" << display_name << ")";

        // Set as default if it's the first agent registered
        if (s_default_agent_id.empty()) {
            s_default_agent_id = id;
        }
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Printer agent already registered: " << id;
    }
}

bool NetworkAgentFactory::is_printer_agent_registered(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    return s_printer_agents.find(id) != s_printer_agents.end();
}

const PrinterAgentInfo* NetworkAgentFactory::get_printer_agent_info(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto it = s_printer_agents.find(id);
    return (it != s_printer_agents.end()) ? &it->second : nullptr;
}

std::vector<PrinterAgentInfo> NetworkAgentFactory::get_registered_printer_agents()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    std::vector<PrinterAgentInfo> result;
    result.reserve(s_printer_agents.size());

    for (const auto& pair : s_printer_agents) {
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
    auto it = s_printer_agents.find(id);

    if (it == s_printer_agents.end()) {
        BOOST_LOG_TRIVIAL(warning) << "Unknown printer agent ID: " << id;
        return nullptr;
    }

    return it->second.factory(cloud_agent, log_dir);
}

std::string NetworkAgentFactory::get_default_printer_agent_id()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    return s_default_agent_id;
}

void NetworkAgentFactory::set_default_printer_agent_id(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);

    if (s_printer_agents.find(id) != s_printer_agents.end()) {
        s_default_agent_id = id;
        BOOST_LOG_TRIVIAL(info) << "Default printer agent set to: " << id;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Cannot set default to unregistered agent: " << id;
    }
}

} // namespace Slic3r
