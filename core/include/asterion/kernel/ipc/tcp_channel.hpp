#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace asterion::ipc {

// Remote connections are TCP, one of two kinds:
//
// - Plain: on the local network. The client sends these four bytes ("ASTR",
//   0x41535452) before its first frame and the host closes a connection that
//   starts with anything else. They tell an Asterion client from a stray
//   connection; they are not a secret.
// - TLS between two parties that know each other: each has its own
//   self-signed certificate and key, and accepts only the peer certificates
//   whose SHA-256 fingerprints it was given. No CA and no host names.
inline constexpr std::array<unsigned char, 4> connection_magic{'A', 'S', 'T', 'R'};

struct TlsIdentity
{
    std::string certificate_file, private_key_file;
    // Fingerprints of the peer certificates this side accepts.
    std::vector<std::string> peers;

    bool empty() const noexcept
    {
        return certificate_file.empty() && private_key_file.empty() && peers.empty();
    }
};

// A fingerprint as 64 lowercase hex digits. Accepts upper case and the colon
// separated form OpenSSL prints; throws std::invalid_argument for anything else.
std::string normalize_fingerprint(const std::string& text);
// The SHA-256 fingerprint of the DER encoding of a PEM certificate file.
std::string certificate_fingerprint(const std::string& certificate_file);
// Writes a new P-256 key (owner only) and a self-signed certificate for it,
// both durably, and returns the certificate's fingerprint. Validity is not
// checked by TLS here, so the certificate never needs renewing.
std::string create_identity(
    const std::filesystem::path& certificate_file,
    const std::filesystem::path& private_key_file,
    const std::string& name);

// Single-owner framed plain TCP client stream (it sends the connection magic
// first). Distinct channels have independent event loops and may be used
// concurrently. Move ownership between threads; never operate on one channel
// concurrently.
class TcpChannel
{
public:
    TcpChannel();
    ~TcpChannel();
    TcpChannel(TcpChannel&&) noexcept;
    TcpChannel& operator=(TcpChannel&&) noexcept;
    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;
    static TcpChannel
    connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout);
    std::string receive(std::chrono::milliseconds timeout);
    void send(const std::string& payload, std::chrono::milliseconds timeout);
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::ipc
