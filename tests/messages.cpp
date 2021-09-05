#include <cstdlib>
#include <memory>

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

TEST_CASE("Close Connection Check") {
    auto msg_id = static_cast<uint64_t>(random());

    /* Create a test closed connection */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_closed>();
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_closed);
    /* Convert to bl */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic == nullptr);

    /* Check the base fss_message functionality */
    REQUIRE(std::isnan(msg->getLatitude()));
    REQUIRE(std::isnan(msg->getLongitude()));
    REQUIRE(msg->getAltitude() == 0);
    REQUIRE(msg->getTimeStamp() == 0);
}

TEST_CASE("Identity Message Check") {
    auto msg_id = static_cast<uint64_t>(random());

    /* Create a test identity */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("test1");
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);
    /* Get the name */
    REQUIRE(msg->getName() == "test1");
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_identity>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_identity);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getName() == "test1");
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_identity);
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_identity = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_identity>(decoded_generic);
    REQUIRE(decoded_generic_identity != nullptr);
    REQUIRE(decoded_generic_identity->getName() == "test1");
}

TEST_CASE("RTT Request Message Check") {
    auto msg_id = static_cast<uint64_t>(random());

    /* Create a test rtt request */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>();
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_rtt_request);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_rtt_request);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_rtt_request);
    /* Check the id */
    REQUIRE(decoded_generic->getId() == msg_id);
}

TEST_CASE("RTT Response Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    auto request_id = static_cast<uint64_t>(random());

    /* Create a test rtt response */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(request_id);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_rtt_response);
    /* Check the message id */
    REQUIRE(msg->getRequestId() == request_id);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_rtt_response);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getRequestId() == request_id);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_rtt_response);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_rtt_resp = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_rtt_response>(decoded_generic);
    REQUIRE((decoded_generic_rtt_resp)->getRequestId() == request_id);
}

TEST_CASE("Position Report Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    constexpr double pos_lat = -43.5;
    constexpr double pos_lng = 172.0;
    constexpr int max_altitude = 10000;
    auto altitude = random() % max_altitude;
    auto timestamp = static_cast<uint64_t>(random());
    constexpr int icao_24bit_address = 0x00FFFFFF;
    auto icao_address = random() & icao_24bit_address;
    constexpr int circle_degrees = 360;
    auto heading = random() % circle_degrees;
    constexpr int vel_max = 1000;
    auto hor_vel = random() % vel_max;
    auto vert_vel = random() % vel_max;
    constexpr int vfr_squawk = 01200;
    constexpr int flags = 0xAA55;
    constexpr int emitter_type = 14;

    /* Create a test position report */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(pos_lat, pos_lng, altitude, heading, hor_vel, vert_vel, icao_address, "ZK-ABC", vfr_squawk, 0, flags, 1, emitter_type, timestamp);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_position_report);
    /* Check the parameters */
    REQUIRE(msg->getLatitude() == pos_lat);
    REQUIRE(msg->getLongitude() == pos_lng);
    REQUIRE(msg->getAltitude() == altitude);
    REQUIRE(msg->getTimeStamp() == timestamp);
    REQUIRE(msg->getICAOAddress() == icao_address);
    REQUIRE(msg->getHeading() == heading);
    REQUIRE(msg->getHorzVel() == hor_vel);
    REQUIRE(msg->getVertVel() == vert_vel);
    REQUIRE(msg->getCallSign() == "ZK-ABC");
    REQUIRE(msg->getSquawk() == vfr_squawk);
    REQUIRE(msg->getFlags() == flags);
    REQUIRE(msg->getAltitudeType() == 1);
    REQUIRE(msg->getEmitterType() == emitter_type);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_position_report>(msg_id, bl);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_position_report);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    constexpr double lat_lng_tolerance = 0.000001;
    REQUIRE(std::fabs(decoded->getLatitude() - pos_lat) < lat_lng_tolerance);
    REQUIRE(std::fabs(decoded->getLongitude() - pos_lng) < lat_lng_tolerance);
    REQUIRE(decoded->getAltitude() == altitude);
    REQUIRE(decoded->getTimeStamp() == timestamp);
    REQUIRE(decoded->getICAOAddress() == icao_address);
    REQUIRE(decoded->getHeading() == heading);
    REQUIRE(decoded->getHorzVel() == hor_vel);
    REQUIRE(decoded->getVertVel() == vert_vel);
    REQUIRE(decoded->getCallSign() == "ZK-ABC");
    REQUIRE(decoded->getSquawk() == vfr_squawk);
    REQUIRE(decoded->getFlags() == flags);
    REQUIRE(decoded->getAltitudeType() == 1);
    REQUIRE(decoded->getEmitterType() == emitter_type);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_position_report);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    REQUIRE(std::fabs(decoded_generic->getLatitude() - pos_lat) < lat_lng_tolerance);
    REQUIRE(std::fabs(decoded_generic->getLongitude() - pos_lng) < lat_lng_tolerance);
    REQUIRE(decoded_generic->getAltitude() == altitude);
    REQUIRE(decoded_generic->getTimeStamp() == timestamp);
}

TEST_CASE("System Status Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int max_bat_level = 100;
    auto bat_level = random() % max_bat_level;
    constexpr int max_bat_used = 5000;
    auto bat_used = random() % max_bat_used;

    /* Create a test system status */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_system_status>(bat_level, bat_used);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_system_status);
    /* Check the parameters */
    REQUIRE(msg->getBatRemaining() == bat_level);
    REQUIRE(msg->getBatMAHUsed() == bat_used);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_system_status>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getBatRemaining() == bat_level);
    REQUIRE(decoded->getBatMAHUsed() == bat_used);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_status = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_system_status>(decoded_generic);
    REQUIRE(decoded_generic_status != nullptr);
    REQUIRE(decoded_generic_status->getBatRemaining() == bat_level);
    REQUIRE(decoded_generic_status->getBatMAHUsed() == bat_used);
}


TEST_CASE("Search Status Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int max_search_values = 1000;
    auto search_id = static_cast<uint64_t>(random() % max_search_values);
    auto search_total = static_cast<uint64_t>(random() % max_search_values + 1);
    auto search_progress = static_cast<uint64_t>(random() % search_total);

    /* Create a test search status */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_search_status>(search_id, search_progress, search_total);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_search_status);
    /* Check the parameters */
    REQUIRE(msg->getSearchId() == search_id);
    REQUIRE(msg->getSearchCompleted() == search_progress);
    REQUIRE(msg->getSearchTotal() == search_total);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_search_status>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_search_status);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getSearchId() == search_id);
    REQUIRE(decoded->getSearchCompleted() == search_progress);
    REQUIRE(decoded->getSearchTotal() == search_total);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_search_status);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_search = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_search_status>(decoded_generic);
    REQUIRE(decoded_generic_search != nullptr);
    REQUIRE(decoded_generic_search->getSearchId() == search_id);
    REQUIRE(decoded_generic_search->getSearchCompleted() == search_progress);
    REQUIRE(decoded_generic_search->getSearchTotal() == search_total);
}

TEST_CASE("Asset Command Message Check - Basic") {
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(flight_safety_system::transport::asset_command_rtl, timestamp);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(msg->getTimeStamp() == timestamp);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(decoded->getTimeStamp() == timestamp);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_command = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(decoded_generic_command->getTimeStamp() == timestamp);
}

TEST_CASE("Asset Command Message Check - Position") {
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());
    constexpr double goto_lat = -43.5;
    constexpr double goto_lng = 172.0;

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(flight_safety_system::transport::asset_command_goto, timestamp, goto_lat, goto_lng);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == flight_safety_system::transport::asset_command_goto);
    REQUIRE(msg->getTimeStamp() == timestamp);
    REQUIRE(msg->getLatitude() == goto_lat);
    REQUIRE(msg->getLongitude() == goto_lng);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_goto);
    REQUIRE(decoded->getTimeStamp() ==  timestamp);
    REQUIRE(decoded->getLatitude() == goto_lat);
    REQUIRE(decoded->getLongitude() == goto_lng);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_command = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command != nullptr);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_goto);
    REQUIRE(decoded_generic->getTimeStamp() == timestamp);
    REQUIRE(decoded_generic->getLatitude() == goto_lat);
    REQUIRE(decoded_generic->getLongitude() == goto_lng);
}

TEST_CASE("Asset Command Message Check - Altitude") {
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());
    auto altitude = random();

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(flight_safety_system::transport::asset_command_altitude, timestamp, altitude);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_command);
    /* Check the parameters */
    REQUIRE(msg->getCommand() == flight_safety_system::transport::asset_command_altitude);
    REQUIRE(msg->getTimeStamp() == timestamp);
    REQUIRE(msg->getAltitude() == altitude);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_altitude);
    REQUIRE(decoded->getTimeStamp() == timestamp);
    REQUIRE(decoded->getAltitude() == altitude);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_command = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_altitude);
    REQUIRE(decoded_generic->getTimeStamp() == timestamp);
    REQUIRE(decoded_generic->getAltitude() == altitude);
}

TEST_CASE("SMM Settings Message Check") {
    auto msg_id = static_cast<uint64_t>(random());

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>("https://localhost/", "asset", "password1");
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_smm_settings);
    /* Check the parameters */
    REQUIRE(msg->getServerURL() == "https://localhost/");
    REQUIRE(msg->getUsername() == "asset");
    REQUIRE(msg->getPassword() == "password1");
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_smm_settings);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getServerURL() == "https://localhost/");
    REQUIRE(decoded->getUsername() == "asset");
    REQUIRE(decoded->getPassword() == "password1");
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_smm_settings);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_smm = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_smm_settings>(decoded_generic);
    REQUIRE(decoded_generic_smm != nullptr);
    REQUIRE(decoded_generic_smm->getServerURL() == "https://localhost/");
    REQUIRE(decoded_generic_smm->getUsername() == "asset");
    REQUIRE(decoded_generic_smm->getPassword() == "password1");
}

TEST_CASE("Server List Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int port_max = 65535;
    auto server_port = random() % port_max;

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_server_list>();
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_server_list);
    /* Check the parameters */
    REQUIRE(msg->getServers().empty());
    msg->addServer("localhost", server_port);
    auto sl = msg->getServers();
    REQUIRE(!sl.empty());
    REQUIRE(sl[0].first == "localhost");
    REQUIRE(sl[0].second == server_port);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_server_list>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_server_list);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    auto sl2 = decoded->getServers();
    REQUIRE(!sl2.empty());
    REQUIRE(sl2[0].first == "localhost");
    REQUIRE(sl2[0].second == server_port);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_server_list);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_servers = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_server_list>(decoded_generic);
    REQUIRE(decoded_generic_servers != nullptr);
    auto sl3 = decoded_generic_servers->getServers();
    REQUIRE(!sl3.empty());
    REQUIRE(sl3[0].first == "localhost");
    REQUIRE(sl3[0].second == server_port);
}

TEST_CASE("Identity (Non-Aircraft) Message Check") {
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int bit_1 = 13;
    constexpr int bit_2 = 12;
    constexpr int bit_3 = 11;

    /* Create a test identity */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_identity_non_aircraft>();
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity_non_aircraft);
    /* Checking the capabilities */
    msg->addCapability(bit_2);
    REQUIRE(msg->getCapability(bit_1) == false);
    REQUIRE(msg->getCapability(bit_2) == true);
    REQUIRE(msg->getCapability(bit_3) == false);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_identity_non_aircraft>(msg_id, bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_identity_non_aircraft);
    /* Check input == output */
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getCapability(bit_1) == false);
    REQUIRE(decoded->getCapability(bit_2) == true);
    REQUIRE(decoded->getCapability(bit_3) == false);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_identity_non_aircraft);
    REQUIRE(decoded_generic->getId() == msg_id);
}
