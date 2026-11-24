Native notification bridge for J2ME applications on S60 3rd Edition phones,
built and signed by GitHub Actions from the source in this repository.

**Changes in 1.0.9**

1.0.8 turned NotiHelper into an S60 GUI app but omitted
`CEikApplication::ResourceFileName()`. With `ENoAppResourceFile` App Arc still
tried to open a default application resource, so launching the icon failed with
a generic "system error" before the listener thread or the log could start.
The application now returns an empty resource filename, as the SDK's minimal
resource-less GUI example requires. Reboot after installing if 1.0.8 was
already opened once.

**Changes in 1.0.8**

Discofi's test notification failed with `SymbianOS error = -34`
(`KErrCouldNotConnect`): the EXE wrote `listening` to its log, but the shell did
not keep a task for a bare EXE, so the listener was gone before the client
connected. NotiHelper is now a real S60 GUI application. Launching it starts the
listener on a worker thread and immediately sends the empty AppUi to the
background, so the application entry shows the running spinner and the socket
stays open. No protocol changes.

The startup log is also fixed: it used to write 16-bit descriptor bytes with a
16-bit length, so `listening` appeared as `l?i?s?t?e`. It is now plain ASCII and
readable from any file manager.

**Changes in 1.0.7**

The listener no longer waits for `CMsvSession`. The socket is opened first, so
a messaging-store failure can only remove the Inbox copy of a notification: the
system note and the acknowledgement byte still work, which is what Discofi and
ResyGram need. The startup log now records `listening` before the `msv=...`
line, so a failed message store can no longer look like "no agent started".

**Changes in 1.0.6**

The 1.0.5 menu entry appeared but the process did not stay running, so Discofi
still reported that no native agent had answered. Two changes make this
diagnosable and use the known-good launch path:

- removed `KAppLaunchInBackground` from the registration resource; FP1's
  launcher reliably starts the process with the default foreground launch;
- the EXE now shows a `NotiHelper: Started` note and writes a startup trace to
  `C:\Data\NotiHelper.log` (`start`, `note=0`, `msv=ok`, `listening`, or the
  first failing stage with its error code).

If the note appears but Discofi still fails, open `C:\Data\NotiHelper.log` with
the file manager and send its contents; it will show exactly which step fails.
No protocol changes.

**Changes in 1.0.5**

1.0.4 installed and signed correctly, but the package only contained the EXE
in `\sys\bin`; S60 never registered it as an application, so there was no icon
in the menu and the listener could not be started. The build now compiles and
installs `NotiHelper.rsc` (the menu caption) and `NotiHelper_reg.rsc` (the
`APP_REGISTRATION_INFO` entry). The entry launches the headless EXE in the
background. No protocol changes.

**Changes in 1.0.4**

1.0.1-1.0.3 failed with the generic "imposible instalar" / "No se ha podido
instalar" because the EXE inside the package had an invalid E32 UID triple.
`NotiHelper.mmp` declared a single UID, which the toolchain used as UID2 and
left the Secure ID (UID3) at `0x00000000`; S60's installer rejects non-GUI
executables without an explicit SID. The MMP now declares
`UID 0x100039CE 0xA0000D15` (KUidApp as UID2, `0xA0000D15` as SID), and the
build verifies the E32 header before signing. No protocol changes.

**Changes in 1.0.3**

1.0.2 got past the installer's signature check but the phone still called it
incompatible and refused to install. The package now declares the S60 3.1
(`0x102032BE`) and S60 3.2 (`0x102752AE`) platform IDs, so the installer can
match it to the E51 instead of asking whether to install an incompatible
application. The development certificate also becomes valid in 2000 rather than
at build time, so an E51 whose clock has reset does not reject it as "not valid
yet". No code changes.

**Changes in 1.0.2**

1.0.1 was signed with an RSA-2048/SHA-256 development certificate, which S60
3rd cannot validate: Symbian 9.x before v9.5 only implements MD5 and SHA-1, and
the installer reports such a package as damaged ("archivo dañado"/"file
corrupted") even though its checksums are correct. The certificate is now
RSA-1024/SHA-1 with no X.509 extensions, matching what the SDK's makekeys
produces.

**Install**

1. Requires Open4All and the installserver patch: the package is signed with a
   throwaway development certificate generated during the build.
2. Copy `NotiHelper.sisx` to the phone and open it.
3. Launch **NotiHelper** once after each reboot. It has no UI; it only starts
   the loopback listener on `127.0.0.1:4711`.

**What a client gets per notification**

- an Avkon global note (`Title: text`);
- the same text as an unread entry in **Messaging → Inbox**, which survives the
  note fading.

The Inbox entry is a local SMS-DELIVER message written to the store; nothing is
sent over the network, and replying to it would be a real SMS to a sender that
is not a phone number.

The phone keeps background Java applications running on its own (the spinner in
the task list means exactly that), so the polling client does not need a second
helper - see the README for what does end an application and what cannot be
prevented.

Clients: [ResyGram](https://github.com/resymbian/ResyGram) and
[Discofi](https://github.com/resymbian/Discofi). Protocol and build details are
in the [README](https://github.com/resymbian/NotiHelper#readme).
