#ifndef slic3r_PrinterCommLogger_hpp_
#define slic3r_PrinterCommLogger_hpp_

#include <string>
#include <mutex>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Singleton logger for BBL printer communication
// Logs all MQTT messages to a markdown file for protocol analysis
class PrinterCommLogger
{
public:
    static PrinterCommLogger& instance();

    // Initialize with log directory path
    void initialize(const std::string& log_dir);

    // Log outgoing message
    void log_send(const std::string& device_id, const std::string& tunnel_type, const nlohmann::json& json_data);

    // Log incoming message
    void log_recv(const std::string& device_id, const std::string& tunnel_type, const nlohmann::json& json_data);

    // Enable/disable logging at runtime
    void set_enabled(bool enabled);
    bool is_enabled() const;

    // Get current log file path (for debugging)
    std::string get_log_file_path() const;

private:
    PrinterCommLogger() = default;
    ~PrinterCommLogger();

    PrinterCommLogger(const PrinterCommLogger&) = delete;
    PrinterCommLogger& operator=(const PrinterCommLogger&) = delete;

    // Write a log entry
    void write_entry(const std::string& direction, const std::string& device_id,
                     const std::string& tunnel_type, const nlohmann::json& json_data);

    // Ensure file is open, rotate if needed
    void ensure_file_open();

    // Extract command name from JSON for markdown header
    static std::string extract_command_name(const nlohmann::json& json_data);

    // Get current timestamp string
    static std::string get_timestamp();

    mutable std::mutex m_mutex;
    std::string m_log_dir;
    std::string m_log_file_path;
    std::ofstream m_file;
    bool m_enabled = true;
    bool m_initialized = false;
    size_t m_current_file_size = 0;

    static constexpr size_t MAX_FILE_SIZE = 50 * 1024 * 1024; // 50MB
};

} // namespace Slic3r

#endif // slic3r_PrinterCommLogger_hpp_
