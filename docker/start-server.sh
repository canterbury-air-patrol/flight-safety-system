#!/bin/bash -ex

CONFIG_FILE=/etc/fss/server.json

echo "{" > ${CONFIG_FILE}
echo "\"port\": ${LISTEN_PORT}," >> ${CONFIG_FILE}
echo "\"ssl\": {" >> ${CONFIG_FILE}
echo "\"ca_public_key\": \"/certs/ca.public.pem\"," >> ${CONFIG_FILE}
echo "\"server_private_key\": \"/certs/${NAME}.private.pem\"," >> ${CONFIG_FILE}
echo "\"server_public_key\": \"/certs/${NAME}.public.pem\"" >> ${CONFIG_FILE}
echo "}," >>  ${CONFIG_FILE}
echo "\"postgres\": {" >> ${CONFIG_FILE}
echo "\"host\": \"${DB_HOST}\"," >> ${CONFIG_FILE}
echo "\"user\": \"${DB_USER}\"," >> ${CONFIG_FILE}
echo "\"db\": \"${DB_NAME}\"," >> ${CONFIG_FILE}
echo "\"pass\": \"${DB_PASS}\"" >> ${CONFIG_FILE}
echo "}" >> ${CONFIG_FILE}
echo "}" >> ${CONFIG_FILE}

cat /etc/fss/server.json

ls /certs/

if [ ! -f "/certs/${NAME}.private.pem" ]; then
    (cd /certs/ && ./generate-server.sh $NAME)
fi

fss-server