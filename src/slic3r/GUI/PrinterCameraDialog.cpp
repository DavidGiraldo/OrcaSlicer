#include "PrinterCameraDialog.hpp"

#include "I18N.hpp"
#include "format.hpp"
#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

PrinterCameraDialog::PrinterCameraDialog(wxWindow* parent, const std::string& host)
    : WebViewHostDialog(parent)
    , m_host(host)
{
    // A modeless dialog only hides on close, which would leave the stream running in the background.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });

    // TRN %1% is the printer's network address, such as 192.168.1.11.
    const wxString title = format_wxstr(_L("Camera - %1%"), m_host);
    if (!create_webview("web/orca/printer_camera.html", title, wxSize(800, 500), wxSize(400, 260))) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, _L("Web view is unavailable.")), wxSizerFlags().Border(wxALL, 20));
        SetSizer(sizer);
    }
}

void PrinterCameraDialog::add_user_scripts()
{
    if (wxWebView* view = browser())
        view->AddUserScript(wxString::FromUTF8("window.ORCA_CAMERA_HOST = " + nlohmann::json(m_host).dump() + ";"));
}

// Keyed by the ids the page's T() lookups use.
nlohmann::json PrinterCameraDialog::localized_strings() const
{
    return {
        {"cam_connecting", _u8L("Connecting to the camera...")},
        {"cam_reconnecting", _u8L("Connection lost. Reconnecting...")},
        {"cam_unavailable", _u8L("The camera did not answer. Retrying...")},
        {"cam_no_host", _u8L("Set the printer's address in its connection settings to view the camera.")},
        {"cam_unsupported", _u8L("This system's web view cannot play WebRTC video.")},
    };
}

void PrinterCameraDialog::on_script_message(const nlohmann::json& payload)
{
    handle_common_script_command(payload);
}

}} // namespace Slic3r::GUI
