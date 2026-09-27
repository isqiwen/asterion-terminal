#include "node_enrollment.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fstream>
#include <stdexcept>
namespace asterion::terminal {
namespace {
void check(bool ok) {
  if (!ok)
    throw std::runtime_error("cannot provision node identity");
}
struct Identity {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
      EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free};
  std::unique_ptr<X509, decltype(&X509_free)> cert{X509_new(), X509_free};
  Identity(const char* name, int serial, Identity* issuer, const std::string& san = {},
           ipc::PeerRole role = ipc::PeerRole::unknown) {
    check(key && cert);
    auto* ca = issuer ? issuer->cert.get() : cert.get();
    check(X509_set_version(cert.get(), 2) == 1 &&
          ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial) == 1);
    check(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) &&
          X509_gmtime_adj(X509_getm_notAfter(cert.get()), 365L * 86400));
    check(X509_set_pubkey(cert.get(), key.get()) == 1);
    check(X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()), "CN", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char*>(name), -1, -1, 0) == 1);
    if (role != ipc::PeerRole::unknown) {
      const auto subject = ipc::role_subject(role);
      check(X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()), "OU", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(subject.c_str()), -1,
                                       -1, 0) == 1);
    }
    check(X509_set_issuer_name(cert.get(), X509_get_subject_name(ca)) == 1);
    auto ext = [&](int nid, const std::string& value) {
      X509V3_CTX context{};
      X509V3_set_ctx(&context, ca, cert.get(), nullptr, nullptr, 0);
      std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> e(
          X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str()), X509_EXTENSION_free);
      check(e && X509_add_ext(cert.get(), e.get(), -1) == 1);
    };
    ext(NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
    ext(NID_key_usage, issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
    ext(NID_subject_key_identifier, "hash");
    ext(NID_authority_key_identifier, "keyid:always");
    if (issuer) {
      ext(NID_ext_key_usage, san.empty() ? "clientAuth" : "serverAuth,clientAuth");
      if (!san.empty())
        ext(NID_subject_alt_name, san);
    }
    check(X509_sign(cert.get(), issuer ? issuer->key.get() : key.get(), EVP_sha256()) > 0);
  }
  void save(const std::filesystem::path& path, bool private_key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    check(bool(bio));
    check((private_key ? PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0,
                                                  nullptr, nullptr)
                       : PEM_write_bio_X509(bio.get(), cert.get())) == 1);
    char* data = nullptr;
    const auto count = BIO_get_mem_data(bio.get(), &data);
    check(count > 0);
    // Created 0600 from the first byte; a key never exists with wider access.
    write_file_durably(path, std::string_view(data, static_cast<std::size_t>(count)));
  }
};
} // namespace
void create_node_identity(const std::filesystem::path& root, const std::string& host) {
  const bool ip = host.find(':') != std::string::npos ||
                  host.find_first_not_of("0123456789.") == std::string::npos;
  // Every role is issued now: the CA key is never persisted, so no
  // certificate can be added later without re-enrolling the node.
  Identity ca("Asterion node administration CA", 1, nullptr),
      client("Asterion Terminal", 2, &ca, {}, ipc::PeerRole::admin),
      server("Asterion Node", 3, &ca, (ip ? "IP:" : "DNS:") + host, ipc::PeerRole::service),
      trading("Asterion trading client", 4, &ca, {}, ipc::PeerRole::client);
  ca.save(root / "ca.crt", false);
  client.save(root / "client.crt", false);
  client.save(root / "client.key", true);
  // Business-only identity to hand to other operators: it cannot deploy,
  // upload or run programs on the node.
  trading.save(root / "trading-client.crt", false);
  trading.save(root / "trading-client.key", true);
  server.save(root / "server.crt", false);
  server.save(root / "server.key", true);
  // CA signing key stays in memory and is destroyed; never uploaded or persisted.
}
} // namespace asterion::terminal
