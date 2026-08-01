#include <stddef.h>
#include <stdint.h>

/* Every entry point takes the name of the ECPG connection to run on, so the
 * caller can keep telemetry writes and command/config reads on separate
 * connections (see db.cpp). */
int db_connect(const char *conn, const char *host, int port, const char *user, const char *pass, const char *db);
void db_disconnect(const char *conn);
int db_ping(const char *conn);

/* Something this file's statements depend on that the database cannot serve.
 * SCHEMA_PROBLEM_MISSING (a column) and SCHEMA_PROBLEM_MISSING_EXTENSION are
 * fatal -- the statements using them can only ever fail.
 * SCHEMA_PROBLEM_TOO_WIDE means the column is declared wide enough to overflow
 * the fixed host buffer it is fetched into, so an over-long value would be
 * truncated and the row silently dropped; actual_len and buffer_len describe
 * that, and are 0 for the other two. */
#define SCHEMA_PROBLEM_MISSING 0
#define SCHEMA_PROBLEM_TOO_WIDE 1
#define SCHEMA_PROBLEM_MISSING_EXTENSION 2

/* For SCHEMA_PROBLEM_MISSING_EXTENSION, table is the catalogue the extension
 * would have been found in and column is the extension name. */
struct schema_problem_s {
    char *table;
    char *column;
    int reason;
    int actual_len;
    int buffer_len;
};

/* Checks the database carries every column the statements in server-db.pgc
 * read or write, plus the PostGIS extension they all depend on. Returns a
 * NULL-terminated array of the problems found; an empty (non-NULL) array means
 * the schema is usable. error_out (may be NULL) is set to 1 if the check itself
 * could not be completed -- a failed query, or an allocation failure that would
 * otherwise have dropped a problem from the list -- so an incomplete check is
 * never mistaken for a clean one.
 *
 * Ownership differs from db_free_asset_commands / db_free_fss_servers: the
 * caller consumes the whole list in one pass rather than taking rows out of it,
 * so db_free_schema_problems frees the entries and their strings as well as the
 * array. */
struct schema_problem_s **db_schema_check(const char *conn, int *error_out);
void db_free_schema_problems(struct schema_problem_s **problems);

/* error_out (may be NULL) is set to 1 when the query itself failed (e.g. the
 * connection is down), distinct from a 0 return for a genuinely unknown
 * asset name (todo/60) -- mirrors the db_active_fss_servers_get(..., int
 * *error_out) convention. */
unsigned long long db_get_asset_id(const char *conn, const char *asset_name, int *error_out);

/* todo/34: these six writers return 1 on success, 0 if sqlca.sqlcode < 0
 * (e.g. disk-full) -- the caller (db.cpp) throws database_error on 0 so a
 * silent per-INSERT failure isn't indistinguishable from a successful
 * write to db_write_queue's caller. */
int db_rtt_create_entry(const char *conn, unsigned long long asset_id, unsigned long long delta);
int db_status_create_entry(const char *conn, unsigned long long asset_id, unsigned short bat_percent,
                           unsigned int bat_mah_used, double bat_voltage);
int db_search_status_create_entry(const char *conn, unsigned long long asset_id, unsigned long long search_id,
                                  unsigned long long search_completed, unsigned long long search_total);
/* gps_fix_valid is the wire flags word's coords-valid bit, stored so fss-web
 * can tell a GPS-backed position from the autopilot's dead-reckoned estimate.
 * A non-finite latitude or longitude means the report carried no coordinates
 * at all (the wire no-fix sentinel) and writes NULL geometry -- the same
 * NaN<->NULL convention asset_command_s already uses in the read direction. */
int db_position_create_entry(const char *conn, unsigned long long asset_id, double latitude, double longitude,
                             int altitude, int gps_fix_valid);

/* Records the per-connection message id the server stamped onto the dispatched
 * command, on the assets_assetcommand row identified by its primary key
 * (command_dbid), and reopens the row's ack cycle. Called once per command per
 * connection, not once per resend (todo/68), so the stored id names the
 * delivery the row's ack columns describe. The ack itself is matched on the row
 * id, not on this column. */
int db_command_set_dispatch_id(const char *conn, unsigned long long command_dbid, unsigned long long dispatch_id);

/* Updates the ack fields on the assets_assetcommand row identified by its primary
 * key (command_dbid). The caller translates the wire's per-connection acked id to
 * the row it dispatched before calling (todo/68), so this names one row outright
 * and needs no asset scoping. ack_state is the fss_command_ack_outcome int,
 * ack_superseded_by the fss_command_ack_reason int, ack_timestamp the FMU
 * wall-clock ms. The update never lowers an already-terminal ack_state back to a
 * non-terminal one (a late "received" cannot clobber a settled outcome). */
int db_command_record_ack(const char *conn, unsigned long long command_dbid, int ack_state,
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

/* The asset's newest command row, or NULL when it has none. error_out (may be
 * NULL) is set to 1 when the read itself failed -- a query error or an
 * allocation failure -- so a read outage on the path that carries TERM and
 * DISARM is never returned as "no pending command" (todo/69, the
 * db_get_asset_id convention above). A row whose command string was truncated
 * is reported as absent rather than as a failure: it is named on stderr and is
 * undispatchable either way. */
struct asset_command_s *db_asset_command_get(const char *conn, unsigned long long asset_id_arg, int *error_out);

/* One newest-command row per asset, as returned by the batched read below.
 * Carries the same fields as asset_command_s plus the asset_id it belongs to,
 * so a single query can answer for many assets at once (todo/23 phase 1). */
struct asset_command_row_s {
    unsigned long long asset_id;
    char *command;
    unsigned long long timestamp;
    unsigned long long dbid;
    /* NaN when the row's position is NULL (see asset_command_s). */
    double latitude;
    double longitude;
    uint32_t altitude;
    /* Non-zero when the row's altitude is NULL. */
    int altitude_null;
};

/* Batched replacement for calling db_asset_command_get once per asset: returns
 * the newest command row for each id in asset_ids in a single round-trip, as a
 * NULL-terminated array. Assets with no command row -- or whose command string
 * was truncated -- are simply absent from the result (same per-row drop as the
 * single-row read). error_out (may be NULL) is set to 1 on a mid-cursor read
 * error, mirroring db_active_fss_servers_get; the accumulated rows are then a
 * partial set. Returns NULL only on allocation failure. count == 0 returns an
 * empty list without querying.
 *
 * Mapping: rows are NOT positionally aligned with asset_ids -- the result is
 * ordered by asset_id and omits assets with no command, so a caller must key on
 * each row's own asset_id field, never on the input index.
 *
 * Ownership: the caller owns each row and its command string. Free every
 * commands[i]->command and every commands[i], then pass the array to
 * db_free_asset_commands, which frees only the array itself (not the rows) --
 * the same split ownership db_active_fss_servers_get / db_free_fss_servers use. */
struct asset_command_row_s **db_asset_commands_get(const char *conn, const unsigned long long *asset_ids, size_t count,
                                                   int *error_out);
/* Frees the array returned by db_asset_commands_get. Frees ONLY the array, not
 * the rows or their command strings -- free those first (see Ownership above). */
void db_free_asset_commands(struct asset_command_row_s **commands);

struct smm_settings_s {
    char *address;
    char *username;
    char *password;
};

/* The asset's SMM settings, or NULL when it has none configured. error_out (may
 * be NULL) is set to 1 when the read itself failed -- a query error or an
 * allocation failure -- so the caller can keep its cached settings instead of
 * overwriting them with a failure (todo/69). Truncated credentials are reported
 * as absent rather than as a failure, as in db_asset_command_get. */
struct smm_settings_s *db_asset_smm_settings_get(const char *conn, unsigned long long asset_id_arg, int *error_out);

struct fss_server_s {
    char *address;
    int port;
};

/* On return *error_out is non-zero if the cursor was cut short by a
 * mid-iteration error (the returned list is then partial and must not be
 * treated as complete); zero on a clean read. error_out may be NULL. */
struct fss_server_s **db_active_fss_servers_get(const char *conn, int *error_out);

void db_free_fss_servers(struct fss_server_s **servers);
