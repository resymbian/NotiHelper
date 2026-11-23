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

# The .pkg references NotiHelper.exe in the current directory.
cp "$EPOCROOT/epoc32/release/gcce/urel/NotiHelper.exe" .

makesis ./NotiHelper.pkg ./NotiHelper_unsigned.sis

# Generate a fresh self-signed development certificate (RSA, no passphrase)
# if one was not supplied in the source directory. RSA rather than DSA: the
# OpenSSL 1.1 in this image cannot generate DSA parameters, and signsis accepts
# either.
if [ ! -f discofi.key ] || [ ! -f discofi.cer ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -keyout discofi.key -out discofi.cer \
        -subj "/CN=NotiHelper/OU=Resymbian/O=Resymbian/C=ES"
fi

signsis ./NotiHelper_unsigned.sis ./NotiHelper.sisx \
    discofi.cer discofi.key ""

cp ./NotiHelper.sisx "$OUT"/NotiHelper.sisx
ls -l "$OUT"/NotiHelper.sisx
echo "Built $OUT/NotiHelper.sisx"
