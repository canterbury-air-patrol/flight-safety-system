#ifdef HAVE_CATCH2_CATCH_ALL_HPP
#include <catch2/catch_all.hpp>
#elif HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#elif HAVE_CATCH_HPP
#include <catch.hpp>
#else
#error No catch header
#endif

#include <string_view>

#include "secure-string.hpp"

namespace fss = flight_safety_system;

TEST_CASE("secure_string: default construction yields empty string")
{
    fss::secure_string s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
}

TEST_CASE("secure_string: construction from string_view")
{
    fss::secure_string s(std::string_view{"hello"});
    REQUIRE(!s.empty());
    REQUIRE(s.size() == 5);
    REQUIRE(s == std::string_view{"hello"});
}

TEST_CASE("secure_string: copy constructor")
{
    fss::secure_string a(std::string_view{"secret"});
    fss::secure_string b(a);
    REQUIRE(b == std::string_view{"secret"});
    REQUIRE(b.size() == a.size());
}

TEST_CASE("secure_string: move constructor leaves source empty")
{
    fss::secure_string a(std::string_view{"move-me"});
    fss::secure_string b(std::move(a));
    REQUIRE(b == std::string_view{"move-me"});
    REQUIRE(a.empty());
}

TEST_CASE("secure_string: copy assignment")
{
    fss::secure_string a(std::string_view{"alpha"});
    fss::secure_string b(std::string_view{"beta"});
    b = a;
    REQUIRE(b == std::string_view{"alpha"});
}

TEST_CASE("secure_string: move assignment leaves source empty")
{
    fss::secure_string a(std::string_view{"data"});
    fss::secure_string b;
    b = std::move(a);
    REQUIRE(b == std::string_view{"data"});
    REQUIRE(a.empty());
}

TEST_CASE("secure_string: assign(ptr, n) replaces content")
{
    fss::secure_string s(std::string_view{"old"});
    constexpr std::string_view newval{"newvalue"};
    s.assign(newval.data(), newval.size());
    REQUIRE(s.size() == newval.size());
    REQUIRE(s == newval);
}

TEST_CASE("secure_string: clear makes string empty")
{
    fss::secure_string s(std::string_view{"sensitive"});
    REQUIRE(!s.empty());
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
}

TEST_CASE("secure_string: operator== and operator!= between secure_strings")
{
    fss::secure_string a(std::string_view{"abc"});
    fss::secure_string b(std::string_view{"abc"});
    fss::secure_string c(std::string_view{"xyz"});
    REQUIRE(a == b);
    REQUIRE(!(a != b));
    REQUIRE(a != c);
    REQUIRE(!(a == c));
}

TEST_CASE("secure_string: operator== and operator!= against string_view")
{
    fss::secure_string s(std::string_view{"hello"});
    REQUIRE(s == std::string_view{"hello"});
    REQUIRE(!(s != std::string_view{"hello"}));
    REQUIRE(s != std::string_view{"world"});
    REQUIRE(!(s == std::string_view{"world"}));
}

TEST_CASE("secure_string: string_view comparison with different length is not equal")
{
    fss::secure_string s(std::string_view{"hi"});
    REQUIRE(s != std::string_view{"hix"});
    REQUIRE(s != std::string_view{"h"});
}

TEST_CASE("secure_string: data() returns pointer to content")
{
    fss::secure_string s(std::string_view{"test"});
    REQUIRE(s.data() != nullptr);
    REQUIRE(std::string_view(s.data(), s.size()) == "test");
}

TEST_CASE("secure_string: toNulTerminated() matches content and is NUL-terminated")
{
    fss::secure_string s(std::string_view{"p4ssw0rd"});
    std::string nt = s.toNulTerminated();
    REQUIRE(nt == "p4ssw0rd");
    REQUIRE(nt.c_str()[nt.size()] == '\0');
}

TEST_CASE("secure_string: toNulTerminated() on empty string is empty")
{
    fss::secure_string s;
    REQUIRE(s.toNulTerminated().empty());
}

TEST_CASE("secure_string: toNulTerminated() does not disturb data()/size()")
{
    fss::secure_string s(std::string_view{"unchanged"});
    (void)s.toNulTerminated();
    REQUIRE(s.size() == 9);
    REQUIRE(std::string_view(s.data(), s.size()) == "unchanged");
}
