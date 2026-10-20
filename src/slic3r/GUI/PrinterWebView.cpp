#include "PrinterWebView.hpp"

#include "PrinterWebViewHandler.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <string> // orca-mcp: camera_host()
#include <utility>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <memory>
#include <wx/log.h>
#include "libslic3r/Utils.hpp"
#include <wx/event.h>
#include <boost/log/trivial.hpp>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
// orca-mcp: the camera icon and dialog
#include "PrinterCameraDialog.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Http.hpp"
#include "I18N.hpp"
#include <nlohmann/json.hpp>
#include <wx/utils.h>
#include <wx/webview.h>

class wxWindow;

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace Slic3r {
namespace GUI {

#ifdef __linux__
// Workaround for #7210: WebKitGTK crashes on vue-resize's hidden <object> probe used by
// older Fluidd/Mainsail pages. Swap that <object> for a <div> shim at appendChild time
// and bridge resize events through a fake contentDocument.defaultView so vue-resize keeps
// working. Workaround proposed by @VittC.
static void inject_vue_resize_workaround(wxWebView *webView)
{
    webView->AddUserScript(
        "(function() {"
        "  'use strict';"
        "  function isVueResizeObject(el) {"
        "    return el && el.tagName === 'OBJECT'"
        "        && el.type === 'text/html'"
        "        && el.getAttribute('aria-hidden') === 'true'"
        "        && el.getAttribute('tabindex') === '-1';"
        "  }"
        "  function isResizeObserverParent(p) {"
        "    return p && p.classList && p.classList.contains('resize-observer');"
        "  }"
        "  function makeShim(orig, parentForRO) {"
        "    var shim = document.createElement('div');"
        "    shim.setAttribute('aria-hidden', 'true');"
        "    shim.setAttribute('tabindex', '-1');"
        "    shim.style.display = 'none';"
        "    var fakeWin = document.createElement('div');"
        "    var ro = null;"
        "    var origRemoveEL = fakeWin.removeEventListener.bind(fakeWin);"
        "    fakeWin.removeEventListener = function(type, fn, opts) {"
        "      origRemoveEL(type, fn, opts);"
        "      if (type === 'resize' && ro) { ro.disconnect(); ro = null; }"
        "    };"
        "    Object.defineProperty(shim, 'contentDocument', {"
        "      configurable: true,"
        "      get: function() { return { defaultView: fakeWin }; }"
        "    });"
        "    Object.defineProperty(shim, 'contentWindow', {"
        "      configurable: true,"
        "      get: function() { return fakeWin; }"
        "    });"
        "    if (typeof orig.onload === 'function') { shim.onload = orig.onload; }"
        "    queueMicrotask(function() {"
        "      if (parentForRO && typeof ResizeObserver !== 'undefined') {"
        "        ro = new ResizeObserver(function() {"
        "          fakeWin.dispatchEvent(new Event('resize'));"
        "        });"
        "        ro.observe(parentForRO);"
        "      }"
        "      if (typeof shim.onload === 'function') {"
        "        try { shim.onload(new Event('load')); } catch (e) {}"
        "      }"
        "      shim.dispatchEvent(new Event('load'));"
        "    });"
        "    return shim;"
        "  }"
        "  var origAppend = Node.prototype.appendChild;"
        "  Node.prototype.appendChild = function(child) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origAppend.call(this, makeShim(child, this));"
        "    }"
        "    return origAppend.call(this, child);"
        "  };"
        "  var origInsertBefore = Node.prototype.insertBefore;"
        "  Node.prototype.insertBefore = function(child, ref) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origInsertBefore.call(this, makeShim(child, this), ref);"
        "    }"
        "    return origInsertBefore.call(this, child, ref);"
        "  };"
        "  console.log('[vr-fix] vue-resize WebKitGTK patch active');"
        "})();",
        wxWEBVIEW_INJECT_AT_DOCUMENT_START
    );
}
#endif

// The strings of the page shown when the printer has no connection set up
// (resources/web/orca/missing_connection.html), keyed by its data-i18n ids.
static void add_page_strings(wxWebView *webView)
{
    const nlohmann::json strings = {
        {"printer_connection", _u8L("Printer Connection")},
        {"printer_connection_setup", _u8L("Please set up your printer connection to view the device.")},
    };
    webView->AddUserScript(wxString::FromUTF8(WebViewHostDialog::localized_strings_script(strings)));
}

// orca-mcp: the host of the selected printer's camera, or empty when PrinterCameraDialog cannot show it.
// Only Creality's print host (K-series firmware) serves the WebRTC camera the dialog speaks.
static std::string camera_host()
{
    const DynamicPrintConfig& config    = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto*               host_type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (host_type == nullptr || host_type->value != htCrealityPrint)
        return {};
    const std::string print_host = config.opt_string("print_host");
    return print_host.empty() ? std::string() : Http::get_host_from_url(print_host);
}

// orca-mcp: the message the camera icon posts to open PrinterCameraDialog.
static const char* const k_open_camera_command = "orca_open_camera";

// orca-mcp: adds a Camera icon to the printer's page that asks Orca to open PrinterCameraDialog. It goes
// at the end of Fluidd's navigation rail, cloned from the last item so it takes Fluidd's look; a page
// without that rail (Mainsail, a custom UI) gets a floating button instead. The observer puts the icon
// back whenever Fluidd re-renders the rail.
static wxString camera_icon_script()
{
    static const char* const body = R"JS(
(function () {
  if (window.__orcaCameraIcon) return;
  window.__orcaCameraIcon = true;
  const ID = "orca-camera-icon";
  // Material Design Icons "camera", the set Fluidd draws its rail with.
  const PATH = "M4,4H7L9,2H15L17,4H20A2,2 0 0,1 22,6V18A2,2 0 0,1 20,20H4A2,2 0 0,1 2,18V6A2,2 0 0,1 4,4M12,7A5,5 0 0,0 7,12A5,5 0 0,0 12,17A5,5 0 0,0 17,12A5,5 0 0,0 12,7M12,9A3,3 0 0,1 15,12A3,3 0 0,1 12,15A3,3 0 0,1 9,12A3,3 0 0,1 12,9Z";
  function open(event) {
    event.preventDefault();
    event.stopPropagation();
    window.wx.postMessage(JSON.stringify({ command: COMMAND }));
  }
  function addToRail() {
    const rail = document.querySelector(".v-navigation-drawer .nav-items");
    const items = rail ? rail.querySelectorAll("a.v-list-item") : [];
    if (!items.length) return false;
    const icon = items[items.length - 1].cloneNode(true);
    icon.id = ID;
    icon.setAttribute("href", "#");
    icon.removeAttribute("aria-haspopup");
    icon.removeAttribute("aria-expanded");
    [...icon.classList].filter((c) => c.endsWith("--active") || c.startsWith("router-link")).forEach((c) => icon.classList.remove(c));
    icon.title = TITLE;
    const path = icon.querySelector("svg path");
    if (path) path.setAttribute("d", PATH);
    const label = icon.querySelector(".v-list-item__title");
    if (label) label.textContent = TITLE;
    icon.addEventListener("click", open);
    rail.appendChild(icon);
    return true;
  }
  function addFloating() {
    const button = document.createElement("button");
    button.id = ID;
    button.title = TITLE;
    button.style.cssText = "position:fixed;left:12px;bottom:12px;z-index:10000;width:40px;height:40px;border:none;" +
      "border-radius:50%;background:#009688;cursor:pointer;display:flex;align-items:center;justify-content:center";
    button.innerHTML = `<svg viewBox="0 0 24 24" width="22" height="22"><path fill="#fff" d="${PATH}"/></svg>`;
    button.addEventListener("click", open);
    document.body.appendChild(button);
  }
  const ensure = () => document.getElementById(ID) || addToRail();
  new MutationObserver(ensure).observe(document.documentElement, { childList: true, subtree: true });
  ensure();
  setTimeout(() => document.getElementById(ID) || addFloating(), 5000);
})();
)JS";
    // TRN Tooltip of the icon added to the printer's web page that opens its camera.
    return wxString::FromUTF8("(function () { const TITLE = " + nlohmann::json(_u8L("Camera")).dump() +
                              "; const COMMAND = " + nlohmann::json(k_open_camera_command).dump() + ";" + body + "})();");
}

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_browser(nullptr)
    , m_zoomFactor(100)
    , m_apikey()
    , m_apikey_sent(false)
    , m_url_deferred()
    , m_handler(std::make_unique<PrinterWebViewHandler>(*this))
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

      // Create the webview
    create_browser();
    m_reset_on_show = WebView::NeedsRecreateOnShow();

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    /* m_browser->GetUserAgent() may lead crash
    if (wxGetApp().get_mode() == comDevelop) {
        wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("Backend: %s Version: %s", m_browser->GetClassInfo()->GetClassName(),
            wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("User Agent: %s", m_browser->GetUserAgent());
    }
    */

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    m_handler.reset();
    // orca-mcp: the camera dialog is a top-level window, so it would outlive this panel.
    if (m_camera_dialog)
        m_camera_dialog->Destroy();

    // Destroy the webview
    if(m_browser){
        m_browser->Destroy();
        m_browser = nullptr;
    }


    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}

void PrinterWebView::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    add_page_strings(m_browser);

#ifdef __linux__
    inject_vue_resize_workaround(m_browser);

    auto cookiesPath = boost::filesystem::path(data_dir() + "/cache/cookies.db");
    auto wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
    auto wv_ctx = webkit_web_view_get_context(wv);
    auto cookieManager = webkit_web_context_get_cookie_manager(wv_ctx);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrinterWebView::OnNewWindow, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this);
    update_mode();
}

void PrinterWebView::reset_browser()
{
    m_browser->Destroy(); // also removes it from the sizer
    create_browser();
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    Layout();

    // OnLoaded may have cleared m_url_deferred already, so requeue the last url for Show().
    m_apikey_sent  = false;
    m_url_deferred = m_url;
}

void PrinterWebView::load_url(wxString& url, wxString apikey)
{
//    this->Show();
//    this->Raise();
    m_url = url;
    m_apikey = apikey;
    m_apikey_sent = false;
    m_handler = create_printer_webview_handler(*this);

    if (this->IsShown()) {
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
        m_browser->LoadURL(url);
    } else {
        m_url_deferred = url;
    }
    //m_browser->SetFocus();
    UpdateState();
}

// orca-mcp: opens the camera of the selected printer, or raises it if it is already open. A dialog
// left on another printer is replaced, so one camera stream runs at a time.
void PrinterWebView::open_camera()
{
    const std::string host = camera_host();
    if (host.empty())
        return;
    if (m_camera_dialog) {
        if (m_camera_host == host) {
            m_camera_dialog->Raise();
            return;
        }
        m_camera_dialog->Close();
    }
    auto* dialog    = new PrinterCameraDialog(wxGetApp().mainframe, host);
    m_camera_dialog = dialog;
    m_camera_host   = host;
    dialog->Show();
}

bool PrinterWebView::Show(bool show)
{
    if (show && std::exchange(m_reset_on_show, false))
        reset_browser();
    if (show && !m_url_deferred.empty()) {
        m_browser->LoadURL(m_url_deferred);
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
    }
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

void PrinterWebView::update_mode()
{
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();
    
    // RemoveAllUserScripts causes WebView to forget about our script message handler, 
    // so re-add it here.
    m_browser->RemoveScriptMessageHandler("wx");
    if (m_browser->AddScriptMessageHandler("wx"))
        WebView::MarkScriptMessageHandlerAdded(m_browser);
    else
        wxLogError("Could not add script message handler");

#ifdef __linux__
    // Re-inject the vue-resize/WebKitGTK workaround that RemoveAllUserScripts just cleared.
    inject_vue_resize_workaround(m_browser);
#endif
    add_page_strings(m_browser);

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent& evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    //ORCA: url loaded successfully, safe to clear
    m_url_deferred.clear();
    SendAPIKey();
    // orca-mcp: the Camera icon, for a printer whose camera PrinterCameraDialog can show.
    if (!camera_host().empty())
        m_browser->RunScript(camera_icon_script());
  
    if (m_handler != nullptr) {
        m_handler->on_loaded(evt);
        return;
    }
}

void PrinterWebView::OnNewWindow(wxWebViewEvent& evt)
{
  const wxString url = evt.GetURL();
  if (!url.empty())
    wxLaunchDefaultBrowser(url);
  evt.Veto();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt)
{
  // orca-mcp: the Camera icon. Opened after the callback returns: GTK and macOS deliver the message
  // inside the native web view callback, where window work is unsafe.
  const nlohmann::json message = nlohmann::json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
  if (message.is_object() && message.value("command", "") == k_open_camera_command) {
    CallAfter([this] { open_camera(); });
    return;
  }
  if (m_handler != nullptr)
    m_handler->on_script_message(evt);
}


} // GUI
} // Slic3r
