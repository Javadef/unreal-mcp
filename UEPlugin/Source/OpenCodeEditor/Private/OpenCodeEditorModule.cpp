#include "OpenCodeEditorModule.h"
#include "OpenCodeCoreModule.h"
#include "OpenCodeTCPServer.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "FOpenCodeEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogOpenCodeEditor, Log, All);

const FName FOpenCodeEditorModule::ChatTabId = FName(TEXT("OpenCodeChat"));

FOpenCodeEditorModule& FOpenCodeEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FOpenCodeEditorModule>(TEXT("OpenCodeEditor"));
}

// ===================== Chat Panel Widget =====================

class SOpenCodeChatPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOpenCodeChatPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Header", "OpenCode Bridge - Chat Panel"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(4)
			[
				SAssignNew(LogScrollBox, SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(LogTextBlock, STextBlock)
					.Text(LOCTEXT("Welcome", "OpenCode Bridge connected.\n\nTCP server running on port 3099.\nType a raw JSON command below to test:\n\n{\"id\":\"test\",\"tool\":\"ping\",\"args\":{}}"))
					.AutoWrapText(true)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SAssignNew(InputTextBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("InputHint", "Type JSON request..."))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Send", "Send"))
					.OnClicked(this, &SOpenCodeChatPanel::OnSendClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Clear", "Clear"))
					.OnClicked(this, &SOpenCodeChatPanel::OnClearClicked)
				]
			]
		];
	}

	FReply OnSendClicked()
	{
		FString Input = InputTextBox->GetText().ToString();
		if (!Input.IsEmpty())
		{
			AppendLog(TEXT(">>> ") + Input);

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Input);
			if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
			{
				FString Id;
				Root->TryGetStringField(TEXT("id"), Id);
			}

			FOpenCodeCoreModule& CoreModule = FOpenCodeCoreModule::Get();
			if (FOpenCodeTCPServer* Server = CoreModule.GetServer())
			{
				Server->SendMessage(Input);
				// Note: Response will arrive asynchronously via TCP.
				// For direct testing, we could also call CoreModule.HandleMessage synchronously.
			}

			InputTextBox->SetText(FText::GetEmpty());
		}
		return FReply::Handled();
	}

	FReply OnClearClicked()
	{
		LogTextBlock->SetText(FText::GetEmpty());
		return FReply::Handled();
	}

	static void AppendLog(const FString& Text)
	{
		if (Instance.IsValid())
		{
			FString Current = Instance->LogTextBlock->GetText().ToString();
			Current += TEXT("\n") + Text;
			Instance->LogTextBlock->SetText(FText::FromString(Current));
			Instance->LogScrollBox->ScrollToEnd();
		}
	}

	static TSharedPtr<SOpenCodeChatPanel> Instance;
	TSharedPtr<SScrollBox> LogScrollBox;
	TSharedPtr<STextBlock> LogTextBlock;
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
};

TSharedPtr<SOpenCodeChatPanel> SOpenCodeChatPanel::Instance;

// ===================== Module =====================

void FOpenCodeEditorModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ChatTabId,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			TSharedRef<SDockTab> Tab = SNew(SDockTab)
				.TabRole(ETabRole::NomadTab);

			TSharedRef<SOpenCodeChatPanel> Panel = SNew(SOpenCodeChatPanel);
			SOpenCodeChatPanel::Instance = Panel;
			Tab->SetContent(Panel);
			return Tab;
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "OpenCode Bridge"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& Section = Menu->FindOrAddSection("OpenCodeBridge");

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			FName(TEXT("OpenCodeBridge")),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				FGlobalTabmanager::Get()->TryInvokeTab(FOpenCodeEditorModule::ChatTabId);
			})),
			LOCTEXT("ToolbarButton", "OpenCode"),
			LOCTEXT("ToolbarTooltip", "Open the OpenCode Bridge chat panel"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Outliner")
		));
	}));

	UE_LOG(LogOpenCodeEditor, Log, TEXT("OpenCodeEditor module started"));
}

void FOpenCodeEditorModule::ShutdownModule()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ChatTabId);
	SOpenCodeChatPanel::Instance.Reset();
	UE_LOG(LogOpenCodeEditor, Log, TEXT("OpenCodeEditor module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenCodeEditorModule, OpenCodeEditor)
