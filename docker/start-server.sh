#!/bin/bash -ex

CONFIG_FILE=/etc/fss/server.json

jq -n \
    --arg port "${LISTEN_PORT}" \
    --arg ca "/certs/ca.public.pem" \
    --arg priv "/certs/${NAME}.private.pem" \
    --arg pub "/certs/${NAME}.public.pem" \
    --arg host "${DB_HOST}" \
    --argjson db_port "${DB_PORT:-5432}" \
    --arg user "${DB_USER}" \
    --arg db "${DB_NAME}" \
    --arg pass "${DB_PASS}" \
    '{port: ($port|tonumber), ssl: {ca_public_key: $ca, server_private_key: $priv, server_public_key: $pub}, postgres: {host: $host, port: $db_port, user: $user, db: $db, pass: $pass}}' \
    > "${CONFIG_FILE}"

cat /etc/fss/server.json

ls /certs/

if [ ! -f "/certs/${NAME}.private.pem" ]; then
    (cd /certs/ && ./generate-server.sh $NAME)
fi

fss-server
