#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
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

#include <csignal>

/* Ignore SIGPIPE: background recv/send threads created by tests can
 * outlive the sockets they reference, and a write to a closed socket
 * would otherwise kill the test binary with exit status 141. */
namespace {
struct sigpipe_ignore_init {
    sigpipe_ignore_init() { std::signal(SIGPIPE, SIG_IGN); }
};
sigpipe_ignore_init sigpipe_guard{};
} // namespace
