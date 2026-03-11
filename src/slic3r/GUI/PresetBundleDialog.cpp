#include "PresetBundleDialog.hpp"
#include "ConfigWizard.hpp"
#include "ExportPresetBundleDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include <libslic3r/Config.hpp>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/msw/app.h>
#include <wx/msw/menu.h>
#include <wx/msw/window.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <libslic3r/PresetBundle.hpp>
#include <wx/string.h>
#include "MainFrame.hpp"
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <miniz.h>
namespace Slic3r { 
    namespace GUI {

        PresetBundleDialog::PresetBundleDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
            : DPIDialog(parent, id, _L("PresetBundle"), pos, size, style)
        {
            SetBackgroundColour(*wxWHITE);
            SetMinSize(DESIGN_WINDOW_SIZE);
            create();
            wxGetApp().UpdateDlgDarkUI(this);

            m_watcher = new wxFileSystemWatcher();
            m_watcher->SetOwner(this);

            Bind(wxEVT_FSWATCHER, &PresetBundleDialog::OnFSWatch, this);

            m_watcher->Add(wxFileName(wxGetApp().preset_bundle->dir_user_presets_local.c_str())); // _local
            m_watcher->Add(wxFileName(wxGetApp().preset_bundle->dir_user_presets_subscribed.c_str())); // _subscribed
        }

        PresetBundleDialog::~PresetBundleDialog()
        {
            if (m_watcher)
            {
                m_watcher->RemoveAll();
                delete m_watcher;
            }
        }

        void PresetBundleDialog::OnFSWatch(wxFileSystemWatcherEvent& e)
        {
            GUI::wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem);
            wxGetApp().mainframe->update_side_preset_ui();

            ListBundles();
            e.Skip();
        }

        void PresetBundleDialog::load_url(wxString &url)
        {
            if (!m_browser) return;
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__<< " enter, url=" << url.ToStdString();
            WebView::LoadUrl(m_browser, url);
            m_browser->SetFocus();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< " exit";
        }

        void PresetBundleDialog::create()
        {
            app_config = get_app_config();
            
            wxString TargetUrl = from_u8( (boost::filesystem::path(resources_dir()) / "web/dialog/PresetBundleDialog/index.html").make_preferred().string() );
            wxString strlang = wxGetApp().current_language_code_safe();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(", strlang=%1%") % into_u8(strlang);
            if (strlang != "")
                TargetUrl = wxString::Format("%s?lang=%s", std::string(TargetUrl.mb_str()), strlang);

            TargetUrl = "file://" + TargetUrl;

            wxBoxSizer *topsizer = new wxBoxSizer(wxVERTICAL);
            SetTitle(_L("Preset Bundle"));

            m_browser = WebView::CreateWebView(this, TargetUrl);
            if (m_browser == nullptr) {
                wxLogError("Could not init m_browser");
                return;
            }

            SetSizer(topsizer);
            topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

             // Set a more sensible size for web browsing
            wxSize pSize = FromDIP(wxSize(820, 660));
            SetSize(pSize);

            int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
            int screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
            int MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
            wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
            Move(tmpPT);

            Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PresetBundleDialog::OnScriptMessage, this, m_browser->GetId());
            
            load_url(TargetUrl);
        }

        bool PresetBundleDialog::DeleteBundleById(const wxString& id)
        { 
            auto* bundle = wxGetApp().preset_bundle;
            if (!bundle || id.empty()) return false;

            const std::string bundle_id = id.ToStdString();
            auto it = bundle->m_bundles.find(bundle_id);
            if (it == bundle->m_bundles.end()) return false;

            const std::string metadata_path = it->second.path;
            const boost::filesystem::path bundle_dir = boost::filesystem::path(metadata_path).parent_path();

            const BundleType bundle_type = it->second.bundle_type;
            if(bundle_type == BundleType::Subscribed)
            {
                //do unsubscribe before deleting locally
            }

            auto remove_from_collection = [&](PresetCollection& c) {
                std::vector<std::string> to_delete;
                for (const auto& p : c.get_presets()) {
                    if (p.bundle_id == bundle_id)
                        to_delete.push_back(p.name);
                }
                for (const auto& name : to_delete)
                    c.delete_preset(name);
            };

            remove_from_collection(bundle->prints);
            remove_from_collection(bundle->filaments);
            remove_from_collection(bundle->printers);

            bundle->m_bundles.erase(it);

            boost::system::error_code ec;
            if (!bundle_dir.empty() && boost::filesystem::exists(bundle_dir))
                boost::filesystem::remove_all(bundle_dir, ec);

            bundle->update_compatible(PresetSelectCompatibleType::Always);
            return true;
        }

        bool PresetBundleDialog::UnsubscribeBundleById(const std::string& id)
        {
            return wxGetApp().unsubscribe_bundle(id);
        }

        void PresetBundleDialog::on_dpi_changed(const wxRect &suggested_rect) { this->Refresh(); }

        void PresetBundleDialog::RunScript(const wxString &s)
        {
            if (!m_browser) return;

            WebView::RunScript(m_browser, s);
        }

        void PresetBundleDialog::OnScriptMessage(wxWebViewEvent& e)
        {
            try {
                wxString strInput = e.GetString();
                BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;OnRecv:" << strInput.c_str();
                json     j        = json::parse(strInput.utf8_string());

                wxString strCmd = j["command"];
                BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;Command:" << strCmd;

                if( strCmd == "request_bundles")
                {
                    ListBundles();
                }
                else if (strCmd == "close_page") {
                        this->EndModal(wxID_CANCEL);
                }
                else if (strCmd == "export_page")
                {
                    wxGetApp().CallAfter([this]() {
                        ExportPresetBundleDialog dlg(this);
                        dlg.ShowModal();
                    });
                }
                else if ( strCmd == "top_row_menu_action" )
                {
                    if(j["action"] == "open_folder")
                    {
                        std::string id = j["bundle_id"];
                        OpenFolder(id);
                    }
                    else if(j["action"] == "delete_bundle")
                    {
                        std::string id = j["bundle_id"];
                        DeleteBundle(id);
                    }
                    else if(j["action"] == "unsubscribe_bundle")
                    {
                        std::string id = j["bundle_id"];
                        UnsubscribeBundle(id);
                    }
                }

            } catch (std::exception &e) {
                BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;Error:" << e.what();
            }
        }

        // call on dialog create to populate the js local store
        void PresetBundleDialog::ListBundles()
        {
            json res;
            res["command"] = "list_bundles";
            res["sequence_id"] = "2000";
            res["data"] = json::array();

            const auto& all_bundles = wxGetApp().preset_bundle->m_bundles;
            for (const auto& bundle : all_bundles )
            {
                const auto& metadata = bundle.second;
                json temp;
                temp["id"] = metadata.id;
                temp["name"] = metadata.name;
                temp["type"] = metadata.bundle_type == Subscribed ? "Subscribed" : metadata.bundle_type == Local ? "Local" : "Default";
                temp["author"] = metadata.author;
                temp["version"] = metadata.version;
                temp["description"] = metadata.description;
                temp["path"] = metadata.path;

                temp["printers"] = metadata.printer_presets;
                temp["filaments"] = metadata.filament_presets;
                temp["presets"] = metadata.print_presets;

                res["data"].push_back(std::move(temp));
            }
            
            wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(res.dump(-1, ' ', false, json::error_handler_t::ignore)));
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }

        void PresetBundleDialog::OpenFolder(const std::string& id)
        {
            wxString target = _L(wxGetApp().preset_bundle->m_bundles.find(id)->second.path);
            wxFileName fn(target);
            if (fn.FileExists())
                target = fn.GetPath();

            if (target.empty() || !wxFileName::DirExists(target)) {
                wxMessageBox(_L("Bundle folder does not exist."), _L("Open Folder"), wxOK | wxICON_WARNING, this);
                return;
            }

            if (!wxLaunchDefaultApplication(target)) {
                wxMessageBox(_L("Failed to open folder."), _L("Open Folder"), wxOK | wxICON_ERROR, this);
            }
        }

        void PresetBundleDialog::DeleteBundle(const std::string& id)
        {
            if (id.empty())
                return;

            const int rc = wxMessageBox(
                _L("Delete selected bundle from folder and all presets loaded from it?"),
                _L("Delete Bundle"),
                wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                this);

            if (rc != wxYES)
                return;

            if (!DeleteBundleById(id)) {
                wxMessageBox(_L("Failed to remove bundle."), _L("Remove Bundle"), wxOK | wxICON_ERROR, this);
                return;
            }
            wxGetApp().mainframe->update_side_preset_ui();
        }

        void PresetBundleDialog::UnsubscribeBundle(const std::string& id)
        {
            if (id.empty())
                return;

            const int rc = wxMessageBox(
                _L("Unsubscribe bundle and delete bundle from folder?"),
                _L("Unsubscribe and Delete Bundle"),
                wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                this);

            if (rc != wxYES)
                return;

            if(!UnsubscribeBundleById(id))
            {
                wxMessageBox(_L("Failed to unsubscribe bundle."), _L("Unsubscribe Bundle"), wxOK | wxICON_ERROR, this);
                return;
            }
            
            if (!DeleteBundleById(id)) {
                wxMessageBox(_L("Failed to remove bundle."), _L("Remove Bundle"), wxOK | wxICON_ERROR, this);
                return;
            }
            wxGetApp().mainframe->update_side_preset_ui();
        }

    }
}
