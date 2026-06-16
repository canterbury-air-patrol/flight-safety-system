#include <stdint.h>

/* Every entry point takes the name of the ECPG connection to run on, so the
 * caller can keep telemetry writes and command/config reads on separate
 * connections (see db.cpp). */
int db_connect(const char *conn, const char *host, int port, const char *user, const char *pass, const char *db);
void db_disconnect(const char *conn);
int db_ping(const char *conn);

unsigned long long db_get_asset_id(const char *conn, const char *asset_name);
void db_rtt_create_entry(const char *conn, unsigned long long asset_id, unsigned long long delta);
void db_status_create_entry(const char *conn, unsigned long long asset_id, unsigned short bat_percent,
                            unsigned int bat_mah_used, double bat_voltage);
void db_search_status_create_entry(const char *conn, unsigned long long asset_id, unsigned long long search_id,
                                   unsigned long long search_completed, unsigned long long search_total);
void db_position_create_entry(const char *conn, unsigned long long asset_id, double latitude, double longitude,
                              int altitude);

/* Records the per-connection message id the server stamped onto the dispatched
 * command, on the assets_assetcommand row identified by its primary key
 * (command_dbid). Lets a later command-ack be matched back to this specific
 * command via dispatch_id == acked_command_id. */
void db_command_set_dispatch_id(const char *conn, unsigned long long command_dbid, unsigned long long dispatch_id);

/* Updates the ack fields on the assets_assetcommand row whose dispatch_id equals
 * the acked id. ack_state is the fss_command_ack_outcome int, ack_superseded_by
 * the fss_command_ack_reason int, ack_timestamp the FMU wall-clock ms. The
 * update never lowers an already-terminal ack_state back to a non-terminal one
 * (a late "received" cannot clobber a settled outcome). */
void db_command_record_ack(const char *conn, unsigned long long dispatch_id, int ack_state,
                           unsigned long long ack_timestamp, int ack_superseded_by);

struct asset_command_s {
    char *command;
    unsigned long long timestamp;
    unsigned long long dbid;
    /* NaN when the row's position is NULL — the dispatch layer's coordinate
     * validation then refuses to send a GOTO built from this row. */
    double latitude;
    double longitude;
    uint32_t altitude;
    /* Non-zero when the row's altitude is NULL; altitude is unsigned so it
     * has no NaN-style sentinel of its own. */
    int altitude_null;
};

struct asset_command_s *db_asset_command_get(const char *conn, unsigned long long asset_id_arg);

struct smm_settings_s {
    char *address;
    char *username;
    char *password;
};

struct smm_settings_s *db_asset_smm_settings_get(const char *conn, unsigned long long asset_id_arg);

struct fss_server_s {
    char *address;
    int port;
};

/* On return *error_out is non-zero if the cursor was cut short by a
 * mid-iteration error (the returned list is then partial and must not be
 * treated as complete); zero on a clean read. error_out may be NULL. */
struct fss_server_s **db_active_fss_servers_get(const char *conn, int *error_out);

void db_free_fss_servers(struct fss_server_s **servers);
