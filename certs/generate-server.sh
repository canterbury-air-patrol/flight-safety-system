#!/bin/bash -ex

CA_PRIVATE_PEM=ca.private.pem
CA_PUBLIC_PEM=ca.public.pem

SERVER_TMPL=${1}.tmpl
SERVER_PRIVATE_PEM=${1}.private.pem
SERVER_PUBLIC_PEM=${1}.public.pem

certtool --generate-privkey > ${SERVER_PRIVATE_PEM}

echo "" > ${SERVER_TMPL}
echo "organization = Flight Safety System" >> ${SERVER_TMPL}
echo "cn = $1" >> ${SERVER_TMPL}
echo "tls_www_server" >> ${SERVER_TMPL}
echo "encryption_key" >> ${SERVER_TMPL}
echo "signing_key" >> ${SERVER_TMPL}
echo "dns_name = $1" >> ${SERVER_TMPL}

certtool --generate-certificate --load-privkey ${SERVER_PRIVATE_PEM} \
    --load-ca-certificate ${CA_PUBLIC_PEM} --load-ca-privkey ${CA_PRIVATE_PEM} \
    --template ${SERVER_TMPL} --outfile ${SERVER_PUBLIC_PEM}