#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/log.hpp>
#include <asterion/kernel/process/owner.hpp>
#include "ipc/local_security.hpp"
#include "ipc/tls_security.hpp"
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>

namespace asterion::service {
namespace {

// Repeated failures are logged at counts 1, 2, 4, 8, ... only.
bool every_power_of_two(std::uint64_t count)
{
    return count && (count & (count - 1)) == 0;
}

using Clock = std::chrono::steady_clock;
std::atomic<bool> stop_flag{false};
static_assert(std::atomic<bool>::is_always_lock_free, "stop flag is set from a signal handler");

extern "C" void signal_stop(int)
{
    stop_flag.store(true);
}

using Tcp = asio::ip::tcp;
using Local = asio::local::stream_protocol;
using Tls = asio::ssl::stream<Tcp::socket>;

struct LocalListener
{
    Local::acceptor acceptor;
    std::string endpoint;

    LocalListener(asio::io_context& io, std::string path)
        : acceptor(io),
          endpoint(std::move(path))
    {
        if (endpoint.empty() || endpoint.front() != '/' || endpoint.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid local socket path");
        // Login services must recreate their transient socket directory after reboot,
        // including launchd starts that happen before a Terminal window opens.
        const auto parent = std::filesystem::path(endpoint).parent_path();
        if (::mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST)
            throw Error(ErrorCode::unavailable, "cannot create IPC socket directory");
        acceptor.open();
        acceptor.bind(Local::endpoint(endpoint));
        try {
            if (::chmod(endpoint.c_str(), 0600) != 0)
                throw Error(ErrorCode::unavailable, "cannot secure IPC endpoint");
            acceptor.listen();
        } catch (...) {
            ::unlink(endpoint.c_str());
            throw;
        }
    }

    ~LocalListener()
    {
        release();
    }

    // Stop accepting and remove the socket file. Also used when the host ends
    // without destroying its owners, so a later start can bind the path again.
    void release()
    {
        asio::error_code ignored;
        acceptor.close(ignored);
        if (!endpoint.empty())
            ::unlink(endpoint.c_str());
        endpoint.clear();
    }
};

// How any thread prompts a running host: one advance on its loop, however
// many requests arrive before it runs.
struct Waker
{
    asio::io_context& io;
    const std::function<void()> advance;
    std::atomic<bool> requested{false};

    void wake()
    {
        if (!requested.exchange(true))
            asio::post(io, [this] {
                requested = false;
                advance();
            });
    }
};

std::mutex running_mutex;
std::vector<Waker*> running;

} // namespace

void Transport::validate() const
{
    if (remote() ? (!endpoint.empty() || !port) : (endpoint.empty() || port || !tls.empty()))
        throw std::invalid_argument("choose --endpoint OR --bind/--port");
    if (!tls.empty())
        ipc::detail::validate_identity(tls);
}

void install_stop_signals()
{
    struct sigaction action{};
    action.sa_handler = signal_stop;
    sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0)
        throw std::runtime_error("cannot install stop signal handlers");
    // Vendor libraries own sockets that our IPC wrappers cannot configure.
    // A disconnected peer must produce EPIPE, not terminate the whole service.
    action.sa_handler = SIG_IGN;
    if (::sigaction(SIGPIPE, &action, nullptr) != 0)
        throw std::runtime_error("cannot install stop signal handlers");
}

void request_stop() noexcept
{
    stop_flag.store(true);
}

bool stop_requested() noexcept
{
    return stop_flag.load();
}

void wake_io_owner()
{
    std::lock_guard lock(running_mutex);
    for (auto* host : running)
        host->wake();
}

void reset_stop_request() noexcept
{
    stop_flag.store(false);
}

struct RpcHost::Impl
{
    enum class Phase
    {
        handshake,
        reading,
        pending,
        writing,
        closed
    };

    struct PayloadBudget
    {
        std::size_t limit, used = 0;
    };

    struct Session
    {
        Impl& host;
        Peer peer;
        const LocalEndpoint* lane;

        bool control() const
        {
            return lane && lane->during_drain;
        }

        Phase phase;
        Clock::time_point deadline;
        Reply reply;
        const std::shared_ptr<PayloadBudget> budget;
        std::size_t retained_bytes = 0, input_bytes = 0;

        Session(Impl& host, Peer peer, Phase phase, const LocalEndpoint* lane)
            : host(host),
              peer(std::move(peer)),
              lane(lane),
              phase(phase),
              deadline(Clock::now() + host.options.handshake),
              budget(host.payload_budgets.at(lane))
        {}

        virtual ~Session()
        {
            budget->used -= retained_bytes;
        }

        bool retain(std::size_t bytes)
        {
            if (bytes > budget->limit - budget->used) {
                host.failed(Error(ErrorCode::resource_exhausted, "RPC payload capacity is full"));
                return false;
            }
            budget->used += bytes;
            retained_bytes += bytes;
            return true;
        }

        void release(std::size_t bytes)
        {
            budget->used -= bytes;
            retained_bytes -= bytes;
        }

        virtual void send(std::string response) = 0;
        virtual void close_transport() = 0;

        void close()
        {
            if (phase == Phase::closed)
                return;
            if (phase == Phase::handshake)
                --host.handshakes;
            phase = Phase::closed;
            reply = {};
            close_transport();
        }

        void poll(Clock::time_point now)
        {
            if (phase == Phase::closed)
                return;
            if (phase != Phase::pending) {
                if (now >= deadline) {
                    if (phase == Phase::writing && every_power_of_two(++host.failures))
                        log::warn("rpc send timed out ({} transport failures)", host.failures);
                    close();
                } else if (host.stopping && !control() && phase != Phase::writing)
                    close();
                return;
            }
            try {
                if (auto response = reply()) {
                    const bool finished = !response->more;
                    if (finished) {
                        reply = {};
                        release(input_bytes);
                        input_bytes = 0;
                    }
                    send(std::move(response->payload));
                    // What a finished request released may be what another one waits
                    // for. A stream's next frame stays on the periodic check: frames
                    // must not set each other off.
                    if (finished)
                        host.waker.wake();
                }
            } catch (const std::exception& error) {
                host.failed(error);
                close();
            }
        }
    };

    template <class Stream>
    struct StreamSession final : Session, std::enable_shared_from_this<StreamSession<Stream>>
    {
        Stream stream;
        std::array<unsigned char, 4> header{};
        std::string payload;

        StreamSession(
            Impl& host,
            Peer peer,
            Stream stream,
            Phase phase,
            const LocalEndpoint* lane = nullptr)
            : Session(host, std::move(peer), phase, lane),
              stream(std::move(stream))
        {}

        void close_transport() override
        {
            asio::error_code ignored;
            stream.lowest_layer().close(ignored);
        }

        void start()
        {
            if constexpr (std::is_same_v<Stream, Tls>) {
                stream.async_handshake(
                    asio::ssl::stream_base::server,
                    [self = this->shared_from_this()](asio::error_code error) {
                        if (self->phase == Phase::closed)
                            return;
                        if (error) {
                            self->close();
                            return;
                        }
                        self->admitted();
                    });
            } else if constexpr (std::is_same_v<Stream, Tcp::socket>) {
                asio::async_read(
                    stream,
                    asio::buffer(header),
                    [self = this->shared_from_this()](asio::error_code error, std::size_t) {
                        if (self->phase == Phase::closed)
                            return;
                        if (error || self->header != ipc::connection_magic) {
                            self->close();
                            return;
                        }
                        self->admitted();
                    });
            } else {
                read();
            }
        }

        // The connection proved what it is (TLS peer or connection magic).
        void admitted()
        {
            if (this->host.connections(nullptr) - this->host.handshakes
                >= this->host.options.connections) {
                this->close();
                return;
            }
            --this->host.handshakes;
            read();
        }

        void read()
        {
            this->phase = Phase::reading;
            if (this->host.stopping && !this->control()) {
                this->close();
                return;
            }
            this->deadline = Clock::now() + this->host.options.receive;
            asio::async_read(
                stream,
                asio::buffer(header),
                [self = this->shared_from_this()](asio::error_code error, std::size_t) {
                    if (error || (self->host.stopping && !self->control())
                        || self->phase == Phase::closed) {
                        self->close();
                        return;
                    }
                    const auto& h = self->header;
                    const auto size = (std::uint32_t{h[0]} << 24) | (std::uint32_t{h[1]} << 16)
                        | (std::uint32_t{h[2]} << 8) | h[3];
                    const auto limit =
                        self->lane ? self->lane->request_bytes : self->host.options.request_bytes;
                    if (!size || size > limit) {
                        self->close();
                        return;
                    }
                    if (!self->retain(size)) {
                        self->close();
                        return;
                    }
                    self->input_bytes = size;
                    self->payload.resize(size);
                    asio::async_read(
                        self->stream,
                        asio::buffer(self->payload),
                        [self](asio::error_code error, std::size_t) {
                            if (error || (self->host.stopping && !self->control())
                                || self->phase == Phase::closed) {
                                self->close();
                                return;
                            }
                            try {
                                self->phase = Phase::pending;
                                const auto& handler =
                                    self->lane ? self->lane->handler : self->host.handler;
                                self->reply = handler(self->peer, std::move(self->payload));
                                self->poll(Clock::now());
                                // The service starts on an accepted request now.
                                self->host.waker.wake();
                            } catch (const std::exception& failure) {
                                self->host.failed(failure);
                                self->close();
                            }
                        });
                });
        }

        void send(std::string response) override
        {
            if (response.empty() || response.size() > ipc::Channel::max_frame) {
                this->close();
                return;
            }
            if (!this->retain(response.size())) {
                this->close();
                return;
            }
            this->phase = Phase::writing;
            this->deadline = Clock::now() + this->host.options.send;
            payload = std::move(response);
            const auto size = static_cast<std::uint32_t>(payload.size());
            header = {
                static_cast<unsigned char>(size >> 24),
                static_cast<unsigned char>(size >> 16),
                static_cast<unsigned char>(size >> 8),
                static_cast<unsigned char>(size)
            };
            const std::array<asio::const_buffer, 2> buffers{
                asio::buffer(header),
                asio::buffer(payload)
            };
            asio::async_write(
                stream,
                buffers,
                [self = this->shared_from_this()](asio::error_code error, std::size_t) {
                    const auto size = self->payload.size();
                    std::string{}.swap(self->payload);
                    self->release(size);
                    if (self->phase == Phase::closed)
                        return;
                    if (error)
                        self->close();
                    else if (self->reply)
                        self->phase = Phase::pending;
                    else
                        self->read();
                });
        }
    };

    Progress& progress;
    Transport transport;
    Handler handler;
    Options options;
    // Sessions can outlive the session list while cancelled Asio callbacks drain.
    // Keep their reservations alive until their actual buffers are destroyed.
    std::map<const LocalEndpoint*, std::shared_ptr<PayloadBudget>> payload_budgets;
    asio::io_context io;
    asio::steady_timer tick{io};
    Waker waker{io, [this] {
                    advance();
                }};
    std::unique_ptr<LocalListener> local;
    std::vector<std::unique_ptr<LocalListener>> auxiliary;
    std::unique_ptr<ProcessOwner> owner;
    std::unique_ptr<Tcp::acceptor> tcp;
    std::shared_ptr<asio::ssl::context> tls;
    std::vector<std::shared_ptr<Session>> sessions;
    std::size_t handshakes = 0;
    std::uint64_t failures = 0;
    bool stopping = false, drained = true, finished = false;
    Clock::time_point drain_deadline;

    Impl(Transport transport, Handler handler, Options options, Progress& progress)
        : progress(progress),
          transport(std::move(transport)),
          handler(std::move(handler)),
          options(std::move(options))
    {
        this->transport.validate();
        if (this->options.owner_pid)
            owner = std::make_unique<ProcessOwner>(this->options.owner_pid);
        for (const auto& endpoint : this->options.local_endpoints)
            if (endpoint.path.empty() || !endpoint.handler || !endpoint.connections
                || !endpoint.request_bytes || endpoint.request_bytes > ipc::Channel::max_frame
                || endpoint.payload_bytes < endpoint.request_bytes)
                throw std::logic_error("invalid RPC host options");
        if (!this->handler || !this->options.connections || !this->options.handshakes
            || !this->options.request_bytes || this->options.request_bytes > ipc::Channel::max_frame
            || this->options.payload_bytes < this->options.request_bytes)
            throw std::logic_error("invalid RPC host options");
        payload_budgets.emplace(
            nullptr,
            std::make_shared<PayloadBudget>(PayloadBudget{this->options.payload_bytes}));
        for (const auto& endpoint : this->options.local_endpoints)
            payload_budgets.emplace(
                &endpoint,
                std::make_shared<PayloadBudget>(PayloadBudget{endpoint.payload_bytes}));
        if (this->transport.remote()) {
            if (!this->transport.tls.empty())
                tls = ipc::detail::tls_context(this->transport.tls);
            const Tcp::endpoint endpoint(
                asio::ip::make_address(this->transport.bind),
                this->transport.port);
            tcp = std::make_unique<Tcp::acceptor>(io);
            tcp->open(endpoint.protocol());
            tcp->set_option(asio::socket_base::reuse_address(true));
            tcp->bind(endpoint);
            tcp->listen();
            accept_tcp();
        } else {
            local = std::make_unique<LocalListener>(io, this->transport.endpoint);
            accept_local(*local, nullptr);
        }
        for (const auto& endpoint : this->options.local_endpoints) {
            auxiliary.push_back(std::make_unique<LocalListener>(io, endpoint.path));
            accept_local(*auxiliary.back(), &endpoint);
        }
    }

    std::size_t connections(const LocalEndpoint* lane) const
    {
        return std::ranges::count_if(sessions, [lane](const auto& session) {
            return session->lane == lane && session->phase != Phase::closed;
        });
    }

    bool business_pending() const
    {
        return std::ranges::any_of(sessions, [](const auto& session) {
            return !session->control();
        });
    }

    void failed(const std::exception& error)
    {
        if (every_power_of_two(++failures))
            log::warn(
                "rpc connection failed: {} ({} transport failures)",
                error_name(classify(error)),
                failures);
    }

    void accept_local(LocalListener& listener, const LocalEndpoint* lane)
    {
        listener.acceptor.async_accept(
            [this, &listener, lane](asio::error_code error, Local::socket socket) {
                if (!listener.acceptor.is_open())
                    return;
                const auto capacity = lane ? lane->connections : options.connections;
                if (!error && connections(lane) < capacity) {
                    try {
                        ipc::detail::verify_local_peer(socket.native_handle());
                        auto session = std::make_shared<StreamSession<Local::socket>>(
                            *this,
                            Peer{},
                            std::move(socket),
                            Phase::reading,
                            lane);
                        sessions.push_back(session);
                        session->start();
                    } catch (const std::exception& failure) {
                        failed(failure);
                    }
                }
                accept_local(listener, lane);
            });
    }

    void accept_tcp()
    {
        tcp->async_accept([this](asio::error_code error, Tcp::socket socket) {
            if (stopping)
                return;
            if (!error && handshakes < options.handshakes
                && connections(nullptr) < options.connections + options.handshakes) {
                asio::error_code ignored;
                Peer peer{socket.remote_endpoint(ignored).address().to_string()};
                // Until the TLS handshake or the connection magic completes.
                const auto begin = [this](auto session) {
                    ++handshakes;
                    sessions.push_back(session);
                    session->start();
                };
                if (tls)
                    begin(
                        std::make_shared<StreamSession<Tls>>(
                            *this,
                            std::move(peer),
                            Tls(std::move(socket), *tls),
                            Phase::handshake));
                else
                    begin(
                        std::make_shared<StreamSession<Tcp::socket>>(
                            *this,
                            std::move(peer),
                            std::move(socket),
                            Phase::handshake));
            }
            accept_tcp();
        });
    }

    void advance()
    {
        // A wake-up may still be queued when the host has ended.
        if (finished)
            return;
        progress.finish();
        const auto now = Clock::now();
        if (owner && !owner->alive())
            request_stop();
        if (!stopping && stop_requested()) {
            stopping = true;
            drain_deadline = now + options.drain;
            asio::error_code ignored;
            if (local)
                local->acceptor.close(ignored);
            if (tcp)
                tcp->close(ignored);
            for (std::size_t i = 0; i < auxiliary.size(); ++i)
                if (!options.local_endpoints[i].during_drain)
                    auxiliary[i]->acceptor.close(ignored);
        }
        const auto stage = !stopping ? Stage::running
            : business_pending()     ? Stage::draining_replies
                                     : Stage::stopping_resources;
        const bool resources_stopped = !options.advance || options.advance(stage);
        for (auto& session : sessions)
            session->poll(now);
        std::erase_if(sessions, [](const auto& session) {
            return session->phase == Phase::closed;
        });
        const bool complete = stage == Stage::stopping_resources && resources_stopped;
        if (stopping && (complete || now >= drain_deadline)) {
            finished = true;
            drained = complete;
            asio::error_code ignored;
            for (auto& listener : auxiliary)
                listener->acceptor.close(ignored);
            if (!complete) {
                // The caller must exit without destroying owners. Pending replies can
                // own suspended locals still referenced by accepted worker operations.
                // Keep them alive until that exit, not merely until run() returns false.
                // Socket files are not owners: remove them now.
                if (local)
                    local->release();
                for (auto& listener : auxiliary)
                    listener->release();
                io.stop();
                return;
            }
            for (auto& session : sessions)
                session->close();
            sessions.clear();
            return;
        }
        // Socket reads, writes and TLS handshakes share the nonblocking event loop,
        // and finished work announces itself through the waker. This check is what
        // observes deadlines and stop requests, and work that announces nothing.
        tick.expires_after(Milliseconds{10});
        tick.async_wait([this](asio::error_code error) {
            if (!error)
                advance();
        });
    }
};

RpcHost::RpcHost(Transport transport, Handler handler, Options options, Progress& progress)
    : impl_(
          std::make_unique<Impl>(
              std::move(transport),
              std::move(handler),
              std::move(options),
              progress))
{}

RpcHost::~RpcHost() = default;

bool RpcHost::run()
{
    struct Running
    {
        Waker& host;

        explicit Running(Waker& host)
            : host(host)
        {
            std::lock_guard lock(running_mutex);
            running.push_back(&host);
        }

        ~Running()
        {
            std::lock_guard lock(running_mutex);
            std::erase(running, &host);
        }
    } registered{impl_->waker};

    impl_->advance();
    impl_->io.run();
    return impl_->drained;
}

} // namespace asterion::service
