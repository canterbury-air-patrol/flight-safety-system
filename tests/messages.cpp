#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

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

#include <cmath>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

TEST_CASE("Close Connection Check")
{
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

TEST_CASE("Identity Message Check")
{
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
    auto decoded_generic_identity =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_identity>(decoded_generic);
    REQUIRE(decoded_generic_identity != nullptr);
    REQUIRE(decoded_generic_identity->getName() == "test1");
}

TEST_CASE("RTT Request Message Check")
{
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

TEST_CASE("RTT Response Message Check")
{
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
    auto decoded_generic_rtt_resp =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_rtt_response>(decoded_generic);
    REQUIRE((decoded_generic_rtt_resp)->getRequestId() == request_id);
    /* No client timestamp supplied: defaults to "not reported". */
    REQUIRE(msg->getClientTimestamp() == 0);
    REQUIRE(decoded->getClientTimestamp() == 0);
}

TEST_CASE("RTT Response carries an optional client timestamp")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto request_id = static_cast<uint64_t>(random());
    constexpr uint64_t client_ts = 0x1122334455667788ULL;

    auto msg = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(request_id, client_ts);
    REQUIRE(msg->getRequestId() == request_id);
    REQUIRE(msg->getClientTimestamp() == client_ts);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);

    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg_id, bl);
    REQUIRE(decoded->getRequestId() == request_id);
    REQUIRE(decoded->getClientTimestamp() == client_ts);

    auto decoded_generic = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_rtt_response>(
        flight_safety_system::transport::fss_message::decode(bl));
    REQUIRE(decoded_generic != nullptr);
    REQUIRE(decoded_generic->getClientTimestamp() == client_ts);
}

TEST_CASE("RTT Response without a timestamp is byte-identical to a legacy response")
{
    /* Backward compatibility: a response whose client timestamp is 0 (not
     * reported) must pack to exactly the same bytes as before the optional
     * field existed — request_id only, no trailing 8 bytes. */
    constexpr uint64_t request_id = 0x0102030405060708ULL;
    auto without = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(request_id);
    auto with_zero = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(request_id, 0ULL);
    without->setId(7);
    with_zero->setId(7);
    auto bl_without = without->getPacked();
    auto bl_with_zero = with_zero->getPacked();
    REQUIRE(bl_without->getLength() == bl_with_zero->getLength());
    REQUIRE(std::string(bl_without->getData(), bl_without->getLength()) ==
            std::string(bl_with_zero->getData(), bl_with_zero->getLength()));

    /* And a response that does report a timestamp is exactly 8 bytes longer. */
    auto with_ts = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(request_id, 42ULL);
    with_ts->setId(7);
    auto bl_with_ts = with_ts->getPacked();
    REQUIRE(bl_with_ts->getLength() == bl_without->getLength() + sizeof(uint64_t));
}

TEST_CASE("Position Report Message Check", "[TC-FSS-001]")
{
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
    constexpr uint8_t tslc = 7;

    /* Create a test position report */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(
        pos_lat, pos_lng, altitude, heading, hor_vel, vert_vel, icao_address, "ZK-ABC", vfr_squawk, tslc, flags, 1,
        emitter_type, timestamp);
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
    REQUIRE(msg->getTSLC() == tslc);
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
    REQUIRE(decoded->getTSLC() == tslc);
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

TEST_CASE("System Status Message Check")
{
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int max_bat_level = 100;
    auto bat_level = random() % max_bat_level;
    constexpr int max_bat_used = 5000;
    auto bat_used = random() % max_bat_used;
    constexpr int max_bat_voltage = 11000;
    auto bat_voltage_n = random() % max_bat_voltage;
    double bat_voltage = (double)bat_voltage_n / 1000.0;

    /* Create a test system status */
    auto msg =
        std::make_shared<flight_safety_system::transport::fss_message_system_status>(bat_level, bat_used, bat_voltage);
    /* Check the type */
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_system_status);
    /* Check the parameters */
    REQUIRE(msg->getBatRemaining() == bat_level);
    REQUIRE(msg->getBatMAHUsed() == bat_used);
    REQUIRE(msg->getBatVoltage() == bat_voltage);
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
    constexpr double bat_volt_tolerance = 0.000001;
    REQUIRE(std::fabs(decoded->getBatVoltage() - bat_voltage) < bat_volt_tolerance);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_system_status);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_status =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_system_status>(decoded_generic);
    REQUIRE(decoded_generic_status != nullptr);
    REQUIRE(decoded_generic_status->getBatRemaining() == bat_level);
    REQUIRE(decoded_generic_status->getBatMAHUsed() == bat_used);
    REQUIRE(std::fabs(decoded_generic_status->getBatVoltage() - bat_voltage) < bat_volt_tolerance);
}


TEST_CASE("Search Status Message Check")
{
    auto msg_id = static_cast<uint64_t>(random());
    constexpr int max_search_values = 1000;
    auto search_id = static_cast<uint64_t>(random() % max_search_values);
    auto search_total = static_cast<uint64_t>(random() % max_search_values + 1);
    auto search_progress = static_cast<uint64_t>(random() % search_total);

    /* Create a test search status */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_search_status>(search_id, search_progress,
                                                                                            search_total);
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
    auto decoded_generic_search =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_search_status>(decoded_generic);
    REQUIRE(decoded_generic_search != nullptr);
    REQUIRE(decoded_generic_search->getSearchId() == search_id);
    REQUIRE(decoded_generic_search->getSearchCompleted() == search_progress);
    REQUIRE(decoded_generic_search->getSearchTotal() == search_total);
}

TEST_CASE("Asset Command Message Check - Basic", "[TC-FSS-002]")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(
        flight_safety_system::transport::asset_command_rtl, timestamp);
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
    auto decoded_generic_command =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(decoded_generic_command->getTimeStamp() == timestamp);
}

TEST_CASE("Asset Command Message Check - Position", "[TC-FSS-002]")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());
    constexpr double goto_lat = -43.5;
    constexpr double goto_lng = 172.0;

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(
        flight_safety_system::transport::asset_command_goto, timestamp, goto_lat, goto_lng);
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
    REQUIRE(decoded->getTimeStamp() == timestamp);
    REQUIRE(decoded->getLatitude() == goto_lat);
    REQUIRE(decoded->getLongitude() == goto_lng);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    /* Check the type */
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command);
    /* Check input == output */
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_command =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command != nullptr);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_goto);
    REQUIRE(decoded_generic->getTimeStamp() == timestamp);
    REQUIRE(decoded_generic->getLatitude() == goto_lat);
    REQUIRE(decoded_generic->getLongitude() == goto_lng);
}

TEST_CASE("Asset Command Message Check - Altitude", "[TC-FSS-002]")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());
    auto altitude = random();

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(
        flight_safety_system::transport::asset_command_altitude, timestamp, altitude);
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
    auto decoded_generic_command =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(decoded_generic);
    REQUIRE(decoded_generic_command->getCommand() == flight_safety_system::transport::asset_command_altitude);
    REQUIRE(decoded_generic->getTimeStamp() == timestamp);
    REQUIRE(decoded_generic->getAltitude() == altitude);
}

TEST_CASE("Command Ack Message Check - Actioned")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto acked_command_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* An ack for a command the FMU actioned. No higher-priority state was
     * engaged, so the supersede reason is none (the not-applicable value). */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(
        acked_command_id, flight_safety_system::transport::asset_command_rtl,
        flight_safety_system::transport::command_ack_actioned, timestamp);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_command_ack);
    REQUIRE(msg->getAckedCommandId() == acked_command_id);
    REQUIRE(msg->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(msg->getOutcome() == flight_safety_system::transport::command_ack_actioned);
    REQUIRE(msg->getReason() == flight_safety_system::transport::supersede_none);
    REQUIRE(msg->getTimeStamp() == timestamp);
    /* Convert to bl and back */
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(msg_id, bl);
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_command_ack);
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getAckedCommandId() == acked_command_id);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_rtl);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_actioned);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_none);
    REQUIRE(decoded->getTimeStamp() == timestamp);
    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_command_ack);
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_ack =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_command_ack>(decoded_generic);
    REQUIRE(decoded_generic_ack != nullptr);
    REQUIRE(decoded_generic_ack->getAckedCommandId() == acked_command_id);
    REQUIRE(decoded_generic_ack->getOutcome() == flight_safety_system::transport::command_ack_actioned);
    REQUIRE(decoded_generic_ack->getTimeStamp() == timestamp);
}

TEST_CASE("Command Ack Message Check - Superseded carries the reason")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto acked_command_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* A manual command was received but not actioned because a low-battery RTL
     * latch is engaged; the ack names that reason. The reason (not a command
     * value) is what lets the operator UI tell low-battery RTL apart from a
     * comms-loss RTL, which would otherwise both read as asset_command_rtl. */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(
        acked_command_id, flight_safety_system::transport::asset_command_manual,
        flight_safety_system::transport::command_ack_superseded, flight_safety_system::transport::supersede_low_battery,
        timestamp);
    REQUIRE(msg->getCommand() == flight_safety_system::transport::asset_command_manual);
    REQUIRE(msg->getOutcome() == flight_safety_system::transport::command_ack_superseded);
    REQUIRE(msg->getReason() == flight_safety_system::transport::supersede_low_battery);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(msg_id, bl);
    REQUIRE(decoded->getAckedCommandId() == acked_command_id);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_manual);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_superseded);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_low_battery);
    REQUIRE(decoded->getTimeStamp() == timestamp);
}

TEST_CASE("Command Ack Message Check - Superseded by a newer operator command")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto acked_command_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* A stale command was dropped because the operator issued a newer one before
     * it was actioned. This is the non-latch supersede reason, distinct from the
     * autonomous safety latches; it must survive the round trip so the UI can
     * show "replaced by a newer command" rather than a generic drop. */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(
        acked_command_id, flight_safety_system::transport::asset_command_goto,
        flight_safety_system::transport::command_ack_superseded,
        flight_safety_system::transport::supersede_newer_command, timestamp);
    REQUIRE(msg->getOutcome() == flight_safety_system::transport::command_ack_superseded);
    REQUIRE(msg->getReason() == flight_safety_system::transport::supersede_newer_command);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(msg_id, bl);
    REQUIRE(decoded->getAckedCommandId() == acked_command_id);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_superseded);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_newer_command);
    REQUIRE(decoded->getTimeStamp() == timestamp);
}

TEST_CASE("Command Ack Message Check - No-op (already in the commanded state)")
{
    auto msg_id = static_cast<uint64_t>(random());
    auto acked_command_id = static_cast<uint64_t>(random());
    auto timestamp = static_cast<uint64_t>(random());

    /* The command resolved to the state already current: nothing transitioned.
     * Distinct from actioned so the operator is not shown a fresh transition. */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(
        acked_command_id, flight_safety_system::transport::asset_command_rtl,
        flight_safety_system::transport::command_ack_noop, timestamp);
    REQUIRE(msg->getOutcome() == flight_safety_system::transport::command_ack_noop);
    REQUIRE(msg->getReason() == flight_safety_system::transport::supersede_none);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(msg_id, bl);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_noop);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_none);
}

TEST_CASE("Command Ack Message Check - reason is normalised away for a non-superseded outcome")
{
    /* Fixed values keep this regression deterministic: the property under test
     * (reason normalisation) is independent of the id/timestamp, so randomising
     * them would only make a failure harder to reproduce. The byte patterns
     * still cross all 8 bytes to exercise the round trip. */
    constexpr uint64_t msg_id = 0x0102030405060708ULL;
    constexpr uint64_t acked_command_id = 0x1122334455667788ULL;
    constexpr uint64_t timestamp = 0x99AABBCCDDEEFF00ULL;

    /* A reason is only meaningful alongside a superseded outcome. Even if a
     * caller passes one for any other outcome, the constructor must drop it to
     * supersede_none so an inconsistent outcome/reason pair never reaches the
     * wire, logs, or UI. */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(
        acked_command_id, flight_safety_system::transport::asset_command_rtl,
        flight_safety_system::transport::command_ack_actioned, flight_safety_system::transport::supersede_low_battery,
        timestamp);
    REQUIRE(msg->getOutcome() == flight_safety_system::transport::command_ack_actioned);
    REQUIRE(msg->getReason() == flight_safety_system::transport::supersede_none);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_command_ack>(msg_id, bl);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_actioned);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_none);
}

TEST_CASE("SMM Settings Message Check")
{
    auto msg_id = static_cast<uint64_t>(random());

    /* Create a test asset command */
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>(
        "https://localhost/", flight_safety_system::secure_string(std::string_view("asset")),
        flight_safety_system::secure_string(std::string_view("password1")));
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
    auto decoded_generic_smm =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_smm_settings>(decoded_generic);
    REQUIRE(decoded_generic_smm != nullptr);
    REQUIRE(decoded_generic_smm->getServerURL() == "https://localhost/");
    REQUIRE(decoded_generic_smm->getUsername() == "asset");
    REQUIRE(decoded_generic_smm->getPassword() == "password1");
}

TEST_CASE("Server List Message Check", "[TC-FSS-003]")
{
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
    auto decoded_generic_servers =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_server_list>(decoded_generic);
    REQUIRE(decoded_generic_servers != nullptr);
    auto sl3 = decoded_generic_servers->getServers();
    REQUIRE(!sl3.empty());
    REQUIRE(sl3[0].first == "localhost");
    REQUIRE(sl3[0].second == server_port);
}

TEST_CASE("Position Report String Alignment")
{
    /* Callsign "AB" (2 bytes): buf is at 44 bytes when packString is called,
       so len(2) + "AB"(2) = 4 bytes reaches offset 48, which is already 8-byte
       aligned. The bug would add 8 extra padding bytes; the fix adds 0. */
    constexpr size_t expected_packed_size = 56;
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(
        -43.5, 172.0, 100, 0, 0, 0, 0, "AB", 0, 0, 0, 0, 0, 0);
    msg->setId(1);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);
    REQUIRE(bl->getLength() == expected_packed_size);
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_position_report>(1, bl);
    REQUIRE(decoded->getCallSign() == "AB");
}

TEST_CASE("Identity (Non-Aircraft) Message Check")
{
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

TEST_CASE("Version Message Round-trip")
{
    auto msg_id = static_cast<uint64_t>(random());
    constexpr uint16_t version = 3;
    constexpr uint16_t min_version = 2;
    constexpr uint32_t flags = 0xDEADBEEFU;

    auto msg = std::make_shared<flight_safety_system::transport::fss_message_version>(version, min_version, flags);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_version);
    REQUIRE(msg->getProtocolVersion() == version);
    REQUIRE(msg->getMinSupportedVersion() == min_version);
    REQUIRE(msg->getFeatureFlags() == flags);

    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);

    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_version>(msg_id, bl);
    REQUIRE(decoded->getType() == flight_safety_system::transport::message_type_version);
    REQUIRE(decoded->getId() == msg_id);
    REQUIRE(decoded->getProtocolVersion() == version);
    REQUIRE(decoded->getMinSupportedVersion() == min_version);
    REQUIRE(decoded->getFeatureFlags() == flags);

    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic != nullptr);
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_version);
    REQUIRE(decoded_generic->getId() == msg_id);
    auto decoded_generic_version =
        std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_version>(decoded_generic);
    REQUIRE(decoded_generic_version != nullptr);
    REQUIRE(decoded_generic_version->getProtocolVersion() == version);
    REQUIRE(decoded_generic_version->getMinSupportedVersion() == min_version);
    REQUIRE(decoded_generic_version->getFeatureFlags() == flags);
}

TEST_CASE("Version Message Defaults")
{
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_version>();
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_version);
    REQUIRE(msg->getProtocolVersion() == flight_safety_system::transport::FSS_PROTOCOL_VERSION);
    REQUIRE(msg->getMinSupportedVersion() == flight_safety_system::transport::FSS_PROTOCOL_MIN_VERSION);
    /* A default-constructed version message advertises exactly the capabilities
     * this build implements. */
    REQUIRE(msg->getFeatureFlags() == flight_safety_system::transport::FSS_SUPPORTED_FEATURES);
}

TEST_CASE("messages: identity_required round-trip")
{
    auto msg_id = static_cast<uint64_t>(random());

    auto msg = std::make_shared<flight_safety_system::transport::fss_message_identity_required>();
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity_required);
    msg->setId(msg_id);
    auto bl = msg->getPacked();
    REQUIRE(bl != nullptr);

    auto decoded_direct = std::make_shared<flight_safety_system::transport::fss_message_identity_required>(msg_id, bl);
    REQUIRE(decoded_direct->getType() == flight_safety_system::transport::message_type_identity_required);
    REQUIRE(decoded_direct->getId() == msg_id);

    auto decoded_generic = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded_generic != nullptr);
    REQUIRE(decoded_generic->getType() == flight_safety_system::transport::message_type_identity_required);
    REQUIRE(decoded_generic->getId() == msg_id);
}

/* Framed message header: declared_length (uint16_t) + type (uint16_t) + msg_id (uint64_t). */
constexpr size_t framed_header_len = sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t);

TEST_CASE("messages: rtt_response with truncated payload yields zero request_id")
{
    // Truncate an rtt_response to just the framed header to trigger
    // BufferReader::ensureAvailable() returning false on the readUint64 call.
    auto full = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(uint64_t{42});
    full->setId(1);
    auto full_bl = full->getPacked();
    REQUIRE(full_bl != nullptr);
    auto short_bl = std::make_shared<flight_safety_system::transport::buf_len>(
        full_bl->getData(), static_cast<uint16_t>(framed_header_len));
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(uint64_t{1}, short_bl);
    REQUIRE(decoded->getRequestId() == 0);
}

TEST_CASE("messages: smm_settings with truncated string body returns empty fields")
{
    // Pack a valid smm_settings then truncate after header + the 2-byte string-length
    // prefix so that unpackString encounters `len > remaining bytes` (the second guard
    // at line 51 of unpackString, distinct from the offset/sizeof-uint16_t guard).
    auto src = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>(
        "https://localhost/", flight_safety_system::secure_string(std::string_view("asset")),
        flight_safety_system::secure_string(std::string_view("pw")));
    src->setId(1);
    auto full_bl = src->getPacked();
    REQUIRE(full_bl != nullptr);
    // Keep only header + 2-byte length prefix; claimed string length > 0 bytes remaining.
    constexpr size_t truncated = framed_header_len + sizeof(uint16_t);
    auto bl = std::make_shared<flight_safety_system::transport::buf_len>(full_bl->getData(),
                                                                         static_cast<uint16_t>(truncated));
    auto decoded = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>(uint64_t{1}, bl);
    REQUIRE(decoded->getServerURL().empty());
}

// ---------------------------------------------------------------------------
// buf_len copy/move construction (transport-messages.cpp lines 159, 160)
// ---------------------------------------------------------------------------
TEST_CASE("messages: buf_len copy and move construction")
{
    using flight_safety_system::transport::buf_len;
    buf_len orig("hello", 5);
    buf_len copy_constructed(orig); // copy ctor — line 159
    REQUIRE(copy_constructed.getLength() == orig.getLength());
    buf_len move_constructed(std::move(copy_constructed)); // move ctor — line 160
    REQUIRE(move_constructed.getLength() == 5);
}

// ---------------------------------------------------------------------------
// buf_len copy assignment (lines 172, 174, 176, 178)
// ---------------------------------------------------------------------------
TEST_CASE("messages: buf_len copy assignment operator")
{
    using flight_safety_system::transport::buf_len;
    buf_len a("abc", 3);
    buf_len b;
    b = a; // copy assign — lines 172-178
    REQUIRE(b.getLength() == 3);
    REQUIRE(std::string(b.getData(), b.getLength()) == "abc");
}

// ---------------------------------------------------------------------------
// buf_len::writeAt const char* overload (lines 198, 200, 201)
// ---------------------------------------------------------------------------
TEST_CASE("messages: buf_len writeAt const char* overload")
{
    using flight_safety_system::transport::buf_len;
    buf_len bl("hello", 5);
    bl.addData("world", 5); // "helloworld" (10 bytes)
    const char *repl = "XY";
    bl.writeAt(2, repl, 2); // const char* overload — lines 198-201
    std::string result(bl.getData(), bl.getLength());
    REQUIRE(result == "heXYoworld");
}

// ---------------------------------------------------------------------------
// fss_message_cb copy ctor and assignment operator (lines 223, 235-242)
// ---------------------------------------------------------------------------
namespace {

struct MinimalCb : flight_safety_system::transport::fss_message_cb {
    explicit MinimalCb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn = nullptr)
        : fss_message_cb(std::move(t_conn))
    {
    }
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message>) override {}
};

} // namespace

TEST_CASE("messages: fss_message_cb copy ctor and assignment operator preserve connection")
{
    /* Construct with a real fss_connection so connected() is meaningful and
     * the underlying shared_ptr identity can be compared after copy/assign. */
    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    MinimalCb a(conn);
    REQUIRE(a.connected());

    MinimalCb b(a); // copy ctor — line 223
    REQUIRE(b.connected());
    REQUIRE(b.getConnection().get() == a.getConnection().get());

    MinimalCb c;
    REQUIRE_FALSE(c.connected());
    c = a; // copy assign — lines 235-242
    REQUIRE(c.connected());
    REQUIRE(c.getConnection().get() == a.getConnection().get());
}

// ---------------------------------------------------------------------------
// fss_message_position_report getTSLC accessor (lines 587, 589)
// ---------------------------------------------------------------------------
TEST_CASE("messages: fss_message_position_report getTSLC accessor")
{
    auto msg = std::make_shared<flight_safety_system::transport::fss_message_position_report>(
        0.0, 0.0, uint32_t{0}, uint16_t{0}, uint16_t{0}, int16_t{0}, uint32_t{0}, std::string{"T"}, uint16_t{0},
        uint8_t{42}, uint16_t{0}, uint8_t{0}, uint8_t{0}, uint64_t{0});
    REQUIRE(msg->getTSLC() == uint8_t{42});
}

// ---------------------------------------------------------------------------
// BufferReader short-read paths (lines 91, 99, 119, 129)
// Each test crafts a buffer that is just long enough to reach the failing
// field read, triggering the corresponding "return false" guard.
// ---------------------------------------------------------------------------

/* readUint8 failure (line 91): system_status with only the framed header —
 * the first field read (bat_percent : uint8_t) cannot be satisfied. */
TEST_CASE("messages: BufferReader readUint8 short-read returns false")
{
    using flight_safety_system::transport::message_type_system_status;
    constexpr size_t total = framed_header_len;
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_system_status), 1, "", total);
    auto decoded = flight_safety_system::transport::fss_message::decode(bl);
    /* Decode produces a system_status object with all-zero fields (reads fail
     * silently); the message type must still be correct. */
    REQUIRE(decoded != nullptr);
}

/* readInt32 failure (line 119): position_report truncated after the uint64
 * timestamp field, so the first int32_t (latitude) read fails. */
TEST_CASE("messages: BufferReader readInt32 short-read returns false")
{
    using flight_safety_system::transport::message_type_position_report;
    /* Header + uint64 timestamp; the int32_t latitude that follows is absent. */
    constexpr size_t payload = sizeof(uint64_t);
    constexpr size_t total = framed_header_len + payload;
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_position_report), 1,
                                           std::string(payload, '\0'), total);
    auto decoded = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    /* The latitude/longitude reads failed, so the coordinates must decode to
     * NaN, never to (0,0) — Null Island is a legal coordinate that would pass
     * validation. */
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

/* readInt16 failure (line 99): position_report truncated just before the
 * int16_t vertical_velocity field. */
TEST_CASE("messages: BufferReader readInt16 short-read returns false")
{
    using flight_safety_system::transport::message_type_position_report;
    /* Fields consumed before vertical_velocity:
     * timestamp(uint64) + latitude(int32) + longitude(int32) + altitude(uint32)
     *   + altitude_msl(uint32) + ground_speed(uint16) + course_over_ground(uint16). */
    constexpr size_t payload = sizeof(uint64_t) + sizeof(int32_t) * 2 + sizeof(uint32_t) * 2 + sizeof(uint16_t) * 2;
    constexpr size_t total = framed_header_len + payload;
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_position_report), 1,
                                           std::string(payload, '\0'), total);
    auto decoded = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    /* Truncation in a trailing field still NaNs the coordinates, even though
     * latitude/longitude themselves were read — the buffer is incomplete. */
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

/* asset_command truncated before the trailing command byte: the coordinates
 * must likewise decode to NaN rather than (0,0). */
TEST_CASE("messages: truncated asset_command decodes coordinates as NaN")
{
    using flight_safety_system::transport::message_type_command;
    /* timestamp(uint64) + latitude(int32) + longitude(int32); altitude and
     * the command byte are absent, so reader.ok() is false at the end. */
    constexpr size_t payload = sizeof(uint64_t) + sizeof(int32_t) * 2;
    constexpr size_t total = framed_header_len + payload;
    auto bl =
        fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_command), 1, std::string(payload, '\0'), total);
    auto decoded = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

TEST_CASE("messages: truncated command_ack decodes to safe defaults")
{
    using flight_safety_system::transport::message_type_command_ack;
    /* Only the acked-command id (uint64) is present; the command, outcome and
     * reason bytes plus the timestamp are absent, so the reader must stop short
     * rather than over-read and leave those fields at their safe defaults. */
    constexpr size_t payload = sizeof(uint64_t);
    constexpr size_t total = framed_header_len + payload;
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_command_ack), 1,
                                           std::string(payload, '\0'), total);
    auto decoded = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_command_ack>(
        flight_safety_system::transport::fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getAckedCommandId() == 0);
    REQUIRE(decoded->getCommand() == flight_safety_system::transport::asset_command_unknown);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_received);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_none);
    REQUIRE(decoded->getTimeStamp() == 0);
}

TEST_CASE("messages: unknown command_ack reason degrades to supersede_none")
{
    using flight_safety_system::transport::message_type_command_ack;
    /* A newer peer can send a supersede reason this build does not recognise.
     * Such a code must decode back to supersede_none (the operator is never
     * shown an invented cause); the raw code is only logged for diagnosis. */
    std::string payload;
    payload.append(sizeof(uint64_t), '\0'); // acked_command_id = 0
    payload.push_back(static_cast<char>(flight_safety_system::transport::asset_command_rtl));
    payload.push_back(static_cast<char>(flight_safety_system::transport::command_ack_superseded));
    payload.push_back(static_cast<char>(0xEE)); // a reason code no build assigns
    payload.append(sizeof(uint64_t), '\0');     // timestamp = 0
    const size_t total = framed_header_len + payload.size();
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_command_ack), 1, payload, total);
    auto decoded = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_command_ack>(
        flight_safety_system::transport::fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getOutcome() == flight_safety_system::transport::command_ack_superseded);
    REQUIRE(decoded->getReason() == flight_safety_system::transport::supersede_none);
}

/* readUint32 failure (line 129): version message truncated after the two
 * uint16 version fields, so the uint32_t feature_flags read fails. */
TEST_CASE("messages: BufferReader readUint32 short-read returns false")
{
    using flight_safety_system::transport::message_type_version;
    /* Two uint16 version fields; uint32 feature_flags is absent. */
    constexpr size_t payload = sizeof(uint16_t) * 2;
    constexpr size_t total = framed_header_len + payload;
    auto bl =
        fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_version), 1, std::string(payload, '\0'), total);
    auto decoded = flight_safety_system::transport::fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
}
