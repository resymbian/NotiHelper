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
 *
 * The process is a real S60 GUI application, not a bare EXE. The App Arc
 * registration gives it a menu icon and a task-list entry: launching it puts
 * the listener in a worker thread and sends the (empty) app UI to the
 * background, so the icon shows the running spinner. A bare EXE ran the socket
 * loop but the shell did not keep a task for it, and the process was usually
 * gone by the time a client tried to connect (KErrCouldNotConnect, -34).
 */

#include <e32base.h>
#include <e32std.h>
#include <f32file.h>
#include <es_sock.h>
#include <in_sock.h>
#include <aknglobalnote.h>
#include <aknnotifystd.h>
#include <avkon.hrh>

// Application framework: an S60 GUI app keeps its window group, and therefore
// its task-list spinner, alive while the listener thread serves notifications.
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <apgtask.h>
#include <eikstart.h>

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

// Bind to every local interface, not just 127.0.0.1. On some S60 builds the
// J2ME runtime routes a literal 127.0.0.1 through the selected IAP or an IPv6
// mapped loopback, which a loopback-only listener never sees and the client
// reports as KErrCouldNotConnect (-34). Accept is still restricted to loopback,
// so the port is never reachable from the network.
const TUint32 KNotiHelperBindAddress = KInetAddrAny;
// A Java modified UTF-8 field can hold three bytes per BMP character. The old
// 512-byte cap rejected perfectly ordinary emoji messages, so the bridge
// answered 0 even though the note itself would have fitted. The clients trim
// below this cap; this is a hard wire limit, not a target length.
const TInt KMaxFieldLength = 8192;
const TInt KMaxNoteLength = 160;

static void ServeL();
static TInt ListenerThreadEntry(TAny* aParameter);
static void WriteStatus(const TDesC& aText);
static void WriteStatusError(const TDesC& aStage, TInt aError);

/** S60 application: identifies the EXE and creates the document. */
class CNotiHelperApplication : public CAknApplication
{
public:
    static CApaApplication* NewApplication();

private:
    TUid AppDllUid() const;
    CApaDocument* CreateDocumentL();

    // ENoAppResourceFile requires an empty name, otherwise App Arc tries to
    // open a non-existent application resource and the launch dies with a
    // generic system error before ConstructL (and before the log is written).
    TFileName ResourceFileName() const;
};

/** Non-file document whose only job is to create the AppUi. */
class CNotiHelperDocument : public CAknDocument
{
public:
    static CNotiHelperDocument* NewL(CEikApplication& aApp);
    static CNotiHelperDocument* NewLC(CEikApplication& aApp);
    ~CNotiHelperDocument();

    CEikAppUi* CreateAppUiL();

private:
    CNotiHelperDocument(CEikApplication& aApp);
    void ConstructL();
};

/**
 * Invisible AppUi. Starting the GUI framework keeps the process in the task
 * list; SendToBackground hides the empty view and leaves the spinner visible.
 * The blocking socket loop runs on a worker thread so the AppUi can keep
 * answering window-server events while notifications arrive.
 */
class CNotiHelperAppUi : public CAknAppUi
{
public:
    void ConstructL();
    ~CNotiHelperAppUi();

    void HandleForegroundEventL(TBool aForeground);
    void HandleCommandL(TInt aCommand);

private:
    void SendToBackground();
    RThread iListenerThread;
};

CApaApplication* CNotiHelperApplication::NewApplication()
{
    return new CNotiHelperApplication;
}

TUid CNotiHelperApplication::AppDllUid() const
{
    return TUid::Uid(0xA0000D15);
}

CApaDocument* CNotiHelperApplication::CreateDocumentL()
{
    return CNotiHelperDocument::NewL(*this);
}

TFileName CNotiHelperApplication::ResourceFileName() const
{
    return TFileName();
}

CNotiHelperDocument* CNotiHelperDocument::NewL(CEikApplication& aApp)
{
    CNotiHelperDocument* self = NewLC(aApp);
    CleanupStack::Pop(self);
    return self;
}

CNotiHelperDocument* CNotiHelperDocument::NewLC(CEikApplication& aApp)
{
    CNotiHelperDocument* self = new (ELeave) CNotiHelperDocument(aApp);
    CleanupStack::PushL(self);
    self->ConstructL();
    return self;
}

CNotiHelperDocument::CNotiHelperDocument(CEikApplication& aApp)
    : CAknDocument(aApp)
{
}

CNotiHelperDocument::~CNotiHelperDocument()
{
}

void CNotiHelperDocument::ConstructL()
{
}

CEikAppUi* CNotiHelperDocument::CreateAppUiL()
{
    return new (ELeave) CNotiHelperAppUi;
}

void CNotiHelperAppUi::ConstructL()
{
    // No resource file and no screen furniture: there is no view to draw.
    BaseConstructL(ENoAppResourceFile | ENoScreenFurniture);

    User::LeaveIfError(iListenerThread.Create(_L("NotiHelperListener"),
        ListenerThreadEntry, 0x10000, NULL, NULL));
    iListenerThread.SetPriority(EPriorityNormal);
    iListenerThread.Resume();

    // A launch from the menu should not leave an empty window on screen.
    SendToBackground();
}

CNotiHelperAppUi::~CNotiHelperAppUi()
{
    if (iListenerThread.Handle())
    {
        iListenerThread.Kill(0);
        iListenerThread.Close();
    }
}

void CNotiHelperAppUi::SendToBackground()
{
    TApaTask self(iEikonEnv->WsSession());
    self.SetWgId(iEikonEnv->RootWin().Identifier());
    self.SendToBackground();
}

void CNotiHelperAppUi::HandleForegroundEventL(TBool aForeground)
{
    CAknAppUi::HandleForegroundEventL(aForeground);
    if (aForeground) SendToBackground();
}

void CNotiHelperAppUi::HandleCommandL(TInt aCommand)
{
    if (aCommand == EAknCmdExit)
    {
        if (iListenerThread.Handle()) iListenerThread.Kill(0);
    }
    CAknAppUi::HandleCommandL(aCommand);
}

/** CMsvSession requires an observer, but we do not need its events. */
class TNullMsvObserver : public MMsvSessionObserver
{
public:
    void HandleSessionEventL(TMsvSessionEvent, TAny*, TAny*, TAny*) {}
};

/** Read exactly aTargetLength bytes into aBuffer. */
static void ReadExactL(RSocket& aSocket, TDes8& aBuffer, TInt aTargetLength)
{
    while (aBuffer.Length() < aTargetLength)
    {
        TInt needed = aTargetLength - aBuffer.Length();
        TPtr8 slice(const_cast<TUint8*>(aBuffer.Ptr()) + aBuffer.Length(), 0, needed);
        TSockXfrLength len = 0;
        TRequestStatus status;
        aSocket.RecvOneOrMore(slice, 0, status, len);
        User::WaitForRequest(status);
        User::LeaveIfError(status.Int());
        TInt readBytes = len();
        if (readBytes <= 0)
        {
            readBytes = slice.Length();
        }
        if (readBytes <= 0)
        {
            User::Leave(KErrEof);
        }
        aBuffer.SetLength(aBuffer.Length() + readBytes);
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
    lengthBytes.Zero();
    ReadExactL(aSocket, lengthBytes, 2);

    // If an HTTP proxy on the device connected and sent CONNECT / GET / POST:
    if ((lengthBytes[0] == 'C' && lengthBytes[1] == 'O') || // CONNECT
        (lengthBytes[0] == 'G' && lengthBytes[1] == 'E') || // GET
        (lengthBytes[0] == 'P' && lengthBytes[1] == 'O'))   // POST
    {
        WriteStatus(_L("proxy handshake"));
        TBuf8<1> b;
        TInt crlfCount = 0;
        for (TInt i = 0; i < 4096; i++)
        {
            b.Zero();
            ReadExactL(aSocket, b, 1);
            if (b[0] == '\r' || b[0] == '\n')
            {
                crlfCount++;
                if (crlfCount >= 4) break;
            }
            else
            {
                crlfCount = 0;
            }
        }
        if (lengthBytes[0] == 'C')
        {
            _LIT8(KHttp200, "HTTP/1.1 200 Connection Established\r\n\r\n");
            TRequestStatus status;
            aSocket.Write(KHttp200, status);
            User::WaitForRequest(status);
        }
        lengthBytes.Zero();
        ReadExactL(aSocket, lengthBytes, 2);
    }

    TInt b0 = (TInt)lengthBytes[0];
    TInt b1 = (TInt)lengthBytes[1];
    TInt length = (b0 << 8) | b1;

    TBuf<64> lenLog(_L("utf len="));
    lenLog.AppendNum(length);
    lenLog.Append(_L(" b0="));
    lenLog.AppendNum(b0, EHex);
    lenLog.Append(_L(" b1="));
    lenLog.AppendNum(b1, EHex);
    WriteStatus(lenLog);

    if (length < 0 || length > KMaxFieldLength) User::Leave(KErrTooBig);
    if (length == 0) return HBufC::NewL(0);

    HBufC8* raw = HBufC8::NewLC(length);
    TPtr8 rawPtr(const_cast<TUint8*>(raw->Ptr()), 0, length);
    ReadExactL(aSocket, rawPtr, length);

    HBufC* text = HBufC::NewL(length);
    TPtr target = text->Des();
    AppendModifiedUtf8L(rawPtr, target);
    CleanupStack::PopAndDestroy(raw);
    return text;
}

/**
 * Only this phone may post. IsLoopback() covers 127.x.x.x and ::1, but the
 * S60 Java stack sometimes opens the connection against the device's own IAP
 * address instead. That address is still local: it is the LocalName() of the
 * accepted socket. Comparing the two keeps the wildcard bind from opening the
 * port to the network without refusing a valid local client.
 */
static TBool IsLocalPeer(RSocket& aConnection)
{
    TInetAddr remote;
    aConnection.RemoteName(remote);
    if (remote.IsLoopback()) { return ETrue; }

    TInetAddr local;
    aConnection.LocalName(local);
    if (local.Family() != remote.Family()) { return EFalse; }
    return local.Address() == remote.Address();
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

    TMsvId serviceId = sms->ServiceId();
    if (serviceId == KMsvNullIndexEntryId)
    {
        TRAP_IGNORE(serviceId = sms->DefaultServiceL());
        if (serviceId == KMsvNullIndexEntryId)
        {
            serviceId = KMsvLocalServiceIndexEntryId;
        }
    }

    TMsvEntry entry;
    entry.iDate.HomeTime();
    entry.SetInPreparation(ETrue);
    entry.iMtm = KUidMsgTypeSMS;
    entry.iType = KUidMsvMessageEntry;
    entry.iServiceId = serviceId;
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
    TBuf<KMaxNoteLength> message;
    if (aTitle.Length() > 0)
    {
        message.Append(aTitle.Left(32));
        message.Append(_L(": "));
    }
    TInt remaining = KMaxNoteLength - message.Length();
    if (remaining > 0 && aText.Length() > 0)
    {
        message.Append(aText.Left(remaining));
    }

    CAknGlobalNote* note = CAknGlobalNote::NewLC();
    note->ShowNoteL(EAknGlobalInformationNote, message);
    CleanupStack::PopAndDestroy(note);
}

static void AnswerL(RSocket& aSocket, TBool aOk)
{
    TBuf8<1> reply;
    reply.Append((TUint8)(aOk ? 1 : 0));

    TRequestStatus status;
    aSocket.Write(reply, status);
    User::WaitForRequest(status);
    User::LeaveIfError(status.Int());
}

static void HandleConnectionL(CMsvSession* aSession, RSocket& aConnection,
    TBool& aAnswered)
{
    WriteStatus(_L("read title"));
    HBufC* title = ReadUtfL(aConnection);
    CleanupStack::PushL(title);
    WriteStatus(_L("read text"));
    HBufC* text = ReadUtfL(aConnection);
    CleanupStack::PushL(text);

    // Answer the client immediately so it never hits client-side socket timeouts.
    AnswerL(aConnection, ETrue);
    aAnswered = ETrue;
    WriteStatus(_L("reply=1"));

    // Trap the note display: if the Avkon global note server is busy, the note
    // must not cause the notification to fail.
    TRAPD(noteError, ShowNotificationL(*title, *text));
    WriteStatusError(_L("note"), noteError);

    // The Inbox entry is secondary.
    if (aSession != NULL)
    {
        TRAPD(inboxError, CreateInboxSmsL(*aSession, *title, *text));
        WriteStatusError(_L("inbox"), inboxError);
    }

    CleanupStack::PopAndDestroy(text);
    CleanupStack::PopAndDestroy(title);
}

/**
 * Startup/connection trace. There is no console on the phone, so the only way
 * to see where the agent stopped is a file the user can open from the file
 * manager (with Open4All active). The log accumulates all stages of one run;
 * ClearStatus() truncates it once when ServeL starts.
 */
static void ClearStatus()
{
    RFs fs;
    if (fs.Connect() != KErrNone) return;

    RFile file;
    file.Replace(fs, _L("C:\\Data\\NotiHelper.log"),
        EFileWrite | EFileShareAny);
    file.Close();
    fs.Close();
}

static void WriteStatus(const TDesC& aText)
{
    RFs fs;
    if (fs.Connect() != KErrNone) return;

    RFile file;
    TInt error = file.Open(fs, _L("C:\\Data\\NotiHelper.log"),
        EFileWrite | EFileShareAny);
    if (error == KErrNotFound)
    {
        error = file.Create(fs, _L("C:\\Data\\NotiHelper.log"),
            EFileWrite | EFileShareAny);
    }

    if (error == KErrNone)
    {
        TInt size = 0;
        file.Size(size);
        file.Seek(ESeekEnd, size);

        // TDesC is 16-bit: casting its pointer to TUint8* and using Length()
        // wrote only every other byte, so "listening" showed up as l?i?s?t?e.
        // Copy to 8-bit first; all stage names are plain ASCII.
        HBufC8* ascii = HBufC8::NewLC(aText.Length() + 1);
        TPtr8 bytes = ascii->Des();
        bytes.Copy(aText);
        bytes.Append('\n');
        file.Write(bytes);
        CleanupStack::PopAndDestroy(ascii);
        file.Close();
    }
    fs.Close();
}

static void WriteStatusError(const TDesC& aStage, TInt aError)
{
    TBuf<128> line(aStage);
    line.Append(_L("="));
    line.AppendNum(aError);
    WriteStatus(line);
}

static void LeaveStatusL(TInt aError, const TDesC& aStage)
{
    if (aError != KErrNone)
    {
        WriteStatusError(aStage, aError);
        User::Leave(aError);
    }
}

static void ServeL()
{
    ClearStatus();
    WriteStatus(_L("boot"));
    WriteStatus(_L("start"));

    // Bring up the listener first. A synchronous Avkon note can stay on screen
    // for seconds; showing it before Listen() left a window where a client got
    // -34 even though the icon had clearly been launched. The note is moved
    // below the "listening" marker.
    RSocketServ socketServer;
    LeaveStatusL(socketServer.Connect(), _L("socketsrv"));
    CleanupClosePushL(socketServer);

    RSocket listener;
    LeaveStatusL(listener.Open(socketServer, KAfInet, KSockStream,
        KProtocolInetTcp), _L("socketopen"));
    CleanupClosePushL(listener);

    TInetAddr address(KNotiHelperBindAddress, KNotiHelperPort);
    LeaveStatusL(listener.Bind(address), _L("bind"));
    WriteStatus(_L("bind=any"));
    LeaveStatusL(listener.Listen(4), _L("listen"));
    WriteStatus(_L("listening"));

    // Now that the port is live, the startup note is safe and doubles as
    // visible proof that the menu entry really launched the process.
    TRAPD(noteError, ShowNotificationL(_L("NotiHelper"), _L("Started")));
    WriteStatusError(_L("note"), noteError);

    TNullMsvObserver msvObserver;
    CMsvSession* messages = NULL;
    TRAPD(msvError, messages = CMsvSession::OpenSyncL(msvObserver));
    if (msvError == KErrNone && messages != NULL)
    {
        CleanupStack::PushL(messages);
        WriteStatus(_L("msv=ok"));
    }
    else
    {
        // Do not leave: a broken message store must not kill the note bridge.
        WriteStatusError(_L("msv"), msvError);
        messages = NULL;
    }

    for (;;)
    {
        RSocket connection;
        TInt openErr = connection.Open(socketServer);
        if (openErr != KErrNone)
        {
            WriteStatusError(_L("connopen"), openErr);
            User::After(200000);
            continue;
        }
        CleanupClosePushL(connection);

        TRequestStatus acceptStatus;
        listener.Accept(connection, acceptStatus);
        User::WaitForRequest(acceptStatus);
        if (acceptStatus.Int() != KErrNone)
        {
            WriteStatusError(_L("accept"), acceptStatus.Int());
            CleanupStack::PopAndDestroy(&connection);
            continue;
        }
        WriteStatus(_L("accept"));

        // The wildcard bind is only for compatibility with the J2ME routing.
        // Never act on a request that did not come from this device. A J2ME
        // stack that addresses the phone's own IAP rather than 127.0.0.1 is
        // still local and is accepted by IsLocalPeer().
        if (!IsLocalPeer(connection))
        {
            WriteStatus(_L("reject=remote"));
            CleanupStack::PopAndDestroy(&connection);
            continue;
        }
        TInetAddr remote;
        connection.RemoteName(remote);
        if (remote.IsLoopback())
        {
            WriteStatus(_L("remote=loopback"));
        }
        else
        {
            TBuf<64> line(_L("remote=local="));
            line.AppendNum(remote.Address(), EHex);
            WriteStatus(line);
        }

        TBool answered = EFalse;
        TRAPD(handlerError, HandleConnectionL(messages, connection, answered));
        WriteStatusError(_L("handler"), handlerError);

        if (!answered && handlerError != KErrDisconnected && handlerError != KErrTimedOut && handlerError != KErrEof)
        {
            TRAPD(answerError, AnswerL(connection, EFalse));
            WriteStatusError(_L("reply"), answerError);
        }

        CleanupStack::PopAndDestroy(&connection);
    }

    if (messages != NULL) CleanupStack::PopAndDestroy(messages);
    CleanupStack::PopAndDestroy(&listener);
    CleanupStack::PopAndDestroy(&socketServer);
}

/** Entry point for the listener worker created by the AppUi. */
static TInt ListenerThreadEntry(TAny* /*aParameter*/)
{
    CTrapCleanup* cleanup = CTrapCleanup::New();
    if (cleanup == NULL) return KErrNoMemory;

    CActiveScheduler* scheduler = new CActiveScheduler;
    if (scheduler == NULL)
    {
        delete cleanup;
        return KErrNoMemory;
    }
    CActiveScheduler::Install(scheduler);

    TInt result = KErrNone;
    TRAP(result, ServeL());
    WriteStatusError(_L("exit"), result);

    CActiveScheduler::Install(NULL);
    delete scheduler;
    delete cleanup;
    return result;
}

/**
 * Run the application framework, not ServeL directly. EikStart keeps the
 * window group, and hence the task-list spinner, alive for the whole session.
 */
GLDEF_C TInt E32Main()
{
    return EikStart::RunApplication(CNotiHelperApplication::NewApplication);
}
