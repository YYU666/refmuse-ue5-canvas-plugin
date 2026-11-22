// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "AssetToolsModule.h"
#include "ContentBrowserDataMenuContexts.h"
#include "ContentBrowserDataSubsystem.h"
#include "Containers/Ticker.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "IWebBrowserSingleton.h"
#include "IAssetTools.h"
#include "IContentBrowserDataModule.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Internationalization/Regex.h"
#include "RefmuseCanvasAssetCommands.h"
#include "RefmuseCanvasAssetImporter.h"
#include "RefmuseCanvasAssetReceiver.h"
#include "SWebBrowser.h"
#include "ToolMenus.h"
#include "UObject/StrongObjectPtr.h"
#include "WebBrowserModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogRefmuseCanvas, Log, All);

namespace RefmuseCanvas
{
static const FName TabId(TEXT("RefmuseCanvas"));
static const TCHAR* AssetReceiverBinding = TEXT("refmuseassetreceiver");

// Deliberately accept only an explicit loopback origin, not paths, credentials,
// fragments, query parameters, DNS names or command-line URL fragments.
static bool NormalizeOrigin(const FString& Input, FString& Origin)
{
    const FString Value = Input.TrimStartAndEnd();
    const FRegexPattern Pattern(TEXT("^http://(127\\.0\\.0\\.1|localhost):([1-9][0-9]{0,4})/?$"));
    FRegexMatcher Match(Pattern, Value);
    if (!Match.FindNext()) return false;
    const int32 Port = FCString::Atoi(*Match.GetCaptureGroup(2));
    if (Port < 1 || Port > 65535) return false;
    Origin = FString::Printf(TEXT("http://%s:%d"), *Match.GetCaptureGroup(1), Port);
    return true;
}

static bool IsWritableDestination(const FString& Destination)
{
    if (!IsProjectAssetFolder(Destination)) return false;
    const TArray<FString> Paths{Destination};
    return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().AllPassWritableFolderFilter(Paths);
}

class SCanvasPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCanvasPanel) {} SLATE_END_ARGS()

    virtual ~SCanvasPanel() override { DisconnectAssetReceiver(); }

    void Construct(const FArguments& Args)
    {
        AssetReceiver.Reset(NewObject<URefmuseCanvasAssetReceiver>());
        ConfigPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RefmuseCanvas/Connection.ini"));
        FString SavedOrigin;
        GConfig->GetString(TEXT("Connection"), TEXT("Origin"), SavedOrigin, ConfigPath);
        const FString EnvironmentOrigin = FPlatformMisc::GetEnvironmentVariable(TEXT("REFMUSE_CANVAS_URL"));
        if (!EnvironmentOrigin.IsEmpty()) SavedOrigin = EnvironmentOrigin;
        Status = TEXT("Enter the local canvas origin including its port, then Connect.");
        ChildSlot
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(8)
            [ SNew(STextBlock).Text(FText::FromString(TEXT("Refmuse Canvas - local editor connection (experimental)"))) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0)
            [ SAssignNew(OriginInput, SEditableTextBox)
                .Text(FText::FromString(SavedOrigin))
                .HintText(FText::FromString(TEXT("http://127.0.0.1:<port>"))) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8)
            [ SNew(SButton).Text(FText::FromString(TEXT("Connect / Reconnect")))
                .OnClicked(this, &SCanvasPanel::Connect) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
            [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Status); }) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1)
                [ SAssignNew(CanvasProjectInput, SEditableTextBox)
                    .HintText(FText::FromString(TEXT("Canvas project ID (prj_...)")))
                    .OnTextChanged_Lambda([this](const FText&) { ResetAssetSelection(); }) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)
                [ SNew(SButton).Text(FText::FromString(TEXT("Read Selected PNG")))
                    .IsEnabled_Lambda([this] { return CanReadAsset(); })
                    .OnClicked(this, &SCanvasPanel::ReadSelectedPng) ]
                + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(FText::FromString(TEXT("Import Texture2D")))
                    .IsEnabled_Lambda([this] { return CanImportLegacyPng(); })
                    .OnClicked(this, &SCanvasPanel::ImportSelectedPng) ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
            [ SNew(SButton).Text(FText::FromString(TEXT("Copy Selected Canvas Asset (Ctrl+Shift+C)")))
                .IsEnabled_Lambda([this] { return CanStartAssetRequest(); })
                .OnClicked(this, &SCanvasPanel::CopySelectedAsset) ]
            + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
            [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(GetAssetStatus()); }) ]
            + SVerticalBox::Slot().FillHeight(1)
            [ SAssignNew(BrowserSlot, SBox) ]
        ];
        if (!SavedOrigin.IsEmpty()) Connect();
    }

    bool CanPasteCanvasAsset() const
    {
        return CanStartAssetRequest() && AssetReceiver->GetCopied() != nullptr;
    }

    bool PasteCanvasAsset(const FString& Destination)
    {
        if (!CanPasteCanvasAsset() || !IsWritableDestination(Destination)) return false;
        const FCanvasAssetSource Source = *AssetReceiver->GetCopied();
        ResetAssetSelection();
        FString Script;
        if (!LoadAssetScript(Script))
        {
            ShowPasteResult(false);
            return false;
        }

        const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        AssetReceiver->BeginPaste(Source, RequestId);
        PasteDestination = Destination;
        PasteTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(SharedThis(this), &SCanvasPanel::TickPaste), 0.1f);
        // Identity fields were validated before entering the private clipboard.
        Browser->ExecuteJavascript(Script + FString::Printf(TEXT("('%s','%s','%s','paste','%s','%s','%s');"),
            *Origin, *Source.ProjectId, *RequestId, *Source.NodeId, *Source.Digest, *Source.Extension));
        return true;
    }

    bool StartAssetCommand(const FAssetCommand& Command, FString& Message)
    {
        if (!CanStartAssetRequest())
        {
            Message = TEXT("The canvas is unavailable or another asset request is active.");
            return false;
        }
        if (!IsCanvasProjectId(Command.ProjectId))
        {
            Message = TEXT("A valid canvas project ID is required.");
            return false;
        }

        if (Command.Action == TEXT("copy"))
        {
            if (!IsCanvasNodeId(Command.NodeId))
            {
                Message = TEXT("A valid saved canvas node ID is required.");
                return false;
            }
            if (!BeginAssetCopy(Command.ProjectId, Command.NodeId))
            {
                Message = GetAssetStatus();
                return false;
            }
        }
        else if (Command.Action == TEXT("paste"))
        {
            if (!IsWritableDestination(Command.Destination))
            {
                Message = TEXT("The destination must be a writable folder under /Game.");
                return false;
            }
            const FCanvasAssetSource* Copied = AssetReceiver->GetCopied();
            if (!Command.CopyId.IsValid() || Command.CopyId != CommandCopyId || !Copied)
            {
                Message = TEXT("A current copy ID from this editor session is required. Copy the node again.");
                return false;
            }
            if (!Copied->ProjectId.Equals(Command.ProjectId, ESearchCase::CaseSensitive))
            {
                Message = TEXT("The requested project differs from the copied asset. Copy the node again.");
                return false;
            }
            const FCanvasAssetSource Source = *Copied;
            if (!PasteCanvasAsset(Command.Destination))
            {
                Message = GetAssetStatus();
                return false;
            }
            CommandSource = Source;
        }
        else
        {
            Message = TEXT("Unknown asset command. Use copy or paste.");
            return false;
        }

        PendingAssetCommand = Command.Id;
        return true;
    }

    bool ReadAssetCommandResult(const FAssetCommand& Command, bool& bOk, FString& Message,
        TOptional<FCanvasAssetSource>& Source)
    {
        if (PendingAssetCommand != Command.Id || !IsBrowserOnOrigin())
        {
            Message = TEXT("The editor's canvas changed or became unavailable during the asset command.");
            ResetAssetSelection();
            return true;
        }
        if (AssetReceiver->IsReceiving() || !PasteDestination.IsEmpty()) return false;

        if (Command.Action == TEXT("copy"))
        {
            if (const FCanvasAssetSource* Copied = AssetReceiver->GetCopied())
            {
                CommandCopyId = Command.Id;
                Source = *Copied;
                bOk = true;
            }
        }
        else if (bPasteSaved)
        {
            Source = MoveTemp(CommandSource);
            bOk = true;
        }
        Message = bOk && Command.Action == TEXT("copy") ? TEXT("Asset copied. Use copyId in the paste command.") : GetAssetStatus();
        PendingAssetCommand.Invalidate();
        CommandSource.Reset();
        return true;
    }

    virtual FReply OnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override
    {
        if (Event.GetKey() == EKeys::C && Event.IsControlDown() && Event.IsShiftDown()
            && !Event.IsAltDown() && !Event.IsCommandDown() && Browser.IsValid()
            && (Browser->HasKeyboardFocus() || Browser->HasFocusedDescendants()))
        {
            return Event.IsRepeat() ? FReply::Handled() : CopySelectedAsset();
        }
        return SCompoundWidget::OnPreviewKeyDown(Geometry, Event);
    }

private:
    bool IsBrowserOnOrigin() const
    {
        if (!Browser.IsValid()) return false;
        const FString Url = Browser->GetUrl();
        return Url == Origin || Url.StartsWith(Origin + TEXT("/"), ESearchCase::CaseSensitive);
    }

    bool CanReadAsset() const
    {
        return CanStartAssetRequest()
            && IsCanvasProjectId(CanvasProjectInput->GetText().ToString().TrimStartAndEnd());
    }

    bool CanStartAssetRequest() const
    {
        return IsBrowserOnOrigin() && !PendingAssetCommand.IsValid() && PasteDestination.IsEmpty() && !AssetReceiver->IsReceiving();
    }

    bool CanImportLegacyPng() const
    {
        return IsBrowserOnOrigin() && !PendingAssetCommand.IsValid() && PasteDestination.IsEmpty() && AssetReceiver->GetPrepared() != nullptr;
    }

    void ResetAssetSelection()
    {
        if (PasteTicker.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(PasteTicker);
            PasteTicker.Reset();
        }
        PasteDestination.Reset();
        PendingAssetCommand.Invalidate();
        CommandCopyId.Invalidate();
        CommandSource.Reset();
        bPasteSaved = false;
        AssetReceiver->Reset();
        AssetStatus.Reset();
    }

    void DisconnectAssetReceiver()
    {
        if (AssetReceiver.IsValid())
        {
            ResetAssetSelection();
            if (Browser.IsValid()) Browser->UnbindUObject(AssetReceiverBinding, AssetReceiver.Get(), true);
        }
    }

    FString GetAssetStatus() const
    {
        if (!AssetStatus.IsEmpty()) return AssetStatus;
        const FString& ReceiverStatus = AssetReceiver->GetStatus();
        return ReceiverStatus.IsEmpty() ? TEXT("Select one saved PNG, JPEG, or PCM WAV node to copy it into UE.") : ReceiverStatus;
    }

    bool LoadAssetScript(FString& Script)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RefmuseCanvas"));
        if (Plugin.IsValid() && FFileHelper::LoadFileToString(Script, *(Plugin->GetBaseDir() / TEXT("Resources/FetchCanvasAsset.js"))))
        {
            return true;
        }
        AssetStatus = TEXT("The plugin's canvas asset reader resource is missing.");
        return false;
    }

    FReply ReadSelectedPng()
    {
        if (!CanReadAsset()) return FReply::Handled();
        ResetAssetSelection();
        FString Script;
        if (!LoadAssetScript(Script)) return FReply::Handled();
        const FString ProjectId = CanvasProjectInput->GetText().ToString().TrimStartAndEnd();
        const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        AssetReceiver->Begin(ProjectId, RequestId);
        // All three arguments contain only native-validated URL/identifier
        // characters. The page never provides executable script to the editor.
        Browser->ExecuteJavascript(Script + FString::Printf(TEXT("('%s','%s','%s');"), *Origin, *ProjectId, *RequestId));
        return FReply::Handled();
    }

    FReply ImportSelectedPng()
    {
        if (!CanImportLegacyPng()) return FReply::Handled();
        const FPreparedCanvasAsset Image = AssetReceiver->TakePrepared();
        ImportCanvasAsset(Image, TEXT("/Game/RefmuseCanvas/") + Image.ProjectId, AssetStatus);
        return FReply::Handled();
    }

    FReply CopySelectedAsset()
    {
        if (CanStartAssetRequest()) BeginAssetCopy(FString(), FString());
        return FReply::Handled();
    }

    bool BeginAssetCopy(const FString& ProjectId, const FString& NodeId)
    {
        ResetAssetSelection();
        FString Script;
        if (!LoadAssetScript(Script)) return false;
        const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const TCHAR* Action = NodeId.IsEmpty() ? TEXT("copy") : TEXT("copy-node");
        AssetReceiver->BeginCopy(RequestId, ProjectId, NodeId);
        Browser->ExecuteJavascript(Script + FString::Printf(TEXT("('%s','%s','%s','%s','%s');"),
            *Origin, *ProjectId, *RequestId, Action, *NodeId));
        return true;
    }

    bool TickPaste(float)
    {
        if (AssetReceiver->IsReceiving() && IsBrowserOnOrigin()) return true;
        PasteTicker.Reset();
        const FString Destination = MoveTemp(PasteDestination);
        PasteDestination.Reset();
        bool bSaved = false;
        if (!IsBrowserOnOrigin() || !IsWritableDestination(Destination))
        {
            AssetStatus = TEXT("The canvas connection or destination is no longer available.");
            AssetReceiver->Reset();
        }
        else if (AssetReceiver->GetPrepared())
        {
            const FPreparedCanvasAsset Asset = AssetReceiver->TakePrepared();
            bSaved = ImportCanvasAsset(Asset, Destination, AssetStatus);
        }
        else
        {
            AssetStatus = AssetReceiver->GetStatus();
            AssetReceiver->Reset();
        }
        bPasteSaved = bSaved;
        ShowPasteResult(bSaved);
        return false;
    }

    void ShowPasteResult(bool bSaved) const
    {
        FNotificationInfo Info(FText::FromString(AssetStatus));
        Info.ExpireDuration = 5.0f;
        if (const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
        {
            Notification->SetCompletionState(bSaved ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        }
    }

    FReply Connect()
    {
        FString Validated;
        if (!NormalizeOrigin(OriginInput->GetText().ToString(), Validated))
        {
            Status = TEXT("Invalid origin. Use http://127.0.0.1:<port> or http://localhost:<port> (1-65535). No path or token.");
            return FReply::Handled();
        }
        // One editor tab owns one browser; reconnect never starts another service.
        DisconnectAssetReceiver();
        BrowserSlot->SetContent(SNullWidget::NullWidget);
        Browser.Reset();
        Origin = Validated;
        IWebBrowserSingleton* Singleton = IWebBrowserModule::Get().GetSingleton();
        if (!Singleton)
        {
            Status = TEXT("UE WebBrowser is unavailable. The local canvas service has not been changed.");
            return FReply::Handled();
        }
        const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
        const FTCHARToUTF8 ProjectUtf8(*ProjectPath);
        FMD5 ProjectHash;
        ProjectHash.Update(reinterpret_cast<const uint8*>(ProjectUtf8.Get()), ProjectUtf8.Length());
        uint8 ProjectDigest[16];
        ProjectHash.Final(ProjectDigest);
        FBrowserContextSettings Context(TEXT("RefmuseCanvas_") + BytesToHex(ProjectDigest, UE_ARRAY_COUNT(ProjectDigest)));
        // In-memory cookies/storage: never persist a backend session token or
        // share browser state with other UE projects. Canvas projects stay durable
        // in the backend; select the intended project after opening this panel.
        Context.bPersistSessionCookies = false;
        Context.bIgnoreCertificateErrors = false;
        FCreateBrowserWindowSettings Settings;
        Settings.InitialURL = Origin + TEXT("/");
        Settings.Context = Context;
        Settings.BrowserFrameRate = 30;
        TSharedPtr<IWebBrowserWindow> Window = Singleton->CreateBrowserWindow(Settings);
        if (!Window.IsValid())
        {
            Status = TEXT("UE could not create a browser window. See the version test receipt.");
            return FReply::Handled();
        }
        Status = TEXT("Loading local canvas...");
        BrowserSlot->SetContent(
            SAssignNew(Browser, SWebBrowser, Window)
            .ShowControls(false).ShowErrorMessage(false)
            .OnLoadStarted(this, &SCanvasPanel::ResetAssetSelection)
            .OnLoadCompleted(this, &SCanvasPanel::Loaded)
            .OnLoadError_Lambda([this] {
                ResetAssetSelection();
                Status = TEXT("Connection failed. Check the configured origin and start the backend separately, then reconnect.");
            })
            .OnBeforeNavigation_Lambda([this](const FString& Url, const FWebNavigationRequest&) {
                return !(Url == Origin || Url.StartsWith(Origin + TEXT("/")));
            })
            .OnBeforePopup_Lambda([](FString, FString) { return true; })
            // Never forward web console strings, URLs, page titles or credentials
            // to UE logs. Only a fixed test sentinel is recognized below.
            .OnConsoleMessage_Lambda([](const FString& Message, const FString&, int32, EWebBrowserConsoleLogSeverity) {
                if (Message == TEXT("REFMUSE_SMOKE_ROOT_OK")) UE_LOG(LogRefmuseCanvas, Display, TEXT("Smoke: React root rendered."));
                if (Message == TEXT("REFMUSE_SMOKE_CANVAS_OK")) UE_LOG(LogRefmuseCanvas, Display, TEXT("Smoke: canvas DOM rendered."));
                if (Message == TEXT("REFMUSE_SMOKE_API_OK")) UE_LOG(LogRefmuseCanvas, Display, TEXT("Smoke: authenticated projects API reachable."));
            })
        );
        Browser->BindUObject(AssetReceiverBinding, AssetReceiver.Get(), true);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConfigPath), true);
        GConfig->SetString(TEXT("Connection"), TEXT("Origin"), *Origin, ConfigPath);
        GConfig->Flush(false, ConfigPath);
        return FReply::Handled();
    }

    void Loaded()
    {
        Status = TEXT("Page loaded. Select the intended canvas project here. Backend errors remain visible in the canvas.");
        UE_LOG(LogRefmuseCanvas, Display, TEXT("Local page load completed (URL and page output omitted)."));
        if (FParse::Param(FCommandLine::Get(), TEXT("RefmuseCanvasSmoke")))
        {
            Browser->ExecuteJavascript(TEXT(
                "setTimeout(async()=>{"
                "if(document.querySelector('#root')?.children.length)console.log('REFMUSE_SMOKE_ROOT_OK');"
                "if(document.querySelector('.react-flow'))console.log('REFMUSE_SMOKE_CANVAS_OK');"
                "try{const r=await fetch('/api/projects');if(r.ok)console.log('REFMUSE_SMOKE_API_OK')}catch{}"
                "},3000);"));
        }
    }

    FString ConfigPath, Origin, Status, AssetStatus;
    TSharedPtr<SEditableTextBox> OriginInput, CanvasProjectInput;
    TSharedPtr<SBox> BrowserSlot;
    TSharedPtr<SWebBrowser> Browser;
    TStrongObjectPtr<URefmuseCanvasAssetReceiver> AssetReceiver;
    FString PasteDestination;
    FTSTicker::FDelegateHandle PasteTicker;
    FGuid PendingAssetCommand;
    FGuid CommandCopyId;
    TOptional<FCanvasAssetSource> CommandSource;
    bool bPasteSaved = false;
};
}

class FRefmuseCanvasModule : public IModuleInterface
{
public:
    virtual bool SupportsDynamicReloading() override { return false; }

    virtual void StartupModule() override
    {
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(RefmuseCanvas::TabId,
            FOnSpawnTab::CreateLambda([this](const FSpawnTabArgs&) {
                TSharedRef<RefmuseCanvas::SCanvasPanel> Panel = SNew(RefmuseCanvas::SCanvasPanel);
                CanvasPanel = Panel;
                return SNew(SDockTab).TabRole(ETabRole::NomadTab)[ Panel ];
            })).SetDisplayName(FText::FromString(TEXT("Refmuse Canvas")));
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRefmuseCanvasModule::RegisterMenu));
        if (FParse::Param(FCommandLine::Get(), TEXT("RefmuseCanvasAssetCommands")))
        {
            if (Inbox.Open())
            {
                UE_LOG(LogRefmuseCanvas, Display, TEXT("Asset command inbox: %s"), *Inbox.GetDirectory());
                CommandTicker = FTSTicker::GetCoreTicker().AddTicker(
                    FTickerDelegate::CreateRaw(this, &FRefmuseCanvasModule::TickAssetCommands), 0.25f);
            }
            else
            {
                UE_LOG(LogRefmuseCanvas, Warning, TEXT("Asset commands unavailable: could not create the project Saved inbox."));
            }
        }
        SmokeTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) {
            if (FParse::Param(FCommandLine::Get(), TEXT("RefmuseCanvasSmoke"))
                || FParse::Param(FCommandLine::Get(), TEXT("RefmuseCanvasAssetCommands")))
                FGlobalTabmanager::Get()->TryInvokeTab(RefmuseCanvas::TabId);
            return false;
        }), 2.0f);
        UE_LOG(LogRefmuseCanvas, Display, TEXT("Editor module started."));
    }

    virtual void ShutdownModule() override
    {
        FTSTicker::GetCoreTicker().RemoveTicker(SmokeTicker);
        if (CommandTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(CommandTicker);
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(RefmuseCanvas::TabId))
        {
            Tab->SetContent(SNullWidget::NullWidget);
            Tab->RequestCloseTab();
        }
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(RefmuseCanvas::TabId);
    }

private:
    bool TickAssetCommands(float)
    {
        const TSharedPtr<RefmuseCanvas::SCanvasPanel> Panel = CanvasPanel.Pin();
        FString Message = TEXT("The editor's canvas panel is not open.");
        if (PendingCommand.IsSet())
        {
            bool bOk = false;
            TOptional<RefmuseCanvas::FCanvasAssetSource> Source;
            if (!Panel || Panel->ReadAssetCommandResult(*PendingCommand, bOk, Message, Source))
            {
                FinishCommand(*PendingCommand, bOk, Message, Source);
                PendingCommand.Reset();
            }
            return true;
        }

        RefmuseCanvas::FAssetCommand Command;
        if (Inbox.Read(Command))
        {
            if (Panel && Panel->StartAssetCommand(Command, Message)) PendingCommand = MoveTemp(Command);
            else FinishCommand(Command, false, Message, {});
        }
        return true;
    }

    void FinishCommand(const RefmuseCanvas::FAssetCommand& Command, bool bOk, const FString& Message,
        const TOptional<RefmuseCanvas::FCanvasAssetSource>& Source)
    {
        Inbox.Reply(Command, bOk, Message, Source);
        UE_LOG(LogRefmuseCanvas, Display, TEXT("Asset command %s ok=%d: %s"),
            *Command.Id.ToString(EGuidFormats::Digits).ToLower(), bOk, *Message);
    }

    void RegisterMenu()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
        Menu->FindOrAddSection(TEXT("WindowLayout")).AddMenuEntry(TEXT("OpenRefmuseCanvas"),
            FText::FromString(TEXT("Refmuse Canvas")), FText::FromString(TEXT("Open the local Refmuse canvas")),
            FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([] {
                FGlobalTabmanager::Get()->TryInvokeTab(RefmuseCanvas::TabId);
            })));

        UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AddNewContextMenu"))
            ->FindOrAddSection(TEXT("RefmuseCanvas"), FText::GetEmpty(),
                FToolMenuInsert(NAME_None, EToolMenuInsertType::First))
            .AddDynamicEntry(TEXT("RefmuseCanvasPaste"),
                FNewToolMenuSectionDelegate::CreateRaw(this, &FRefmuseCanvasModule::AddPasteMenuEntry));
    }

    void AddPasteMenuEntry(FToolMenuSection& Section)
    {
        const TSharedPtr<RefmuseCanvas::SCanvasPanel> Panel = CanvasPanel.Pin();
        if (!Panel || !Panel->CanPasteCanvasAsset()) return;
        const auto* Context = Section.FindContext<UContentBrowserDataMenuContext_AddNewMenu>();
        if (!Context || !Context->bCanBeModified || Context->SelectedPaths.Num() != 1) return;

        UContentBrowserDataSubsystem* Data = IContentBrowserDataModule::Get().GetSubsystem();
        FName InternalPath;
        if (!Data || Data->TryConvertVirtualPath(Context->SelectedPaths[0], InternalPath) != EContentBrowserPathType::Internal) return;
        const FString Destination = InternalPath.ToString();
        if (!RefmuseCanvas::IsWritableDestination(Destination)) return;

        Section.AddMenuEntry(TEXT("PasteCanvasAsset"), FText::FromString(TEXT("Paste Canvas Asset")),
            FText::FromString(TEXT("Verify and save the copied asset in this folder.")), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakPanel = CanvasPanel, Destination] {
                if (const TSharedPtr<RefmuseCanvas::SCanvasPanel> LivePanel = WeakPanel.Pin())
                {
                    LivePanel->PasteCanvasAsset(Destination);
                }
            })));
    }

    TWeakPtr<RefmuseCanvas::SCanvasPanel> CanvasPanel;
    FTSTicker::FDelegateHandle SmokeTicker;
    FTSTicker::FDelegateHandle CommandTicker;
    RefmuseCanvas::FAssetCommandInbox Inbox;
    TOptional<RefmuseCanvas::FAssetCommand> PendingCommand;
};

IMPLEMENT_MODULE(FRefmuseCanvasModule, RefmuseCanvas)
