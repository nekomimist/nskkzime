#pragma once
#include "package.h"
#include <functional>

namespace nskkzime::setup {
struct Backend {
    std::function<fs::path(bool)> read_registration;
    std::function<void(const fs::path&, bool)> register_dll;
    std::function<void(const fs::path&)> set_startup;
};
class Installation {
    fs::path root_;
    Backend backend_;
    std::function<Bundle(const fs::path&)> verify_;
    void assert_registration(bool x86, const fs::path& expected) const;
    void set_target(const std::string& target, const std::vector<std::string>& allowed);
public:
    Installation(fs::path root, Backend backend, std::function<Bundle(const fs::path&)> verifier = verify_package)
        : root_(fs::absolute(root).lexically_normal()), backend_(std::move(backend)), verify_(std::move(verifier)) {}
    std::string managed(const fs::path& dll, bool x86 = false) const;
    Bundle installed(const std::string& version) const;
    Json journal() const;
    Json status() const;
    void preflight(const std::string& target = {}) const;
    Bundle stage(const fs::path& source);
    void transition(const std::string& target);
    void recover();
};
std::string unique_suffix();
}
