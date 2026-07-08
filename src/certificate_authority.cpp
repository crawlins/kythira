#include <raft/certificate_authority_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace raft::testing {

certificate_authority::certificate_authority(ca_options options)
    : _impl(std::make_unique<impl>(options)) {}

certificate_authority::certificate_authority(std::unique_ptr<impl> existing_impl)
    : _impl(std::move(existing_impl)) {}

certificate_authority::~certificate_authority() = default;
certificate_authority::certificate_authority(certificate_authority&&) noexcept = default;
certificate_authority& certificate_authority::operator=(certificate_authority&&) noexcept = default;

auto certificate_authority::from_existing(std::string ca_cert_pem, std::string ca_key_pem)
    -> certificate_authority {
    return certificate_authority(
        std::make_unique<impl>(std::move(ca_cert_pem), std::move(ca_key_pem)));
}

auto certificate_authority::root_certificate_pem() const -> const std::string& {
    return _impl->root_pem;
}

auto certificate_authority::issue(leaf_certificate_options options) -> pem_material {
    auto not_before = options.not_before;
    auto not_after = not_before + options.validity;
    return _impl->issue_with_window(std::move(options), not_before, not_after);
}

auto certificate_authority::issue_expired(leaf_certificate_options options) -> pem_material {
    auto now = std::chrono::system_clock::now();
    auto not_before = now - std::chrono::hours(24 * 2);
    auto not_after = now - std::chrono::hours(24);
    return _impl->issue_with_window(std::move(options), not_before, not_after);
}

auto certificate_authority::issue_not_yet_valid(leaf_certificate_options options) -> pem_material {
    auto now = std::chrono::system_clock::now();
    auto not_before = now + std::chrono::hours(24);
    auto not_after = not_before + options.validity;
    return _impl->issue_with_window(std::move(options), not_before, not_after);
}

auto certificate_authority::revoke(const pem_material& cert) -> void {
    _impl->revoke(cert);
}

auto certificate_authority::mark_revoked_externally(
    std::uint64_t serial, std::chrono::system_clock::time_point revoked_at) -> void {
    _impl->mark_revoked_externally(serial, revoked_at);
}

auto certificate_authority::crl_pem() const -> std::string {
    return _impl->crl_pem();
}

auto certificate_authority::export_root_material() const -> pem_material {
    pem_material out;
    out.certificate_pem = _impl->root_pem;
    out.private_key_pem = detail::serialize_key(_impl->ca_key.get());
    return out;
}

auto certificate_authority::sign_csr(std::string csr_pem, csr_signing_options options)
    -> pem_material {
    return _impl->sign_csr(csr_pem, options);
}

namespace detail_testing {
auto unsafe_extract_ca_private_key_pem(const certificate_authority& ca) -> std::string {
    return detail::serialize_key(ca._impl->ca_key.get());
}
}  // namespace detail_testing

// ---------------------------------------------------------------------------
// generate_key_and_csr (certificate_provider.hpp)
// ---------------------------------------------------------------------------

auto generate_key_and_csr(leaf_certificate_options options) -> csr_material {
    auto key = detail::generate_key(options.algorithm);
    auto subject_name = detail::build_name(options.subject);

    detail::x509_req_ptr req{X509_REQ_new()};
    if (!req) detail::throw_openssl_error("X509_REQ_new failed");
    if (X509_REQ_set_version(req.get(), 0) != 1)
        detail::throw_openssl_error("X509_REQ_set_version failed");
    if (X509_REQ_set_subject_name(req.get(), subject_name.get()) != 1) {
        detail::throw_openssl_error("X509_REQ_set_subject_name failed");
    }
    if (X509_REQ_set_pubkey(req.get(), key.get()) != 1) {
        detail::throw_openssl_error("X509_REQ_set_pubkey failed");
    }

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, nullptr, nullptr, req.get(), nullptr, 0);
    X509_EXTENSION* san_ext = X509V3_EXT_conf_nid(
        nullptr, &ctx, NID_subject_alt_name,
        detail::build_san_value(options.dns_names, options.ip_addresses).c_str());
    if (san_ext == nullptr) detail::throw_openssl_error("X509V3_EXT_conf_nid(SAN) failed");

    STACK_OF(X509_EXTENSION)* exts = sk_X509_EXTENSION_new_null();
    sk_X509_EXTENSION_push(exts, san_ext);
    bool added = X509_REQ_add_extensions(req.get(), exts) == 1;
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    if (!added) detail::throw_openssl_error("X509_REQ_add_extensions failed");

    if (X509_REQ_sign(req.get(), key.get(), EVP_sha256()) == 0) {
        detail::throw_openssl_error("X509_REQ_sign failed");
    }

    auto bio = detail::make_bio();
    if (PEM_write_bio_X509_REQ(bio.get(), req.get()) != 1) {
        detail::throw_openssl_error("PEM_write_bio_X509_REQ failed");
    }

    csr_material out;
    out.private_key_pem = detail::serialize_key(key.get());
    out.csr_pem = detail::bio_to_string(bio.get());
    return out;
}

// ---------------------------------------------------------------------------
// temp_cert_files
// ---------------------------------------------------------------------------

namespace {

auto write_file(const std::filesystem::path& path, const std::string& content, bool restrict_perms)
    -> std::string {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::filesystem::filesystem_error("temp_cert_files: cannot open file for writing",
                                                path, std::make_error_code(std::errc::io_error));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (restrict_perms) {
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }
    return path.string();
}

}  // namespace

temp_cert_files::temp_cert_files(const pem_material& material) {
    auto unique =
        std::to_string(material.serial) + "_" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("kythira_ca_" + unique);
    std::filesystem::create_directories(dir);
    _dir = dir.string();

    _cert_path = write_file(dir / "cert.pem", material.certificate_pem, /*restrict_perms=*/false);
    _key_path = write_file(dir / "key.pem", material.private_key_pem, /*restrict_perms=*/true);
    if (!material.chain_pem.empty()) {
        _chain_path = write_file(dir / "chain.pem", material.chain_pem, /*restrict_perms=*/false);
    }
}

temp_cert_files::~temp_cert_files() {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);  // best-effort; ec deliberately ignored
}

auto temp_cert_files::cert_path() const -> const std::string& {
    return _cert_path;
}
auto temp_cert_files::key_path() const -> const std::string& {
    return _key_path;
}
auto temp_cert_files::chain_path() const -> const std::string& {
    return _chain_path;
}

namespace {

// Writes `content` to `final_path` via a same-directory temp file + rename,
// so a concurrent reader never observes a partially-written file.
void replace_file_atomically(const std::filesystem::path& final_path, const std::string& content,
                             bool restrict_perms) {
    auto tmp_path = final_path;
    tmp_path += ".tmp";
    write_file(tmp_path, content, restrict_perms);
    std::filesystem::rename(tmp_path, final_path);
}

}  // namespace

auto temp_cert_files::replace_atomically(const pem_material& material) -> void {
    std::filesystem::path dir(_dir);
    replace_file_atomically(
        _cert_path.empty() ? dir / "cert.pem" : std::filesystem::path(_cert_path),
        material.certificate_pem, /*restrict_perms=*/false);
    replace_file_atomically(_key_path.empty() ? dir / "key.pem" : std::filesystem::path(_key_path),
                            material.private_key_pem, /*restrict_perms=*/true);
    if (!material.chain_pem.empty()) {
        auto chain_target =
            _chain_path.empty() ? dir / "chain.pem" : std::filesystem::path(_chain_path);
        replace_file_atomically(chain_target, material.chain_pem, /*restrict_perms=*/false);
        _chain_path = chain_target.string();
    }
    if (_cert_path.empty()) _cert_path = (dir / "cert.pem").string();
    if (_key_path.empty()) _key_path = (dir / "key.pem").string();
}

}  // namespace raft::testing
