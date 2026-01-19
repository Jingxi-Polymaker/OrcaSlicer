#include "QidiPrinterAgent.hpp"
#include "Http.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <cstdint>
#include <cctype>
#include <sstream>

namespace {

std::string to_hex_string(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << value;
    return stream.str();
}

bool looks_like_host(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    if (value.find(' ') != std::string::npos) {
        return false;
    }
    return value.find('.') != std::string::npos || value.find(':') != std::string::npos;
}

std::string normalize_filament_type(const std::string& filament_type)
{
    std::string trimmed = filament_type;
    boost::trim(trimmed);
    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper.find("PLA") != std::string::npos)
        return "PLA";
    if (upper.find("ABS") != std::string::npos)
        return "ABS";
    if (upper.find("PETG") != std::string::npos)
        return "PETG";
    if (upper.find("TPU") != std::string::npos)
        return "TPU";
    if (upper.find("ASA") != std::string::npos)
        return "ASA";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";
    if (upper.find("PC") != std::string::npos)
        return "PC";
    if (upper.find("PVA") != std::string::npos)
        return "PVA";

    return trimmed;
}
} // namespace

namespace Slic3r {

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : OrcaPrinterAgent(std::move(log_dir))
{
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: Constructor";
}

QidiPrinterAgent::~QidiPrinterAgent() = default;

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{.id = "qidi", .name = "Qidi Printer Agent", .version = QidiPrinterAgent_VERSION, .description = "Qidi printer agent"};
}

int QidiPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int QidiPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int QidiPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    (void) username;
    (void) password;
    (void) use_ssl;
    store_host(dev_id, dev_ip);
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: connect_printer - dev_id=" << dev_id << ", dev_ip=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
    auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: Invalid JSON request";
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    if (json.contains("pushing") && json["pushing"].contains("command")) {
        const auto& command = json["pushing"]["command"];
        if (command.is_string() && command.get<std::string>() == "pushall") {
            return sync_filament_list(dev_id);
        }
    }

    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::sync_filament_list(const std::string& dev_id)
{
    const std::string host = resolve_host(dev_id);
    if (host.empty()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: Missing host for dev_id=" << dev_id;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    std::vector<QidiSlotInfo> slots;
    int                       box_count = 0;
    std::string               error;
    if (!fetch_slot_info(host, slots, box_count, error)) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: Failed to fetch slot info: " << error;
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    QidiFilamentDict dict;
    if (!fetch_filament_dict(host, dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: Failed to fetch filament dict: " << error;
    }

    uint64_t tray_exist_bits = 0;
    for (const auto& slot : slots) {
        if (slot.filament_exists && slot.slot_index >= 0 && slot.slot_index < 64) {
            tray_exist_bits |= (1ULL << slot.slot_index);
        }
    }

    uint64_t ams_exist_bits = 0;
    for (int i = 0; i < box_count && i < 64; ++i) {
        ams_exist_bits |= (1ULL << i);
    }

    nlohmann::json payload;
    payload["print"]["command"] = "push_status";
    payload["print"]["msg"]     = 0;

    nlohmann::json ams;
    ams["ams_exist_bits"]  = to_hex_string(ams_exist_bits);
    ams["tray_exist_bits"] = to_hex_string(tray_exist_bits);

    nlohmann::json ams_units = nlohmann::json::array();
    for (int ams_id = 0; ams_id < box_count; ++ams_id) {
        nlohmann::json ams_unit;
        ams_unit["id"] = std::to_string(ams_id);

        nlohmann::json trays = nlohmann::json::array();
        for (int slot_id = 0; slot_id < 4; ++slot_id) {
            const int          slot_index = ams_id * 4 + slot_id;
            const QidiSlotInfo slot       = slot_index < static_cast<int>(slots.size()) ? slots[slot_index] : QidiSlotInfo{};

            std::string tray_color = "00000000";
            std::string tray_type;
            std::string tray_info_idx;

            if (slot.filament_exists) {
                std::string filament_type = "PLA";
                auto        filament_it   = dict.filaments.find(slot.filament_type);
                if (filament_it != dict.filaments.end()) {
                    filament_type = filament_it->second;
                }
                tray_type     = normalize_filament_type(filament_type);
                tray_info_idx = map_filament_type_to_setting_id(tray_type);
                if (tray_info_idx.empty()) {
                    tray_info_idx = "unknown";
                }

                std::string color    = "#FFFFFF";
                auto        color_it = dict.colors.find(slot.color_index);
                if (color_it != dict.colors.end()) {
                    color = color_it->second;
                }
                tray_color = normalize_color(color);
            }

            nlohmann::json tray;
            tray["id"]            = std::to_string(slot_id);
            tray["tag_uid"]       = "0000000000000000";
            tray["tray_color"]    = tray_color;
            tray["ctype"]         = 0;
            tray["cols"]          = nlohmann::json::array({tray_color});
            tray["tray_info_idx"] = slot.filament_exists ? tray_info_idx : "";
            tray["tray_type"]     = slot.filament_exists ? tray_type : "";
            trays.push_back(tray);
        }

        ams_unit["tray"] = trays;
        ams_units.push_back(ams_unit);
    }

    ams["ams"]              = ams_units;
    payload["print"]["ams"] = ams;

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

std::string QidiPrinterAgent::resolve_host(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto                        it = host_by_device.find(dev_id);
    if (it != host_by_device.end()) {
        return it->second;
    }
    return looks_like_host(dev_id) ? dev_id : "";
}

void QidiPrinterAgent::store_host(const std::string& dev_id, const std::string& host)
{
    if (host.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    host_by_device[dev_id] = host;
}

bool QidiPrinterAgent::fetch_slot_info(const std::string& host, std::vector<QidiSlotInfo>& slots, int& box_count, std::string& error) const
{
    std::string url = "http://" + host + "/printer/objects/query?save_variables=variables";
    for (int i = 0; i < 16; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.contains("result") || !json["result"].contains("status") || !json["result"]["status"].contains("save_variables") ||
        !json["result"]["status"]["save_variables"].contains("variables")) {
        error = "Unexpected JSON structure";
        return false;
    }

    auto& variables = json["result"]["status"]["save_variables"]["variables"];
    auto& status    = json["result"]["status"];

    box_count = variables.value("box_count", 1);
    if (box_count < 0) {
        box_count = 0;
    }

    const int max_slots = box_count * 4;
    slots.clear();
    slots.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        QidiSlotInfo slot;
        slot.slot_index    = i;
        slot.color_index   = variables.value("color_slot" + std::to_string(i), 1);
        slot.filament_type = variables.value("filament_slot" + std::to_string(i), 1);
        slot.vendor_type   = variables.value("vendor_slot" + std::to_string(i), 0);

        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        slot.filament_exists        = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button    = box_stepper["runout_button"].get<int>();
                slot.filament_exists = (runout_button == 0);
            }
        }
        slots.push_back(slot);
    }

    return true;
}

void QidiPrinterAgent::parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_section     = false;
    std::string        section_header = "[" + section_name + "]";

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            in_section = (line == section_header);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (in_section) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    int index     = std::stoi(key);
                    result[index] = value;
                } catch (...) {}
            }
        }
    }
}

void QidiPrinterAgent::parse_filament_sections(const std::string& content, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    int                current_fila_index = -1;

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            current_fila_index = -1;
            if (line.size() > 5 && line.substr(0, 5) == "[fila" && line.back() == ']') {
                std::string num_str = line.substr(5, line.size() - 6);
                try {
                    current_fila_index = std::stoi(num_str);
                } catch (...) {
                    current_fila_index = -1;
                }
            }
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (current_fila_index > 0) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                if (key == "filament") {
                    result[current_fila_index] = value;
                }
            }
        }
    }
}

bool QidiPrinterAgent::fetch_filament_dict(const std::string& host, QidiFilamentDict& dict, std::string& error) const
{
    std::string url = "http://" + host + "/server/files/config/officiall_filas_list.cfg";

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
}

std::string QidiPrinterAgent::normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    if (!value.empty() && value[0] == '#') {
        value = value.substr(1);
    }
    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (normalized.size() == 6) {
        normalized += "FF";
    }
    if (normalized.size() != 8) {
        return "00000000";
    }
    return normalized;
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    std::string upper = filament_type;
    boost::trim(upper);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper == "PLA") {
        return "QD_1_0_1";
    }
    if (upper == "ABS") {
        return "QD_1_0_11";
    }
    if (upper == "PETG") {
        return "QD_1_0_41";
    }
    if (upper == "TPU") {
        return "QD_1_0_50";
    }
    return "";
}

void QidiPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_message_fn;
        cloud_fn = on_message_fn;
        queue_fn = queue_on_main_fn;
    }

    auto dispatch = [dev_id, payload, local_fn, cloud_fn]() {
        if (local_fn) {
            local_fn(dev_id, payload);
            return;
        }
        if (cloud_fn) {
            cloud_fn(dev_id, payload);
        }
    };

    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

} // namespace Slic3r
