#include <cstddef>
#include <memory>
#include <string>
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

#include <unistd.h>

#include "fss-transport-ssl.hpp"
#include "test_helpers.hpp"

constexpr const char *CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char *SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char *SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char *CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char *CLIENT_PUBLIC_FILE = "certs/client.public.pem";
constexpr const char *CLIENT_CRL_FILE = "certs/client.crl.pem";
constexpr const char *EMPTY_CRL_FILE = "certs/empty.crl.pem";


TEST_CASE("SSL - Connection Create (failure)")
{
    auto conn = std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);

    REQUIRE(!conn->connectTo("localhost", 1));
    REQUIRE(!conn->connectTo("127.0.0.1", 1));
    REQUIRE(!conn->connectTo("::1", 1));

    REQUIRE(!conn->connectTo("this.host.does.not.exist", 1));
}

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}


TEST_CASE("SSL - Listen Socket")
{
    client_conn = nullptr;
    const uint16_t listen_port = fss_test::pick_port();
    REQUIRE(listen_port != 0);
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    std::shared_ptr<flight_safety_system::transport::fss_connection> conn =
        std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(
            CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    // Drain identity before dropping the client connection.  Dropping conn
    // while the server's recv thread hasn't yet dequeued the identity bytes
    // causes a race where message_type_closed arrives first.
    std::shared_ptr<flight_safety_system::transport::fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = client_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);

    conn = nullptr;

    REQUIRE(fss_test::wait_for([&]() {
        msg = client_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_closed);

    msg = client_conn->getMsg();
    REQUIRE(msg == nullptr);

    client_conn = nullptr;
}

class test_ssl_message_cb : public flight_safety_system::transport::fss_message_cb {
private:
    std::shared_ptr<flight_safety_system::transport::fss_message> first{};
public:
    explicit test_ssl_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn)
        : fss_message_cb(std::move(t_conn)) {};
    auto getFirstMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message> { return this->first; }
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override
    {
        this->first = std::move(message);
    }
};


TEST_CASE("SSL - Listen - Callback")
{
    client_conn = nullptr;
    const uint16_t listen_port = fss_test::pick_port();
    REQUIRE(listen_port != 0);

    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    std::shared_ptr<flight_safety_system::transport::fss_connection> conn =
        std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(
            CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    auto cb = std::make_shared<test_ssl_message_cb>(client_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == client_conn);
    client_conn->setHandler(cb.get());

    REQUIRE(client_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());

    REQUIRE(fss_test::wait_for([&]() { return cb->getFirstMsg() != nullptr; }));

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_conn = nullptr;
}

TEST_CASE("ssl: isPeerCertRevoked detects revoked cert via CRL")
{
    const uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    std::shared_ptr<flight_safety_system::transport::fss_connection> server_conn;

    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        port,
        [&server_conn](std::shared_ptr<flight_safety_system::transport::fss_connection> c) -> bool {
            server_conn = std::move(c);
            return true;
        },
        CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(client->connectTo("localhost", port));

    REQUIRE(fss_test::wait_for([&] { return server_conn != nullptr; }));

    REQUIRE(server_conn->isPeerCertRevoked(CLIENT_CRL_FILE));
    REQUIRE_FALSE(server_conn->isPeerCertRevoked(EMPTY_CRL_FILE));
    REQUIRE_FALSE(server_conn->isPeerCertRevoked(""));

    server_conn = nullptr;
}

TEST_CASE("ssl: disconnect after CRL revocation terminates client session")
{
    const uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    std::shared_ptr<flight_safety_system::transport::fss_connection> server_conn;

    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        port,
        [&server_conn](std::shared_ptr<flight_safety_system::transport::fss_connection> c) -> bool {
            server_conn = std::move(c);
            return true;
        },
        CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = flight_safety_system::transport_ssl::fss_connection_client::create(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE, "localhost", port);
    REQUIRE(client != nullptr);
    REQUIRE(fss_test::wait_for([&] { return server_conn != nullptr; }));

    REQUIRE(server_conn->isPeerCertRevoked(CLIENT_CRL_FILE));
    server_conn->disconnect();

    REQUIRE(fss_test::wait_for([&] {
        auto msg = client->getMsg();
        return msg != nullptr && msg->getType() == flight_safety_system::transport::message_type_closed;
    }));

    server_conn = nullptr;
}

TEST_CASE("SSL - Negotiated cipher suite is AEAD (TLS 1.2+)")
{
    const uint16_t listen_port = fss_test::pick_port();
    REQUIRE(listen_port != 0);
    client_conn = nullptr;

    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn->connectTo("localhost", listen_port));

    std::string desc = conn->getSessionDesc();
    REQUIRE_FALSE(desc.empty());

    // Protocol must be TLS 1.2 or TLS 1.3 — TLS 1.0/1.1 are excluded by the priority string
    bool modern_tls = desc.find("TLS1.2") != std::string::npos || desc.find("TLS1.3") != std::string::npos;
    REQUIRE(modern_tls);

    // Cipher must be AEAD (AES-GCM, AES-CCM, or ChaCha20-Poly1305)
    bool aead = desc.find("GCM") != std::string::npos || desc.find("POLY1305") != std::string::npos ||
                desc.find("-CCM") != std::string::npos;
    REQUIRE(aead);

    client_conn = nullptr;
}
