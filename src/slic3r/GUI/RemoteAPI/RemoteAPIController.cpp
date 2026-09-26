// src/slic3r/GUI/RemoteAPI/RemoteAPIController.cpp
#include "RemoteAPIController.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"    // M4b: ModelObject/ModelInstance for GET /objects
#include "slic3r/GUI/NotificationManager.hpp" // in-app change notifications
#include "libslic3r/AppConfig.hpp"             // remote_api_notify toggle
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp" // PUT /preset: refresh a filament slot's sidebar combo
#include "slic3r/GUI/Jobs/Job.hpp"         // arrange/orient: Job::PREPARE_STATE_* scope
#include "slic3r/GUI/GUI.hpp"              // project routes: into_path / into_u8 / from_u8
#include <boost/algorithm/string/predicate.hpp> // project routes: iequals on the extension
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/BuildVolume.hpp"    // all_paths_inside: expose the plate-boundary warning
#include "libslic3r/Format/bbs_3mf.hpp" // LoadStrategy (used by Plater::load_files)
#include "libslic3r/Slicing.hpp"          // M4c: layer_height_profile_adaptive, t_layer_height_range
#include "slic3r/GUI/GUI_ObjectList.hpp" // M4c: obj_list()->update_info_items
#include "slic3r/GUI/GLCanvas3D.hpp"      // plate_render: offscreen thumbnail/gcode render
#include "slic3r/GUI/Camera.hpp"          // plate_render: ViewAngleType
#include "libslic3r/GCode/ThumbnailData.hpp" // plate_render: ThumbnailData/ThumbnailsParams
#include <miniz.h>                        // plate_render: RGBA -> PNG in memory
#include "libslic3r/FlushVolCalc.hpp"      // project scope: g_max_flush_volume bound
#include "slic3r/GUI/WipeTowerDialog.hpp"  // project scope: is_flush_config_modified

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cctype>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

static Controller *g_controller = nullptr; // set in ctor, cleared in dtor

Controller::Controller()  { g_controller = this; }
Controller::~Controller() { g_controller = nullptr; }

// --- in-app notifications for API-driven changes -----------------------------
// Gated by the remote_api_notify setting (default on). Thread-agnostic: marshals
// to the GUI thread via CallAfter, so it is safe from io threads and from inside
// run_on_ui alike. Deliberately NOT used for slice / arrange / orient - Orca's
// own progress + result notifications already cover those (avoid double-up).
static void api_notify(const std::string &text, bool warning = false)
{
    wxGetApp().CallAfter([text, warning]() {
        auto *ac = wxGetApp().app_config;
        bool enabled = (ac && ac->has("remote_api_notify")) ? ac->get_bool("remote_api_notify") : true;
        if (!enabled) return;
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr) return;
        NotificationManager *nm = plater->get_notification_manager();
        if (nm == nullptr) return;
        // Coalesce rapid-fire API messages into ONE toast: anything arriving within
        // a 5s quiet window is re-shown together as a list instead of a toast flood.
        // Plain text, ASCII only (the notification font lacks bullet/arrow glyphs);
        // notifications are toggled via Preferences (remote_api_notify).
        static std::vector<std::string> recent;   // GUI thread only
        static bool       recent_warning = false;
        static wxLongLong last_ms = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - last_ms).GetValue() > 5000) { recent.clear(); recent_warning = false; }
        last_ms = now;
        recent.push_back(text);
        recent_warning |= warning;

        std::string body;
        if (recent.size() == 1)
            body = recent.front();
        else {
            body = "Remote changes:";
            size_t shown = 0;
            for (const std::string &m : recent) {
                if (shown < 6) {
                    std::string item = m;
                    size_t pos = 0;   // indent detail lines under their list entry
                    while ((pos = item.find('\n', pos)) != std::string::npos) { item.replace(pos, 1, "\n   "); pos += 4; }
                    body += "\n - " + item;
                }
                ++shown;
            }
            if (shown > 6)
                body += "\n(+ " + std::to_string(shown - 6) + " more)";
        }
        nm->close_notification_of_type(NotificationType::RemoteAPIChange);
        nm->push_notification(NotificationType::RemoteAPIChange,
            recent_warning ? NotificationManager::NotificationLevel::WarningNotificationLevel
                           : NotificationManager::NotificationLevel::RegularNotificationLevel,
            body);
    });
}

// Human label / unit for a config key, from OrcaSlicer's own definitions.
static std::string api_config_label(const std::string &key)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    return (d != nullptr && !d->label.empty()) ? d->label : key;
}
static std::string api_config_unit(const std::string &key)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    return (d != nullptr && !d->sidetext.empty()) ? (std::string(" ") + d->sidetext) : std::string();
}

static int find_object_index(const Model &model, uint64_t id); // defined with the M4b object handlers below


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

// --- F8 gates (GUI thread only) ----------------------------------------------
// The periodic auto-backup exporter (MainFrame, every 10s) pumps the event
// queue while it serializes the project, so a queued CallAfter API mutation
// could execute INSIDE export_3mf and mutate the model/presets being written -
// observed as heap-corruption crashes under config-write and preset-save
// bursts. Two-way exclusion: API tasks arriving during an export are parked on
// s_deferred_ui_tasks and flushed when the export ends; the exporter skips a
// cycle (re-arming itself) if an API task is on the stack (nested-pump case).
namespace {

struct UiTask : std::enable_shared_from_this<UiTask>
{
    std::shared_ptr<std::promise<nlohmann::json>> promise;
    std::shared_ptr<std::atomic<bool>>            cancelled;
    std::function<nlohmann::json()>               fn;

    void schedule()
    {
        auto self = shared_from_this();
        wxGetApp().CallAfter([self] { self->run(); });
    }
    void run(); // defined after the gate statics below
};

int  s_api_ui_task_depth  = 0;
bool s_backup_in_progress = false;
std::deque<std::shared_ptr<UiTask>> s_deferred_ui_tasks;

void UiTask::run()
{
    if (cancelled->load()) return; // caller already returned 504; do nothing
    if (s_backup_in_progress) {    // parked until set_backup_in_progress(false)
        s_deferred_ui_tasks.push_back(shared_from_this());
        return;
    }
    ++s_api_ui_task_depth;
    // Push the next periodic backup out of the mutation window so it doesn't
    // fire between the calls of an API burst.
    Slic3r::backup_defer(3);
    try {
        promise->set_value(fn());
    } catch (...) {
        promise->set_exception(std::current_exception());
    }
    --s_api_ui_task_depth;
}

} // namespace

bool Controller::api_ui_task_active() { return s_api_ui_task_depth > 0; }

void Controller::set_backup_in_progress(bool v)
{
    s_backup_in_progress = v;
    if (!v && !s_deferred_ui_tasks.empty()) {
        auto parked = std::move(s_deferred_ui_tasks);
        s_deferred_ui_tasks.clear();
        for (auto &t : parked)
            t->schedule(); // re-queued, not run inline: let the export unwind first
    }
}

nlohmann::json Controller::run_on_ui(std::function<nlohmann::json()> fn, int timeout_s)
{
    auto task       = std::make_shared<UiTask>();
    task->promise   = std::make_shared<std::promise<nlohmann::json>>();
    // Set if the io side gives up (10s timeout) before the GUI lambda runs. The
    // lambda checks it at the top, so a timed-out request does NOT still apply
    // its side effects (PUT config / reslice) on the GUI thread afterwards.
    task->cancelled = std::make_shared<std::atomic<bool>>(false);
    task->fn        = std::move(fn);
    auto future     = task->promise->get_future();
    task->schedule();
    if (future.wait_for(std::chrono::seconds(timeout_s)) != std::future_status::ready) {
        task->cancelled->store(true);
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
        PartPlateList &plates      = plater->get_partplate_list();
        auto           plate_valid = plates.get_curr_plate()->is_slice_result_valid();
        return {
            {"app", SLIC3R_APP_NAME},
            {"app_version", SoftFever_VERSION},
            {"api_version", "1.0"},
            {"capabilities", {"status", "config", "slice", "events", "model", "preset", "gcode", "objects", "arrange", "orient", "object_config", "slice_breakdown", "plate_render", "plates"}},
            {"current_plate", plates.get_curr_plate_index()},
            {"plate_count", plates.get_plate_count()},
            // The undo-stack test the "unsaved changes" prompt uses (Plater::close_with_confirm), plus
            // the title bar's dirty flag, which also covers edits that take no snapshot (plate names).
            {"project_dirty", !plater->up_to_date(false, false) || plater->is_project_dirty()},
            {"presets_dirty", wxGetApp().has_current_preset_changes()},
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

// Percent-decode a query value. Clients (e.g. httpx) percent-encode ',' as %2C, so the
// keys list must be decoded BEFORE splitting on ',' - otherwise the whole list is one
// unmatched key and the filter returns nothing.
static std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) { return c <= '9' ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10); };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += char(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Percent-decoded value of query parameter `name` in a request target; empty when absent.
static std::string query_param(const std::string &target, const char *name)
{
    auto qpos = target.find('?');
    if (qpos == std::string::npos) return {};
    const std::string key = std::string(name) + "=";
    const std::string q   = target.substr(qpos + 1);
    size_t start = 0;
    while (start <= q.size()) {
        auto amp = q.find('&', start);
        std::string item = q.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (item.size() >= key.size() && item.compare(0, key.size(), key) == 0)
            return url_decode(item.substr(key.size()));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return {};
}

// Optional ?plate=N on routes that act on "the current plate". Returns false and fills `err` when
// the value is not an integer; `plate` stays -1 when the parameter is absent.
static bool read_plate_param(const std::string &target, int &plate, Response &err)
{
    plate = -1;
    const std::string s = query_param(target, "plate");
    if (s.empty()) return true;
    try {
        size_t used = 0;
        plate = std::stoi(s, &used);
        if (used == s.size() && plate >= 0) return true;
    } catch (...) {}
    err = { 400, {{"error", "bad_param"}, {"param", "plate"}} };
    return false;
}

// GUI thread. Makes `plate` the current plate as a click on its tab would; -1 leaves the current
// plate alone. Returns an error object, or null on success.
static nlohmann::json select_plate_for(Plater *plater, int plate)
{
    PartPlateList &plates = plater->get_partplate_list();
    if (plate < 0) return nullptr;
    if (plate >= plates.get_plate_count()) return {{"error", "unknown_plate"}, {"count", plates.get_plate_count()}};
    if (plate == plates.get_curr_plate_index()) return nullptr;
    // Switching retargets the background process; mid-slice that would orphan the running print.
    if (plater->is_background_process_slicing()) return {{"error", "busy_slicing"}};
    if (plater->select_plate(plate) != 0) return {{"error", "select_failed"}};
    return nullptr;
}

// Plate-scoped settings are stored in the plate's own config; an absent key means "same as the
// global setting". Report that as "default" and give the value that actually applies alongside.
static std::string bed_type_name(BedType bt)
{
    const auto &names = ConfigOptionEnum<BedType>::get_enum_names();
    return size_t(bt) < names.size() ? names[size_t(bt)] : std::string();
}

static std::string print_seq_name(PrintSequence ps)
{
    if (ps == PrintSequence::ByDefault) return "default";
    const auto &names = ConfigOptionEnum<PrintSequence>::get_enum_names();
    return size_t(ps) < names.size() ? names[size_t(ps)] : std::string();
}

// GUI thread. Everything the sidebar's plate tab and the Plate Settings dialog show for plate i.
static nlohmann::json plate_json(Plater *plater, int i)
{
    PartPlateList &plates = plater->get_partplate_list();
    PartPlate     *plate  = plates.get_plate(i);
    const Model   &model  = plater->model();

    nlohmann::json objects = nlohmann::json::array();
    for (size_t oi = 0; oi < model.objects.size(); ++oi)
        for (size_t ii = 0; ii < model.objects[oi]->instances.size(); ++ii)
            if (plate->contain_instance(int(oi), int(ii)))
                objects.push_back({{"id", (uint64_t) model.objects[oi]->id().id},
                                   {"name", model.objects[oi]->name},
                                   {"instance", ii}});

    const BedType     bed_type   = plate->get_bed_type();
    const DynamicConfig &project = wxGetApp().preset_bundle->project_config;
    const BedType     global_bed = project.has("curr_bed_type") ? project.opt_enum<BedType>("curr_bed_type") : btDefault;
    const Vec3d       origin     = plate->get_origin();
    const BoundingBoxf3 bed      = plate->get_build_volume();
    nlohmann::json spiral = "default";
    if (plate->config()->has("spiral_mode"))
        spiral = plate->config()->opt_bool("spiral_mode");

    return {
        {"index", i},
        {"name", plate->get_plate_name()},
        {"current", i == plates.get_curr_plate_index()},
        {"locked", plate->is_locked()},
        {"empty", plate->empty()},
        {"printable", plate->has_printable_instances()},
        {"slice_result_valid", plate->is_slice_result_valid()},
        {"origin", {origin.x(), origin.y()}},
        {"bed", {{"min", {bed.min.x(), bed.min.y()}}, {"max", {bed.max.x(), bed.max.y()}}}},
        {"bed_type", bed_type == btDefault ? "default" : bed_type_name(bed_type)},
        {"effective_bed_type", bed_type_name(bed_type == btDefault ? global_bed : bed_type)},
        {"print_sequence", print_seq_name(plate->get_print_seq())},
        {"effective_print_sequence", print_seq_name(plate->get_real_print_seq())},
        {"first_layer_sequence", plate->get_first_layer_print_sequence()},
        {"spiral_mode", spiral},
        {"effective_spiral_mode", plate->get_spiral_vase_mode()},
        {"objects", objects},
    };
}

Response Controller::handle_get_config(const std::string &target)
{
    // Optional filter: /api/v1/config?keys=layer_height,wall_loops
    std::vector<std::string> keys;
    auto qpos = target.find("?keys=");
    if (qpos != std::string::npos) {
        std::string list = url_decode(target.substr(qpos + 6));
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

// A vector option that deserialized to ZERO elements is a live crash, not a
// validation failure. ConfigOptionVector::get_at and ::set_to_index both read
// values.front() with no empty check (Config.hpp), and of the vector types only
// ConfigOptionFloatsTempl guards the empty string - coFloatsOrPercents, coInts,
// coBools and coPoints clear their vector and still return true. Print::apply
// clones the option, so the empty vector reaches slicing with data() == nullptr
// and faults on the first read. The GUI cannot reach this state (it writes typed
// values through set_at, and its float parse throws on ""), so this API is the
// writer that has to check.
//
// Emptiness is legitimate exactly where the definition's own default is empty,
// e.g. post_process - "no scripts" is a real value there. Calibrating against
// the default keeps that working without an explicit key list to maintain.
static std::string empty_vector_error(const std::string &key, const DynamicPrintConfig &staged)
{
    const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(staged.option(key));
    if (vec == nullptr || !vec->empty())
        return {};
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def != nullptr)
        if (const auto *dflt = dynamic_cast<const ConfigOptionVectorBase *>(def->default_value.get()))
            if (dflt->empty())
                return {};
    return "expected at least one value, got an empty list";
}

// ---------------------------------------------------------------------------
// Project-scope writes (CFS / multi-material)
//
// PresetBundle::project_config holds the keys of s_project_options - a list that
// grows with upstream - but opening all of them would
// be unsafe. Their consumers index them with unchecked arithmetic whose bound
// comes from a DIFFERENT vector, so a wrong-length write
// is an out-of-bounds heap access rather than a validation failure - e.g.
// Sidebar::auto_calc_flushing_volumes sizes its loop from filament_colour and
// writes flush_volumes_matrix at [n*from + to] (Plater.cpp), and
// Print::_make_wipe_tower uses filament_map values as direct extruder indices.
//
// The usual guard does not help: set_deserialize_strict rejects a malformed
// scalar or enum, but ConfigOptionFloatsTempl::deserialize (Config.hpp) accepts
// any input and unconditionally returns true, so a short or non-numeric matrix
// sails through as "applied". Hence an explicit allow-list, each entry carrying
// the invariant its consumers assume, checked after the whole batch is staged.
static bool project_key_writable(const std::string &key)
{
    static const std::set<std::string> s_writable {
        "flush_volumes_matrix",  // filaments^2 * nozzles
        "flush_multiplier",      // one per nozzle
        "flush_multiplier_fast", // one per nozzle
        "filament_colour",       // its length IS the filament count - kept fixed
        "curr_bed_type",         // enum; btDefault rejected
        "prime_volume_mode",     // enum, self-validating
        "wipe_tower_x",          // per-plate vector, written via set_at
        "wipe_tower_y",
    };
    return s_writable.count(key) > 0;
}

// Why a recognized project key stays read-only. Keeps the 422 actionable rather
// than a bare "not_editable_in_current_config".
static const char *project_key_block_reason(const std::string &key)
{
    if (key == "flush_volumes_vector")
        return "derived_from_flush_volumes_matrix";
    if (key == "filament_colour_type" || key == "filament_multi_colour")
        return "indexed_unchecked_by_the_colour_picker";
    if (key == "filament_map" || key == "filament_volume_map" || key == "filament_nozzle_map")
        return "values_are_unchecked_extruder_indices";
    if (key == "filament_map_mode" || key == "nozzle_volume_type")
        return "tied_to_filament_map";
    if (key == "has_filament_switcher" || key == "enable_filament_dynamic_map")
        return "live_printer_state_not_a_setting";
    return "not_writable_project_key";
}

// ConfigOptionFloatsTempl::deserialize pushes a zero-initialized value for any
// token it cannot parse and still returns true, so "a,b,c" would be stored as
// {0,0,0} and reported as applied - a silent wrong answer that no range check can
// catch, because 0 is a perfectly valid purge volume. Vet the raw string first.
static std::string numeric_list_error(const std::string &s)
{
    if (s.empty())
        return "expected a comma-separated list of numbers, got an empty string";
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        std::string  tok   = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t b     = tok.find_first_not_of(" \t");
        const size_t e     = tok.find_last_not_of(" \t");
        tok                = (b == std::string::npos) ? std::string() : tok.substr(b, e - b + 1);
        try {
            size_t pos = 0;
            // The parsed value is deliberately dropped: this only decides whether
            // the token is a number at all. stod is [[nodiscard]], hence the cast.
            static_cast<void>(std::stod(tok, &pos));
            if (pos != tok.size())
                throw std::invalid_argument("trailing characters");
        } catch (const std::exception &) {
            return "\"" + tok + "\" is not a number";
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return {};
}

// Same class of silent failure for colours: ConfigOptionStrings::deserialize
// accepts anything, and a colour the parser cannot read becomes black in the
// flush estimator rather than an error.
static std::string colour_list_error(const std::string &s)
{
    size_t start = 0;
    while (true) {
        const size_t semi = s.find(';', start);
        std::string  tok  = s.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        const size_t b    = tok.find_first_not_of(" \t");
        const size_t e    = tok.find_last_not_of(" \t");
        tok               = (b == std::string::npos) ? std::string() : tok.substr(b, e - b + 1);
        const bool ok = (tok.size() == 7 || tok.size() == 9) && tok[0] == '#' &&
                        std::all_of(tok.begin() + 1, tok.end(),
                                    [](char c) { return std::isxdigit((unsigned char) c) != 0; });
        if (!ok)
            return "\"" + tok + "\" is not a #RRGGBB or #RRGGBBAA colour";
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return {};
}

// Checks the staged value against the invariant its consumers assume. Runs over
// the FINAL staged config, after every key in the batch is written, so a request
// that changes filament_colour and flush_volumes_matrix together is judged
// against the new filament count regardless of key order. Returns "" when valid.
static std::string project_value_error(const std::string        &key,
                                       const DynamicPrintConfig &staged,
                                       int                       nozzles,
                                       size_t                    filaments_before,
                                       const BoundingBoxf       &bed)
{
    auto floats_of = [&staged](const std::string &k) -> std::vector<double> {
        const auto *o = staged.option<ConfigOptionFloats>(k);
        return o == nullptr ? std::vector<double>{} : o->values;
    };

    if (key == "flush_volumes_matrix") {
        const std::vector<double> v = floats_of(key);
        const auto *colours = staged.option<ConfigOptionStrings>("filament_colour");
        const size_t filaments = colours == nullptr ? filaments_before : colours->values.size();
        const size_t want = filaments * filaments * size_t(nozzles > 0 ? nozzles : 1);
        if (v.size() != want)
            return "expected " + std::to_string(want) + " values (filaments^2 x nozzles = " +
                   std::to_string(filaments) + "^2 x " + std::to_string(nozzles) + "), got " +
                   std::to_string(v.size());
        for (double d : v)
            if (!std::isfinite(d) || d < 0. || d > double(g_max_flush_volume))
                return "each value must be a finite number within [0, " + std::to_string(g_max_flush_volume) + "]";
        return {};
    }
    if (key == "flush_multiplier" || key == "flush_multiplier_fast") {
        const std::vector<double> v = floats_of(key);
        if (v.size() != size_t(nozzles > 0 ? nozzles : 1))
            return "expected " + std::to_string(nozzles) + " value(s), one per nozzle, got " +
                   std::to_string(v.size());
        // Same range the wipe-tower dialog's spin control enforces.
        for (double d : v)
            if (!std::isfinite(d) || d < 0. || d > 3.)
                return "each value must be a finite number within [0, 3]";
        return {};
    }
    if (key == "filament_colour") {
        const auto *o = staged.option<ConfigOptionStrings>(key);
        const size_t n = o == nullptr ? 0 : o->values.size();
        // The length of filament_colour is what defines the filament count for
        // the wipe tower and the flush matrix. Changing it here would desync
        // every vector sized from it, so colours may be recoloured but not
        // added or removed - that is the filament-preset selection's job.
        if (n != filaments_before)
            return "filament count is fixed here: expected " + std::to_string(filaments_before) +
                   " colours (';'-separated), got " + std::to_string(n);
        return {};
    }
    if (key == "curr_bed_type") {
        // btDefault is reachable only through a raw write (the GUI never offers
        // it) and is a null dereference on the slicing path: its bed-temperature
        // key is "", so the lookup returns nullptr and GCode.cpp dereferences it.
        if (staged.opt_enum<BedType>(key) == btDefault)
            return "\"Default Plate\" is not selectable; pick a concrete plate type";
        return {};
    }
    if (key == "wipe_tower_x" || key == "wipe_tower_y") {
        // Upstream deleted the render-time clamp that used to pull an out-of-bed
        // tower back on, on the stated grounds that "the stored position is
        // already clamped onto the bed, by set_default_wipe_tower_pos_for_plate
        // and again on every drag" (GLCanvas3D::reload_scene). Those are the two
        // writers it knows about; this API is a third, so the invariant is ours
        // to keep now. Reject rather than silently relocate the tower - the rest
        // of this handler validates, it never coerces.
        const std::vector<double> v = floats_of(key);
        const bool  is_x = (key == "wipe_tower_x");
        const double lo  = is_x ? bed.min.x() : bed.min.y();
        const double hi  = is_x ? bed.max.x() : bed.max.y();
        for (double d : v) {
            if (!std::isfinite(d))
                return "must be a finite number";
            if (hi > lo && (d < lo || d > hi)) {
                std::ostringstream ss;
                ss << "off the bed: " << d << " is outside [" << lo << ", " << hi << "]";
                return ss.str();
            }
        }
        return {};
    }
    return {};
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
        std::vector<std::array<std::string, 3>> changes; // key, old, new -> notification

        // Project scope (CFS / multi-material) is staged on its own copy, and is
        // checked AFTER the three presets so that a key present in both lists keeps
        // routing to the preset. Upstream 542cd18d19 dropped the only such key,
        // wipe_tower_rotation_angle, from s_project_options, so the intersection is
        // empty today - the order stays as the guard for the next overlap.
        DynamicPrintConfig     proj_new = bundle->project_config;
        std::set<std::string>  proj_keys;
        const int              nozzles  = bundle->get_printer_extruder_count();
        // Plate-local bed bounds, for the wipe-tower position check. printable_area
        // is a printer-preset key, so it is not in project_config and has to come
        // from here. An empty/unreadable area leaves the box degenerate, which the
        // check treats as "cannot judge" rather than "reject everything".
        BoundingBoxf bed;
        if (const auto *area = bundle->printers.get_edited_preset().config
                                     .option<ConfigOptionPoints>("printable_area"))
            for (const Vec2d &p : area->values)
                bed.merge(p);
        const auto            *colours0 = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const size_t           filaments_before = colours0 == nullptr ? 0 : colours0->values.size();
        const int              plate_idx = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();

        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            Target *tgt = nullptr;
            for (auto &t : targets)
                if (t.cfg.option(key) != nullptr) { tgt = &t; break; }
            if (tgt == nullptr && proj_new.option(key) != nullptr) {
                if (!project_key_writable(key)) {
                    errors[key] = project_key_block_reason(key);
                    continue;
                }
                try {
                    std::string oldv = proj_new.opt_serialize(key);
                    std::string sval = json_value_to_config_string(it.value());
                    // Vet the raw text: the vector deserializers below cannot fail,
                    // so garbage would be stored as zeros and reported as applied.
                    std::string terr;
                    if (key == "flush_volumes_matrix" || key == "flush_multiplier" ||
                        key == "flush_multiplier_fast" || key == "wipe_tower_x" || key == "wipe_tower_y")
                        terr = numeric_list_error(sval);
                    else if (key == "filament_colour")
                        terr = colour_list_error(sval);
                    if (!terr.empty()) {
                        errors[key] = terr;
                        continue;
                    }
                    if (key == "wipe_tower_x" || key == "wipe_tower_y") {
                        // Per-plate vectors: the GUI writes them with set_at at the
                        // plate index (GLCanvas3D::WipeTowerInfo::apply_wipe_tower).
                        // A whole-vector deserialize would collapse them to a single
                        // element and move every other plate's tower.
                        ConfigOptionFloat one;
                        if (!one.deserialize(sval))
                            throw std::runtime_error("expected a number");
                        proj_new.option<ConfigOptionFloats>(key, true)->set_at(&one, plate_idx, 0);
                    } else {
                        proj_new.set_deserialize_strict(key, sval);
                    }
                    // handle_legacy (run inside set_deserialize) may rename or clear
                    // a key and still report success, which would otherwise be
                    // reported as applied while nothing was written.
                    if (proj_new.option(key) == nullptr) {
                        errors[key] = "key_dropped_by_legacy_handler";
                        continue;
                    }
                    proj_keys.insert(key);
                    applied.push_back(key);
                    // Re-serialized, not echoed: handle_legacy can rewrite the value.
                    changes.push_back({ key, oldv, proj_new.opt_serialize(key) });
                } catch (const std::exception &e) {
                    errors[key] = e.what();
                }
                continue;
            }
            if (tgt == nullptr) {
                // F9: distinguish a real typo from a recognized Orca setting that
                // simply isn't editable through the preset configs (plate-scope and
                // computed-layer keys - GET /config serializes the merged config,
                // which is wider than anything writable).
                errors[key] = (print_config_def.get(key) != nullptr)
                                  ? "not_editable_in_current_config"
                                  : "unknown_key";
                continue;
            }
            try {
                std::string oldv = tgt->cfg.opt_serialize(key);
                std::string sval = json_value_to_config_string(it.value());
                // Orca's own validation: throws BadOptionTypeException /
                // BadOptionValueException on garbage.
                tgt->cfg.set_deserialize_strict(key, sval);
                if (std::string ee = empty_vector_error(key, tgt->cfg); !ee.empty())
                    throw std::runtime_error(ee);
                tgt->touched = true;
                applied.push_back(key);
                changes.push_back({ key, oldv, tgt->cfg.opt_serialize(key) });
            } catch (const std::exception &e) {
                errors[key] = e.what();
            }
        }

        // Second pass over the FINAL staged project config: the invariants are
        // cross-key (the matrix size depends on filament_colour), so they can only
        // be judged once the whole batch is written, not as each key arrives.
        for (const std::string &key : proj_keys) {
            std::string err = project_value_error(key, proj_new, nozzles, filaments_before, bed);
            if (!err.empty()) errors[key] = err;
        }

        // Atomic: if any key failed validation, apply NOTHING and report errors.
        // (Previously, valid keys in a mixed batch applied before the 422.)
        if (!errors.empty()) {
            std::string emsg = "Couldn't update settings";
            for (auto it2 = errors.begin(); it2 != errors.end(); ++it2) {
                emsg += "\n - ";
                emsg += api_config_label(it2.key());
            }
            api_notify(emsg, true);
            return {{"applied", nlohmann::json::array()}, {"errors", errors}};
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

        // Project scope has no Tab, so the preset path above does not apply. There
        // is no single "apply a project config change" helper in-tree either - every
        // GUI writer hand-composes this epilogue (WipeTowerDialog::open_flushing_dialog,
        // Sidebar::auto_calc_flushing_volumes, Plater::priv::on_select_bed_type,
        // PlaterPresetComboBox::sync_colour_config). This mirrors them, once per
        // request rather than once per key.
        if (!proj_keys.empty()) {
            Plater *plater = wxGetApp().plater();
            bundle->project_config.apply(proj_new);

            const bool touched_flush = proj_keys.count("flush_volumes_matrix") ||
                                       proj_keys.count("flush_multiplier") ||
                                       proj_keys.count("flush_multiplier_fast");
            const bool touched_tower = proj_keys.count("wipe_tower_x") || proj_keys.count("wipe_tower_y");

            if (proj_keys.count("curr_bed_type")) {
                // The bed type is cached in AppConfig as well, globally and per
                // printer, and export_selections below reads the global copy back -
                // so these have to run first. on_bed_type_change (not
                // Sidebar::set_bed_type_accord_combox) because the latter fires
                // wxEVT_COMBOBOX and would re-enter this same write path.
                BedType    bt = bundle->project_config.opt_enum<BedType>("curr_bed_type");
                AppConfig *ac = wxGetApp().app_config;
                ac->set("curr_bed_type", std::to_string(int(bt)));
                ac->set_printer_setting(bundle->printers.get_selected_preset_name(),
                                        "curr_bed_type", std::to_string(int(bt)));
                plater->on_bed_type_change(bt);
            }
            if (touched_flush)
                wxGetApp().sidebar().set_flushing_volume_warning(is_flush_config_modified());

            bundle->export_selections(*wxGetApp().app_config);
            // Once per request: it re-arms the auto-backup exporter.
            plater->update_project_dirty_from_presets();

            // Flip the flag synchronously. on_config_change only arms a 500 ms
            // one-shot timer, so a GET /status issued right after this 200 would
            // otherwise still report the stale slice as valid.
            plater->get_partplate_list().invalid_all_slice_result();

            // Most project keys are outside Plater::priv::config's whitelist and so
            // are ignored by the per-key loop, but the unconditional tail is what we
            // want: schedule_background_process + title dirty + auto-reslice.
            plater->on_config_change(bundle->full_config());

            if (touched_flush || touched_tower) {
                plater->get_view3D_canvas3D()->reload_scene(true);
                plater->update();
            }
        }

        if (!changes.empty()) {
            std::string msg = "Settings updated";
            int shown = 0;
            for (auto &ch : changes) {
                if (shown < 6) {
                    msg += "\n - ";
                    msg += api_config_label(ch[0]);
                    msg += ": " + ch[1] + " -> " + ch[2] + api_config_unit(ch[0]);
                }
                ++shown;
            }
            if (shown > 6)
                msg += "\n(+ " + std::to_string(shown - 6) + " more)";
            api_notify(msg, false);
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
                    {"message", m_slice.message},
                    {"plate", m_slice.plate}};
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
                s.plate   = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
            }, "slice.progress");
    });

    plater->Bind(EVT_PROCESS_COMPLETED, [this](SlicingProcessCompletedEvent &evt) {
        evt.Skip(); // REQUIRED (see above)
        if (evt.error()) {
            auto msg = evt.format_error_message();
            set_slice_state([&](SliceState &s) {
                s.state = "error"; s.percent = -1; s.message = msg.first;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
                s.plate = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
            }, "slice.error");
            return;
        }
        if (evt.cancelled()) {
            set_slice_state([&](SliceState &s) {
                s.state = "idle"; s.percent = -1; s.message = "cancelled";
            }, "slice.cancelled");
            return;
        }
        // Success: harvest stats on the GUI thread (we ARE on it - wx handler).
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
            // The "toolpath goes beyond the plate boundaries" warning is a GUI plater
            // notification (GLCanvas3D), computed from the gcode viewer's bed-containment
            // check - so it never reaches res->warnings and was invisible to the API.
            // Recompute it here from the same data the viewer uses (BuildVolume::
            // all_paths_inside over the extrude moves) and surface it in warnings[].
            BoundingBoxf3 paths_bbox;
            bool any_path = false;
            for (const auto &mv : res->moves) {
                if (mv.type == EMoveType::Extrude && mv.extrusion_role != erCustom &&
                    mv.width != 0.f && mv.height != 0.f) {
                    paths_bbox.merge(mv.position.cast<double>());
                    any_path = true;
                }
            }
            if (any_path && !wxGetApp().plater()->build_volume().all_paths_inside(*res, paths_bbox))
                warnings.push_back({{"level", 3},
                                    {"message", "A G-code path goes beyond the plate boundaries."},
                                    {"code", "TOOLPATH_OUTSIDE"}});

            // F14: per-feature (extrusion-role) breakdown for the slice-analytics MCP.
            // PrintEstimatedStatistics has no per-role TIME, so sum it per role from the
            // moves (Normal mode, index 0 - matches estimated_time_seconds). Filament comes
            // from used_filaments_per_role; flow is the COMMANDED volumetric rate
            // (feedrate * mm3_per_mm), the right basis for detecting flow-ceiling clamping.
            // Role keys are STABLE Orca config-feature tokens (NOT localized role_to_string)
            // so the MCP predicted_flows keys match by string - see the fork-breakdown plan.
            auto role_token = [](int r) -> const char* {
                switch (r) {
                    case erExternalPerimeter:        return "outer_wall";
                    case erPerimeter:                return "inner_wall";
                    case erInternalInfill:           return "sparse_infill";
                    case erSolidInfill:              return "internal_solid_infill";
                    case erTopSolidInfill:           return "top_surface";
                    case erGapFill:                  return "gap_infill";
                    case erBridgeInfill:             return "bridge";
                    case erOverhangPerimeter:        return "overhang_perimeter";
                    case erBottomSurface:            return "bottom_surface";
                    case erIroning:                  return "ironing";
                    case erInternalBridgeInfill:     return "internal_bridge";
                    case erSkirt:                    return "skirt";
                    case erBrim:                     return "brim";
                    case erSupportMaterial:          return "support";
                    case erSupportMaterialInterface: return "support_interface";
                    case erSupportTransition:        return "support_transition";
                    case erWipeTower:                return "wipe_tower";
                    case erCustom:                   return "custom";
                    case erMixed:                    return "mixed";
                    default:                         return nullptr; // erNone / erCount
                }
            };
            struct RoleAgg { double time_s = 0.0, max_flow = 0.0, sum_ft = 0.0; };
            RoleAgg agg[erCount];
            for (const auto &mv : res->moves) {
                if (mv.type != EMoveType::Extrude) continue;
                int r = (int) mv.extrusion_role;
                if (r <= 0 || r >= erCount) continue;
                double t = mv.time[0];
                double vr = mv.volumetric_rate(); // feedrate * mm3_per_mm
                agg[r].time_s += t;
                agg[r].sum_ft += vr * t;
                if (vr > agg[r].max_flow) agg[r].max_flow = vr;
            }
            double total_time = res->print_statistics.modes.empty() ? 0.0
                                : (double) res->print_statistics.modes.front().time;
            const auto &fpr = res->print_statistics.used_filaments_per_role;
            nlohmann::json roles = nlohmann::json::array();
            for (int r = 1; r < erCount; ++r) {
                const char *tok = role_token(r);
                if (tok == nullptr) continue;
                auto it = fpr.find((ExtrusionRole) r);
                bool has_fil = (it != fpr.end());
                if (agg[r].time_s <= 0.0 && !has_fil) continue;
                nlohmann::json role_j = {
                    {"role", tok},
                    {"time_s", agg[r].time_s},
                    {"time_pct", total_time > 0.0 ? 100.0 * agg[r].time_s / total_time : 0.0},
                    {"flow_mm3_s", {{"max", agg[r].max_flow},
                                    {"mean", agg[r].time_s > 0.0 ? agg[r].sum_ft / agg[r].time_s : 0.0}}}
                };
                if (has_fil) {
                    // used_filaments_per_role.first is METERS (GUI convention); emit mm to
                    // match top-level filament_used_mm. .second is grams.
                    role_j["filament_mm"] = it->second.first * 1000.0;
                    role_j["filament_g"]  = it->second.second;
                }
                roles.push_back(role_j);
            }
            stats["breakdown"] = {
                {"mode", "normal"},
                {"total_time_s", total_time},
                {"roles", roles},
                {"metrics", nlohmann::json::object()},
                {"layers", nlohmann::json::array()}
            };
        }
        set_slice_state([&](SliceState &s) {
            s.state = "done"; s.percent = 100; s.message = "";
            s.stats = stats; s.warnings = warnings;
            s.plate = plates.get_curr_plate_index();
        }, "slice.done");
    });
}

// GUI thread. Why the current plate cannot be sliced, found the way the Slice button finds it
// (Plater::validate_current_plate, then the print's own validation), so the caller gets the text a
// user would see in the notification instead of a generic "cannot be sliced".
static nlohmann::json slice_block_reason(Plater *plater)
{
    bool fits = true, validate_error = false;
    plater->validate_current_plate(fits, validate_error);
    if (!fits)
        return {{"reason", "outside"},
                {"message", "An object is partly outside the plate, exceeds the height limit, or uses a "
                            "filament this plate cannot print. Move it fully on or off the plate."}};
    StringObjectException err = plater->background_process().validate();
    plater->post_process_string_object_exception(err);
    if (err.string.empty())
        return {{"reason", "invalid_state"}, {"message", "The plate cannot be sliced in its current state."}};
    nlohmann::json out = {{"reason", "validation"}, {"message", err.string}};
    if (!err.opt_key.empty()) out["opt_key"] = err.opt_key;
    if (auto *po = dynamic_cast<const PrintObject *>(err.object))
        out["object"] = po->model_object()->name;
    else if (auto *mi = dynamic_cast<const ModelInstance *>(err.object))
        out["object"] = mi->get_object()->name;
    return out;
}

Response Controller::handle_slice(const std::string &target)
{
    int plate = -1;
    Response bad;
    if (!read_plate_param(target, plate, bad)) return bad;
    // All checks + the state transition run on the GUI thread inside run_on_ui,
    // so they are serialized (no TOCTOU between the guard and the state change).
    nlohmann::json r = run_on_ui([this, plate]() -> nlohmann::json {
        if (slice_state().state == "slicing")
            return {{"error", "already_slicing"}};
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty())
            return {{"error", "nothing_to_slice"}};
        if (nlohmann::json e = select_plate_for(plater, plate); !e.is_null())
            return e;
        const int curr = plater->get_partplate_list().get_curr_plate_index();
        // A prior validation failure (e.g. an object that sat outside the bed
        // when the background process last ran) stays LATCHED on the plate
        // (process_completed_with_error) and Plater::reslice() returns directly
        // on it without re-validating. GUI edits clear the latch via the
        // idle-time schedule_background_process() pump, but API-side model
        // mutations never run it - so a once-invalid plate could never slice
        // again over the API even after the geometry was fixed. Force a
        // synchronous re-validation here: it clears the latch when the model
        // has changed since the failure, and re-latches (-> slice_not_started
        // below) when the plate is still genuinely unsliceable.
        plater->update(false, /*force_background_processing_update=*/true);
        // reslice() no-ops on an already-valid plate -> no completion event ->
        // status would stick at "slicing". Report the existing result instead.
        if (plater->get_partplate_list().get_curr_plate()->is_slice_result_valid())
            return {{"started", false}, {"already_valid", true}, {"plate", curr}};
        set_slice_state([curr](SliceState &s) {
            s.state = "slicing"; s.percent = 0; s.message = "starting";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
            s.plate = curr;
        }, "slice.started");
        plater->reslice();
        // F2: reslice() bails out synchronously - with NO completion event -
        // when the plate can't be sliced (update_background_process() INVALID,
        // e.g. an object fully outside the bed; or a prior hard error pinned on
        // the plate). m_is_slicing is only set on the started paths, so if it
        // is false here nothing is coming and the state would wedge at
        // "slicing" forever. Flip it to error instead.
        if (!plater->is_background_process_slicing()) {
            nlohmann::json why = slice_block_reason(plater);
            const std::string message = why["message"].get<std::string>();
            set_slice_state([&message, curr](SliceState &s) {
                s.state = "error"; s.percent = -1;
                s.message = message;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
                s.plate = curr;
            }, "slice.error");
            why["error"] = "slice_not_started";
            why["plate"] = curr;
            return why;
        }
        return {{"started", true}, {"plate", curr}};
    });
    if (r.contains("error")) {
        if (r["error"] == "already_slicing" || r["error"] == "busy_slicing") return { 409, r };
        if (r["error"] == "unknown_plate") return { 404, r };
        return { 422, r };
    }
    if (r.value("already_valid", false)) return { 200, r };
    return { 202, r };
}

Response Controller::handle_slice_status()
{
    SliceState s = slice_state();
    nlohmann::json j = {{"state", s.state}, {"percent", s.percent}, {"message", s.message}, {"plate", s.plate}};
    if (!s.stats.is_null()) {
        j["stats"] = s.stats;
        // F14: surface the per-role breakdown at TOP LEVEL - the slice-analytics MCP
        // reads status.breakdown (build_breakdown / prediction_check), not stats.breakdown.
        if (s.stats.is_object() && s.stats.contains("breakdown"))
            j["breakdown"] = s.stats["breakdown"];
    }
    j["warnings"] = s.warnings;
    return { 200, j };
}

// F2: POST /api/v1/slice/cancel - abort a running slice, or unwedge a stale
// "slicing" state (previously the only recovery was killing the app). Stops the
// background process if it is actually running, then resets the API state; the
// EVT_PROCESS_COMPLETED(cancelled) handler, if it fires too, sets the same
// idle/cancelled state again (idempotent).
Response Controller::handle_slice_cancel()
{
    nlohmann::json r = run_on_ui([this]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        bool was_slicing = slice_state().state == "slicing";
        if (plater != nullptr)
            plater->stop_background_slicing();
        set_slice_state([](SliceState &s) {
            s.state = "idle"; s.percent = -1; s.message = "cancelled";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, "slice.cancelled");
        return {{"cancelled", was_slicing}};
    });
    return { 200, r };
}

// M4a: POST /api/v1/model  body {"path":"<orca-host path>"}
// Accepts .stl/.obj/.3mf/.step/.stp (loaded via LoadStrategy::LoadModel, no embedded
// config). STEP import can raise modal dialogs that would wedge the single GUI
// thread with no remote way to dismiss: StepMeshDialog and the non-UTF8 name
// warning are gated by app-config flags forced safe for the duration of the load
// (deflection defaults come from app config); the multipart-object prompt in
// Plater::load_files is skipped while an API task is on the stack (keeps the
// solids as separate objects, the dialog's No answer).
Response Controller::handle_load_model(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch route
    if (!in.is_object() || !in.contains("path") || !in["path"].is_string())
        return { 400, {{"error", "missing_path"}} };
    std::string path = in["path"].get<std::string>();

    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    auto ends_with = [&lower](const char *suf) {
        size_t n = std::char_traits<char>::length(suf);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suf) == 0;
    };
    const bool is_step = ends_with(".step") || ends_with(".stp");
    if (!(ends_with(".stl") || ends_with(".obj") || ends_with(".3mf") || is_step)) {
        api_notify("Can't load " + boost::filesystem::path(path).filename().string() + ": unsupported format", true);
        return { 422, {{"error", "unsupported_format"},
                       {"detail", "remote load supports .stl, .obj, .3mf, .step, .stp"}} };
    }

    // 120 s instead of the default 10: OCCT read + tessellation of a real-world
    // STEP (and mesh import of a very large STL) can exceed 10 s, and the load
    // would still complete on the GUI thread after a ui_timeout was returned.
    nlohmann::json r = run_on_ui([path, is_step]() -> nlohmann::json {
        if (!boost::filesystem::exists(path))
            return {{"error", "not_found"}};
        Plater *plater = wxGetApp().plater();
        // Use the stored deflection defaults instead of StepMeshDialog and skip
        // the non-UTF8 name warning while loading; restore the user's flags after.
        AppConfig *cfg = wxGetApp().app_config;
        const bool prev_mesh_dlg  = cfg->get_bool("enable_step_mesh_setting");
        const bool prev_utf8_warn = cfg->get_bool("step_not_utf8_no_warn");
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", false);
            cfg->set_bool("step_not_utf8_no_warn", true);
        }
        // Upstream dropped the std::vector<std::string> overload of load_files;
        // it only wrapped each string in an fs::path, so this is the same call.
        std::vector<size_t> idxs = plater->load_files(
            std::vector<boost::filesystem::path>{ boost::filesystem::path(path) },
            LoadStrategy::LoadModel);
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", prev_mesh_dlg);
            cfg->set_bool("step_not_utf8_no_warn", prev_utf8_warn);
        }
        if (idxs.empty())
            return {{"error", "load_failed"}};
        nlohmann::json objects = nlohmann::json::array();
        for (size_t i : idxs) {
            const ModelObject *mo = plater->model().objects[i];
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"index", i}, {"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        return {{"loaded", true}, {"objects", objects}};
    }, /*timeout_s=*/120);
    std::string fn = boost::filesystem::path(path).filename().string();
    if (r.contains("error")) {
        api_notify("Couldn't load " + fn, true);
        if (r["error"] == "not_found") return { 404, r };
        return { 422, r };
    }
    api_notify("Added " + fn + " to the plate");
    return { 200, r };
}

// M4a: PUT /api/v1/preset  body {"type":"print|filament|printer","name":"..."}
Response Controller::handle_select_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };
    // Filaments go into a project slot, 1-based as the sidebar numbers them.
    long slot = 1;
    if (in.contains("slot")) {
        if (type != Preset::TYPE_FILAMENT || !in["slot"].is_number_integer() || in["slot"].get<long>() < 1)
            return { 400, {{"error", "bad_param"}, {"param", "slot"}} };
        slot = in["slot"].get<long>();
    }

    nlohmann::json r = run_on_ui([type, name, slot]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        PresetCollection &presets = type == Preset::TYPE_PRINT    ? bundle->prints :
                                    type == Preset::TYPE_FILAMENT ? bundle->filaments :
                                                                    bundle->printers;
        // Validate up front: select_preset silently falls back to a visible preset
        // for an unknown name, so its return can't be trusted for a 422.
        Preset *preset = presets.find_preset(name, false, true);
        if (preset == nullptr)
            return {{"error", "unknown_preset"}};
        // find_preset follows renamed_from; select_preset_by_name does not, so select by the real name.
        const std::string real_name = preset->name;
        // select_preset_by_name only matches a visible preset and otherwise falls back to the first
        // visible one while still reporting success. A printer that is merely not installed is made
        // visible first, as the sidebar does when a printer model is picked; anything else hidden
        // would silently select a different preset, so refuse it.
        if (!preset->is_visible) {
            if (type != Preset::TYPE_PRINTER)
                return {{"error", "preset_not_installed"}};
            preset->is_visible = true;
        }
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return {{"error", "tab_unavailable"}};
        if (type == Preset::TYPE_FILAMENT && size_t(slot) > bundle->filament_presets.size())
            return {{"error", "bad_slot"}, {"slots", bundle->filament_presets.size()}};
        // Discard un-applied GUI edits on EVERY collection Tab::select_preset may
        // inspect. It consults dependent tabs too (switching a print/printer preset
        // checks the filament/print collections' dirty state), and ANY dirty one
        // pops a modal UnsavedChangesDialog that would wedge the GUI thread with no
        // remote way to dismiss it. Discarding is acceptable for an automation API
        // (callers apply changes deliberately via PUT /config).
        auto discard_if_dirty = [](PresetCollection &c) {
            if (c.current_is_dirty()) c.discard_current_changes();
        };
        discard_if_dirty(bundle->prints);
        discard_if_dirty(bundle->filaments);
        discard_if_dirty(bundle->sla_materials);
        discard_if_dirty(bundle->printers);
        Plater *plater = wxGetApp().plater();
        if (type == Preset::TYPE_FILAMENT) {
            // A filament is used through its project slot, not the Tab: selecting it in the Tab
            // alone changes nothing that slices. Write the slot as the sidebar combo does
            // (Plater::priv::on_select_preset), then also select it in the Tab below, as the
            // slot's edit button does, so that PUT /config and POST /preset/save act on this
            // filament. With several filaments, the Tab's own sync does not touch the slots.
            const size_t idx         = size_t(slot - 1);
            const bool   was_support = is_support_filament(int(idx));
            bundle->set_filament_preset(idx, real_name);
            plater->update_project_dirty_from_presets();
            bundle->export_selections(*wxGetApp().app_config);
            Sidebar &sidebar = plater->sidebar();
            sidebar.update_dynamic_filament_list();
            if (is_support_filament(int(idx)) != was_support && wxGetApp().app_config->get("auto_calculate_flush") == "all")
                sidebar.auto_calc_flushing_volumes(int(idx));
        }
        bool ok = tab->select_preset(real_name, false, "", /*force_select=*/true, /*force_no_transfer=*/true);
        if (!ok) return {{"error", "select_cancelled"}};
        // Tab::select_preset reports success even when it fell back to another preset.
        const std::string selected = presets.get_selected_preset_name();
        if (selected != real_name)
            return {{"error", "selection_fell_back"}, {"selected", selected}};
        if (type == Preset::TYPE_FILAMENT) {
            plater->on_config_change(bundle->full_config());
            return {{"selected", real_name}, {"slot", slot}};
        }
        return {{"selected", real_name}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_preset") {
            api_notify("No such " + type_s + " preset '" + name + "'", true);
            return { 422, r };
        }
        if (r["error"] == "preset_not_installed" || r["error"] == "bad_slot")
            return { 422, r };
        if (r["error"] == "selection_fell_back") {
            api_notify("Could not select " + type_s + " preset '" + name + "'", true);
            return { 409, r };
        }
        return { 500, r };
    }
    api_notify("Switched to " + type_s + " preset '" + r["selected"].get<std::string>() + "'");
    return { 200, r };
}

// POST /api/v1/preset/save  body {"type":"print|filament|printer","name":"...","detach":false?}
// Persists the collection's currently edited settings as a named user preset
// (create or update) via Tab::save_preset, which with a non-empty name runs the
// GUI Save flow without any dialog.
Response Controller::handle_save_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    bool detach = in.value("detach", false);
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };
    // SavePresetDialog normally enforces the name rules; mirror them headless.
    while (!name.empty() && (name.front() == ' ' || name.back() == ' ')) {
        if (name.front() == ' ') name.erase(name.begin());
        else name.pop_back();
    }
    static const std::string unusable_symbols = "<>[]:/\\|?*\"";
    bool bad_char = name.find_first_of(unusable_symbols) != std::string::npos;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20) bad_char = true;
    if (name.empty() || name == "." || name == ".." || name.size() > 128 || bad_char)
        return { 400, {{"error", "invalid_name"}} };

    nlohmann::json r = run_on_ui([type, name, detach]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        PresetCollection &presets = type == Preset::TYPE_PRINT    ? bundle->prints :
                                    type == Preset::TYPE_FILAMENT ? bundle->filaments :
                                                                    bundle->printers;
        const Preset *existing = presets.find_preset(name, false);
        if (existing != nullptr && (existing->is_system || existing->is_default))
            return {{"error", "name_reserved"}};
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return {{"error", "tab_unavailable"}};
        bool created = existing == nullptr;
        tab->save_preset(name, detach, /*save_to_project=*/false);
        // save_preset returns void; confirm the preset actually landed.
        const Preset *saved = presets.find_preset(name, false);
        if (saved == nullptr) return {{"error", "save_failed"}};
        return {{"saved", name}, {"created", created}};
    });
    if (r.contains("error")) {
        if (r["error"] == "name_reserved") {
            api_notify("Can't overwrite built-in " + type_s + " preset '" + name + "'", true);
            return { 409, r };
        }
        return { 500, r };
    }
    api_notify((r["created"].get<bool>() ? "Saved new " : "Updated ") + type_s + " preset '" + name + "'");
    return { 200, r };
}

// GET /api/v1/presets -> preset names per collection, with system/selected flags.
Response Controller::handle_get_presets()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        auto dump = [](const PresetCollection &c) {
            nlohmann::json arr = nlohmann::json::array();
            const std::string sel = c.get_selected_preset_name();
            for (const Preset &p : c.get_presets()) {
                if (p.is_default) continue;
                arr.push_back({{"name", p.name}, {"system", p.is_system},
                               {"visible", p.is_visible}, {"selected", p.name == sel}});
            }
            return arr;
        };
        return {{"print", dump(bundle->prints)},
                {"filament", dump(bundle->filaments)},
                {"printer", dump(bundle->printers)}};
    });
    return { 200, r };
}

// PUT /api/v1/objects/{id}/layer_height  body {"mode":"adaptive","quality":0..1} | {"mode":"reset"}
// Mirrors GLCanvas3D::LayersEditing::adaptive_layer_height_profile / reset_layer_height_profile.
Response Controller::handle_put_layer_height(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("mode") || !in["mode"].is_string())
        return { 400, {{"error", "missing_mode"}, {"detail", "adaptive|reset"}} };
    std::string mode = in["mode"].get<std::string>();
    if (mode != "adaptive" && mode != "reset")
        return { 400, {{"error", "unknown_mode"}, {"detail", "adaptive|reset"}} };
    double quality = in.value("quality", 0.5);
    if (!(quality >= 0.0 && quality <= 1.0))
        return { 400, {{"error", "quality_out_of_range"}, {"detail", "0..1"}} };

    nlohmann::json r = run_on_ui([id, mode, quality]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        if (mode == "reset") {
            mo->layer_height_profile.clear();
            plater->schedule_background_process();
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Reset layer height profile on '" + mo->name + "'");
            return {{"id", id}, {"mode", "reset"}};
        }
        const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
        // object_max_z 0 -> computed from the object's raw bounding box inside.
        SlicingParameters sp = PrintObject::slicing_parameters(
            full_cfg, *mo, 0.f, plater->fff_print().shrinkage_compensation());
        std::vector<double> profile = layer_height_profile_adaptive(sp, *mo, (float) quality);
        if (profile.size() < 4)
            return {{"error", "profile_failed"}};
        mo->layer_height_profile.set(std::move(profile));
        const std::vector<double> &prof = mo->layer_height_profile.get();
        plater->schedule_background_process();
        wxGetApp().obj_list()->update_info_items(idx);
        double hmin = prof[1], hmax = prof[1];
        for (size_t i = 1; i < prof.size(); i += 2) {
            hmin = std::min(hmin, prof[i]);
            hmax = std::max(hmax, prof[i]);
        }
        api_notify("Applied adaptive layer height to '" + mo->name + "'");
        return {{"id", id}, {"mode", "adaptive"}, {"quality", quality},
                {"points", (unsigned) (prof.size() / 2)},
                {"layer_height_min", hmin}, {"layer_height_max", hmax}};
    });
    if (r.contains("error"))
        return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

// PUT /api/v1/objects/{id}/height_range
//   body {"min_z":a,"max_z":b,"layer_height":h}  (exact same range = update)
//      | {"clear":true}                          (remove all ranges)
Response Controller::handle_put_height_range(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };
    bool clear = in.value("clear", false);
    double min_z = in.value("min_z", -1.0);
    double max_z = in.value("max_z", -1.0);
    double lh    = in.value("layer_height", 0.0);
    if (!clear) {
        if (!in.contains("min_z") || !in.contains("max_z") || !in.contains("layer_height"))
            return { 400, {{"error", "missing_fields"}, {"detail", "min_z, max_z, layer_height (or clear:true)"}} };
        if (!(min_z >= 0.0) || !(max_z > min_z))
            return { 400, {{"error", "bad_range"}, {"detail", "need 0 <= min_z < max_z"}} };
    }

    nlohmann::json r = run_on_ui([id, clear, min_z, max_z, lh]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        auto ranges_json = [mo]() {
            nlohmann::json a = nlohmann::json::array();
            for (const auto &kv : mo->layer_config_ranges)
                a.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                             {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0}});
            return a;
        };
        if (clear) {
            mo->layer_config_ranges.clear();
            plater->changed_object(idx);
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Cleared height ranges on '" + mo->name + "'");
            return {{"id", id}, {"height_ranges", ranges_json()}};
        }
        // Printer limits, same rules as ObjectList's get_min/max_layer_height.
        const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        double minh = pcfg.opt_float("min_layer_height", 0);
        double maxh = pcfg.opt_float("max_layer_height", 0);
        if (maxh < EPSILON) maxh = 0.75 * pcfg.opt_float("nozzle_diameter", 0);
        if (lh < minh || lh > maxh)
            return {{"error", "layer_height_out_of_range"}, {"min", minh}, {"max", maxh}};
        t_layer_height_range key{ min_z, max_z };
        for (const auto &kv : mo->layer_config_ranges) {
            if (kv.first == key) continue;
            if (min_z < kv.first.second && kv.first.first < max_z)
                return {{"error", "overlaps_existing"},
                        {"existing", {kv.first.first, kv.first.second}}};
        }
        DynamicPrintConfig c;
        c.set_key_value("layer_height", new ConfigOptionFloat(lh));
        c.set_key_value("extruder",     new ConfigOptionInt(0));
        mo->layer_config_ranges[key].assign_config(std::move(c));
        plater->changed_object(idx);
        wxGetApp().obj_list()->update_info_items(idx);
        api_notify("Set height range " + std::to_string(min_z) + "-" + std::to_string(max_z) + " mm on '" + mo->name + "'");
        return {{"id", id}, {"height_ranges", ranges_json()}};
    });
    if (r.contains("error"))
        return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

// Shared: parse {"type","name"} and resolve the collection. Returns nullptr collection on bad type.
static PresetCollection *api_preset_collection(const std::string &type_s)
{
    auto *bundle = wxGetApp().preset_bundle;
    return type_s == "print"    ? &bundle->prints :
           type_s == "filament" ? &bundle->filaments :
           type_s == "printer"  ? &bundle->printers : nullptr;
}

// POST /api/v1/preset/config  body {"type","name"} -> full config of that named preset.
// POST (not GET) so names with spaces/@ need no URL encoding; read-only.
Response Controller::handle_get_preset_config(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    nlohmann::json r = run_on_ui([type_s, name]() -> nlohmann::json {
        PresetCollection *presets = api_preset_collection(type_s);
        if (presets == nullptr) return {{"error", "unknown_type"}};
        const Preset *p = presets->find_preset(name, false);
        if (p == nullptr) return {{"error", "unknown_preset"}};
        nlohmann::json cfg = nlohmann::json::object();
        for (const std::string &k : p->config.keys()) cfg[k] = p->config.opt_serialize(k);
        return {{"name", p->name}, {"system", p->is_system}, {"config", cfg}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_type") return { 400, r };
        return { 404, r };
    }
    return { 200, r };
}

// DELETE /api/v1/preset  body {"type","name"} -> remove a USER preset (file + list entry).
// Guards: never system/default, never the selected preset (select another first),
// never a base preset that other presets inherit from (same rule as the GUI).
Response Controller::handle_delete_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };
    nlohmann::json r = run_on_ui([type, type_s, name]() -> nlohmann::json {
        PresetCollection *presets = api_preset_collection(type_s);
        const Preset *p = presets->find_preset(name, false);
        if (p == nullptr) return {{"error", "unknown_preset"}};
        if (p->is_system || p->is_default) return {{"error", "builtin_preset"}};
        if (presets->get_selected_preset_name() == name)
            return {{"error", "preset_selected"},
                    {"detail", "select another preset first, then delete"}};
        for (const Preset &other : presets->get_presets())
            if (other.name != name && other.inherits() == name)
                return {{"error", "has_children"},
                        {"detail", "other presets inherit from this one"}};
        if (!presets->delete_preset(name))
            return {{"error", "delete_failed"}};
        // Refresh the tab's preset list + the plater sidebar combo.
        if (Tab *tab = wxGetApp().get_tab(type); tab != nullptr)
            tab->update_tab_ui();
        wxGetApp().plater()->sidebar().update_presets(type);
        api_notify("Deleted " + type_s + " preset '" + name + "'");
        return {{"deleted", name}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_preset") return { 404, r };
        if (r["error"] == "delete_failed")  return { 500, r };
        return { 409, r };
    }
    return { 200, r };
}

// M4a: GET /api/v1/gcode  -> raw G-code of the current plate's last successful slice
Response Controller::handle_get_gcode(const std::string &target)
{
    int plate_idx = -1;
    Response bad;
    if (!read_plate_param(target, plate_idx, bad)) return bad;
    nlohmann::json meta = run_on_ui([plate_idx]() -> nlohmann::json {
        PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
        if (plate_idx >= plates.get_plate_count())
            return {{"error", "unknown_plate"}, {"count", plates.get_plate_count()}};
        // Each plate keeps its own slice result, so a given plate is read without selecting it.
        PartPlate *plate = plate_idx < 0 ? plates.get_curr_plate() : plates.get_plate(plate_idx);
        if (!plate->is_slice_result_valid())
            return {{"error", "not_sliced"}};
        GCodeProcessorResult *res = plate->get_slice_result();
        if (res == nullptr || res->filename.empty())
            return {{"error", "not_sliced"}};
        return {{"path", res->filename}};
    });
    if (meta.contains("error"))
        return { meta["error"] == "unknown_plate" ? 404 : 409, meta };
    // Read off the GUI thread (only the state lookup above needed it).
    std::string path = meta["path"].get<std::string>();
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return { 500, {{"error", "gcode_file_missing"}} };
    std::ostringstream ss;
    ss << f.rdbuf();
    Response r;
    r.status = 200;
    r.raw_body = ss.str();
    r.raw_content_type = "text/plain";
    return r;
}


Response Controller::handle_plate_render(const std::string &target)
{
    // GET /api/v1/plate/render?view=editor|preview&angle=iso|top|front|left|right|rear|bottom&width=800&height=600
    auto qparam = [&target](const char *name) { return query_param(target, name); };

    int plate = -1;
    Response bad;
    if (!read_plate_param(target, plate, bad)) return bad;

    std::string view = qparam("view");
    if (view.empty()) view = "editor";
    if (view != "editor" && view != "preview")
        return { 400, {{"error", "bad_param"}, {"param", "view"},
                       {"allowed", nlohmann::json::array({"editor", "preview"})}} };

    std::string angle = qparam("angle");
    if (angle.empty()) angle = "iso";
    static const std::map<std::string, Camera::ViewAngleType> angle_map = {
        {"iso",    Camera::ViewAngleType::Iso},   {"top",  Camera::ViewAngleType::Top},
        {"front",  Camera::ViewAngleType::Front}, {"left", Camera::ViewAngleType::Left},
        {"right",  Camera::ViewAngleType::Right}, {"rear", Camera::ViewAngleType::Rear},
        {"bottom", Camera::ViewAngleType::Bottom}};
    auto ait = angle_map.find(angle);
    if (ait == angle_map.end())
        return { 400, {{"error", "bad_param"}, {"param", "angle"},
                       {"allowed", nlohmann::json::array({"iso", "top", "front", "left", "right", "rear", "bottom"})}} };

    // frame=plate  -> zoom to the whole bed (where the part sits, footprint)
    // frame=object -> zoom to the model/toolpaths (detail)
    // default: plate for the editor view, object for the preview view.
    std::string frame = qparam("frame");
    if (!frame.empty() && frame != "plate" && frame != "object")
        return { 400, {{"error", "bad_param"}, {"param", "frame"},
                       {"allowed", nlohmann::json::array({"plate", "object"})}} };

    long w = 800, h = 600;
    try {
        const std::string ws = qparam("width"), hs = qparam("height");
        if (!ws.empty()) w = std::stol(ws);
        if (!hs.empty()) h = std::stol(hs);
    } catch (...) {
        return { 400, {{"error", "bad_param"}, {"param", "width/height"}} };
    }
    w = std::min(std::max(w, 64L), 2048L);
    h = std::min(std::max(h, 64L), 2048L);

    const Camera::ViewAngleType va           = ait->second;
    const bool                  preview      = (view == "preview");
    const bool                  frame_object = frame.empty() ? preview : (frame == "object");
    // Filled on the GUI thread; run_on_ui blocks until the lambda completes, and
    // the shared_ptr keeps the buffer alive even if the io side times out first.
    auto png_out = std::make_shared<std::string>();

    // 30s (not the 10s default): a large plate at 2048px can outrun the default.
    nlohmann::json j = run_on_ui([va, w, h, preview, frame_object, png_out, plate]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (nlohmann::json e = select_plate_for(plater, plate); !e.is_null())
            return e;
        ThumbnailData data;
        if (preview) {
            PartPlate *plate = plater->get_partplate_list().get_curr_plate();
            if (!plate->is_slice_result_valid())
                return {{"error", "no_slice_result"}};
            GLCanvas3D *canvas = plater->get_preview_canvas3D();
            if (canvas == nullptr)
                return {{"error", "render_failed"}, {"reason", "no preview canvas"}};
            // The preview only auto-reloads while its panel is on screen
            // (Plater::priv::update: `if (is_preview_shown()) preview->reload_print()`),
            // so an API-driven slice leaves the GCodeViewer empty. Load it on demand.
            if (!canvas->render_gcode_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object)) {
                plater->reload_print();
                if (!canvas->render_gcode_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object))
                    return {{"error", "render_failed"}, {"reason", "gcode preview render unavailable"}};
            }
        } else {
            GLCanvas3D *canvas = plater->get_view3D_canvas3D();
            if (canvas == nullptr || !canvas->render_plate_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object))
                return {{"error", "render_failed"}, {"reason", "editor plate render failed"}};
        }
        size_t png_size = 0;
        void  *png      = tdefl_write_image_to_png_file_in_memory_ex(
            (const void *)data.pixels.data(), data.width, data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
        if (png == nullptr)
            return {{"error", "render_failed"}, {"reason", "png encode failed"}};
        png_out->assign((const char *)png, png_size);
        mz_free(png);
        return {{"bytes", png_size}};
    }, 30);

    if (j.contains("error")) {
        const std::string err = j["error"].get<std::string>();
        if (err == "unknown_plate") return { 404, j };
        return { err == "no_slice_result" || err == "busy_slicing" ? 409 : 500, j };
    }
    Response r;
    r.status           = 200;
    r.raw_body         = *png_out;
    r.raw_content_type = "image/png";
    return r;
}


Response Controller::handle_get_objects()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        nlohmann::json objects = nlohmann::json::array();
        const Model &model = plater->model();
        PartPlateList &plates = plater->get_partplate_list();
        for (size_t i = 0; i < model.objects.size(); ++i) {
            const ModelObject *mo = model.objects[i];
            auto sz = mo->bounding_box_exact().size();
            // Plate of each instance; -1 when it sits on no plate.
            nlohmann::json instance_plates = nlohmann::json::array();
            for (size_t ii = 0; ii < mo->instances.size(); ++ii)
                instance_plates.push_back(plates.find_instance(int(i), int(ii)));
            nlohmann::json o = {
                {"id", (uint64_t) mo->id().id},
                {"index", i},
                {"name", mo->name},
                {"size_mm", {sz.x(), sz.y(), sz.z()}},
                {"instances", (unsigned) mo->instances.size()},
                {"plate", instance_plates.empty() ? -1 : instance_plates[0].get<int>()},
                {"instance_plates", instance_plates},
            };
            if (!mo->instances.empty()) {
                const ModelInstance *mi = mo->instances.front();
                auto off = mi->get_offset();
                auto rot = mi->get_rotation();
                auto scl = mi->get_scaling_factor();
                o["transform"] = {
                    {"offset",   {off.x(), off.y(), off.z()}},
                    {"rotation", {rot.x(), rot.y(), rot.z()}},
                    {"scale",    {scl.x(), scl.y(), scl.z()}},
                };
                // World-space bbox of instance 0. `offset` above is the MESH
                // ORIGIN, which only coincides with the bbox centre for a
                // centred, unrotated mesh - so plate contact is NOT derivable
                // from it. bbox_min[2] is the real answer: ~0 sits on the
                // plate, >0 floats, <0 sinks into it.
                const BoundingBoxf3 bb = mo->instance_bounding_box(*mi, false);
                if (bb.defined) {
                    o["bbox_min"] = {bb.min.x(), bb.min.y(), bb.min.z()};
                    o["bbox_max"] = {bb.max.x(), bb.max.y(), bb.max.z()};
                    o["on_plate"] = (std::abs(bb.min.z()) < 0.05);
                }
            }
            // M4c readback: per-object overrides + variable-layer-height state
            {
                const DynamicPrintConfig ocfg = mo->config.get();
                nlohmann::json cfgj = nlohmann::json::object();
                for (const std::string &k : ocfg.keys()) cfgj[k] = ocfg.opt_serialize(k);
                o["config"] = cfgj;
                o["custom_layer_profile"] = !mo->layer_height_profile.empty();
                nlohmann::json rngs = nlohmann::json::array();
                for (const auto &kv : mo->layer_config_ranges)
                    rngs.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                                    {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0}});
                o["height_ranges"] = rngs;
            }
            objects.push_back(std::move(o));
        }
        return {{"objects", objects}, {"count", model.objects.size()}};
    });
    return { 200, r };
}


static int find_object_index(const Model &model, uint64_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if ((uint64_t) model.objects[i]->id().id == id)
            return (int) i;
    return -1;
}

Response Controller::handle_delete_object(uint64_t id)
{
    nlohmann::json r = run_on_ui([id]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        std::string oname = plater->model().objects[idx]->name;
        plater->remove((size_t) idx);
        api_notify("Removed '" + oname + "'");
        return {{"deleted", true}, {"id", id}, {"count", plater->model().objects.size()}};
    });
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

Response Controller::handle_transform_object(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    auto read_vec = [&in](const char *k, bool &present) -> Vec3d {
        present = in.contains(k) && in[k].is_array() && in[k].size() == 3;
        if (!present) return Vec3d(0, 0, 0);
        return Vec3d(in[k][0].get<double>(), in[k][1].get<double>(), in[k][2].get<double>());
    };
    bool has_t = false, has_r = false, has_s = false;
    Vec3d tr = read_vec("translate", has_t);
    Vec3d ro = read_vec("rotate", has_r);   // degrees
    Vec3d sc = read_vec("scale", has_s);     // absolute factor
    int instance = 0, target_plate = -1;
    if (in.contains("instance")) {
        if (!in["instance"].is_number_integer() || in["instance"].get<int>() < 0)
            return { 400, {{"error", "bad_param"}, {"param", "instance"}} };
        instance = in["instance"].get<int>();
    }
    if (in.contains("plate")) {
        if (!in["plate"].is_number_integer() || in["plate"].get<int>() < 0)
            return { 400, {{"error", "bad_param"}, {"param", "plate"}} };
        target_plate = in["plate"].get<int>();
    }
    if (!has_t && !has_r && !has_s && target_plate < 0)
        return { 400, {{"error", "no_transform"},
                       {"detail", "provide plate (index), translate (mm), rotate (deg), and/or scale (factor)"}} };

    nlohmann::json r = run_on_ui([=]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        ModelObject *mo = plater->model().objects[idx];
        if (mo->instances.empty()) return {{"error", "no_instance"}};
        if (size_t(instance) >= mo->instances.size())
            return {{"error", "unknown_instance"}, {"instances", mo->instances.size()}};
        if (target_plate >= plates.get_plate_count())
            return {{"error", "unknown_plate"}, {"count", plates.get_plate_count()}};
        ModelInstance *mi = mo->instances[instance];
        if (target_plate >= 0) {
            // Keep the instance where it sits relative to its plate, as dragging it across would;
            // one on no plate is centred on the target instead.
            PartPlate  *target = plates.get_plate(target_plate);
            const int   source = plates.find_instance(idx, instance);
            if (source >= 0) {
                mi->set_offset(mi->get_offset() - plates.get_plate(source)->get_origin() + target->get_origin());
            } else {
                const BoundingBoxf3 bed = target->get_build_volume();
                const BoundingBoxf3 bb  = mo->instance_bounding_box(instance, false);
                const Vec3d shift = bed.center() - bb.center();
                mi->set_offset(mi->get_offset() + Vec3d(shift.x(), shift.y(), 0.0));
            }
        }
        if (has_t) mi->set_offset(mi->get_offset() + tr);
        if (has_r) mi->set_rotation(mi->get_rotation() + ro * 0.017453292519943295); // deg->rad
        if (has_s) mi->set_scaling_factor(sc);
        // A move can carry the instance onto a different plate, so re-register it with the plate
        // list the way upstream's own move path does. is_new=true applies a spiral-vase plate's
        // object settings without the confirmation dialog the GUI would show.
        plates.notify_instance_update(idx, instance, /*is_new=*/true);
        plater->changed_object(idx);
        api_notify(std::string(target_plate >= 0 ? "Moved to plate " + std::to_string(target_plate + 1) + ":"
                               : has_t ? "Moved" : (has_r ? "Rotated" : "Resized")) + " '" + mo->name + "'");
        auto off = mi->get_offset(); auto rot = mi->get_rotation(); auto scl = mi->get_scaling_factor();
        return {{"id", id}, {"instance", instance}, {"plate", plates.find_instance(idx, instance)}, {"transform", {
            {"offset",   {off.x(), off.y(), off.z()}},
            {"rotation", {rot.x(), rot.y(), rot.z()}},
            {"scale",    {scl.x(), scl.y(), scl.z()}}}}};
    });
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

Response Controller::handle_duplicate_object(uint64_t id)
{
    nlohmann::json r = run_on_ui([id]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        Model &model = plater->model();
        int idx = find_object_index(model, id);
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        ModelObject *mo = model.objects[idx];
        if (mo->instances.empty()) return {{"error", "no_instance"}};
        const ModelInstance *src = mo->instances.front();
        Vec3d off = src->get_offset() + Vec3d(10.0, 10.0, 0.0);
        mo->add_instance(off, src->get_scaling_factor(), src->get_rotation(), src->get_mirror());
        // Upstream's increase_instances registers every new copy with the plate it
        // lands on (Plater.cpp, "Register Instance Copies and Moves with Their Plate"),
        // because the plate's filament list and wipe tower preview are read from that
        // registry. changed_object() does not touch it, so do it here too - otherwise a
        // duplicate is saved on no plate and gets no prime tower.
        plater->get_partplate_list().notify_instance_update(idx, (int) mo->instances.size() - 1, /*is_new=*/true);
        plater->changed_object(idx);
        api_notify("Duplicated '" + mo->name + "'");
        return {{"duplicated", true}, {"id", id}, {"instances", (unsigned) mo->instances.size()}};
    });
    if (r.contains("error")) return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

Response Controller::handle_put_object_config(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };
    nlohmann::json r = run_on_ui([id, in]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        DynamicPrintConfig cfg = mo->config.get(); // copy of existing per-object overrides
        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();
        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            if (print_config_def.get(key) == nullptr) { errors[key] = "unknown_key"; continue; }
            try {
                cfg.set_deserialize_strict(key, json_value_to_config_string(it.value()));
                if (std::string ee = empty_vector_error(key, cfg); !ee.empty())
                    throw std::runtime_error(ee);
                applied.push_back(key);
            } catch (const std::exception &e) { errors[key] = e.what(); }
        }
        if (!errors.empty()) { // atomic: apply nothing on any error
            api_notify(std::string("Couldn't set object settings on '") + mo->name + "'", true);
            return {{"applied", nlohmann::json::array()}, {"errors", errors}};
        }
        mo->config.assign_config(std::move(cfg));
        plater->changed_object(idx);
        if (!applied.empty())
            api_notify("Updated " + std::to_string(applied.size()) + " setting(s) on '" + mo->name + "'");
        return {{"applied", applied}, {"errors", errors}, {"object", mo->name}};
    });
    if (r.contains("error")) return { 404, r };
    return { r["errors"].empty() ? 200 : 422, r };
}

Response Controller::handle_arrange(const std::string &target)
{
    int plate = -1;
    Response bad;
    if (!read_plate_param(target, plate, bad)) return bad;
    nlohmann::json r = run_on_ui([plate]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) return {{"error", "empty"}};
        if (!plater->get_ui_job_worker().is_idle()) return {{"busy", true}};
        if (nlohmann::json e = select_plate_for(plater, plate); !e.is_null())
            return e;
        if (plate >= 0 && plater->get_partplate_list().get_curr_plate()->is_locked())
            return {{"error", "plate_locked"}};
        // The job's scope comes from a prepare state that outlives the call: MENU is the plate
        // menu's "this plate only", DEFAULT is everything. Set it every time.
        plater->set_prepare_state(plate >= 0 ? Job::PREPARE_STATE_MENU : Job::PREPARE_STATE_DEFAULT);
        plater->arrange();
        return {{"started", true}, {"plate", plate}};
    });
    if (r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        if (e == "unknown_plate") return { 404, r };
        if (e == "busy_slicing" || e == "plate_locked") return { 409, r };
        return { 422, r };
    }
    if (r.value("busy", false)) return { 409, {{"error", "job_running"}} };
    return { 202, r };
}

Response Controller::handle_orient(const std::string &target)
{
    int plate = -1;
    Response bad;
    if (!read_plate_param(target, plate, bad)) return bad;
    nlohmann::json r = run_on_ui([plate]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) return {{"error", "empty"}};
        if (!plater->get_ui_job_worker().is_idle()) return {{"busy", true}};
        if (nlohmann::json e = select_plate_for(plater, plate); !e.is_null())
            return e;
        if (plate >= 0 && plater->get_partplate_list().get_curr_plate()->is_locked())
            return {{"error", "plate_locked"}};
        // The job's scope comes from a prepare state that outlives the call: MENU is the plate
        // menu's "this plate only", DEFAULT is everything. Set it every time.
        plater->set_prepare_state(plate >= 0 ? Job::PREPARE_STATE_MENU : Job::PREPARE_STATE_DEFAULT);
        plater->orient();
        return {{"started", true}, {"plate", plate}};
    });
    if (r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        if (e == "unknown_plate") return { 404, r };
        if (e == "busy_slicing" || e == "plate_locked") return { 409, r };
        return { 422, r };
    }
    if (r.value("busy", false)) return { 409, {{"error", "job_running"}} };
    return { 202, r };
}

Response Controller::handle_jobs_status()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        return {{"idle", plater->get_ui_job_worker().is_idle()}};
    });
    return { 200, r };
}

Response Controller::handle_get_plates()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater        *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        nlohmann::json list   = nlohmann::json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i)
            list.push_back(plate_json(plater, i));
        return {{"count", plates.get_plate_count()}, {"current", plates.get_curr_plate_index()}, {"plates", list}};
    });
    return { 200, r };
}

Response Controller::handle_get_plate(int index)
{
    nlohmann::json r = run_on_ui([index]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (index < 0 || index >= plater->get_partplate_list().get_plate_count())
            return {{"error", "unknown_plate"}, {"count", plater->get_partplate_list().get_plate_count()}};
        return plate_json(plater, index);
    });
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

// Maps the "error" a plate lambda returned to its status.
static Response plate_error(const nlohmann::json &r)
{
    const std::string e = r["error"].get<std::string>();
    if (e == "unknown_plate") return { 404, r };
    if (e == "busy_slicing" || e == "last_plate" || e == "plate_not_empty" || e == "plate_locked") return { 409, r };
    return { 422, r };
}

// GUI thread. Switching or removing plates retargets the background process at the new current
// plate; doing that mid-slice would leave the running print pointing at a plate that is no longer
// current, so refuse instead.
static bool plate_switch_blocked(Plater *plater) { return plater->is_background_process_slicing(); }

Response Controller::handle_add_plate(const std::string &body)
{
    nlohmann::json in = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    if (in.contains("name") && !in["name"].is_string()) return { 400, {{"error", "bad_param"}, {"param", "name"}} };
    nlohmann::json r = run_on_ui([in]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plate_switch_blocked(plater)) return {{"error", "busy_slicing"}};
        if (!plater->can_add_plate()) return {{"error", "cannot_add_plate"}};
        const int index = plater->add_plate(); // selects the new plate, as the toolbar button does
        if (index < 0) return {{"error", "cannot_add_plate"}};
        if (in.contains("name")) {
            plater->get_partplate_list().get_plate(index)->set_plate_name(in["name"].get<std::string>());
            wxGetApp().obj_list()->reload_all_plates();
        }
        api_notify("Added plate " + std::to_string(index + 1));
        return plate_json(plater, index);
    });
    if (r.contains("error")) return plate_error(r);
    return { 201, r };
}

Response Controller::handle_duplicate_plate(int index)
{
    nlohmann::json r = run_on_ui([index]() -> nlohmann::json {
        Plater        *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        if (index < 0 || index >= plates.get_plate_count()) return {{"error", "unknown_plate"}};
        if (!plater->can_add_plate()) return {{"error", "cannot_add_plate"}};
        const int copy = plater->duplicate_plate(index);
        if (copy < 0) return {{"error", "cannot_add_plate"}};
        api_notify("Duplicated plate " + std::to_string(index + 1));
        return plate_json(plater, copy);
    });
    if (r.contains("error")) return plate_error(r);
    return { 201, r };
}

Response Controller::handle_delete_plate(int index, bool force)
{
    nlohmann::json r = run_on_ui([index, force]() -> nlohmann::json {
        Plater        *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        if (index < 0 || index >= plates.get_plate_count()) return {{"error", "unknown_plate"}};
        if (!plater->can_delete_plate()) return {{"error", "last_plate"}};
        if (plate_switch_blocked(plater)) return {{"error", "busy_slicing"}};
        // PartPlateList::delete_plate moves the plate's instances onto the last plate or off every
        // plate; it never deletes them. That is surprising enough to require an explicit force.
        nlohmann::json on_plate = plate_json(plater, index)["objects"];
        if (!on_plate.empty() && !force) return {{"error", "plate_not_empty"}, {"objects", on_plate}};
        if (plater->delete_plate(index) < 0) return {{"error", "delete_failed"}};
        api_notify("Deleted plate " + std::to_string(index + 1));
        return {{"deleted", index}, {"moved_objects", on_plate}, {"count", plates.get_plate_count()},
                {"current", plates.get_curr_plate_index()}};
    });
    if (r.contains("error")) return plate_error(r);
    return { 200, r };
}

Response Controller::handle_select_plate(int index)
{
    nlohmann::json r = run_on_ui([index]() -> nlohmann::json {
        Plater        *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        if (index < 0 || index >= plates.get_plate_count()) return {{"error", "unknown_plate"}};
        if (index != plates.get_curr_plate_index()) {
            if (plate_switch_blocked(plater)) return {{"error", "busy_slicing"}};
            if (plater->select_plate(index) != 0) return {{"error", "select_failed"}};
        }
        return plate_json(plater, index);
    });
    if (r.contains("error")) return plate_error(r);
    return { 200, r };
}

Response Controller::handle_put_plate(int index, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };

    // Validate the whole body before touching the plate, so a bad field changes nothing.
    nlohmann::json errors = nlohmann::json::object();
    BedType        bed_type  = btDefault;
    PrintSequence  print_seq = PrintSequence::ByDefault;
    std::vector<int> first_layer;
    for (auto it = in.begin(); it != in.end(); ++it) {
        const std::string &k = it.key();
        const auto        &v = it.value();
        if (k == "name") {
            if (!v.is_string()) errors[k] = "expected a string";
        } else if (k == "locked") {
            if (!v.is_boolean()) errors[k] = "expected true or false";
        } else if (k == "bed_type") {
            if (!v.is_string() || (v != "default" && !ConfigOptionEnum<BedType>::from_string(v.get<std::string>(), bed_type)))
                errors[k] = "expected \"default\" or a bed type name";
        } else if (k == "print_sequence") {
            if (!v.is_string() || (v != "default" && !ConfigOptionEnum<PrintSequence>::from_string(v.get<std::string>(), print_seq)))
                errors[k] = "expected \"default\", \"by layer\" or \"by object\"";
        } else if (k == "first_layer_sequence") {
            if (!v.is_array()) { errors[k] = "expected an array of filament numbers (1-based), [] for auto"; continue; }
            for (const auto &n : v) {
                if (!n.is_number_integer() || n.get<int>() < 1) { errors[k] = "filament numbers are 1-based integers"; break; }
                first_layer.push_back(n.get<int>());
            }
        } else if (k == "spiral_mode") {
            if (!v.is_boolean() && v != "default") errors[k] = "expected true, false or \"default\"";
        } else {
            errors[k] = "unknown_key";
        }
    }
    if (!errors.empty()) return { 422, {{"error", "invalid_settings"}, {"errors", errors}} };

    nlohmann::json r = run_on_ui([index, in, bed_type, print_seq, first_layer]() -> nlohmann::json {
        Plater        *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        if (index < 0 || index >= plates.get_plate_count()) return {{"error", "unknown_plate"}};
        PartPlate *plate = plates.get_plate(index);
        const size_t filaments = wxGetApp().preset_bundle->filament_presets.size();
        for (int n : first_layer)
            if (size_t(n) > filaments)
                return {{"error", "invalid_settings"}, {"errors", {{"first_layer_sequence", "the project has " + std::to_string(filaments) + " filament(s)"}}}};

        if (in.contains("name")) {
            plate->set_plate_name(in["name"].get<std::string>());
            wxGetApp().obj_list()->reload_all_plates();
            plater->set_plater_dirty(true); // a rename takes no undo snapshot; flag it for saving
        }
        if (in.contains("locked")) {
            plater->take_snapshot("lock partplate");
            plates.lock_plate(index, in["locked"].get<bool>());
        }
        // The rest mirrors the Plate Settings dialog's confirm handler (Plater::open_platesettings_dialog).
        bool settings = false;
        if (in.contains("bed_type"))       { plate->set_bed_type(bed_type); settings = true; }
        if (in.contains("print_sequence")) { plate->set_print_seq(print_seq); settings = true; }
        if (in.contains("first_layer_sequence")) { plate->set_first_layer_print_sequence(first_layer); settings = true; }
        if (in.contains("spiral_mode")) {
            const auto &s = in["spiral_mode"];
            if (s == "default")
                plate->set_spiral_vase_mode(false, true);
            else if (!s.get<bool>())
                plate->set_spiral_vase_mode(false, false);
            else if (!plate->get_spiral_vase_mode()) {
                // set_spiral_vase_mode(true, false) asks the user whether to apply the vase-mode
                // object settings; the caller asking for spiral mode is that answer.
                plate->config()->set_key_value("spiral_mode", new ConfigOptionBool(true));
                plate->set_vase_mode_related_object_config();
            }
            settings = true;
        }
        if (settings) {
            plater->update_project_dirty_from_presets();
            plater->set_plater_dirty(true);
            plater->config_change_notification(*plate->config(), std::string("print_sequence"));
            plater->update();
            wxGetApp().obj_list()->update_selections();
        }
        api_notify("Updated plate " + std::to_string(index + 1));
        return plate_json(plater, index);
    });
    if (r.contains("error")) return plate_error(r);
    return { 200, r };
}

// GUI thread. Opening or replacing a project with unsaved work would make Plater::close_with_confirm
// and the preset check show modal dialogs no API client can answer. Refuse instead, or - when the
// caller asked to discard - drop the changes first so both prompts have nothing to ask about.
static nlohmann::json dirty_gate(Plater *plater, bool discard)
{
    const bool model_dirty   = !plater->up_to_date(false, false) || plater->is_project_dirty();
    const bool presets_dirty = wxGetApp().has_current_preset_changes();
    if (!model_dirty && !presets_dirty) return nullptr;
    if (!discard)
        return {{"error", "project_dirty"}, {"model_dirty", model_dirty}, {"presets_dirty", presets_dirty},
                {"detail", "save the project first, or pass \"discard\": true"}};
    if (model_dirty) {
        plater->up_to_date(true, false);
        plater->up_to_date(true, true);
    }
    if (presets_dirty) {
        // As GUI_App::check_and_keep_current_preset_changes does when the user picks "Discard".
        const PrinterTechnology tech = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
        for (Tab *tab : wxGetApp().tabs_list)
            if (tab->supports_printer_technology(tech) && tab->current_preset_is_dirty())
                tab->get_presets()->discard_current_changes();
        wxGetApp().load_current_presets(false);
    }
    return nullptr;
}

static bool ends_with_3mf(const std::string &path)
{
    return path.size() >= 4 && boost::iequals(path.substr(path.size() - 4), ".3mf");
}

Response Controller::handle_project_save(const std::string &body)
{
    nlohmann::json in = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    if (in.contains("path") && !in["path"].is_string()) return { 400, {{"error", "bad_param"}, {"param", "path"}} };
    const std::string requested = in.value("path", std::string());

    nlohmann::json r = run_on_ui([requested]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        std::string path = requested.empty() ? into_u8(plater->get_project_filename(".3mf")) : requested;
        if (path.empty()) return {{"error", "no_project_path"}, {"detail", "the project was never saved; pass a path"}};
        const boost::filesystem::path fs_path = into_path(from_u8(path));
        if (!ends_with_3mf(path) || !boost::filesystem::is_directory(fs_path.parent_path()))
            return {{"error", "bad_path"}, {"detail", "path must end in .3mf and its folder must exist"}};

        // Plater::save_project without its dialogs: it asks for a file name, and shows a modal
        // box when the write fails.
        SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
        if (wxGetApp().app_config->get_bool("export_sources_full_pathnames"))
            strategy = strategy | SaveStrategy::FullPathSources;
        // The exporter pumps the event queue; park other API tasks until it is done, as the
        // auto-backup does, so none of them mutates the model mid-write.
        set_backup_in_progress(true);
        int written = -1;
        try {
            written = plater->export_3mf(fs_path, strategy);
        } catch (...) {
            set_backup_in_progress(false);
            throw;
        }
        set_backup_in_progress(false);
        if (written < 0) return {{"error", "save_failed"}, {"path", path}};

        Slic3r::remove_backup(plater->model(), false);
        plater->set_project_filename(from_u8(path));
        plater->up_to_date(true, false);
        plater->up_to_date(true, true);
        wxGetApp().update_saved_preset_from_current_preset();
        plater->reset_project_dirty_after_save();
        plater->update_title_dirty_status();
        api_notify("Saved project " + fs_path.filename().string());
        return {{"saved", path}};
    }, 120);
    if (r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        return { e == "no_project_path" ? 409 : e == "bad_path" ? 422 : 500, r };
    }
    wxGetApp().remote_api_server().broadcast({{"event", "project.saved"}, {"project", r["saved"]}});
    return { 200, r };
}

Response Controller::handle_project_open(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    if (!in.contains("path") || !in["path"].is_string()) return { 400, {{"error", "missing_path"}} };
    const std::string path    = in["path"].get<std::string>();
    const bool        discard = in.value("discard", false);
    if (!ends_with_3mf(path)) return { 422, {{"error", "unsupported_format"}, {"detail", "only .3mf projects"}} };

    nlohmann::json r = run_on_ui([path, discard]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (!boost::filesystem::exists(into_path(from_u8(path)))) return {{"error", "not_found"}, {"path", path}};
        if (plater->is_background_process_slicing()) return {{"error", "busy_slicing"}};
        if (nlohmann::json e = dirty_gate(plater, discard); !e.is_null()) return e;
        // "<loadall>" skips the drop dialog that asks whether to open the project or only import
        // its geometry. Loading also warns, in a modal box, about custom G-code in the project's
        // presets; the caller has chosen the file, so silence that for this one load.
        AppConfig  *cfg       = wxGetApp().app_config;
        const auto  prev_warn = cfg->get("no_warn_when_modified_gcodes");
        cfg->set("no_warn_when_modified_gcodes", "true");
        try {
            plater->load_project(from_u8(path), "<loadall>");
        } catch (...) {
            cfg->set("no_warn_when_modified_gcodes", prev_warn);
            throw;
        }
        cfg->set("no_warn_when_modified_gcodes", prev_warn);
        return {{"project", into_u8(plater->get_project_filename(".3mf"))},
                {"plate_count", plater->get_partplate_list().get_plate_count()},
                {"objects", plater->model().objects.size()}};
    }, 120);
    if (r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        return { e == "not_found" ? 404 : 409, r };
    }
    return { 200, r }; // project.opened is broadcast by the load itself
}

Response Controller::handle_project_new(const std::string &body)
{
    nlohmann::json in = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    const bool discard = in.value("discard", false);
    nlohmann::json r = run_on_ui([discard]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) return {{"error", "busy_slicing"}};
        if (nlohmann::json e = dirty_gate(plater, discard); !e.is_null()) return e;
        plater->new_project(/*skip_confirm=*/true);
        return {{"created", true}, {"plate_count", plater->get_partplate_list().get_plate_count()}};
    }, 60);
    if (r.contains("error")) return { 409, r };
    wxGetApp().remote_api_server().broadcast({{"event", "project.new"}});
    return { 200, r };
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
        if (is("POST", "/api/v1/slice"))        return handle_slice(t);
        if (is("GET",  "/api/v1/slice/status")) return handle_slice_status();
        if (is("POST", "/api/v1/slice/cancel")) return handle_slice_cancel();
        if (is("POST", "/api/v1/model")) {
            try {
                return handle_load_model(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("PUT", "/api/v1/preset")) {
            try {
                return handle_select_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/preset/save")) {
            try {
                return handle_save_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/preset/config")) {
            try {
                return handle_get_preset_config(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("DELETE", "/api/v1/preset")) {
            try {
                return handle_delete_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("GET",  "/api/v1/gcode"))        return handle_get_gcode(t);
        if (is("GET",  "/api/v1/objects"))      return handle_get_objects();
        if (is("GET",  "/api/v1/plate/render")) return handle_plate_render(t);
        if (is("GET",  "/api/v1/presets"))      return handle_get_presets();
        if (is("POST", "/api/v1/arrange"))      return handle_arrange(t);
        if (is("POST", "/api/v1/orient"))       return handle_orient(t);
        if (is("GET",  "/api/v1/jobs/status"))  return handle_jobs_status();
        if (is("POST", "/api/v1/project/save") || is("POST", "/api/v1/project/open") || is("POST", "/api/v1/project/new")) {
            try {
                if (is("POST", "/api/v1/project/save")) return handle_project_save(req.body);
                if (is("POST", "/api/v1/project/open")) return handle_project_open(req.body);
                return handle_project_new(req.body);
            } catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("GET",  "/api/v1/plates"))       return handle_get_plates();
        if (is("POST", "/api/v1/plates")) {
            try { return handle_add_plate(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        {
            // Plate sub-routes: /api/v1/plates/<index>[/<action>], index 0-based as in PartPlateList.
            static const std::string pfx = "/api/v1/plates/";
            std::string path = t.substr(0, t.find('?'));
            if (path.size() > pfx.size() && path.compare(0, pfx.size(), pfx) == 0) {
                std::string rest   = path.substr(pfx.size());
                size_t      slash  = rest.find('/');
                std::string idx_s  = (slash == std::string::npos) ? rest : rest.substr(0, slash);
                std::string action = (slash == std::string::npos) ? std::string() : rest.substr(slash + 1);
                int index = -1;
                try {
                    size_t used = 0;
                    index = std::stoi(idx_s, &used);
                    if (used != idx_s.size()) throw std::invalid_argument(idx_s);
                } catch (...) { return { 400, {{"error", "bad_plate_index"}} }; }
                if (req.method == "GET" && action.empty())
                    return handle_get_plate(index);
                if (req.method == "PUT" && action.empty()) {
                    try { return handle_put_plate(index, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "DELETE" && action.empty())
                    return handle_delete_plate(index, query_param(t, "force") == "true");
                if (req.method == "POST" && action == "select")
                    return handle_select_plate(index);
                if (req.method == "POST" && action == "duplicate")
                    return handle_duplicate_plate(index);
                return { 404, {{"error", "not_found"}} };
            }
        }
        {
            // M4b object sub-routes: /api/v1/objects/<id> and /api/v1/objects/<id>/<action>
            static const std::string pfx = "/api/v1/objects/";
            std::string path = t.substr(0, t.find('?'));
            if (path.size() > pfx.size() && path.compare(0, pfx.size(), pfx) == 0) {
                std::string rest = path.substr(pfx.size());
                size_t slash = rest.find('/');
                std::string id_str = (slash == std::string::npos) ? rest : rest.substr(0, slash);
                std::string action = (slash == std::string::npos) ? std::string() : rest.substr(slash + 1);
                uint64_t oid = 0;
                try { oid = std::stoull(id_str); }
                catch (...) { return { 400, {{"error", "bad_object_id"}} }; }
                if (req.method == "DELETE" && action.empty())
                    return handle_delete_object(oid);
                if (req.method == "POST" && action == "transform") {
                    try { return handle_transform_object(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "POST" && action == "duplicate")
                    return handle_duplicate_object(oid);
                if (req.method == "PUT" && action == "config") {
                    try { return handle_put_object_config(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "PUT" && action == "layer_height") {
                    try { return handle_put_layer_height(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "PUT" && action == "height_range") {
                    try { return handle_put_height_range(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                return { 404, {{"error", "not_found"}} };
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
