#ifndef slic3r_GUI_PrinterCameraDialog_hpp_
#define slic3r_GUI_PrinterCameraDialog_hpp_

#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

// Live view of a Creality K-series printer's camera, opened from the web Device tab. The printer
// streams over WebRTC only, so the bundled page (resources/web/orca/printer_camera.html) runs the
// session in the web view itself and nothing is installed on the printer. Where the web view has no
// WebRTC (WebKitGTK builds without it), the page says so instead of showing video.
class PrinterCameraDialog : public WebViewHostDialog
{
public:
    PrinterCameraDialog(wxWindow* parent, const std::string& host);

    const std::string& host() const { return m_host; }

private:
    void add_user_scripts() override;
    nlohmann::json localized_strings() const override;
    void on_script_message(const nlohmann::json& payload) override;

    std::string m_host;
};

}} // namespace Slic3r::GUI

#endif
