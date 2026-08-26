#ifndef slic3r_GUI_AmsAccessory_hpp_
#define slic3r_GUI_AmsAccessory_hpp_

// AMS Accessory: an external device (e.g. a standalone RFID reader) that decodes
// third-party filament tags and exposes the resolved per-slot filament info over a
// standard protocol (Moonraker first). OrcaSlicer pulls that data and merges it into
// a connected Bambu printer's AMS slots — for slicing, and optionally pushed to the
// printer's AMS slot itself.
//
// This is an additive, opt-in capability: nothing runs unless the user enables an
// accessory for a specific device, so existing behavior is unaffected.

#include <map>
#include <memory>
#include <string>

#include <vector>

#include "GUI_Utils.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"

#include "slic3r/Utils/IPrinterAgent.hpp" // AccessoryFilamentSlot

namespace Slic3r {

class MachineObject;
class IPrinterAgent;

namespace GUI {

// Per-device accessory endpoint configuration, persisted in AppConfig.
struct AmsAccessoryConfig
{
    bool        enabled         = false;
    std::string host_type       = "moonraker"; // reserved for future modules
    std::string host;                          // IP / hostname / URL
    std::string api_key;
    bool        use_ssl         = false;
    bool        push_to_printer = true;        // also send filament to the AMS slot on sync

    bool valid() const { return enabled && !host.empty(); }
};

// Orchestrates fetching accessory filament data and merging it into a printer's AMS.
class AmsAccessoryManager
{
public:
    static AmsAccessoryManager& get();

    AmsAccessoryConfig load(const std::string& dev_id) const;
    void               save(const std::string& dev_id, const AmsAccessoryConfig& cfg);

    // Fetch from the accessory and merge into obj's AMS (by slot index).
    // Returns the number of slots updated, or -1 on fetch/connection failure.
    // No-op (returns 0) when no enabled accessory is configured for obj.
    int sync(MachineObject* obj, std::string* err_out = nullptr);

    // Probe connectivity only: read the accessory's filament data without touching any
    // AMS. Returns the number of populated slots, or -1 on failure (err_out set).
    int test(const AmsAccessoryConfig& cfg, std::string* err_out = nullptr);

    // Fetch resolved filament slots from the accessory (no AMS/printer needed).
    // Returns the number of loaded slots written to out, or -1 on failure (err_out set).
    int fetch(const AmsAccessoryConfig& cfg, std::vector<AccessoryFilamentSlot>& out,
              std::string* err_out = nullptr);

    // Strict tag matching: describe loaded slots whose reported filament_id has no
    // compatible preset for the current printer. Returns an empty string when every
    // slot is usable; otherwise a user-facing error (callers abort the sync with it).
    static std::string unmatched_filament_error(const std::vector<AccessoryFilamentSlot>& slots);

private:
    AmsAccessoryManager() = default;

    std::shared_ptr<IPrinterAgent> agent_for(const std::string& host_type);

    std::map<std::string, std::shared_ptr<IPrinterAgent>> m_agents; // host_type -> agent
};

// Modal dialog to configure and trigger an AMS accessory for a given device.
class AmsAccessoryDialog : public DPIDialog
{
public:
    AmsAccessoryDialog(wxWindow* parent, MachineObject* obj);
    ~AmsAccessoryDialog() override = default;

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    AmsAccessoryConfig collect() const;
    void               apply(const AmsAccessoryConfig& cfg);
    void               on_save(wxCommandEvent& evt);
    void               on_sync(wxCommandEvent& evt);
    void               on_test(wxCommandEvent& evt);

    MachineObject* m_obj{nullptr};
    std::string    m_dev_id;

    ::CheckBox*  m_enable{nullptr};
    ::ComboBox*  m_host_type{nullptr};
    ::TextInput* m_host{nullptr};
    ::TextInput* m_api_key{nullptr};
    ::CheckBox*  m_use_ssl{nullptr};
    ::CheckBox*  m_push{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_AmsAccessory_hpp_
