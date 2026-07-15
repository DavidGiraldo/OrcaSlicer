// src/slic3r/GUI/RemoteAPI/RemoteAPIController.cpp
#include "RemoteAPIController.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <array>
#include <future>
#include <sstream>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

Controller::Controller() = default; // event Binds arrive in Task 10

nlohmann::json Controller::run_on_ui(std::function<nlohmann::json()> fn)
{
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future  = promise->get_future();
    wxGetApp().CallAfter([promise, fn = std::move(fn)] {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        throw std::runtime_error("ui_timeout");
    return future.get(); // rethrows GUI-side exceptions
}

static nlohmann::json config_to_json(const DynamicPrintConfig &cfg,
                                     const std::vector<std::string> *only_keys)
{
    nlohmann::json out = nlohmann::json::object();
    auto emit = [&](const std::string &key) {
        const ConfigOption *opt = cfg.option(key);
        if (opt != nullptr)
            out[key] = opt->serialize(); // canonical string form, same as .ini/.3mf
    };
    if (only_keys) {
        for (const auto &k : *only_keys) emit(k);
    } else {
        for (const auto &k : cfg.keys()) emit(k);
    }
    return out;
}

Response Controller::handle_status()
{
    nlohmann::json j = run_on_ui([this]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        auto *plater = wxGetApp().plater();

        nlohmann::json objects = nlohmann::json::array();
        for (const ModelObject *mo : plater->model().objects) {
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        auto plate_valid = plater->get_partplate_list().get_curr_plate()->is_slice_result_valid();
        return {
            {"app", SLIC3R_APP_NAME},
            {"app_version", SoftFever_VERSION},
            {"api_version", "1.0"},
            {"capabilities", {"status", "config", "slice", "events"}},
            {"project", plater->get_project_filename().ToUTF8().data()},
            {"objects", objects},
            {"presets", {
                {"printer",  bundle->printers.get_selected_preset_name()},
                {"print",    bundle->prints.get_selected_preset_name()},
                {"filaments", bundle->filament_presets}
            }},
            {"modified", {
                {"print",    bundle->prints.current_dirty_options(true)},
                {"filament", bundle->filaments.current_dirty_options(true)},
                {"printer",  bundle->printers.current_dirty_options(true)}
            }},
            {"slice_result_valid", plate_valid}
        };
    });
    j["slicing"] = slice_state().state == "slicing";
    return { 200, j };
}

Response Controller::handle_get_config(const std::string &target)
{
    // Optional filter: /api/v1/config?keys=layer_height,wall_loops
    std::vector<std::string> keys;
    auto qpos = target.find("?keys=");
    if (qpos != std::string::npos) {
        std::string list = target.substr(qpos + 6);
        size_t start = 0;
        while (start <= list.size()) {
            auto comma = list.find(',', start);
            auto item  = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty()) keys.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    nlohmann::json j = run_on_ui([keys = std::move(keys)]() -> nlohmann::json {
        DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
        return config_to_json(cfg, keys.empty() ? nullptr : &keys);
    });
    return { 200, {{"config", j}} };
}

static std::string json_value_to_config_string(const nlohmann::json &v)
{
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        std::ostringstream ss;
        ss << v.get<double>();
        return ss.str();
    }
    throw std::runtime_error("unsupported value type");
}

Response Controller::handle_put_config(const std::string &body)
{
    // May throw nlohmann::json::parse_error - caught by the dispatch route
    // below and turned into a 400, distinct from the generic 500 path.
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };

    // NOTE (Task 8 lesson applied): `in` is captured BY VALUE (moved) into the
    // run_on_ui lambda, never by reference. run_on_ui blocks the calling (io)
    // thread up to 10s via future.wait_for and THROWS on timeout without
    // waiting for the queued CallAfter lambda to finish running on the GUI
    // thread. If we captured `in` by reference, a timeout would unwind this
    // stack frame (destroying `in`) while the still-queued lambda later runs
    // against freed memory - exactly the use-after-free that Task 8 hit.
    // Capturing by value/move means the lambda owns its own copy, independent
    // of this function's stack lifetime.
    nlohmann::json result = run_on_ui([in = std::move(in)]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;

        struct Target {
            Preset::Type        type;
            DynamicPrintConfig  cfg;   // full copy of the edited preset config
            bool                touched { false };
        };
        std::array<Target, 3> targets {{
            { Preset::TYPE_PRINT,    bundle->prints.get_edited_preset().config,    false },
            { Preset::TYPE_FILAMENT, bundle->filaments.get_edited_preset().config, false },
            { Preset::TYPE_PRINTER,  bundle->printers.get_edited_preset().config,  false },
        }};

        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();

        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            Target *tgt = nullptr;
            for (auto &t : targets)
                if (t.cfg.option(key) != nullptr) { tgt = &t; break; }
            if (tgt == nullptr) {
                errors[key] = "unknown_key";
                continue;
            }
            try {
                std::string sval = json_value_to_config_string(it.value());
                // Orca's own validation: throws BadOptionTypeException /
                // BadOptionValueException on garbage.
                tgt->cfg.set_deserialize_strict(key, sval);
                tgt->touched = true;
                applied.push_back(key);
            } catch (const std::exception &e) {
                errors[key] = e.what();
            }
        }

        // Apply through the GUI's own path: dirty markers + live panel refresh.
        // Same idiom as PlaterPresetComboBox::change_extruder_color()
        // (PresetComboBoxes.cpp): load_config() diffs+sets+update_dirty()+
        // reload_config(), then Plater::on_config_change() invalidates the
        // slice and repaints.
        for (auto &t : targets)
            if (t.touched) {
                Tab *tab = wxGetApp().get_tab(t.type);
                if (tab == nullptr) { // tab not built yet (shouldn't happen post_init)
                    errors["_tab"] = "tab_unavailable";
                    continue;
                }
                tab->load_config(t.cfg);
                wxGetApp().plater()->on_config_change(t.cfg);
            }

        return {{"applied", applied}, {"errors", errors}};
    });

    int status = result["errors"].empty() ? 200 : 422;
    return { status, result };
}

Response Controller::dispatch(const Request &req)
{
    try {
        const std::string &t = req.target;
        auto is = [&](const char *m, const char *path) {
            return req.method == m && t.rfind(path, 0) == 0;
        };
        if (is("GET", "/api/v1/status"))       return handle_status();
        if (is("GET", "/api/v1/config"))       return handle_get_config(t);
        if (is("PUT", "/api/v1/config")) {
            try {
                return handle_put_config(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        return { 404, {{"error", "not_found"}} };
    } catch (const std::exception &e) {
        if (std::string(e.what()) == "ui_timeout")
            return { 504, {{"error", "ui_timeout"}} };
        return { 500, {{"error", "internal"}, {"detail", e.what()}} };
    }
}

}}} // namespace
