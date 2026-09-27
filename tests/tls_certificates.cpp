// Ephemeral test PKI only. Never installed or bundled with Terminal.
#include <CLI/CLI.hpp>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
void check(bool ok) { if (!ok) throw std::runtime_error("test certificate generation failed"); }
void extension(X509* certificate, X509* issuer, int nid, const char* value) {
    X509V3_CTX context{}; X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> ext(X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
    check(ext && X509_add_ext(certificate, ext.get(), -1) == 1);
}
struct Identity {
    Key key{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free};
    Cert certificate{X509_new(), X509_free};
    Identity(const char* name, int serial, Identity* issuer = nullptr, bool expired = false, const char* names = "DNS:localhost,IP:127.0.0.1") {
        check(key && certificate);
        auto* cert = certificate.get(); auto* ca = issuer ? issuer->certificate.get() : cert;
        check(X509_set_version(cert, 2) == 1 && ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) == 1);
        check(X509_gmtime_adj(X509_getm_notBefore(cert), -3600) && X509_gmtime_adj(X509_getm_notAfter(cert), expired ? -60 : 86400));
        check(X509_set_pubkey(cert, key.get()) == 1);
        auto* subject = X509_get_subject_name(cert);
        check(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1, -1, 0) == 1);
        check(X509_set_issuer_name(cert, X509_get_subject_name(ca)) == 1);
        extension(cert, ca, NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
        extension(cert, ca, NID_key_usage, issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
        extension(cert, ca, NID_subject_key_identifier, "hash");
        extension(cert, ca, NID_authority_key_identifier, "keyid:always");
        if (issuer) {
            extension(cert, ca, NID_ext_key_usage, "serverAuth,clientAuth");
            extension(cert, ca, NID_subject_alt_name, names);
        }
        check(X509_sign(cert, issuer ? issuer->key.get() : key.get(), EVP_sha256()) > 0);
    }
    void save(const std::filesystem::path& directory, const char* name) {
        auto write = [&](bool private_key) {
            std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free); check(bool(bio));
            check((private_key ? PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) : PEM_write_bio_X509(bio.get(), certificate.get())) == 1);
            char* data = nullptr; const auto length = BIO_get_mem_data(bio.get(), &data); check(length > 0);
            const auto path = directory / (std::string(name) + (private_key ? ".key" : ".crt"));
            std::ofstream file(path, std::ios::binary); file.write(data, length); check(bool(file)); file.close();
            if (private_key) std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        };
        write(false); write(true);
    }
};
int main(int argc, char** argv) {
    CLI::App app{"Generate ephemeral test certificates"}; std::string directory;
    std::string server_names = "DNS:localhost,IP:127.0.0.1";
    app.add_option("directory", directory)->required()->check(CLI::ExistingDirectory);
    app.add_option("--server-names", server_names, "Test server certificate SAN list");
    argv = app.ensure_utf8(argv); CLI11_PARSE(app, argc, argv);
    const std::filesystem::path path(std::u8string(directory.begin(), directory.end()));
    Identity ca("test-ca", 1), other("untrusted-ca", 2), server("server", 3, &ca, false, server_names.c_str()), client("client", 4, &ca), stranger("stranger", 5, &other), expired("expired", 6, &ca, true);
    Identity wrong("wrong-server", 7, &ca, false, "DNS:wrong.example.invalid"); wrong.save(path, "wrong");
    ca.save(path, "ca"); other.save(path, "other"); server.save(path, "server"); client.save(path, "client"); stranger.save(path, "stranger"); expired.save(path, "expired");
}
