Native notification bridge for J2ME applications on S60 3rd Edition phones,
built and signed by GitHub Actions from the source in this repository.

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
