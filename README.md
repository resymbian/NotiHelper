# NotiHelper

Native notification helper for Nokia S60 3rd Edition (tested target: **Nokia
E51, RM-244 / S60 3rd FP1**), for J2ME applications. It gives MIDlets the one
thing the phone's Java runtime does not: a system notification that survives
the application being in the background.

Two clients use it today:

* [ResyGram](https://github.com/resymbian/ResyGram) - Telegram
* [Discofi](https://github.com/resymbian/Discofi) - Discord

A MIDlet with a socket can use it; nothing about it is Telegram- or
Discord-specific.

## What a notification does

One connection produces two visible things:

1. an **Avkon global note** over whatever is on screen (`Title: text`), for the
   seconds it lasts;
2. an **unread entry in Messaging → Inbox**, with the title as the sender and
   the text as the body - the same place a Bluetooth transfer lands in. That one
   remains after the note fades and raises the unread counter.

The Inbox entry is an SMS-DELIVER message written directly to the phone's
message store. It is **not** sent over the network and it is not an operator
message: it is a local log entry. The phone may offer Reply on it, and a reply
would be a real SMS to a sender that is not a phone number, so treat it as a
notification log, not a conversation.

If the messaging store refuses the write (no SMS service configured, a
capability check), the note is still shown and the client still gets `1`. The
Inbox copy is the part that can silently not happen.

## Protocol

Loopback only, one connection per notification:

```
writeUTF(title)     Java modified UTF-8, 2-byte big-endian length
writeUTF(text)      same
read one byte       1 = note shown (Inbox write attempted), 0 = error
```

Client side (CLDC, no `DataOutputStream` needed - it drags an SE-only
superclass into artifacts checked against CLDC):

```java
SocketConnection c = (SocketConnection) Connector.open(
    "socket://127.0.0.1:4711", Connector.READ_WRITE, true);
OutputStream out = c.openOutputStream();
writeUtf(out, title);
writeUtf(out, text);
out.flush();
int ok = c.openInputStream().read();
```

Both clients poll in the background and post one message per batch, not one per
arrival.

## Install

1. The phone needs **Open4All** and the **installserver** patch: the package is
   self-signed.
2. Install `NotiHelper.sisx` (release asset, or built below). The UID is
   `0xA0000D15`; an older `DiscofiNotify` install is replaced, not duplicated.
3. Launch **NotiHelper** once after each reboot. It has no UI - it only starts
   the loopback listener.
4. In the client: ResyGram **Settings → Notifications** then **Test
   notification** in the chat list menu; Discofi **Settings → Notifications →
   Send test notification**. A note appears and the same text is in Inbox.

The package asks for `NetworkServices ReadUserData WriteUserData
LocalServices`: loopback sockets for the first, and the message store for the
rest.

## Build

Everything runs in Docker; no SDK on the host. The image installs the
proprietary Nokia S60 3.1 SDK from the Internet Archive mirror, pinned by
SHA-256, and is for building locally from a legally obtained copy only.

```
docker build -f docker/Dockerfile -t notihelper-sdk .
docker build -f docker/Dockerfile.signtool -t notihelper-sdk:signfix .
chmod 777 out
docker run --rm -v "$PWD:/src:ro" -v "$PWD/out:/out" notihelper-sdk:signfix
```

`out/NotiHelper.sisx` is signed with a generated development certificate
(`openssl req -x509 -newkey rsa:2048`), which is why the phone needs the
installer patch.

With an S60 3rd FP1 SDK already installed on Windows or Linux, `./build.sh`
does the same in place.

### Toolchain notes

Three GnuPoc/OpenSSL 1.1 problems had to be solved before this produced a file:

1. **`makekeys`/`signsis` reject every certificate** with
   `PEM routines:get_name:no start line: Expecting: CERTIFICATE`.
   `signutils.cpp` decides "end of chain" with
   `ERR_GET_FUNC(err) == PEM_F_PEM_READ_BIO`, but OpenSSL 1.1 removed the
   function code from the error queue, so the comparison never matches and the
   normal end-of-input error is treated as a corrupt certificate.
   `docker/Dockerfile.signtool` rebuilds the tool with the library+reason check
   only.
2. **`openssl dsaparam -genkey` fails** in the image, so the certificate is RSA,
   which `signsis` accepts just as well.
3. **`makesis`/`signsis` reject a bare output filename** (`invalid destination
   file`); `./Name.sisx` is accepted.

The PKG header needs a language list and the vendor as two lines -
`%{"Resymbian"}` (localised) and `:"Resymbian"` (unique) - or `makesis` stops
with `No vendor name specified`.

The messaging half links against `msgs` (`CMsvSession`, `CClientMtmRegistry`),
`smcm` (the SMS MTM and `CSmsHeader`), `etext` (rich text) and `gsmu` (the PDU
classes). There is no `msvapi.lib` in this SDK; `msvapi` is only the header
name.

## Status and limits

Built, linked against the real Avkon and Messaging libraries, and signed - but
**not run on a handset** in this repository's development. Nothing here can
execute S60 3.1.

What is known to be outside its reach:

* The note cannot be tapped to open the application; S60's global note API has
  no action callback.
* There is no push. The MIDlet polls, and if the phone suspends Java threads
  while the display is off, nothing arrives until it resumes.
* Only loopback: another device cannot post to it.

## License

MIT. See [LICENSE](LICENSE).
