#include "PrinterCommLogger.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

PrinterCommLogger& PrinterCommLogger::instance()
{
    static PrinterCommLogger instance;
    return instance;
}

PrinterCommLogger::~PrinterCommLogger()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.close();
    }
}

void PrinterCommLogger::initialize(const std::string& log_dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_log_dir = log_dir;
    m_initialized = true;
    BOOST_LOG_TRIVIAL(info) << "PrinterCommLogger initialized with log_dir: " << log_dir;
}

void PrinterCommLogger::set_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
    BOOST_LOG_TRIVIAL(info) << "PrinterCommLogger " << (enabled ? "enabled" : "disabled");
}

bool PrinterCommLogger::is_enabled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

std::string PrinterCommLogger::get_log_file_path() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_log_file_path;
}

void PrinterCommLogger::log_send(const std::string& device_id, const std::string& tunnel_type, const nlohmann::json& json_data)
{
    write_entry("SEND", device_id, tunnel_type, json_data);
}

void PrinterCommLogger::log_recv(const std::string& device_id, const std::string& tunnel_type, const nlohmann::json& json_data)
{
    write_entry("RECV", device_id, tunnel_type, json_data);
}

void PrinterCommLogger::ensure_file_open()
{
    // Must be called with mutex held
    if (!m_initialized || m_log_dir.empty()) {
        return;
    }

    // Check if we need to rotate
    if (m_file.is_open() && m_current_file_size >= MAX_FILE_SIZE) {
        m_file.close();
        m_log_file_path.clear();
        m_current_file_size = 0;
    }

    // Open new file if needed
    if (!m_file.is_open()) {
        // Create log directory if it doesn't exist
        boost::filesystem::path log_path(m_log_dir);
        if (!boost::filesystem::exists(log_path)) {
            boost::filesystem::create_directories(log_path);
        }

        // Generate filename with timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;
#ifdef _WIN32
        localtime_s(&tm_now, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_now);
#endif

        std::ostringstream filename;
        filename << "printer_comm_"
                 << std::put_time(&tm_now, "%Y%m%d_%H%M%S")
                 << ".md";

        m_log_file_path = (log_path / filename.str()).string();
        m_file.open(m_log_file_path, std::ios::out | std::ios::app);

        if (m_file.is_open()) {
            // Write header
            m_file << "# BBL Printer Communication Log\n";
            m_file << "Started: " << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") << "\n\n";
            m_file << "---\n\n";
            m_file.flush();
            m_current_file_size = m_file.tellp();
            BOOST_LOG_TRIVIAL(info) << "PrinterCommLogger: Created log file " << m_log_file_path;
        } else {
            BOOST_LOG_TRIVIAL(error) << "PrinterCommLogger: Failed to create log file " << m_log_file_path;
        }
    }
}

void PrinterCommLogger::write_entry(const std::string& direction, const std::string& device_id,
                                    const std::string& tunnel_type, const nlohmann::json& json_data)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_enabled || !m_initialized) {
        return;
    }

    ensure_file_open();

    if (!m_file.is_open()) {
        return;
    }

    std::string timestamp = get_timestamp();
    std::string command_name = extract_command_name(json_data);
    std::string json_str = json_data.dump(2);

    std::ostringstream entry;
    entry << "## " << timestamp << " | " << direction << " | " << tunnel_type << " | " << device_id << "\n";
    entry << "**" << (direction == "SEND" ? "Command" : "Message") << "**: `" << command_name << "`\n";
    entry << "```json\n";
    entry << json_str << "\n";
    entry << "```\n\n";
    entry << "---\n\n";

    std::string entry_str = entry.str();
    m_file << entry_str;
    m_file.flush();
    m_current_file_size += entry_str.size();
}

std::string PrinterCommLogger::extract_command_name(const nlohmann::json& json_data)
{
    // Try common command patterns in BBL protocol
    static const std::vector<std::string> command_keys = {
        "print", "pushing", "system", "camera", "xcam", "mc_print",
        "info", "upgrade", "liveview", "ledctrl", "fan", "temperature"
    };

    for (const auto& key : command_keys) {
        if (json_data.contains(key)) {
            const auto& section = json_data[key];
            if (section.is_object() && section.contains("command")) {
                return key + "." + section["command"].get<std::string>();
            } else if (section.is_object() && section.contains("sequence_id")) {
                // Some messages use sequence_id without command
                return key + ".message";
            }
            return key;
        }
    }

    // Fallback: return first key if available
    if (json_data.is_object() && !json_data.empty()) {
        return json_data.begin().key();
    }

    return "unknown";
}

std::string PrinterCommLogger::get_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_now, "%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

} // namespace Slic3r
