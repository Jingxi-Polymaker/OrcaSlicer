#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "OrcaPrinterAgent.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

class QidiPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;

    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    struct QidiSlotInfo
    {
        int  slot_index      = 0;
        int  color_index     = 0;
        int  filament_type   = 0;
        int  vendor_type     = 0;
        bool filament_exists = false;
    };

    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    int handle_request(const std::string& dev_id, const std::string& json_str);
    int sync_filament_list(const std::string& dev_id);

    std::string resolve_host(const std::string& dev_id) const;
    void        store_host(const std::string& dev_id, const std::string& host);

    bool fetch_slot_info(const std::string& host, std::vector<QidiSlotInfo>& slots, int& box_count, std::string& error) const;
    bool fetch_filament_dict(const std::string& host, QidiFilamentDict& dict, std::string& error) const;

    static void parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result);
    static void parse_filament_sections(const std::string& content, std::map<int, std::string>& result);

    static std::string normalize_color(const std::string& color);
    static std::string map_filament_type_to_setting_id(const std::string& filament_type);

    void dispatch_message(const std::string& dev_id, const std::string& payload);

    mutable std::mutex                 state_mutex;
    std::map<std::string, std::string> host_by_device;
    OnMessageFn                        on_message_fn;
    OnMessageFn                        on_local_message_fn;
    QueueOnMainFn                      queue_on_main_fn;
};

} // namespace Slic3r

#endif
