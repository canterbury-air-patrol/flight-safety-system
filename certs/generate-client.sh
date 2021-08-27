#!/bin/bash -ex

CA_PUBLIC_PEM=ca.public.pem
CA_PRIVATE_PEM=ca.private.pem

CLIENT_TMPL=client.tmpl
CLIENT_PRIVATE_PEM=client.private.pem
CLIENT_PUBLIC_PEM=client.public.pem

certtool --generate-privkey > ${CLIENT_PRIVATE_PEM}

echo "" > ${CLIENT_TMPL}
echo "organization = Flight Safety System" >> ${CLIENT_TMPL}
echo "cn = $1" >> ${CLIENT_TMPL}
echo "tls_www_client" >> ${CLIENT_TMPL}
echo "encryption_key" >> ${CLIENT_TMPL}
echo "signing_key" >> ${CLIENT_TMPL}
echo "dns_name = $1" >> ${CLIENT_TMPL}

certtool --generate-certificate --load-privkey ${CLIENT_PRIVATE_PEM} \
    --load-ca-certificate ${CA_PUBLIC_PEM} --load-ca-privkey ${CA_PRIVATE_PEM} \
    --template ${CLIENT_TMPL} --outfile ${CLIENT_PUBLIC_PEM}