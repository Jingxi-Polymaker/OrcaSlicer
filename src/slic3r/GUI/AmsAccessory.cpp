#include "AmsAccessory.hpp"

#include <boost/log/trivial.hpp>

#include <wx/msgdlg.h>
#include <wx/sizer.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevFilaSystem.h"

#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

namespace Slic3r { namespace GUI {

static const wxColour ACC_GREY800 = wxColour(50, 58, 61);
static const wxColour ACC_GREY700 = wxColour(107, 107, 107);

static const std::string ACCESSORY_SECTION = "ams_accessory";
// Used when the dialog is opened with no device selected (dev_id empty), and as a
// fallback for a connected device that has no per-device accessory configured.
static const std::string ACCESSORY_GLOBAL = "__global__";

static std::string cfg_key(const std::string& dev_id, const char* field)
{
    return dev_id + "/" + field;
}

// ----------------------------------------------------------------------------
// AmsAccessoryManager
// ----------------------------------------------------------------------------

AmsAccessoryManager& AmsAccessoryManager::get()
{
    static AmsAccessoryManager s_instance;
    return s_instance;
}

AmsAccessoryConfig AmsAccessoryManager::load(const std::string& dev_id) const
{
    AmsAccessoryConfig cfg;
    AppConfig* ac = wxGetApp().app_config;
    if (!ac)
        return cfg;

    // Prefer a per-device config; fall back to the global one (used when the dialog
    // was configured with no device selected, or for a device with no own config).
    std::string key = dev_id;
    if (key.empty() || ac->get(ACCESSORY_SECTION, cfg_key(key, "host")).empty())
        key = ACCESSORY_GLOBAL;

    cfg.enabled         = ac->get(ACCESSORY_SECTION, cfg_key(key, "enabled")) == "true";
    std::string ht      = ac->get(ACCESSORY_SECTION, cfg_key(key, "host_type"));
    if (!ht.empty())
        cfg.host_type   = ht;
    cfg.host            = ac->get(ACCESSORY_SECTION, cfg_key(key, "host"));
    cfg.api_key         = ac->get(ACCESSORY_SECTION, cfg_key(key, "api_key"));
    cfg.use_ssl         = ac->get(ACCESSORY_SECTION, cfg_key(key, "use_ssl")) == "true";
    // Default-on: syncing an accessory writes to the AMS slot unless explicitly disabled.
    cfg.push_to_printer = ac->get(ACCESSORY_SECTION, cfg_key(key, "push")) != "false";
    return cfg;
}

void AmsAccessoryManager::save(const std::string& dev_id, const AmsAccessoryConfig& cfg)
{
    AppConfig* ac = wxGetApp().app_config;
    if (!ac)
        return;

    const std::string key = dev_id.empty() ? ACCESSORY_GLOBAL : dev_id;
    ac->set(ACCESSORY_SECTION, cfg_key(key, "enabled"), cfg.enabled);
    ac->set(ACCESSORY_SECTION, cfg_key(key, "host_type"), cfg.host_type);
    ac->set(ACCESSORY_SECTION, cfg_key(key, "host"), cfg.host);
    ac->set(ACCESSORY_SECTION, cfg_key(key, "api_key"), cfg.api_key);
    ac->set(ACCESSORY_SECTION, cfg_key(key, "use_ssl"), cfg.use_ssl);
    ac->set(ACCESSORY_SECTION, cfg_key(key, "push"), cfg.push_to_printer);
    ac->save();
}

int AmsAccessoryManager::fetch(const AmsAccessoryConfig& cfg, std::vector<AccessoryFilamentSlot>& out,
                               std::string* err_out)
{
    auto set_err = [&](const std::string& m) { if (err_out) *err_out = m; };

    if (cfg.host.empty()) {
        set_err("Enter the accessory address first");
        return -1;
    }

    std::shared_ptr<IPrinterAgent> agent = agent_for(cfg.host_type);
    if (!agent) {
        set_err("Unsupported accessory protocol: " + cfg.host_type);
        return -1;
    }

    out.clear();
    if (!agent->fetch_accessory_filaments(cfg.host, cfg.api_key, cfg.use_ssl, out)) {
        set_err("Could not connect to the accessory at " + cfg.host);
        return -1;
    }

    int n = 0;
    for (const auto& s : out)
        if (s.has_filament)
            ++n;
    return n;
}

int AmsAccessoryManager::test(const AmsAccessoryConfig& cfg, std::string* err_out)
{
    std::vector<AccessoryFilamentSlot> slots;
    return fetch(cfg, slots, err_out);
}

std::shared_ptr<IPrinterAgent> AmsAccessoryManager::agent_for(const std::string& host_type)
{
    auto it = m_agents.find(host_type);
    if (it != m_agents.end())
        return it->second;

    std::shared_ptr<IPrinterAgent> agent;
    if (host_type.empty() || host_type == "moonraker")
        agent = std::make_shared<MoonrakerPrinterAgent>(data_dir());

    if (agent)
        m_agents[host_type] = agent;
    return agent;
}

int AmsAccessoryManager::sync(MachineObject* obj, std::string* err_out)
{
    auto set_err = [&](const std::string& m) { if (err_out) *err_out = m; };

    if (!obj) {
        set_err("No printer selected");
        return -1;
    }

    const AmsAccessoryConfig cfg = load(obj->get_dev_id());
    if (!cfg.valid()) {
        set_err("No AMS accessory is enabled/configured for this printer.");
        return 0; // not enabled/configured — nothing to do (quiet for auto-sync)
    }

    std::shared_ptr<IPrinterAgent> agent = agent_for(cfg.host_type);
    if (!agent) {
        set_err("Unsupported accessory protocol: " + cfg.host_type);
        return -1;
    }

    std::vector<AccessoryFilamentSlot> slots;
    if (!agent->fetch_accessory_filaments(cfg.host, cfg.api_key, cfg.use_ssl, slots)) {
        set_err("Could not read filament data from the accessory at " + cfg.host +
                ".\nCheck the address, that the device is powered on, and reachable on the network.");
        return -1;
    }

    DevFilaSystem* fila = obj->GetFilaSystem();
    if (!fila) {
        set_err("The selected printer has no AMS to sync into.");
        return -1;
    }

    int fetched = 0; // slots the accessory reported as loaded
    int skipped = 0; // reported slots with no matching AMS slot on the printer
    int updated = 0;
    for (const auto& s : slots) {
        if (!s.has_filament)
            continue;
        ++fetched;

        const int         ams_idx  = s.slot_index / 4;
        const int         slot_idx = s.slot_index % 4;
        const std::string ams_id   = std::to_string(ams_idx);
        const std::string slot_id  = std::to_string(slot_idx);

        DevAmsTray* tray = fila->GetAmsTray(ams_id, slot_id);
        if (!tray) {
            // The accessory reported a slot this printer's AMS doesn't have. Skip
            // rather than fabricate AMS units (kept conservative for v1).
            ++skipped;
            BOOST_LOG_TRIVIAL(info) << "AmsAccessory: no AMS slot " << ams_id << "/" << slot_id
                                    << " on printer, skipping accessory lane " << s.slot_index;
            continue;
        }

        tray->setting_id          = s.filament_id; // tray_info_idx / Orca filament id
        tray->m_fila_type         = s.type;
        tray->UpdateColorFromStr(s.color);         // sets both color and wx_color
        tray->is_exists           = true;
        tray->is_slot_placeholder = false;
        if (s.nozzle_temp > 0) {
            tray->nozzle_temp_max = std::to_string(s.nozzle_temp);
            tray->nozzle_temp_min = std::to_string(s.nozzle_temp);
        }
        if (s.bed_temp > 0)
            tray->bed_temp = std::to_string(s.bed_temp);
        tray->set_hold_count(); // shield from being clobbered by the next printer push
        ++updated;

        if (cfg.push_to_printer) {
            const int temp = s.nozzle_temp > 0 ? s.nozzle_temp : 0;
            obj->command_ams_filament_settings(ams_idx, slot_idx, s.filament_id, /*setting_id*/ "",
                                               s.color, s.type, temp, temp);
        }
    }

    BOOST_LOG_TRIVIAL(info) << "AmsAccessory: fetched " << fetched << " loaded slot(s), merged "
                            << updated << ", skipped " << skipped << " into AMS of " << obj->get_dev_id()
                            << (cfg.push_to_printer ? " (pushed to printer)" : "");

    if (updated == 0) {
        if (fetched == 0)
            set_err("Connected to the accessory, but it did not report any loaded filament slots.");
        else
            set_err(std::to_string(fetched) + " filament slot(s) were read from the accessory, but none "
                    "matched an AMS slot on this printer. Check the slot numbering on the accessory.");
    }
    return updated;
}

// ----------------------------------------------------------------------------
// AmsAccessoryDialog
// ----------------------------------------------------------------------------

AmsAccessoryDialog::AmsAccessoryDialog(wxWindow* parent, MachineObject* obj)
    : DPIDialog(parent, wxID_ANY, _L("AMS Accessory"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_obj(obj)
    , m_dev_id(obj ? obj->get_dev_id() : std::string())
{
    SetBackgroundColour(*wxWHITE);

    const int   pad        = FromDIP(20);
    const int   gap        = FromDIP(8);
    const wxSize ctrl_size = wxSize(FromDIP(240), -1);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // Title + intro
    auto* title = new Label(this, ::Label::Head_14, _L("AMS Accessory"));
    title->SetForegroundColour(ACC_GREY800);
    root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    auto* intro = new Label(this,
        _L("Read filament from an external accessory (e.g. an RFID reader) and sync it into this printer's AMS."),
        LB_AUTO_WRAP);
    intro->SetFont(::Label::Body_13);
    intro->SetForegroundColour(ACC_GREY700);
    intro->SetMinSize(wxSize(FromDIP(420), -1));
    intro->SetMaxSize(wxSize(FromDIP(420), -1));
    root->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // checkbox row helper: a CheckBox followed by a clickable Label
    auto add_check_row = [&](::CheckBox*& cb, const wxString& text) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        cb = new ::CheckBox(this);
        auto* lbl = new Label(this, text);
        lbl->SetFont(::Label::Body_13);
        lbl->SetForegroundColour(ACC_GREY800);
        lbl->Bind(wxEVT_LEFT_DOWN, [cb](wxMouseEvent&) { cb->SetValue(!cb->GetValue()); });
        row->Add(cb, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        return row;
    };

    root->Add(add_check_row(m_enable, _L("Enable AMS accessory for this printer")),
              0, wxLEFT | wxRIGHT | wxTOP, pad);

    // field rows: label on the left, themed input on the right
    auto* grid = new wxFlexGridSizer(0, 2, gap, FromDIP(12));
    grid->AddGrowableCol(1, 1);
    auto add_field_row = [&](const wxString& label, wxWindow* ctrl) {
        auto* lbl = new Label(this, label);
        lbl->SetFont(::Label::Body_13);
        lbl->SetForegroundColour(ACC_GREY800);
        grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    m_host_type = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ctrl_size, 0, nullptr, wxCB_READONLY);
    m_host_type->Append("moonraker");
    m_host_type->SetSelection(0);
    add_field_row(_L("Protocol"), m_host_type);

    m_host = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, ctrl_size, wxTE_PROCESS_ENTER);
    m_host->GetTextCtrl()->SetHint(_L("IP address or URL, e.g. 192.168.1.50"));
    add_field_row(_L("Accessory address"), m_host);

    m_api_key = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, ctrl_size, wxTE_PROCESS_ENTER);
    m_api_key->GetTextCtrl()->SetHint(_L("Optional"));
    add_field_row(_L("API key"), m_api_key);

    root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

    root->Add(add_check_row(m_use_ssl, _L("Use HTTPS")), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    root->Add(add_check_row(m_push, _L("Also write filament to the printer's AMS slot")),
              0, wxLEFT | wxRIGHT | wxTOP, gap);

    // Buttons
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* test_btn   = new Button(this, _L("Test"));
    auto* sync_btn   = new Button(this, _L("Sync now"));
    auto* save_btn   = new Button(this, _L("Save"));
    auto* cancel_btn = new Button(this, _L("Cancel"));
    test_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    sync_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    save_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    btns->Add(test_btn, 0, wxRIGHT, gap);
    btns->AddStretchSpacer(1);
    btns->Add(sync_btn, 0, wxRIGHT, gap);
    btns->Add(save_btn, 0, wxRIGHT, gap);
    btns->Add(cancel_btn, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, pad);

    test_btn->Bind(wxEVT_BUTTON, &AmsAccessoryDialog::on_test, this);
    sync_btn->Bind(wxEVT_BUTTON, &AmsAccessoryDialog::on_sync, this);
    save_btn->Bind(wxEVT_BUTTON, &AmsAccessoryDialog::on_save, this);
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    apply(AmsAccessoryManager::get().load(m_dev_id));

    SetSizerAndFit(root);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

AmsAccessoryConfig AmsAccessoryDialog::collect() const
{
    AmsAccessoryConfig cfg;
    cfg.enabled         = m_enable->GetValue();
    cfg.host_type       = m_host_type->GetValue().ToStdString();
    cfg.host            = m_host->GetTextCtrl()->GetValue().ToStdString();
    cfg.api_key         = m_api_key->GetTextCtrl()->GetValue().ToStdString();
    cfg.use_ssl         = m_use_ssl->GetValue();
    cfg.push_to_printer = m_push->GetValue();
    return cfg;
}

void AmsAccessoryDialog::apply(const AmsAccessoryConfig& cfg)
{
    m_enable->SetValue(cfg.enabled);
    int sel = m_host_type->FindString(cfg.host_type);
    m_host_type->SetSelection(sel != wxNOT_FOUND ? sel : 0);
    m_host->GetTextCtrl()->SetValue(cfg.host);
    m_api_key->GetTextCtrl()->SetValue(cfg.api_key);
    m_use_ssl->SetValue(cfg.use_ssl);
    m_push->SetValue(cfg.push_to_printer);
}

void AmsAccessoryDialog::on_save(wxCommandEvent& /*evt*/)
{
    AmsAccessoryManager::get().save(m_dev_id, collect());
    EndModal(wxID_OK);
}

void AmsAccessoryDialog::on_test(wxCommandEvent& /*evt*/)
{
    wxBusyCursor cursor;
    std::string err;
    int n = AmsAccessoryManager::get().test(collect(), &err);
    if (n < 0) {
        wxMessageBox(from_u8(err.empty() ? "Connection failed" : err), _L("AMS Accessory"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    wxMessageBox(wxString::Format(_L("Connected. The accessory reports %d filament slot(s)."), n),
                 _L("AMS Accessory"), wxOK | wxICON_INFORMATION, this);
}

void AmsAccessoryDialog::on_sync(wxCommandEvent& /*evt*/)
{
    const AmsAccessoryConfig cfg = collect();
    AmsAccessoryManager::get().save(m_dev_id, cfg);

    // Explicit user action — never fail silently: explain the prerequisites up front.
    if (!cfg.valid()) {
        wxMessageBox(_L("Enable the accessory and enter its address first."),
                     _L("AMS Accessory"), wxOK | wxICON_WARNING, this);
        return;
    }
    if (!m_obj) {
        wxMessageBox(_L("No printer is selected. Connect/select the Bambu printer you want to "
                        "sync the filament into, then try again.\n\nUse \"Test\" to check the "
                        "accessory connection without a printer."),
                     _L("AMS Accessory"), wxOK | wxICON_WARNING, this);
        return;
    }

    wxBusyCursor cursor;
    std::string err;
    int updated = AmsAccessoryManager::get().sync(m_obj, &err);
    if (updated < 0) {
        wxMessageBox(from_u8(err.empty() ? "Sync failed" : err), _L("AMS Accessory"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    if (updated == 0) {
        wxMessageBox(from_u8(err.empty() ? "Nothing was synced." : err), _L("AMS Accessory"),
                     wxOK | wxICON_WARNING, this);
        return;
    }

    // Refresh the slicer AMS list so the merged data is visible immediately.
    wxGetApp().sidebar().load_ams_list(m_obj);

    wxMessageBox(wxString::Format(_L("Synced %d filament slot(s) from the accessory."), updated),
                 _L("AMS Accessory"), wxOK | wxICON_INFORMATION, this);
}

void AmsAccessoryDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
