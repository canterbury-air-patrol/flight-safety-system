#pragma once

#include <cstring>
#include <strings.h>
#include <string_view>
#include <vector>

namespace flight_safety_system {

class secure_string {
    std::vector<char> data_;
public:
    secure_string() : data_() {}
    explicit secure_string(std::string_view s) : data_(s.begin(), s.end()) {}
    secure_string(const secure_string &o) : data_(o.data_) {}
    secure_string(secure_string &&o) noexcept : data_(std::move(o.data_)) {}
    auto operator=(const secure_string &o) -> secure_string & {
        if (this != &o) { clear(); data_ = o.data_; }
        return *this;
    }
    auto operator=(secure_string &&o) noexcept -> secure_string & {
        if (this != &o) { clear(); data_ = std::move(o.data_); }
        return *this;
    }
    ~secure_string() { clear(); }
    void clear() {
        if (!data_.empty()) {
            explicit_bzero(data_.data(), data_.size());
            data_.clear();
        }
    }
    void assign(const char *p, std::size_t n) {
        clear();
        data_.assign(p, p + n);
    }
    [[nodiscard]] auto data() const -> const char * { return data_.data(); }
    [[nodiscard]] auto size() const -> std::size_t { return data_.size(); }
    [[nodiscard]] auto empty() const -> bool { return data_.empty(); }
    auto operator==(const secure_string &o) const -> bool { return data_ == o.data_; }
    auto operator!=(const secure_string &o) const -> bool { return data_ != o.data_; }
    auto operator==(std::string_view sv) const -> bool {
        return sv.size() == data_.size() && memcmp(data_.data(), sv.data(), data_.size()) == 0;
    }
    auto operator!=(std::string_view sv) const -> bool { return !(*this == sv); }
};

} // namespace flight_safety_system
