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
3. **NotiHelper** now appears in the application menu (registration resources
   are installed). Open it once after each reboot. It has no UI - selecting it
   only starts the loopback listener in the background. Versions before 1.0.5
   installed the EXE without registering it, so there was no icon to open.
4. In the client: ResyGram **Settings → Notifications** then **Test
   notification** in the chat list menu; Discofi **Settings → Notifications →
   Send test notification**. A note appears and the same text is in Inbox.

The package asks for `NetworkServices ReadUserData WriteUserData
LocalServices`: loopback sockets for the first, and the message store for the
rest.

## Running in the background

S60 does **not** suspend a MIDlet that loses focus or the screen: Symbian OS is
fully multitasking and Java threads keep running, so the personality in the task
list with the spinner is the application still alive (the spinner is its
"running" mark, not a stalled start). `pauseApp()` is not called on
backgrounding either. This is why the polling design works: the client opens the
hidden menu, chooses **Go to background** (or, with the red key, keeps it in the
background rather than closing it) and continues to poll and post.

What actually ends it, in order of likelihood:

* **The End key.** On S60 3rd Edition FP1 (the E51) End closes the MIDlet. The
  `Nokia-MIDlet-No-Exit: true` manifest attribute makes End background it
  instead, but it exists only from **FP2** and later, so it does nothing on the
  E51.
* **Memory.** The E51 has around 40 MB of user RAM; under pressure the OS ends
  background applications. Nothing outside the process can prevent that, and a
  resident helper would make it worse by taking RAM itself.
* **The task manager**, or rebooting the phone.

There is no external way to keep a process alive on Symbian: "killing" is the
OS reclaiming memory, not a permission a helper can hold. A watchdog could only
notice afterwards and restart the MIDlet (the notification state lives in RMS,
so a restart is safe), but it would need a resident native process, boot-time
autostart and App Manager launches; on a 40 MB phone that trade is not worth it.
The clients therefore restart cleanly and resume from their last seen message
IDs.

## Build

Everything runs in Docker; no SDK on the host. The image installs the
proprietary Nokia S60 3.1 SDK from the Internet Archive mirror, pinned by
SHA-256, and is for building locally from a legally obtained copy only.

```
docker build -f docker/Dockerfile -t notihelper-sdk .
chmod 777 out
docker run --rm -v "$PWD:/src:ro" -v "$PWD/out:/out" notihelper-sdk
```

`out/NotiHelper.sisx` is signed with a generated development certificate
(`openssl ca -selfsign`, RSA-1024, SHA-1), which is why the phone needs the
installer patch. The certificate is deliberately RSA-1024/SHA-1 with no X.509
extensions: S60 3rd's certificate manager only implements MD5 and SHA-1 (it is
Symbian OS 9.2, and SHA-256 only arrived with the v9.5 CryptoSPI), so a
SHA-256-signed certificate is rejected as a corrupt package. `makekeys` from the
SDK produces the same kind of bare certificate. Its validity starts in 2000 so
that a phone whose real-time clock has reset still accepts it.

The package declares the S60 3.1 (`0x102032BE`) and S60 3.2 (`0x102752AE`)
platform IDs. Without them the installer cannot match the package to the phone
and asks whether to install an incompatible application.

GitHub Actions runs exactly this on every push and tag
(`.github/workflows/build.yml`), verifies the signature with `signsis`,
publishes the `.sisx` plus `SHA256SUMS` as an artifact and updates the release
tagged `v<version>`. The 3.7 GB SDK image is cached between runs; a first cold
build is dominated by the Internet Archive download.

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
   `docker/Dockerfile` drops that comparison and keeps the library+reason check
   before building the tools.
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

Built by GitHub Actions on every push, linked against the real Avkon and
Messaging libraries, and signed - but **not run on a handset** in this
repository's development. Nothing here can execute S60 3.1, so the runtime
behaviour of the messaging store write is the one thing still to confirm.

What is known to be outside its reach:

* The note cannot be tapped to open the application; S60's global note API has
  no action callback.
* There is no push. The MIDlet polls every 1, 5 or 15 minutes, and the intervals
  are as accurate as the phone lets a background thread be.
* Only loopback: another device cannot post to it.
* It cannot keep a MIDlet alive. See "Running in the background" above: the OS
  keeps it running by itself, and only RAM pressure, End or a reboot end it.

## License

MIT. See [LICENSE](LICENSE).
