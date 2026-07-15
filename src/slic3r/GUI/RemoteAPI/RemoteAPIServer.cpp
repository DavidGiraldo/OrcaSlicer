// src/slic3r/GUI/RemoteAPI/RemoteAPIServer.cpp
#include "RemoteAPIServer.hpp"

#include "libslic3r/Thread.hpp"
#include <boost/log/trivial.hpp>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// One HTTP connection. Reads a request, answers it, closes (Connection: close).
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket socket, Server &server)
        : m_stream(std::move(socket)), m_server(server)
    {}

    void run() { do_read(); }

private:
    void do_read()
    {
        m_req = {};
        m_stream.expires_after(std::chrono::seconds(15));
        http::async_read(m_stream, m_buffer, m_req,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return; // closed / timeout / parse error
                self->handle();
            });
    }

    void handle()
    {
        // Auth first, everything else second.
        auto        tok_sv = m_req["X-Api-Token"];
        std::string token(tok_sv.data(), tok_sv.size());
        Response    r;
        if (!m_server.check_token(token)) {
            r = { 401, {{"error", "unauthorized"}} };
        } else if (!m_server.handler()) {
            r = { 503, {{"error", "no_handler"}} };
        } else {
            Request rq { std::string(m_req.method_string()),
                         std::string(m_req.target()),
                         m_req.body() };
            try {
                r = m_server.handler()(rq);
            } catch (const std::exception &e) {
                // The API must never take the slicer down.
                BOOST_LOG_TRIVIAL(error) << "remote-api handler exception: " << e.what();
                r = { 500, {{"error", "internal"}, {"detail", e.what()}} };
            }
        }
        auto res = std::make_shared<http::response<http::string_body>>(
            static_cast<http::status>(r.status), m_req.version());
        res->set(http::field::content_type, "application/json");
        res->set(http::field::server, "orca-remote-api");
        res->keep_alive(false);
        res->body() = r.body.dump();
        res->prepare_payload();
        http::async_write(m_stream, *res,
            [self = shared_from_this(), res](beast::error_code, std::size_t) {
                beast::error_code ec2;
                self->m_stream.socket().shutdown(tcp::socket::shutdown_send, ec2);
            });
    }

    beast::tcp_stream                  m_stream;
    beast::flat_buffer                 m_buffer;
    http::request<http::string_body>   m_req;
    Server                            &m_server;
};

void Server::start(const Config &cfg)
{
    if (m_running) stop();
    m_cfg = cfg;
    try {
        m_ioc = std::make_unique<net::io_context>(1);
        auto address = m_cfg.bind_lan ? net::ip::address_v4::any()
                                      : net::ip::make_address_v4("127.0.0.1");
        m_acceptor = std::make_unique<tcp::acceptor>(
            *m_ioc, tcp::endpoint(address, static_cast<unsigned short>(m_cfg.port)));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "remote-api: failed to bind port " << m_cfg.port
                                 << ": " << e.what();
        m_ioc.reset();
        m_acceptor.reset();
        return; // disabled for this run; the Preferences page shows "failed" via running()
    }
    m_running = true;
    do_accept();
    m_thread = create_thread([this] {
        set_current_thread_name("remote_api");
        this->m_ioc->run();
    });
    BOOST_LOG_TRIVIAL(info) << "remote-api: listening on "
                            << (m_cfg.bind_lan ? "0.0.0.0" : "127.0.0.1")
                            << ":" << m_cfg.port;
}

void Server::do_accept()
{
    m_acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!m_running) return;
        if (!ec)
            std::make_shared<HttpSession>(std::move(socket), *this)->run();
        do_accept();
    });
}

void Server::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_ioc) m_ioc->stop();
    if (m_thread.joinable()) m_thread.join();
    m_acceptor.reset();
    m_ioc.reset();
    {
        std::lock_guard<std::mutex> lk(m_ws_mutex);
        m_ws_sessions.clear();
    }
    BOOST_LOG_TRIVIAL(info) << "remote-api: stopped";
}

bool Server::check_token(const std::string &presented) const
{
    return !m_cfg.token.empty() && presented == m_cfg.token;
}

// WS plumbing — populated in Task 11; broadcast is already safe to call.
void Server::ws_join(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.insert(s);
}
void Server::ws_leave(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.erase(s);
}
void Server::broadcast(const nlohmann::json &event)
{
    (void) event; // Task 11 fills this in once WsSession exists.
}

}}} // namespace
