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

# Without --template, certtool prompts interactively for "next CRL update in
# (days)"; run non-interactively (as any script or test harness invoking this
# does) and it hangs forever reading EOF from a closed stdin. crl_number must
# increase on every regeneration or some TLS stacks treat the new CRL as
# stale; a Unix timestamp is a simple monotonic source across repeated runs.
CRL_TMPL=$(mktemp)
ALL_REVOKED_PEM=$(mktemp)
trap 'rm -f "${CRL_TMPL}" "${ALL_REVOKED_PEM}"' EXIT
printf 'crl_next_update = 3650\ncrl_number = %s\n' "$(date +%s)" > "${CRL_TMPL}"

# certtool --generate-crl does not accumulate repeated --load-certificate
# flags: only the last one lands in the CRL (observed with certtool 3.8.13),
# so one flag per revoked cert silently un-revoked everything but the newest
# revocation (todo/63). Concatenate every revoked cert into one PEM and pass
# it once — that form is honoured on every certtool version.
cat "${REVOKED_DIR}"/*.public.pem > "${ALL_REVOKED_PEM}"

certtool --generate-crl \
    --load-ca-privkey "${CA_PRIVATE_PEM}" \
    --load-ca-certificate "${CA_PUBLIC_PEM}" \
    --load-certificate "${ALL_REVOKED_PEM}" \
    --template "${CRL_TMPL}" \
    --outfile "${CRL_FILE}"

echo "CRL updated: ${CRL_FILE}"
echo "New connections will use the updated CRL automatically."
