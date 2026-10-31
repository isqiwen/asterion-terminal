#include <algorithm>
#include <cctype>
#include <vector>
#include "timed_operation.hpp"
#include "tls_security.hpp"
#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/ipc/tcp_channel.hpp>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdexcept>

namespace asterion::ipc {
namespace {

using Tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

std::string pem(const std::string& filename)
{
    const std::filesystem::path path(std::u8string(filename.begin(), filename.end()));
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path)
        || std::filesystem::file_size(path) > 1024 * 1024)
        throw std::invalid_argument("TLS requires absolute paths to PEM files (maximum 1 MiB)");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::invalid_argument("cannot read TLS identity file");
    return {std::istreambuf_iterator<char>(file), {}};
}

std::string fingerprint(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &size) != 1)
        throw std::runtime_error("cannot compute certificate fingerprint");
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (unsigned int i = 0; i < size; ++i) {
        text += hex[digest[i] >> 4];
        text += hex[digest[i] & 15];
    }
    return text;
}

using detail::run;

Ms remaining(Clock::time_point end)
{
    return std::max(Ms{0}, std::chrono::duration_cast<Ms>(end - Clock::now()));
}

} // namespace

std::string normalize_fingerprint(const std::string& text)
{
    std::string digits;
    for (const char c : text) {
        if (c == ':')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            throw std::invalid_argument("a certificate fingerprint is 64 hex digits");
        digits += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (digits.size() != 64)
        throw std::invalid_argument("a certificate fingerprint is 64 hex digits");
    return digits;
}

std::string certificate_fingerprint(const std::string& certificate_file)
{
    const auto text = pem(certificate_file);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(text.data(), static_cast<int>(text.size())),
        BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(
        bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr,
        X509_free);
    if (!certificate)
        throw std::invalid_argument("not a PEM certificate: " + certificate_file);
    return fingerprint(certificate.get());
}

std::string create_identity(
    const std::filesystem::path& certificate_file,
    const std::filesystem::path& private_key_file,
    const std::string& name)
{
    const auto check = [](bool ok) {
        if (!ok)
            throw std::runtime_error("cannot create a TLS identity");
    };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"),
        EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    check(key && certificate);
    auto* serial = X509_get_serialNumber(certificate.get());
    check(ASN1_INTEGER_set(serial, 1) == 1 && X509_set_version(certificate.get(), 2) == 1);
    check(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600) != nullptr);
    check(
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 100L * 365 * 24 * 3600) != nullptr);
    auto* subject = X509_get_subject_name(certificate.get());
    check(
        X509_NAME_add_entry_by_txt(
            subject,
            "CN",
            MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(name.c_str()),
            -1,
            -1,
            0)
        == 1);
    check(X509_set_issuer_name(certificate.get(), subject) == 1);
    check(X509_set_pubkey(certificate.get(), key.get()) == 1);
    check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    const auto text = [&](auto write) {
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        check(bio && write(bio.get()) == 1);
        char* data = nullptr;
        const auto size = BIO_get_mem_data(bio.get(), &data);
        return std::string(data, static_cast<std::size_t>(size));
    };
    write_file_durably(private_key_file, text([&](BIO* bio) {
        return PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    }));
    write_file_durably(
        certificate_file,
        text([&](BIO* bio) {
            return PEM_write_bio_X509(bio, certificate.get());
        }),
        false);
    return fingerprint(certificate.get());
}

namespace detail {

void validate_identity(const TlsIdentity& identity)
{
    if (identity.certificate_file.empty() || identity.private_key_file.empty()
        || identity.peers.empty())
        throw std::invalid_argument("TLS needs a certificate, its key and the peer fingerprints");
    for (const auto& peer : identity.peers)
        if (normalize_fingerprint(peer) != peer)
            throw std::invalid_argument("peer fingerprints must be 64 lowercase hex digits");
}

std::shared_ptr<asio::ssl::context> tls_context(const TlsIdentity& identity)
{
    validate_identity(identity);
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls);
    if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_3_VERSION) != 1)
        throw std::runtime_error("TLS 1.3 unavailable");
    ctx->set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
    // Self-signed certificates fail chain verification by design; what decides
    // is the peer certificate's fingerprint. A longer chain is never accepted.
    ctx->set_verify_callback([peers = identity.peers](bool, asio::ssl::verify_context& context) {
        auto* store = context.native_handle();
        if (X509_STORE_CTX_get_error_depth(store) != 0)
            return false;
        auto* certificate = X509_STORE_CTX_get_current_cert(store);
        try {
            if (certificate && std::ranges::find(peers, fingerprint(certificate)) != peers.end()) {
                X509_STORE_CTX_set_error(store, X509_V_OK);
                return true;
            }
        } catch (...) {
        }
        return false;
    });
    ctx->set_password_callback([](std::size_t, asio::ssl::context::password_purpose) {
        return std::string{};
    });
    ctx->use_certificate_chain(asio::buffer(pem(identity.certificate_file)));
    ctx->use_private_key(asio::buffer(pem(identity.private_key_file)), asio::ssl::context::pem);
    if (SSL_CTX_check_private_key(ctx->native_handle()) != 1)
        throw std::invalid_argument("TLS certificate/key mismatch");
    SSL_CTX_set_session_cache_mode(ctx->native_handle(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx->native_handle(), 0);
    return ctx;
}

} // namespace detail

struct TcpChannel::Impl
{
    asio::io_context io;
    Tcp::socket socket{io};

    void cancel()
    {
        asio::error_code ignored;
        socket.close(ignored);
    }

    template <class Start>
    void operation(Ms timeout, Start start)
    {
        run(io, timeout, start, [&] {
            cancel();
        });
    }

    void read(void* data, std::size_t size, Ms timeout)
    {
        operation(timeout, [&](auto done) {
            asio::async_read(socket, asio::buffer(data, size), done);
        });
    }
};

TcpChannel::TcpChannel() = default;
TcpChannel::~TcpChannel() = default;
TcpChannel::TcpChannel(TcpChannel&&) noexcept = default;
TcpChannel& TcpChannel::operator=(TcpChannel&&) noexcept = default;

void TcpChannel::close() noexcept
{
    impl_.reset();
}

TcpChannel TcpChannel::connect(const std::string& host, std::uint16_t port, Ms timeout)
{
    if (host.empty() || host.find('\0') != std::string::npos || !port || timeout.count() <= 0)
        throw std::invalid_argument("TCP host, port and positive timeout are required");
    TcpChannel channel;
    channel.impl_ = std::make_unique<Impl>();
    auto& p = *channel.impl_;
    const auto end = Clock::now() + timeout;
    Tcp::resolver resolver(p.io);
    Tcp::resolver::results_type addresses;
    run(
        p.io,
        remaining(end),
        [&](auto done) {
            resolver.async_resolve(
                host,
                std::to_string(port),
                [&, done](asio::error_code ec, auto values) {
                    addresses = std::move(values);
                    done(ec);
                });
        },
        [&] {
            resolver.cancel();
        });
    // IPv4 first: services bind IPv4 addresses by default, and "localhost" often
    // resolves to ::1 first.
    std::vector<Tcp::endpoint> endpoints;
    for (const auto& entry : addresses)
        endpoints.push_back(entry.endpoint());
    std::stable_partition(endpoints.begin(), endpoints.end(), [](const Tcp::endpoint& e) {
        return e.address().is_v4();
    });
    p.operation(remaining(end), [&](auto done) {
        asio::async_connect(p.socket, endpoints, [done](asio::error_code ec, const Tcp::endpoint&) {
            done(ec);
        });
    });
    p.operation(remaining(end), [&](auto done) {
        asio::async_write(p.socket, asio::buffer(connection_magic), [done](auto ec, std::size_t) {
            done(ec);
        });
    });
    return channel;
}

void TcpChannel::send(const std::string& payload, Ms timeout)
{
    if (!impl_)
        throw Error(ErrorCode::unavailable, "TCP connection is closed");
    if (payload.empty() || payload.size() > Channel::max_frame)
        throw std::invalid_argument("invalid frame size");
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::array<unsigned char, 4> header{
        static_cast<unsigned char>(size >> 24),
        static_cast<unsigned char>(size >> 16),
        static_cast<unsigned char>(size >> 8),
        static_cast<unsigned char>(size)
    };
    std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(payload)};
    try {
        impl_->operation(timeout, [&](auto done) {
            asio::async_write(impl_->socket, buffers, [done](auto ec, std::size_t) {
                done(ec);
            });
        });
    } catch (...) {
        close();
        throw;
    }
}

std::string TcpChannel::receive(Ms timeout)
{
    if (!impl_)
        throw Error(ErrorCode::unavailable, "TCP connection is closed");
    try {
        std::array<unsigned char, 4> header{};
        auto end = Clock::now() + timeout;
        impl_->read(header.data(), 1, timeout);
        const auto frame_end = Clock::now() + std::chrono::seconds(10);
        end = timeout.count() < 0 ? frame_end : std::min(end, frame_end);
        impl_->read(header.data() + 1, 3, remaining(end));
        const auto size = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16)
            | (std::uint32_t(header[2]) << 8) | header[3];
        if (!size || size > Channel::max_frame)
            throw Error(ErrorCode::resource_exhausted, "invalid TCP frame size");
        std::string payload(size, '\0');
        impl_->read(payload.data(), size, remaining(end));
        return payload;
    } catch (...) {
        close();
        throw;
    }
}

} // namespace asterion::ipc
