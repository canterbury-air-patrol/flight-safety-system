#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "fss.hpp"
using namespace flight_safety_system;

TEST_CASE("Identity Message Check") {
    /* Create a test identity */
    fss_message_identity *msg = new fss_message_identity("test1");
    /* Check the type */
    REQUIRE(msg->getType() == message_type_identity);
    /* Get the name */
    REQUIRE(msg->getName() == "test1");
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_identity *decoded = new fss_message_identity(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(decoded->getType() == message_type_identity);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getName() == "test1");
    delete decoded;
    delete msg;
}

TEST_CASE("RTT Request Message Check") {
    /* Create a test rtt request */
    fss_message_rtt_request *msg = new fss_message_rtt_request();
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_request);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_rtt_request *decoded = new fss_message_rtt_request(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_request);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    delete decoded;
    delete msg;
}

TEST_CASE("RTT Response Message Check") {
    /* Create a test rtt response */
    fss_message_rtt_response *msg = new fss_message_rtt_response(8765);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_response);
    /* Check the message id */
    REQUIRE(msg->getRequestId() == 8765);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_rtt_response *decoded = new fss_message_rtt_response(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_rtt_response);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getRequestId() == 8765);
    delete decoded;
    delete msg;
}

TEST_CASE("Position Report Message Check") {
    /* Create a test position report */
    fss_message_position_report *msg = new fss_message_position_report(-43.5, 172.0, 1234, 5678);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_position_report);
    /* Check the parameters */
    REQUIRE(msg->getLatitude() == -43.5);
    REQUIRE(msg->getLongitude() == 172.0);
    REQUIRE(msg->getAltitude() == 1234);
    REQUIRE(msg->getTimeStamp() == 5678);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_position_report *decoded = new fss_message_position_report(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_position_report);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getLatitude() == -43.5);
    REQUIRE(decoded->getLongitude() == 172.0);
    REQUIRE(decoded->getAltitude() == 1234);
    REQUIRE(decoded->getTimeStamp() == 5678);
    delete decoded;
    delete msg;
}

TEST_CASE("System Status Message Check") {
    /* Create a test system status */
    fss_message_system_status *msg = new fss_message_system_status(99, 1200);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_system_status);
    /* Check the parameters */
    REQUIRE(msg->getBatRemaining() == 99);
    REQUIRE(msg->getBatMAHUsed() == 1200);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_system_status *decoded = new fss_message_system_status(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getBatRemaining() == 99);
    REQUIRE(decoded->getBatMAHUsed() == 1200);
    delete decoded;
    delete msg;
}


TEST_CASE("Search Status Message Check") {
    /* Create a test search status */
    fss_message_search_status *msg = new fss_message_search_status(1234, 5678, 91011);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_search_status);
    /* Check the parameters */
    REQUIRE(msg->getSearchId() == 1234);
    REQUIRE(msg->getSearchCompleted() == 5678);
    REQUIRE(msg->getSearchTotal() == 91011);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_search_status *decoded = new fss_message_search_status(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_search_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getSearchId() == 1234);
    REQUIRE(decoded->getSearchCompleted() == 5678);
    REQUIRE(decoded->getSearchTotal() == 91011);
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Basic") {
    /* Create a test asset command */
    fss_message_asset_command *msg = new fss_message_asset_command(asset_command_rtl, 1234);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_rtl);
    REQUIRE(msg->getTimeStamp() == 1234);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_asset_command *decoded = new fss_message_asset_command(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_rtl);
    REQUIRE(decoded->getTimeStamp() == 1234);
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Position") {
    /* Create a test asset command */
    fss_message_asset_command *msg = new fss_message_asset_command(asset_command_goto, 1234, -43.5, 172.0);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_goto);
    REQUIRE(msg->getTimeStamp() == 1234);
    REQUIRE(msg->getLatitude() == -43.5);
    REQUIRE(msg->getLongitude() == 172.0);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_asset_command *decoded = new fss_message_asset_command(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_goto);
    REQUIRE(decoded->getTimeStamp() == 1234);
    REQUIRE(decoded->getLatitude() == -43.5);
    REQUIRE(decoded->getLongitude() == 172.0);
    delete decoded;
    delete msg;
}

TEST_CASE("Asset Command Message Check - Altitude") {
    /* Create a test asset command */
    fss_message_asset_command *msg = new fss_message_asset_command(asset_command_altitude, 1234, 5678);
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == asset_command_altitude);
    REQUIRE(msg->getTimeStamp() == 1234);
    REQUIRE(msg->getAltitude() == 5678);
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_asset_command *decoded = new fss_message_asset_command(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getCommand() == asset_command_altitude);
    REQUIRE(decoded->getTimeStamp() == 1234);
    REQUIRE(decoded->getAltitude() == 5678);
    delete decoded;
    delete msg;
}

TEST_CASE("SMM Settings Message Check") {
    /* Create a test asset command */
    fss_message_smm_settings *msg = new fss_message_smm_settings("https://localhost/", "asset", "password1");
    /* Check the type */
    REQUIRE(msg->getType() == message_type_smm_settings);
    /* Check the parameters */
    REQUIRE(msg->getServerURL() == "https://localhost/");
    REQUIRE(msg->getUsername() == "asset");
    REQUIRE(msg->getPassword() == "password1");
    /* Convert to bl and back */
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_smm_settings *decoded = new fss_message_smm_settings(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_smm_settings);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    REQUIRE(decoded->getServerURL() == "https://localhost/");
    REQUIRE(decoded->getUsername() == "asset");
    REQUIRE(decoded->getPassword() == "password1");
    delete decoded;
    delete msg;
}

TEST_CASE("Server List Message Check") {
    /* Create a test asset command */
    fss_message_server_list *msg = new fss_message_server_list();
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
    buf_len *bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    fss_message_server_list *decoded = new fss_message_server_list(2563, bl);
    delete bl;
    /* Check the type */
    REQUIRE(msg->getType() == message_type_server_list);
    /* Check input == output */
    REQUIRE(decoded->getId() == 2563);
    auto sl2 = msg->getServers();
    REQUIRE(!sl2.empty());
    REQUIRE(sl2[0].first == "localhost");
    REQUIRE(sl2[0].second == 20202);
    delete decoded;
    delete msg;
}
