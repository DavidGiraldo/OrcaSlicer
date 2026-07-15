// src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.cpp
#include "RemoteAPIConfig.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <random>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

Config Config::load()
{
    AppConfig *app_config = wxGetApp().app_config;
    Config     cfg;
    cfg.enabled  = app_config->get_bool("remote_api_enabled");
    cfg.bind_lan = app_config->get_bool("remote_api_bind_lan");
    if (app_config->has("remote_api_port")) {
        try { cfg.port = std::stoi(app_config->get("remote_api_port")); }
        catch (...) { cfg.port = 13130; }
    }
    cfg.token = app_config->get("remote_api_token");
    if (cfg.token.empty()) {
        cfg.token = generate_token();
        app_config->set("remote_api_token", cfg.token);
        // No save() here: AppConfig is dirty-flagged and the idle handler persists it.
    }
    return cfg;
}

void Config::save() const
{
    AppConfig *app_config = wxGetApp().app_config;
    app_config->set_bool("remote_api_enabled", enabled);
    app_config->set_bool("remote_api_bind_lan", bind_lan);
    app_config->set("remote_api_port", std::to_string(port));
    app_config->set("remote_api_token", token);
    app_config->save();
}

std::string Config::generate_token()
{
    static const char hex[] = "0123456789abcdef";
    std::random_device              rd;
    std::uniform_int_distribution<> dist(0, 15);
    std::string                     tok(32, '0');
    for (auto &c : tok) c = hex[dist(rd)];
    return tok;
}

}}} // namespace
