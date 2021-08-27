#!/bin/bash -ex

CA_TMPL=ca.tmpl
CA_PRIVATE_PEM=ca.private.pem
CA_PUBLIC_PEM=ca.public.pem

certtool --generate-privkey > ${CA_PRIVATE_PEM}

echo "" > ${CA_TMPL}
echo "cn = Flight Safety System CA" >> ${CA_TMPL}
echo "ca" >> ${CA_TMPL}
echo "cert_signing_key" >> ${CA_TMPL}

certtool --generate-self-signed --load-privkey ${CA_PRIVATE_PEM} --template ${CA_TMPL} --outfile ${CA_PUBLIC_PEM}