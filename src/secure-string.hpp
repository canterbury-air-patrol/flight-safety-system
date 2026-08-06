#pragma once

/* <cstring>, not <strings.h>: glibc declares explicit_bzero in <string.h>
 * (which includes <strings.h>, not the reverse). This header compiled only
 * because every TU that reached it happened to pull <cstring> in first; a
 * standalone include failed. */
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace flight_safety_system {

/* Scope decision (docs/decisions/43-secure-string-scope.md, 2026-07-10):
 * secure_string's goal is limiting
 * *long-lived resident copies* of credentials — cache/member fields such as
 * smm_settings' username/password and db_connection::pass_ — not eliminating
 * every transient copy a credential passes through on its way there. Wire
 * I/O buffers and the parsed Json::Value config are explicitly out of scope:
 * they are freed promptly and scrubbing them would require invasive changes
 * (a wiping allocator for std::string / jsoncpp) for a defence-in-depth gain
 * against an adversary who can already read process memory. Two cheap
 * exceptions are closed directly instead of left inconsistent: the recv-side
 * frame for message_type_smm_settings is wiped right after decode
 * (buf_len::wipeSecure(), called from fss_connection::recvMsg), and the
 * packed buf_len for an outgoing smm_settings message is wiped right after
 * the send attempt (fss_connection::sendMsg). Full coverage of every
 * transient std::string/Json::Value the credential passes through is not the
 * goal.
 */
class secure_string {
    std::vector<char> data_;
public:
    secure_string() : data_() {}
    explicit secure_string(std::string_view s) : data_(s.begin(), s.end()) {}
    secure_string(const secure_string &o) : data_(o.data_) {}
    secure_string(secure_string &&o) noexcept : data_(std::move(o.data_)) {}
    auto operator=(const secure_string &o) -> secure_string &
    {
        if (this != &o)
        {
            clear();
            data_ = o.data_;
        }
        return *this;
    }
    auto operator=(secure_string &&o) noexcept -> secure_string &
    {
        if (this != &o)
        {
            clear();
            data_ = std::move(o.data_);
        }
        return *this;
    }
    ~secure_string() { clear(); }
    void clear()
    {
        if (!data_.empty())
        {
            explicit_bzero(data_.data(), data_.size());
            data_.clear();
        }
    }
    void assign(const char *p, std::size_t n)
    {
        clear();
        data_.assign(p, p + n);
    }
    [[nodiscard]] auto data() const -> const char * { return data_.data(); }
    [[nodiscard]] auto size() const -> std::size_t { return data_.size(); }
    /* A fresh NUL-terminated copy for C APIs that need a c_str()-style
     * pointer (data()/size() stay exact, un-padded, for wire packing).
     * Returns by value rather than caching a scratch buffer on this object:
     * db_connection holds one secure_string pass_ shared by its read and
     * write ECPG connections, each reconnected from its own thread under its
     * own mutex (see fss-server.hpp), so a cached mutable member here would
     * be a data race between them. The returned std::string is a transient
     * copy, out of scope for wiping per the policy above. */
    [[nodiscard]] auto toNulTerminated() const -> std::string { return {data_.begin(), data_.end()}; }
    [[nodiscard]] auto empty() const -> bool { return data_.empty(); }
    auto operator==(const secure_string &o) const -> bool
    {
        return data_.size() == o.data_.size() && constantTimeEquals(data_.data(), o.data_.data(), data_.size());
    }
    auto operator!=(const secure_string &o) const -> bool { return !(*this == o); }
    auto operator==(std::string_view sv) const -> bool
    {
        return sv.size() == data_.size() && constantTimeEquals(data_.data(), sv.data(), data_.size());
    }
    auto operator!=(std::string_view sv) const -> bool { return !(*this == sv); }
private:
    /* Length is checked (short-circuit) by each caller above before this
     * runs — length is not the secret here, only content is. This walks the
     * full length regardless of where a mismatch falls, so operator== can't
     * be turned into a timing oracle if a future caller ever verifies a
     * secret against attacker-supplied input (todo/57). */
    [[nodiscard]] static auto constantTimeEquals(const char *a, const char *b, std::size_t n) -> bool
    {
        volatile unsigned char diff = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return diff == 0;
    }
};

} // namespace flight_safety_system
