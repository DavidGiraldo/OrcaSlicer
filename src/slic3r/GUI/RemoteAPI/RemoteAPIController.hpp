// src/slic3r/GUI/RemoteAPI/RemoteAPIController.hpp
#ifndef slic3r_RemoteAPIController_hpp_
#define slic3r_RemoteAPIController_hpp_

#include "RemoteAPIServer.hpp"

#include <functional>
#include <mutex>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// Snapshot of the last/current slice, maintained on the GUI thread by wx event
// subscriptions (Task 10), read by API threads under the mutex.
struct SliceState
{
    std::string    state { "idle" };  // idle | slicing | done | error
    int            percent { -1 };
    std::string    message;           // progress text or error message
    nlohmann::json stats;             // filled on success (Task 10)
    nlohmann::json warnings = nlohmann::json::array();
};

class Controller
{
public:
    // Must be constructed on the GUI thread (binds wx events in Task 10).
    Controller();

    Response dispatch(const Request &req);

    SliceState slice_state() const { std::lock_guard<std::mutex> lk(m_mutex); return m_slice; }

    // Call once on the GUI thread after the plater exists (from start_remote_api()).
    // Idempotent: guarded by m_events_bound.
    void bind_plater_events();

private:
    Response handle_status();
    Response handle_get_config(const std::string &target);
    Response handle_put_config(const std::string &body);
    Response handle_slice();
    Response handle_slice_status();

    // Mutate m_slice under the lock and broadcast a snapshot (event_name) to WS clients.
    void set_slice_state(const std::function<void(SliceState&)> &mut, const char *event_name);

    // Runs fn on the GUI thread, blocks the calling (io) thread up to 10 s.
    // Throws std::runtime_error("ui_timeout") on expiry.
    nlohmann::json run_on_ui(std::function<nlohmann::json()> fn);

    mutable std::mutex m_mutex;
    SliceState         m_slice;
    bool               m_events_bound { false };
};

}}} // namespace

#endif
