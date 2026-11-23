#!/bin/bash
# Build NotiHelper.sisx with a Nokia S60 3rd Edition FP1 SDK.
#
#   EPOCROOT=/path/to/s60/sdk/ ./build.sh
#
# Requires bldmake/abld and makesis/signsis from the Symbian toolchain.
set -euo pipefail

cd "$(dirname "$0")"

: "${EPOCROOT:?set EPOCROOT to the S60 3rd FP1 SDK root}"

bldmake bldfiles
abld build gcce urel

makesis ./NotiHelper.pkg ./NotiHelper_unsigned.sis

# Self-signed certificate is enough on a jailbroken E51 with Open4All.
if [ ! -f discofi.key ] || [ ! -f discofi.cer ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -keyout discofi.key -out discofi.cer \
    -subj "/CN=NotiHelper/OU=Resymbian/O=Resymbian/C=ES"
fi

signsis ./NotiHelper_unsigned.sis ./NotiHelper.sisx ./discofi.cer ./discofi.key ""

echo "Built NotiHelper.sisx"
