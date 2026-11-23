/**
 * NotiHelper - the native notification bridge for Nokia E51 / S60 3rd Edition FP1.
 *
 * The E51 Java runtime has no Nokia soft notifications and no Pigler support,
 * so the J2ME clients (Discofi, ResyGram) send notifications to this process
 * over a local TCP socket, and it turns them into a note and an Inbox entry.
 *
 * Protocol (one connection per notification):
 *   writeUTF(title)   - Java modified UTF-8, 2-byte big-endian length
 *   writeUTF(text)    - same
 *   read 1 byte       - 1 = stored, 0 = error
 *
 * Two visible effects per notification, in this order:
 *   1. an Avkon global note over whatever is on screen, for the moment it
 *      arrives;
 *   2. an unread SMS-DELIVER entry in Messaging's Inbox, so the notification is
 *      still there after the note fades - the same place a Bluetooth transfer
 *      lands in.
 *
 * The Messaging API links with msgs.lib, not msvapi.lib: this SDK exports
 * CMsvSession/CMsvEntry/CClientMtmRegistry from msgs, the SMS MTM from smcm,
 * and the rich-text pieces from etext. Writing the SMS-DELIVER header directly
 * is deliberate - CSmsClientMtm::SaveMessageL would create an outgoing
 * (SMS-SUBMIT) PDU, which is not what a received notification is.
 *
 * Build with native/e51/docker (GnuPoc toolchain + S60 SDK). See README.md.
 */

#include <e32base.h>
#include <e32std.h>
#include <es_sock.h>
#include <in_sock.h>
#include <aknglobalnote.h>
#include <aknnotifystd.h>
#include <avkon.hrh>

// Messaging: store the notification as a received SMS in Inbox.
#include <msvapi.h>
#include <msvids.h>
#include <msvuids.h>
#include <mtclreg.h>
#include <smsclnt.h>
#include <smut.h>
#include <smuthdr.h>
#include <txtrich.h>
#include <txtfmlyr.h>

const TUint KNotiHelperPort = 4711;
const TUint32 KNotiHelperLoopback = INET_ADDR(127, 0, 0, 1);
const TInt KMaxFieldLength = 512;
const TInt KMaxNoteLength = 160;

/** CMsvSession requires an observer, but we do not need its events. */
class TNullMsvObserver : public MMsvSessionObserver
{
public:
    void HandleSessionEventL(TMsvSessionEvent, TAny*, TAny*, TAny*) {}
};

/** RSocket::Recv until the descriptor is full. */
static void ReadExactL(RSocket& aSocket, TDes8& aBuffer)
{
    TInt offset = 0;
    const TInt maxLength = aBuffer.MaxLength();

    while (offset < maxLength)
    {
        TPtr8 chunk = aBuffer.MidTPtr(offset);
        TRequestStatus status;
        aSocket.Recv(chunk, chunk.MaxLength(), status);
        User::WaitForRequest(status);
        User::LeaveIfError(status.Int());

        if (chunk.Length() == 0)
        {
            User::Leave(KErrEof);
        }

        offset += chunk.Length();
        aBuffer.SetLength(offset);
    }
}

/**
 * Decode Java's modified UTF-8. writeUTF() emits one-, two- and three-byte
 * sequences only: UTF-16 surrogate pairs are written as two three-byte
 * sequences, so no four-byte decoding is needed.
 */
static void AppendModifiedUtf8L(const TDesC8& aSource, TDes& aTarget)
{
    TInt i = 0;

    while (i < aSource.Length())
    {
        TUint8 first = aSource[i++];

        if (first < 0x80)
        {
            aTarget.Append((TText)first);
        }
        else if ((first & 0xE0) == 0xC0)
        {
            if (i >= aSource.Length()) User::Leave(KErrCorrupt);
            TUint8 second = aSource[i++];
            aTarget.Append((TText)(((first & 0x1F) << 6) | (second & 0x3F)));
        }
        else if ((first & 0xF0) == 0xE0)
        {
            if (i + 1 >= aSource.Length()) User::Leave(KErrCorrupt);
            TUint8 second = aSource[i++];
            TUint8 third = aSource[i++];
            aTarget.Append((TText)(((first & 0x0F) << 12) |
                ((second & 0x3F) << 6) | (third & 0x3F)));
        }
        else
        {
            User::Leave(KErrCorrupt);
        }
    }
}

static HBufC* ReadUtfL(RSocket& aSocket)
{
    TBuf8<2> lengthBytes;
    lengthBytes.SetLength(2);
    ReadExactL(aSocket, lengthBytes);

    TInt length = (lengthBytes[0] << 8) | lengthBytes[1];
    if (length < 0 || length > KMaxFieldLength) User::Leave(KErrTooBig);
    if (length == 0) return HBufC::NewL(0);

    HBufC8* raw = HBufC8::NewLC(length);
    TPtr8 rawPtr = raw->Des();
    rawPtr.SetLength(length);
    ReadExactL(aSocket, rawPtr);
    raw->Des().SetLength(length);

    HBufC* text = HBufC::NewL(length);
    TPtr target = text->Des();
    AppendModifiedUtf8L(*raw, target);
    CleanupStack::PopAndDestroy(raw);
    return text;
}

/**
 * Store the notification as an incoming SMS in the phone's Messaging Inbox.
 *
 * Do not call SaveMessageL(): that creates an SMS-SUBMIT (outgoing) PDU. We
 * write an SMS-DELIVER header plus body directly to the message store, then
 * mark the entry complete, visible and unread, which is what makes it appear
 * in Inbox with the unread count the way a received message does.
 */
static void CreateInboxSmsL(CMsvSession& aSession, const TDesC& aSender,
    const TDesC& aText)
{
    CClientMtmRegistry* registry = CClientMtmRegistry::NewL(aSession);
    CleanupStack::PushL(registry);

    CSmsClientMtm* sms = static_cast<CSmsClientMtm*>(
        registry->NewMtmL(KUidMsgTypeSMS));
    if (sms == NULL)
    {
        CleanupStack::PopAndDestroy(registry);
        User::Leave(KErrNotSupported);
    }
    CleanupStack::PushL(sms);

    sms->SwitchCurrentEntryL(KMsvGlobalInBoxIndexEntryId);
    sms->RestoreServiceAndSettingsL();

    TMsvEntry entry;
    entry.iDate.HomeTime();
    entry.SetInPreparation(ETrue);
    entry.iMtm = KUidMsgTypeSMS;
    entry.iType = KUidMsvMessageEntry;
    entry.iServiceId = sms->ServiceId();
    entry.iDetails.Set(aSender);

    TPtrC preview(aText.Left(12));
    entry.iDescription.Set(preview);
    entry.SetSendingState(KMsvSendStateNotApplicable);
    entry.SetUnread(ETrue);
    entry.SetNew(ETrue);

    sms->Entry().CreateL(entry);
    TMsvId newId = entry.Id();
    sms->SwitchCurrentEntryL(newId);

    CParaFormatLayer* para = CParaFormatLayer::NewL();
    CleanupStack::PushL(para);
    CCharFormatLayer* chars = CCharFormatLayer::NewL();
    CleanupStack::PushL(chars);
    CRichText* rich = CRichText::NewL(para, chars);
    CleanupStack::PushL(rich);
    rich->InsertL(0, aText);

    CSmsHeader* header = CSmsHeader::NewL(CSmsPDU::ESmsDeliver, *rich);
    CleanupStack::PushL(header);
    header->SetFromAddressL(aSender);
    header->Deliver().SetServiceCenterTimeStamp(entry.iDate);

    CMsvStore* store = sms->Entry().EditStoreL();
    CleanupStack::PushL(store);
    header->StoreL(*store);
    store->StoreBodyTextL(*rich);
    store->CommitL();

    CleanupStack::PopAndDestroy(store);
    CleanupStack::PopAndDestroy(header);
    CleanupStack::PopAndDestroy(rich);
    CleanupStack::PopAndDestroy(chars);
    CleanupStack::PopAndDestroy(para);

    TMsvEntry stored = sms->Entry().Entry();
    stored.SetInPreparation(EFalse);
    stored.SetComplete(ETrue);
    stored.SetVisible(ETrue);
    stored.iDetails.Set(aSender);
    stored.iDescription.Set(preview);
    sms->Entry().ChangeL(stored);

    CleanupStack::PopAndDestroy(sms);
    CleanupStack::PopAndDestroy(registry);
}

/**
 * Show the note. Newlines become spaces: a global note is one or two lines and
 * the J2ME side already folds multi-line previews, but a title built from a
 * chat name can still arrive with one.
 */
static void ShowNotificationL(const TDesC& aTitle, const TDesC& aText)
{
    CAknGlobalNote* note = CAknGlobalNote::NewLC();

    TBuf<KMaxNoteLength> message;
    message.Copy(aTitle);
    message.Append(_L(": "));
    message.Append(aText);
    if (message.Length() > KMaxNoteLength) message.SetLength(KMaxNoteLength);

    note->ShowNoteL(EAknGlobalInformationNote, message);
    CleanupStack::PopAndDestroy(note);
}

static void HandleConnectionL(CMsvSession& aSession, RSocket& aConnection,
    TBool& aOk)
{
    HBufC* title = ReadUtfL(aConnection);
    CleanupStack::PushL(title);
    HBufC* text = ReadUtfL(aConnection);
    CleanupStack::PushL(text);

    ShowNotificationL(*title, *text);

    // The Inbox entry is the part that survives the note. A messaging write can
    // fail - no SMS service configured, the server refusing the store - and
    // that must not turn a shown note into an error answer.
    TRAPD(inboxError, CreateInboxSmsL(aSession, *title, *text));
    aOk = ETrue;
    (void)inboxError;

    CleanupStack::PopAndDestroy(text);
    CleanupStack::PopAndDestroy(title);
}

static void AnswerL(RSocket& aSocket, TBool aOk)
{
    TBuf8<1> reply;
    reply.Append((TUint8)(aOk ? 1 : 0));

    TRequestStatus status;
    aSocket.Send(reply, reply.Length(), status);
    User::WaitForRequest(status);
    User::LeaveIfError(status.Int());
}

static void ServeL()
{
    TNullMsvObserver msvObserver;
    CMsvSession* messages = CMsvSession::OpenSyncL(msvObserver);
    CleanupStack::PushL(messages);

    RSocketServ socketServer;
    User::LeaveIfError(socketServer.Connect());
    CleanupClosePushL(socketServer);

    RSocket listener;
    User::LeaveIfError(listener.Open(socketServer, KAfInet, KSockStream,
        KProtocolInetTcp));
    CleanupClosePushL(listener);

    TInetAddr address;
    address.SetAddress(KNotiHelperLoopback);
    address.SetPort(KNotiHelperPort);
    User::LeaveIfError(listener.Bind(address));
    User::LeaveIfError(listener.Listen(4));

    for (;;)
    {
        RSocket connection;
        TRequestStatus acceptStatus;
        listener.Accept(connection, acceptStatus);
        User::WaitForRequest(acceptStatus);
        if (acceptStatus.Int() != KErrNone) continue;
        CleanupClosePushL(connection);

        TBool ok = EFalse;
        TRAPD(handlerError, HandleConnectionL(*messages, connection, ok));
        (void)handlerError;

        TRAPD(answerError, AnswerL(connection, ok));
        (void)answerError;

        CleanupStack::PopAndDestroy(&connection);
    }

    CleanupStack::PopAndDestroy(&listener);
    CleanupStack::PopAndDestroy(&socketServer);
    CleanupStack::PopAndDestroy(messages);
}

GLDEF_C TInt E32Main()
{
    CTrapCleanup* cleanup = CTrapCleanup::New();
    if (cleanup == NULL) return KErrNoMemory;

    TInt result = KErrNone;
    TRAP(result, ServeL());

    delete cleanup;
    return result;
}
