#!/bin/sh
# Runs as uid 10001 inside the image. /src is the read-only source mount,
# /out receives the signed SISX. No SDK files are written outside the container.
set -eu

: "${EPOCROOT:?EPOCROOT is not set}"

SRC=${SRC:-/src}
OUT=${OUT:-/out}
WORK=${WORK:-/work/build}

command -v bldmake >/dev/null
command -v abld >/dev/null
command -v makesis >/dev/null
command -v signsis >/dev/null
command -v openssl >/dev/null

[ -f "$SRC/NotiHelper.mmp" ]
[ -f "$SRC/NotiHelper.pkg" ]

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cp -a "$SRC"/. "$WORK"/
cd "$WORK"

bldmake bldfiles
abld build gcce urel

# A single-value UID in the .mmp becomes UID2 and leaves the E32 Secure ID at
# zero. S60 rejects such a package with the generic "Unable to install", so
# check the UID triple before packaging: executable, KUidApp, our SID.
EXE="$EPOCROOT/epoc32/release/gcce/urel/NotiHelper.exe"
elftran -dump h "$EXE" > /tmp/NotiHelper.e32
grep -q 'Secure ID: a0000d15' /tmp/NotiHelper.e32
grep -Eq 'Uids:[[:space:]]+1000007a 100039ce a0000d15' /tmp/NotiHelper.e32

# The .pkg references NotiHelper.exe in the current directory.
cp "$EXE" .

makesis ./NotiHelper.pkg ./NotiHelper_unsigned.sis

# Generate a fresh self-signed development certificate (RSA, no passphrase)
# if one was not supplied in the source directory. RSA rather than DSA: the
# OpenSSL 1.1 in this image cannot generate DSA parameters, and signsis accepts
# either.
#
# 1024-bit RSA with SHA-1 and no X.509 extensions, because that is what the
# device can verify: Symbian OS 9.x before v9.5 only implements MD5 and SHA-1
# (the S60 3rd hash API lists no SHA-256), so a SHA-256-signed certificate makes
# the installer reject the package as corrupt. The same goes for the extra
# basicConstraints/subjectKeyIdentifier extensions OpenSSL adds by default;
# the SDK's makekeys emits a bare certificate and the installer expects that.
#
# The validity starts in 2000 on purpose: an E51 whose RTC has reset has a
# clock years in the past, and a certificate that only becomes valid at build
# time is rejected as "not valid yet".
if [ ! -f discofi.key ] || [ ! -f discofi.cer ]; then
    cat > /tmp/req.cnf <<'EOF'
[req]
prompt = no
distinguished_name = dn
[dn]
EOF
    cat > /tmp/ca.cnf <<'EOF'
[ca]
default_ca = CA_default
[CA_default]
dir = /tmp/ca
database = $dir/index.txt
new_certs_dir = $dir/newcerts
serial = $dir/serial
default_md = sha1
policy = policy_any
[policy_any]
commonName = optional
organizationName = optional
organizationalUnitName = optional
countryName = optional
EOF
    mkdir -p /tmp/ca/newcerts
    : > /tmp/ca/index.txt
    echo 01 > /tmp/ca/serial
    openssl req -new -newkey rsa:1024 -nodes -keyout discofi.key \
        -out /tmp/ca/req.csr -config /tmp/req.cnf \
        -subj "/CN=NotiHelper/OU=Resymbian/O=Resymbian/C=ES"
    openssl ca -selfsign -batch -notext -config /tmp/ca.cnf \
        -keyfile discofi.key -in /tmp/ca/req.csr -out discofi.cer \
        -startdate 000101000000Z -enddate 491231235959Z >/dev/null
    rm -rf /tmp/ca /tmp/req.cnf /tmp/ca.cnf
fi

signsis ./NotiHelper_unsigned.sis ./NotiHelper.sisx \
    discofi.cer discofi.key ""

cp ./NotiHelper.sisx "$OUT"/NotiHelper.sisx
ls -l "$OUT"/NotiHelper.sisx
echo "Built $OUT/NotiHelper.sisx"
