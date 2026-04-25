#!/bin/bash -e

# Usage: revoke-client.sh <client-name>
# Revokes the named client certificate and regenerates the CRL.
# The updated CRL is written to crl.pem in the current directory.
# New connections will use the updated CRL automatically.

if [ -z "$1" ]; then
    echo "Usage: $0 <client-name>" >&2
    exit 1
fi

CA_PUBLIC_PEM=ca.public.pem
CA_PRIVATE_PEM=ca.private.pem
CLIENT_PUBLIC_PEM=${1}.public.pem
CRL_FILE=crl.pem
REVOKED_DIR=revoked

if [ ! -f "${CA_PUBLIC_PEM}" ]; then
    echo "Error: CA certificate not found: ${CA_PUBLIC_PEM}" >&2
    exit 1
fi

if [ ! -f "${CA_PRIVATE_PEM}" ]; then
    echo "Error: CA private key not found: ${CA_PRIVATE_PEM}" >&2
    exit 1
fi

if [ ! -f "${CLIENT_PUBLIC_PEM}" ]; then
    echo "Error: Certificate not found: ${CLIENT_PUBLIC_PEM}" >&2
    exit 1
fi

mkdir -p "${REVOKED_DIR}"
cp "${CLIENT_PUBLIC_PEM}" "${REVOKED_DIR}/${1}.public.pem"

LOAD_CERT_ARGS=()
for cert in "${REVOKED_DIR}"/*.public.pem; do
    LOAD_CERT_ARGS+=("--load-certificate" "${cert}")
done

certtool --generate-crl \
    --load-ca-privkey "${CA_PRIVATE_PEM}" \
    --load-ca-certificate "${CA_PUBLIC_PEM}" \
    "${LOAD_CERT_ARGS[@]}" \
    --outfile "${CRL_FILE}"

echo "CRL updated: ${CRL_FILE}"
echo "New connections will use the updated CRL automatically."
