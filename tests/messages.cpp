#include "config.h"

#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file
#ifdef HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#elif HAVE_CATCH_HPP
#include <catch.hpp>
#else
#error No catch header
#endif

#include <cmath>

#include "fss-transport.hpp"
using namespace flight_safety_system::transport;

TEST_CASE("Close Connection Check") {
    /* Create a test closed connection */
    auto msg = new fss_message_closed();
    /* Check the type */
    REQUIRE(msg->getType() == message_type_closed);
    /* Convert to bl */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded_generic = fss_message::decode(bl);
    REQUIRE(decoded_generic == nullptr);
    delete decoded_generic;

    /* Check the base fss_message functionality */
    REQUIRE(std::isnan(msg->getLatitude()));
    REQUIRE(std::isnan(msg->getLongitude()));
    REQUIRE(msg->getAltitude() == 0);
    REQUIRE(msg->getTimeStamp() == 0);

    delete bl;
    delete msg;
}

TEST_CASE("Identity Message Check") {
    /* Create a test identity */
    auto msg = new fss_message_identity("test1");
    /* Check the type */
    REQUIRE(msg->getType() == message_type_identity);
    /* Get the name */
    REQUIRE(msg->getName() == "test1");
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_identity(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_identity);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getName() == "test1");
    delete decoded;
    auto decoded_generic = fss_message::decode(bl);
    REQUIRE(decoded_generic->getType() == message_type_identity);
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_identity *)decoded_generic)->getName() == "test1");
    delete decoded_generic;
    delete bl;
    delete msg;
}

TEST_CASE("RTT Request Message Check") {
    /* Create a test rtt request */
    auto msg = new fss_message_rtt_request();
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_request);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_rtt_request(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_rtt_request);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    delete decoded;
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_rtt_request);
    /* Check the id */
    REQUIRE(decoded_generic->getId() == 2563);
    delete bl;
    delete decoded_generic;
    delete msg;
}

TEST_CASE("RTT Response Message Check") {
    /* Create a test rtt response */
    auto msg = new fss_message_rtt_response(8765);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_response);
    /* Check the message id */
    REQUIRE(msg->getRequestId() == 8765);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_rtt_response(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_rtt_response);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getRequestId() == 8765);
    delete decoded;
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_rtt_response);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_rtt_response *)decoded_generic)->getRequestId() == 8765);
    delete decoded_generic;
    delete bl;
    delete msg;
}

TEST_CASE("Position Report Message Check") {
    /* Create a test position report */
    auto msg = new fss_message_position_report(-43.5, 172.0, 1234, 240, 200, 1, 217573, "ZK-ABC", 01200, 5678);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_position_report);
    /* Check the parameters */
    REQUIRE(msg->getLatitude() == -43.5);
    REQUIRE(msg->getLongitude() == 172.0);
    REQUIRE(msg->getAltitude() == 1234);
    REQUIRE(msg->getTimeStamp() == 5678);
    REQUIRE(msg->getICAOAddress() == 217573);
    REQUIRE(msg->getHeading() == 240);
    REQUIRE(msg->getHorzVel() == 200);
    REQUIRE(msg->getVertVel() == 1);
    REQUIRE(msg->getCallSign() == "ZK-ABC");
    REQUIRE(msg->getSquawk() == 01200);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_position_report(2563, bl);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_position_report);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getLatitude() == -43.5);
    REQUIRE(decoded->getLongitude() == 172.0);
    REQUIRE(decoded->getAltitude() == 1234);
    REQUIRE(decoded->getTimeStamp() == 5678);
    REQUIRE(decoded->getICAOAddress() == 217573);
    REQUIRE(decoded->getHeading() == 240);
    REQUIRE(decoded->getHorzVel() == 200);
    REQUIRE(decoded->getVertVel() == 1);
    REQUIRE(decoded->getCallSign() == "ZK-ABC");
    REQUIRE(decoded->getSquawk() == 01200);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_position_report);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(decoded_generic->getLatitude() == -43.5);
    REQUIRE(decoded_generic->getLongitude() == 172.0);
    REQUIRE(decoded_generic->getAltitude() == 1234);
    REQUIRE(decoded_generic->getTimeStamp() == 5678);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("System Status Message Check") {
    /* Create a test system status */
    auto msg = new fss_message_system_status(99, 1200);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_system_status);
    /* Check the parameters */
    REQUIRE(msg->getBatRemaining() == 99);
    REQUIRE(msg->getBatMAHUsed() == 1200);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_system_status(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getBatRemaining() == 99);
    REQUIRE(decoded->getBatMAHUsed() == 1200);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_system_status *)decoded_generic)->getBatRemaining() == 99);
    REQUIRE(((fss_message_system_status *)decoded_generic)->getBatMAHUsed() == 1200);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}


TEST_CASE("Search Status Message Check") {
    /* Create a test search status */
    auto msg = new fss_message_search_status(1234, 5678, 91011);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_search_status);
    /* Check the parameters */
    REQUIRE(msg->getSearchId() == 1234);
    REQUIRE(msg->getSearchCompleted() == 5678);
    REQUIRE(msg->getSearchTotal() == 91011);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_search_status(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_search_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getSearchId() == 1234);
    REQUIRE(decoded->getSearchCompleted() == 5678);
    REQUIRE(decoded->getSearchTotal() == 91011);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_search_status);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_search_status *)decoded_generic)->getSearchId() == 1234);
    REQUIRE(((fss_message_search_status *)decoded_generic)->getSearchCompleted() == 5678);
    REQUIRE(((fss_message_search_status *)decoded_generic)->getSearchTotal() == 91011);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Basic") {
    /* Create a test asset command */
    auto msg = new fss_message_asset_command(asset_command_rtl, 1234);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_rtl);
    REQUIRE(msg->getTimeStamp() == 1234);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_asset_command(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_rtl);
    REQUIRE(decoded->getTimeStamp() == 1234);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_asset_command *)decoded_generic)->getCommand() == asset_command_rtl);
    REQUIRE(decoded_generic->getTimeStamp() == 1234);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Position") {
    /* Create a test asset command */
    auto msg = new fss_message_asset_command(asset_command_goto, 1234, -43.5, 172.0);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_goto);
    REQUIRE(msg->getTimeStamp() == 1234);
    REQUIRE(msg->getLatitude() == -43.5);
    REQUIRE(msg->getLongitude() == 172.0);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_asset_command(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_goto);
    REQUIRE(decoded->getTimeStamp() == 1234);
    REQUIRE(decoded->getLatitude() == -43.5);
    REQUIRE(decoded->getLongitude() == 172.0);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_asset_command *)decoded_generic)->getCommand() == asset_command_goto);
    REQUIRE(decoded_generic->getTimeStamp() == 1234);
    REQUIRE(decoded_generic->getLatitude() == -43.5);
    REQUIRE(decoded_generic->getLongitude() == 172.0);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Altitude") {
    /* Create a test asset command */
    auto msg = new fss_message_asset_command(asset_command_altitude, 1234, 5678);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_altitude);
    REQUIRE(msg->getTimeStamp() == 1234);
    REQUIRE(msg->getAltitude() == 5678);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_asset_command(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_altitude);
    REQUIRE(decoded->getTimeStamp() == 1234);
    REQUIRE(decoded->getAltitude() == 5678);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_asset_command *)decoded_generic)->getCommand() == asset_command_altitude);
    REQUIRE(decoded_generic->getTimeStamp() == 1234);
    REQUIRE(decoded_generic->getAltitude() == 5678);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("SMM Settings Message Check") {
    /* Create a test asset command */
    auto msg = new fss_message_smm_settings("https://localhost/", "asset", "password1");
    /* Check the type */
    REQUIRE(msg->getType() == message_type_smm_settings);
    /* Check the parameters */
    REQUIRE(msg->getServerURL() == "https://localhost/");
    REQUIRE(msg->getUsername() == "asset");
    REQUIRE(msg->getPassword() == "password1");
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_smm_settings(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_smm_settings);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getServerURL() == "https://localhost/");
    REQUIRE(decoded->getUsername() == "asset");
    REQUIRE(decoded->getPassword() == "password1");
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_smm_settings);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    REQUIRE(((fss_message_smm_settings *)decoded_generic)->getServerURL() == "https://localhost/");
    REQUIRE(((fss_message_smm_settings *)decoded_generic)->getUsername() == "asset");
    REQUIRE(((fss_message_smm_settings *)decoded_generic)->getPassword() == "password1");
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}

TEST_CASE("Server List Message Check") {
    /* Create a test asset command */
    auto msg = new fss_message_server_list();
    /* Check the type */
    REQUIRE(msg->getType() == message_type_server_list);
    /* Check the parameters */
    REQUIRE(msg->getServers().empty());
    msg->addServer("localhost", 20202);
    auto sl = msg->getServers();
    REQUIRE(!sl.empty());
    REQUIRE(sl[0].first == "localhost");
    REQUIRE(sl[0].second == 20202);
    /* Convert to bl and back */
    msg->setId(2563);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = new fss_message_server_list(2563, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_server_list);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    auto sl2 = decoded->getServers();
    REQUIRE(!sl2.empty());
    REQUIRE(sl2[0].first == "localhost");
    REQUIRE(sl2[0].second == 20202);
    auto decoded_generic = fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == message_type_server_list);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == 2563);
    auto sl3 = ((fss_message_server_list *)decoded_generic)->getServers();
    REQUIRE(!sl3.empty());
    REQUIRE(sl3[0].first == "localhost");
    REQUIRE(sl3[0].second == 20202);
    delete decoded_generic;
    delete bl;
    delete decoded;
    delete msg;
}
