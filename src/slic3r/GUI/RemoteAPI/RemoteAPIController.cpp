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
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"

#include <array>
#include <atomic>
#include <future>
#include <sstream>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

static Controller *g_controller = nullptr; // set in ctor, cleared in dtor

Controller::Controller()  { g_controller = this; }
Controller::~Controller() { g_controller = nullptr; }

void Controller::notify_config_changed(int preset_type)
{
    if (g_controller) g_controller->on_config_changed(preset_type);
}

void Controller::notify_project_opened()
{
    if (g_controller == nullptr) return;
    // On the GUI thread at the end of Plater::load_project.
    std::string name = wxGetApp().plater()->get_project_filename().ToUTF8().data();
    wxGetApp().remote_api_server().broadcast({{"event", "project.opened"}, {"project", name}});
}

void Controller::on_config_changed(int preset_type)
{
    // Called on the GUI thread from Tab::update_dirty. Debounce one event-loop
    // turn: GUI edits and load_config fire this repeatedly.
    {
        std::lock_guard<std::mutex> lk(m_cc_mutex);
        m_cc_pending.insert(preset_type);
        if (m_cc_timer_armed) return;
        m_cc_timer_armed = true;
    }
    wxGetApp().CallAfter([this] {
        std::set<int> pending;
        {
            std::lock_guard<std::mutex> lk(m_cc_mutex);
            pending.swap(m_cc_pending);
            m_cc_timer_armed = false;
        }
        nlohmann::json tabs = nlohmann::json::array();
        for (int t : pending)
            tabs.push_back(t == Preset::TYPE_PRINT        ? "print" :
                           t == Preset::TYPE_FILAMENT     ? "filament" :
                           t == Preset::TYPE_PRINTER      ? "printer" :
                           t == Preset::TYPE_SLA_PRINT    ? "sla_print" :
                           t == Preset::TYPE_SLA_MATERIAL ? "sla_material" : "other");
        wxGetApp().remote_api_server().broadcast({{"event", "config.changed"}, {"tabs", tabs}});
    });
}

nlohmann::json Controller::run_on_ui(std::function<nlohmann::json()> fn)
{
    auto promise   = std::make_shared<std::promise<nlohmann::json>>();
    auto future    = promise->get_future();
    // Set if the io side gives up (10s timeout) before the GUI lambda runs. The
    // lambda checks it at the top, so a timed-out request does NOT still apply
    // its side effects (PUT config / reslice) on the GUI thread afterwards.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    wxGetApp().CallAfter([promise, cancelled, fn = std::move(fn)] {
        if (cancelled->load()) return; // caller already returned 504; do nothing
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        cancelled->store(true);
        throw std::runtime_error("ui_timeout");
    }
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

        // Atomic: if any key failed validation, apply NOTHING and report errors.
        // (Previously, valid keys in a mixed batch applied before the 422.)
        if (!errors.empty())
            return {{"applied", nlohmann::json::array()}, {"errors", errors}};

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

void Controller::set_slice_state(const std::function<void(SliceState&)> &mut, const char *event_name)
{
    nlohmann::json snapshot;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        mut(m_slice);
        snapshot = {{"event", event_name},
                    {"state", m_slice.state},
                    {"percent", m_slice.percent},
                    {"message", m_slice.message}};
        if (!m_slice.stats.is_null()) snapshot["stats"] = m_slice.stats;
    }
    wxGetApp().remote_api_server().broadcast(snapshot);
}

// Tracks which Plater the slice events are bound to. A GUI recreate (language/
// skin switch) builds a NEW Plater and the old Bind()s die with it, so we must
// rebind onto the new plater. File-static (only one Controller ever exists)
// keeps this out of the header. See final whole-branch review.
static Plater *s_bound_plater = nullptr;

void Controller::bind_plater_events()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || s_bound_plater == plater) return; // already bound to this plater
    s_bound_plater = plater;
    m_events_bound = true;

    plater->Bind(EVT_SLICING_UPDATE, [this](SlicingStatusEvent &evt) {
        evt.Skip(); // REQUIRED: let the Plater's own handler run (bound earlier = runs after us)
        if (evt.status.percent >= 0)
            set_slice_state([&](SliceState &s) {
                s.state   = "slicing";
                s.percent = evt.status.percent;
                s.message = evt.status.text;
            }, "slice.progress");
    });

    plater->Bind(EVT_PROCESS_COMPLETED, [this](SlicingProcessCompletedEvent &evt) {
        evt.Skip(); // REQUIRED (see above)
        if (evt.error()) {
            auto msg = evt.format_error_message();
            set_slice_state([&](SliceState &s) {
                s.state = "error"; s.percent = -1; s.message = msg.first;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
            }, "slice.error");
            return;
        }
        if (evt.cancelled()) {
            set_slice_state([&](SliceState &s) {
                s.state = "idle"; s.percent = -1; s.message = "cancelled";
            }, "slice.cancelled");
            return;
        }
        // Success: harvest stats on the GUI thread (we ARE on it — wx handler).
        nlohmann::json stats, warnings = nlohmann::json::array();
        auto &plates = wxGetApp().plater()->get_partplate_list();
        const PrintStatistics &ps = plates.get_current_fff_print().print_statistics();
        stats = {
            {"estimated_time", ps.estimated_normal_print_time},
            {"filament_used_mm", ps.total_used_filament},
            {"filament_used_g", ps.total_weight},
            {"total_cost", ps.total_cost}
        };
        if (GCodeProcessorResult *res = plates.get_curr_plate()->get_slice_result()) {
            if (!res->print_statistics.modes.empty())
                stats["estimated_time_seconds"] = res->print_statistics.modes.front().time;
            for (const auto &w : res->warnings)
                warnings.push_back({{"level", w.level}, {"message", w.msg}, {"code", w.error_code}});
        }
        set_slice_state([&](SliceState &s) {
            s.state = "done"; s.percent = 100; s.message = "";
            s.stats = stats; s.warnings = warnings;
        }, "slice.done");
    });
}

Response Controller::handle_slice()
{
    // All checks + the state transition run on the GUI thread inside run_on_ui,
    // so they are serialized (no TOCTOU between the guard and the state change).
    nlohmann::json r = run_on_ui([this]() -> nlohmann::json {
        if (slice_state().state == "slicing")
            return {{"error", "already_slicing"}};
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty())
            return {{"error", "nothing_to_slice"}};
        // reslice() no-ops on an already-valid plate -> no completion event ->
        // status would stick at "slicing". Report the existing result instead.
        if (plater->get_partplate_list().get_curr_plate()->is_slice_result_valid())
            return {{"started", false}, {"already_valid", true}};
        set_slice_state([](SliceState &s) {
            s.state = "slicing"; s.percent = 0; s.message = "starting";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, "slice.started");
        plater->reslice();
        return {{"started", true}};
    });
    if (r.contains("error")) {
        if (r["error"] == "already_slicing") return { 409, r };
        return { 422, r };
    }
    if (r.value("already_valid", false)) return { 200, r };
    return { 202, r };
}

Response Controller::handle_slice_status()
{
    SliceState s = slice_state();
    nlohmann::json j = {{"state", s.state}, {"percent", s.percent}, {"message", s.message}};
    if (!s.stats.is_null()) j["stats"] = s.stats;
    j["warnings"] = s.warnings;
    return { 200, j };
}

Response Controller::dispatch(const Request &req)
{
    try {
        const std::string &t = req.target;
        // Match method + exact path, allowing only a query string after it
        // (so /api/v1/statuses does NOT prefix-match /api/v1/status).
        auto is = [&](const char *m, const char *path) {
            if (req.method != m) return false;
            size_t n = std::char_traits<char>::length(path);
            return t.compare(0, n, path) == 0 && (t.size() == n || t[n] == '?');
        };
        if (is("GET", "/api/v1/status"))        return handle_status();
        if (is("GET", "/api/v1/config"))        return handle_get_config(t);
        if (is("PUT", "/api/v1/config")) {
            try {
                return handle_put_config(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/slice"))        return handle_slice();
        if (is("GET",  "/api/v1/slice/status")) return handle_slice_status();
        return { 404, {{"error", "not_found"}} };
    } catch (const std::exception &e) {
        if (std::string(e.what()) == "ui_timeout")
            return { 504, {{"error", "ui_timeout"}} };
        return { 500, {{"error", "internal"}, {"detail", e.what()}} };
    }
}

}}} // namespace
