#include <fss-client-ssl.hpp>

#include <iostream>
#include <fstream>
#include <ostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

flight_safety_system::client_ssl::fss_client::fss_client(const std::string &t_fileName)
{
    /* Open the config file */
    std::ifstream configfile(t_fileName);
    if (!configfile.is_open())
    {
        std::cerr << "Failed to load configuration" << std::endl;
        return;
    }
    Json::Value config;
    configfile >> config;

    this->setAssetName(config["name"].asString());

    this->ca_file = config["ssl"]["ca_public_key"].asString();
    this->private_key_file = config["ssl"]["client_private_key"].asString();
    this->public_key_file = config["ssl"]["client_public_key"].asString();

    /* Load all the known servers from the config */
    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(this, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt(), this->ca_file, this->private_key_file, this->public_key_file);
        this->addServer(server);
    }
}

void
flight_safety_system::client_ssl::fss_client::connectTo(const std::string &t_address, uint16_t t_port, bool t_connect)
{
    auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(this, t_address, t_port, this->ca_file, this->private_key_file, this->public_key_file);
    if (t_connect)
    {
        server->reconnect();
    }
    this->addServer(server);
}

auto
flight_safety_system::client_ssl::fss_server::reconnect_to() -> bool
{
    this->setConnection(std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(this->ca_file, this->private_key_file, this->public_key_file));
    return this->getConnection()->connectTo(this->getAddress(), this->getPort());
}