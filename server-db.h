void db_connect(const char *host, const char *user, const char *pass, const char *db);
void db_disconnect();

unsigned long long db_get_asset_id(const char *asset_name);
void db_rtt_create_entry(unsigned long long asset_id, unsigned long long delta);
void db_status_create_entry(unsigned long long asset_id, unsigned short bat_percent, unsigned int bat_mah_used);
void db_search_status_create_entry(unsigned long long asset_id, unsigned long long search_id, unsigned long long search_completed, unsigned long long search_total);
void db_position_create_entry(unsigned long long asset_id, double latitude, double longitude, int altitude);

struct smm_settings_s {
    char *address;
    char *username;
    char *password;
};

struct smm_settings_s *
db_asset_smm_settings_get(unsigned long long asset_id_arg);
