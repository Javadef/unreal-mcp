#include "OpenCodeEditorModule.h"
#include "OpenCodeCoreModule.h"
#include "OpenCodeTCPServer.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"
#include "EditorFontGlyphs.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "Selection.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/PostProcessVolume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "FOpenCodeEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogOpenCodeEditor, Log, All);

const FName FOpenCodeEditorModule::ChatTabId = FName(TEXT("OpenCodeChat"));

FOpenCodeEditorModule& FOpenCodeEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FOpenCodeEditorModule>(TEXT("OpenCodeEditor"));
}

// ===================== Dashboard =====================
//
// The bridge's own tab: what it is doing, what the open level looks like to a
// renderer, and one-click versions of the tools an agent would otherwise have
// to be asked to run. It talks to the Core module through the console manager
// (the OpenCodeBridge.Stats variable and the OpenCodeBridge.Run command), so
// neither module needs new symbols from the other.

namespace OCDash
{
	// Palette. Slate wants linear colours; these are picked in sRGB.
	static FLinearColor Rgb(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return FLinearColor(FColor(R, G, B, A));
	}
	static const FLinearColor Background = Rgb(16, 18, 24);
	static const FLinearColor Card = Rgb(27, 30, 39);
	static const FLinearColor CardOutline = Rgb(46, 51, 64);
	static const FLinearColor Tile = Rgb(34, 38, 49);
	static const FLinearColor Text = Rgb(228, 232, 240);
	static const FLinearColor Muted = Rgb(138, 146, 163);
	static const FLinearColor Accent = Rgb(72, 201, 176);
	static const FLinearColor Blue = Rgb(98, 160, 255);
	static const FLinearColor Amber = Rgb(240, 180, 72);
	static const FLinearColor Red = Rgb(236, 94, 94);

	static FSlateFontInfo Font(const TCHAR* Typeface, int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle(Typeface, Size);
	}

	// Log lines worth showing on the dashboard: Live Coding, the bridge itself,
	// Python, and anything that is an error.
	class FLogTail : public FOutputDevice
	{
	public:
		struct FLine
		{
			FString Text;
			ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		};

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName Interesting[] = {
				FName(TEXT("LogLiveCoding")), FName(TEXT("LogOpenCodeCore")), FName(TEXT("LogOpenCodeEditor")), FName(TEXT("LogPython")),
			};
			bool bKeep = Verbosity <= ELogVerbosity::Error;
			for (const FName& Name : Interesting)
			{
				bKeep |= (Category == Name);
			}
			if (!bKeep)
			{
				return;
			}
			FScopeLock Lock(&Mutex);
			FLine Line;
			Line.Text = FString::Printf(TEXT("%s  %s: %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S")), *Category.ToString(), V).Left(260);
			Line.Verbosity = Verbosity;
			Lines.Add(MoveTemp(Line));
			if (Lines.Num() > 200)
			{
				Lines.RemoveAt(0, Lines.Num() - 200);
			}
			++Revision;
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		void Snapshot(TArray<FLine>& Out, int32 MaxLines, int32& OutRevision)
		{
			FScopeLock Lock(&Mutex);
			const int32 First = FMath::Max(0, Lines.Num() - MaxLines);
			Out.Reset();
			for (int32 i = First; i < Lines.Num(); ++i)
			{
				Out.Add(Lines[i]);
			}
			OutRevision = Revision;
		}

	private:
		FCriticalSection Mutex;
		TArray<FLine> Lines;
		int32 Revision = 0;
	};
}

class SOpenCodeDashboard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOpenCodeDashboard) {}
	SLATE_END_ARGS()

	SOpenCodeDashboard()
		: BackgroundBrush(OCDash::Background, 0.0f)
		, CardBrush(OCDash::Card, 8.0f, OCDash::CardOutline, 1.0f)
		, TileBrush(OCDash::Tile, 6.0f)
		, PillBrush(FLinearColor::White, 10.0f)
	{
	}

	virtual ~SOpenCodeDashboard()
	{
		if (GLog)
		{
			GLog->RemoveOutputDevice(&LogTail);
		}
	}

	void Construct(const FArguments& InArgs)
	{
		if (GLog)
		{
			GLog->AddOutputDevice(&LogTail);
		}

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(&BackgroundBrush)
			.Padding(0)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot().Padding(16, 14, 16, 6)[ BuildHeader() ]
				+ SScrollBox::Slot().Padding(12, 4, 12, 0)[ BuildTiles() ]
				+ SScrollBox::Slot().Padding(16, 8, 16, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()[ BuildLevelCard() ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)[ BuildRecentCard() ]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6, 0, 0, 0)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()[ BuildActionsCard() ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)[ BuildAssetCard() ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)[ BuildUsageCard() ]
					]
				]
				+ SScrollBox::Slot().Padding(16, 12, 16, 16)[ BuildLogCard() ]
			]
		];

		Refresh();
		RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SOpenCodeDashboard::OnTimer));
	}

private:
	// ---------------------------------------------------------------- building blocks

	TSharedRef<SWidget> Label(const FString& InText, const FLinearColor& Color, const TCHAR* Typeface = TEXT("Regular"), int32 Size = 9)
	{
		return SNew(STextBlock).Text(FText::FromString(InText)).ColorAndOpacity(Color).Font(OCDash::Font(Typeface, Size));
	}

	TSharedRef<SWidget> Glyph(const FText& InGlyph, const FLinearColor& Color, int32 Size = 11)
	{
		const FName Style = Size >= 14 ? FName(TEXT("FontAwesome.14")) : (Size >= 12 ? FName(TEXT("FontAwesome.12")) : FName(TEXT("FontAwesome.11")));
		return SNew(STextBlock).Text(InGlyph).ColorAndOpacity(Color).Font(FAppStyle::Get().GetFontStyle(Style));
	}

	// A titled card; `Body` goes under the title row.
	TSharedRef<SWidget> MakeCard(const FText& Icon, const FString& Title, const FString& Subtitle, TSharedRef<SWidget> Body)
	{
		return SNew(SBorder)
			.BorderImage(&CardBrush)
			.Padding(FMargin(14, 12))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)[ Glyph(Icon, OCDash::Accent, 12) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Label(Title, OCDash::Text, TEXT("Bold"), 11) ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10, 1, 0, 0)[ Label(Subtitle, OCDash::Muted, TEXT("Regular"), 8) ]
				]
				+ SVerticalBox::Slot().AutoHeight()[ Body ]
			];
	}

	// "key        value" row whose value is read live.
	TSharedRef<SWidget> InfoRow(const FString& Key, TFunction<FString()> Value, TFunction<FLinearColor()> Color = nullptr)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 2)
			[
				SNew(SBox).WidthOverride(118.0f)[ Label(Key, OCDash::Muted) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 2)
			[
				SNew(STextBlock)
				.Font(OCDash::Font(TEXT("Regular"), 9))
				.AutoWrapText(true)
				.Text_Lambda([Value]() { return FText::FromString(Value()); })
				.ColorAndOpacity_Lambda([Color]() { return FSlateColor(Color ? Color() : OCDash::Text); })
			];
	}

	// Big-number tile.
	TSharedRef<SWidget> StatTile(const FText& Icon, const FString& Caption, TFunction<FString()> Value, TFunction<FLinearColor()> Color = nullptr)
	{
		return SNew(SBox)
			.WidthOverride(150.0f)
			.Padding(FMargin(4))
			[
				SNew(SBorder)
				.BorderImage(&TileBrush)
				.Padding(FMargin(12, 10))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[ Glyph(Icon, OCDash::Muted, 11) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Label(Caption, OCDash::Muted, TEXT("Regular"), 8) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
					[
						SNew(STextBlock)
						.Font(OCDash::Font(TEXT("Bold"), 18))
						.Text_Lambda([Value]() { return FText::FromString(Value()); })
						.ColorAndOpacity_Lambda([Color]() { return FSlateColor(Color ? Color() : OCDash::Text); })
					]
				]
			];
	}

	TSharedRef<SWidget> ActionButton(const FText& Icon, const FString& Caption, const FString& Tooltip, TFunction<void()> OnClick, bool bPrimary = false)
	{
		return SNew(SBox)
			.Padding(FMargin(3))
			[
				SNew(SButton)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>(bPrimary ? TEXT("PrimaryButton") : TEXT("Button")))
				.ContentPadding(FMargin(10, 6))
				.ToolTipText(FText::FromString(Tooltip))
				.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 7, 0)[ Glyph(Icon, FLinearColor::White, 11) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Label(Caption, FLinearColor::White, TEXT("Regular"), 9) ]
				]
			];
	}

	// ---------------------------------------------------------------- sections

	TSharedRef<SWidget> BuildHeader()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				Glyph(FEditorFontGlyphs::Plug, OCDash::Accent, 14)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ Label(TEXT("OpenCode Bridge"), OCDash::Text, TEXT("Bold"), 16) ]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(OCDash::Font(TEXT("Regular"), 8))
					.ColorAndOpacity(OCDash::Muted)
					.Text_Lambda([this]() { return FText::FromString(HeaderLine); })
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(SSpacer) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				// Status pill: tinted by state.
				SNew(SBorder)
				.BorderImage(&PillBrush)
				.BorderBackgroundColor_Lambda([this]() { return FSlateColor((bServerRunning ? OCDash::Accent : OCDash::Red).CopyWithNewOpacity(0.22f)); })
				.Padding(FMargin(12, 5))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 7, 0)
					[
						SNew(STextBlock)
						.Font(FAppStyle::Get().GetFontStyle(TEXT("FontAwesome.9")))
						.Text(FEditorFontGlyphs::Circle)
						.ColorAndOpacity_Lambda([this]() { return FSlateColor(bServerRunning ? OCDash::Accent : OCDash::Red); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(OCDash::Font(TEXT("Bold"), 9))
						.ColorAndOpacity(OCDash::Text)
						.Text_Lambda([this]() { return FText::FromString(bServerRunning ? TEXT("LISTENING  127.0.0.1:3099") : TEXT("SERVER STOPPED")); })
					]
				]
			];
	}

	TSharedRef<SWidget> BuildTiles()
	{
		return SNew(SWrapBox)
			.UseAllottedSize(true)
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Terminal, TEXT("REQUESTS"), [this]() { return FString::FromInt(Total); }) ]
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Exclamation_Triangle, TEXT("FAILED"), [this]() { return FString::FromInt(Failed); },
				[this]() { return Failed > 0 ? OCDash::Red : OCDash::Text; }) ]
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Refresh, TEXT("AVG RESPONSE"), [this]() { return FString::Printf(TEXT("%.1f ms"), AvgMs); }) ]
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Cogs, TEXT("TOOLS"), [this]() { return RegisteredTools > 0 ? FString::FromInt(RegisteredTools) : FString(TEXT("-")); }) ]
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Database, TEXT("ACTORS"), [this]() { return FString::FromInt(ActorCount); }) ]
			+ SWrapBox::Slot()[ StatTile(FEditorFontGlyphs::Check_Circle, TEXT("SELECTED"), [this]() { return FString::FromInt(SelectedCount); },
				[this]() { return SelectedCount > 0 ? OCDash::Blue : OCDash::Text; }) ];
	}

	TSharedRef<SWidget> BuildLevelCard()
	{
		return MakeCard(FEditorFontGlyphs::Lightbulb_O, TEXT("Level"), TEXT("what a renderer would need to match"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Level"), [this]() { return LevelName; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Selection"), [this]() { return SelectionText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Sun"), [this]() { return SunText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Sky light"), [this]() { return SkyText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Height fog"), [this]() { return FogText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Post process"), [this]() { return PostText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Renderer"), [this]() { return RendererText; }) ]
			+ SVerticalBox::Slot().AutoHeight()[ InfoRow(TEXT("Grass"), [this]() { return GrassText; }) ]);
	}

	TSharedRef<SWidget> BuildRecentCard()
	{
		return MakeCard(FEditorFontGlyphs::Terminal, TEXT("Recent calls"), TEXT("newest first"),
			SAssignNew(RecentBox, SVerticalBox));
	}

	TSharedRef<SWidget> BuildUsageCard()
	{
		return MakeCard(FEditorFontGlyphs::Database, TEXT("Tool usage"), TEXT("calls this session"),
			SAssignNew(UsageBox, SVerticalBox));
	}

	TSharedRef<SWidget> BuildLogCard()
	{
		return MakeCard(FEditorFontGlyphs::Info_Circle, TEXT("Activity"), TEXT("Live Coding, Python, bridge and errors"),
			SAssignNew(LogBox, SVerticalBox));
	}

	TSharedRef<SWidget> BuildActionsCard()
	{
		return MakeCard(FEditorFontGlyphs::Play, TEXT("Quick actions"), TEXT("results go to Saved/OpenCodeBridge"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Play, TEXT("Compile"), TEXT("Run a Live Coding compile and wait for it"),
					[this]() { Exec(TEXT("LiveCoding.Compile")); Note(TEXT("Live Coding compile finished - see Activity below")); }, true) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Lightbulb_O, TEXT("Dump lighting"), TEXT("Lights, sky, fog and post-process overrides of this level, as JSON"),
					[this]() { RunTool(TEXT("get_level_lighting"), TEXT("level_lighting.json"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Database, TEXT("Dump scene"), TEXT("Every actor with class and transform, as JSON"),
					[this]() { RunTool(TEXT("get_scene_hierarchy"), TEXT("scene_hierarchy.json"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Folder_Open, TEXT("Open output"), TEXT("Open Saved/OpenCodeBridge in Explorer"),
					[this]() { OpenFolder(OutputDir()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Cogs, TEXT("Plugin source"), TEXT("Open the plugin's folder in Explorer"),
					[this]() { OpenFolder(PluginDir()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Download, TEXT("Copy MCP config"), TEXT("Copy an .mcp.json entry for this bridge to the clipboard"),
					[this]() { CopyMcpConfig(); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(3, 8, 3, 0)
			[
				SNew(STextBlock)
				.Font(OCDash::Font(TEXT("Regular"), 8))
				.AutoWrapText(true)
				.Text_Lambda([this]() { return FText::FromString(LastNote); })
				.ColorAndOpacity_Lambda([this]() { return FSlateColor(bLastNoteIsError ? OCDash::Red : OCDash::Accent); })
			]);
	}

	TSharedRef<SWidget> BuildAssetCard()
	{
		return MakeCard(FEditorFontGlyphs::Download, TEXT("Asset tools"), TEXT("dump one asset to a file"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(3, 0, 3, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(AssetPathBox, SEditableTextBox)
					.HintText(LOCTEXT("AssetHint", "/Game/Path/To/Asset"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.ContentPadding(FMargin(8, 4))
					.ToolTipText(LOCTEXT("FromSelectionTip", "Use the selected actor's static mesh (or its first material when Shift is held)"))
					.OnClicked_Lambda([this]() { UseSelection(); return FReply::Handled(); })
					[
						Label(TEXT("From selection"), FLinearColor::White)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Cogs, TEXT("Material graph"), TEXT("Node graph of a Material or MaterialFunction"),
					[this]() { RunAssetTool(TEXT("get_material_graph"), TEXT("graph.json"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Check, TEXT("Parameters"), TEXT("Effective parameter values of a Material or MaterialInstance"),
					[this]() { RunAssetTool(TEXT("get_material_parameters"), TEXT("params.json"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Terminal, TEXT("HLSL"), TEXT("The HLSL Unreal generates for this material, static switches resolved"),
					[this]() { RunAssetTool(TEXT("get_material_hlsl"), TEXT("hlsl"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Database, TEXT("Mesh data"), TEXT("LOD 0 vertices, all UV channels and indices of a StaticMesh"),
					[this]() { RunAssetTool(TEXT("get_static_mesh_data"), TEXT("mesh.json"), TEXT(",\"includeData\":true")); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Info_Circle, TEXT("Texture info"), TEXT("Runtime format, size, alpha and per-channel statistics of a Texture"),
					[this]() { RunAssetTool(TEXT("get_texture_info"), TEXT("texture.json"), FString()); }) ]
				+ SWrapBox::Slot()[ ActionButton(FEditorFontGlyphs::Download, TEXT("T3D export"), TEXT("Every non-default property of any asset, as text"),
					[this]() { RunAssetTool(TEXT("export_asset_text"), TEXT("t3d"), FString()); }) ]
			]);
	}

	// ---------------------------------------------------------------- actions

	static FString OutputDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OpenCodeBridge"));
	}

	static FString PluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OpenCodeBridge"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir());
	}

	void Note(const FString& InText, bool bError = false)
	{
		LastNote = InText;
		bLastNoteIsError = bError;
	}

	static void Exec(const FString& Command)
	{
		if (GEngine)
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			GEngine->Exec(World, *Command);
		}
	}

	static void OpenFolder(const FString& Directory)
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		FPlatformProcess::ExploreFolder(*Directory);
	}

	// Run a bridge tool through its console command, result to a file.
	void RunTool(const FString& ToolName, const FString& FileName, const FString& JsonArgs)
	{
		if (!IConsoleManager::Get().IsNameRegistered(TEXT("OpenCodeBridge.Run")))
		{
			Note(TEXT("The bridge has not registered its console command yet - send it any request (or restart the editor) first"), true);
			return;
		}
		const FString File = OutputDir() / FileName;
		IFileManager::Get().Delete(*File, false, true, true);
		Exec(FString::Printf(TEXT("OpenCodeBridge.Run %s out=%s %s"), *ToolName, *FileName, *JsonArgs));
		if (IFileManager::Get().FileExists(*File))
		{
			Note(FString::Printf(TEXT("%s  ->  %s  (%lld bytes)"), *ToolName, *File, IFileManager::Get().FileSize(*File)));
		}
		else
		{
			Note(FString::Printf(TEXT("%s failed - see the Output Log"), *ToolName), true);
		}
	}

	void RunAssetTool(const FString& ToolName, const FString& Suffix, const FString& ExtraJson)
	{
		const FString Path = AssetPathBox.IsValid() ? AssetPathBox->GetText().ToString().TrimStartAndEnd() : FString();
		if (Path.IsEmpty())
		{
			Note(TEXT("Enter an asset path first, or use 'From selection'"), true);
			return;
		}
		if (Path.Contains(TEXT(" ")) || Path.Contains(TEXT("\"")))
		{
			Note(TEXT("Asset paths with spaces or quotes cannot be passed through the console command"), true);
			return;
		}
		const FString Name = FPaths::GetBaseFilename(Path);
		RunTool(ToolName, FString::Printf(TEXT("%s.%s"), *Name, *Suffix), FString::Printf(TEXT("{\"assetPath\":\"%s\"%s}"), *Path, *ExtraJson));
	}

	void UseSelection()
	{
		if (!GEditor || !AssetPathBox.IsValid())
		{
			return;
		}
		const bool bWantMaterial = FSlateApplication::Get().GetModifierKeys().IsShiftDown();
		FString Path;
		if (USelection* Selection = GEditor->GetSelectedActors())
		{
			if (AActor* Actor = Selection->GetTop<AActor>())
			{
				if (const UStaticMeshComponent* Component = Actor->FindComponentByClass<UStaticMeshComponent>())
				{
					if (bWantMaterial)
					{
						if (const UMaterialInterface* Material = Component->GetMaterial(0))
						{
							Path = Material->GetOutermost()->GetName();
						}
					}
					else if (const UStaticMesh* Mesh = Component->GetStaticMesh())
					{
						Path = Mesh->GetOutermost()->GetName();
					}
				}
			}
		}
		if (Path.IsEmpty())
		{
			if (USelection* Objects = GEditor->GetSelectedObjects())
			{
				if (const UObject* Object = Objects->GetTop<UObject>())
				{
					Path = Object->GetOutermost()->GetName();
				}
			}
		}
		if (Path.IsEmpty())
		{
			Note(TEXT("Nothing usable is selected: pick an actor with a static mesh"), true);
			return;
		}
		AssetPathBox->SetText(FText::FromString(Path));
		Note(FString::Printf(TEXT("Asset: %s"), *Path));
	}

	void CopyMcpConfig()
	{
		// The MCP server says where it lives when it connects; hand out that copy.
		FString Server;
		if (const IConsoleVariable* Client = IConsoleManager::Get().FindConsoleVariable(TEXT("OpenCodeBridge.Client")))
		{
			Server = Client->GetString();
		}
		if (Server.IsEmpty())
		{
			FPlatformApplicationMisc::ClipboardCopy(TEXT(""));
			Note(TEXT("No MCP server has connected to the bridge yet, so its path is not known. Start your agent once, then copy again."), true);
			return;
		}
		FString Json = FString::Printf(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal-bridge\": {\n      \"command\": \"node\",\n      \"args\": [\"%s\"],\n"
			"      \"env\": { \"UEOC_HOST\": \"127.0.0.1\", \"UEOC_PORT\": \"3099\" }\n    }\n  }\n}\n"),
			*Server.Replace(TEXT("\\"), TEXT("/")));
		FPlatformApplicationMisc::ClipboardCopy(*Json);
		Note(FString::Printf(TEXT("MCP config copied to the clipboard  (%s)"), *Server));
	}

	// ---------------------------------------------------------------- refresh

	EActiveTimerReturnType OnTimer(double, float)
	{
		Refresh();
		return EActiveTimerReturnType::Continue;
	}

	static FString CVar(const TCHAR* Name)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var ? Var->GetString() : FString(TEXT("?"));
	}

	void Refresh()
	{
		// --- bridge ---
		bServerRunning = false;
		if (FModuleManager::Get().IsModuleLoaded(TEXT("OpenCodeCore")))
		{
			if (const FOpenCodeTCPServer* Server = FOpenCodeCoreModule::Get().GetServer())
			{
				bServerRunning = Server->IsRunning();
			}
		}

		FString Build = TEXT("?");
		TSharedPtr<FJsonObject> Stats;
		if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("OpenCodeBridge.Stats")))
		{
			const FString Json = Var->GetString();
			if (Json != LastStatsJson)
			{
				LastStatsJson = Json;
				bStatsChanged = true;
			}
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			FJsonSerializer::Deserialize(Reader, Stats);
		}
		if (Stats.IsValid())
		{
			Total = (int32)Stats->GetNumberField(TEXT("total"));
			Failed = (int32)Stats->GetNumberField(TEXT("failed"));
			AvgMs = Stats->GetNumberField(TEXT("avgMs"));
			RegisteredTools = (int32)Stats->GetNumberField(TEXT("registeredTools"));
			Stats->TryGetStringField(TEXT("build"), Build);
		}
		HeaderLine = FString::Printf(TEXT("%s   |   UE %s   |   plugin built %s   |   MCP server: %s"),
			FApp::GetProjectName(), *FEngineVersion::Current().ToString(EVersionComponent::Patch), *Build,
			CVar(TEXT("OpenCodeBridge.Client")).IsEmpty() || CVar(TEXT("OpenCodeBridge.Client")) == TEXT("?")
				? TEXT("not seen yet") : *CVar(TEXT("OpenCodeBridge.Client")));

		// --- level ---
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		ActorCount = World ? World->GetActorCount() : 0;
		LevelName = World ? World->GetOutermost()->GetName() : FString(TEXT("(no level)"));
		SelectedCount = GEditor ? GEditor->GetSelectedActorCount() : 0;
		SelectionText = TEXT("nothing selected");
		if (GEditor && SelectedCount > 0)
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				if (const AActor* Actor = Selection->GetTop<AActor>())
				{
					SelectionText = FString::Printf(TEXT("%s  (%s)%s"), *Actor->GetActorLabel(), *Actor->GetClass()->GetName(),
						SelectedCount > 1 ? *FString::Printf(TEXT("  +%d more"), SelectedCount - 1) : TEXT(""));
				}
			}
		}

		SunText = TEXT("none");
		SkyText = TEXT("none");
		FogText = TEXT("none");
		for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && World && !It->IsTemplate())
			{
				const FRotator Rotation = It->GetComponentRotation();
				const FColor Color = It->GetLightColor().ToFColor(true);
				SunText = FString::Printf(TEXT("%.2f lux   pitch %.1f  yaw %.1f   rgb(%d, %d, %d)"), It->Intensity, Rotation.Pitch, Rotation.Yaw, Color.R, Color.G, Color.B);
				break;
			}
		}
		for (TObjectIterator<USkyLightComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && World && !It->IsTemplate())
			{
				SkyText = FString::Printf(TEXT("intensity %.2f   %s"), It->Intensity, It->bRealTimeCapture ? TEXT("real-time capture") : TEXT("captured scene"));
				break;
			}
		}
		for (TObjectIterator<UExponentialHeightFogComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && World && !It->IsTemplate())
			{
				FogText = FString::Printf(TEXT("density %.3f   starts at %.0f m"), It->FogDensity, It->StartDistance / 100.0f);
				break;
			}
		}
		int32 Volumes = 0;
		int32 UnboundVolumes = 0;
		for (TObjectIterator<APostProcessVolume> It; It; ++It)
		{
			if (It->GetWorld() == World && World && !It->IsTemplate())
			{
				++Volumes;
				UnboundVolumes += It->bUnbound ? 1 : 0;
			}
		}
		PostText = Volumes == 0 ? FString(TEXT("no volumes")) : FString::Printf(TEXT("%d volume%s (%d unbound)"), Volumes, Volumes == 1 ? TEXT("") : TEXT("s"), UnboundVolumes);

		static const TCHAR* GiNames[] = { TEXT("none"), TEXT("Lumen"), TEXT("screen space"), TEXT("plugin") };
		static const TCHAR* AaNames[] = { TEXT("none"), TEXT("FXAA"), TEXT("TAA"), TEXT("MSAA"), TEXT("TSR"), TEXT("SMAA") };
		const int32 Gi = FCString::Atoi(*CVar(TEXT("r.DynamicGlobalIlluminationMethod")));
		const int32 Aa = FCString::Atoi(*CVar(TEXT("r.AntiAliasingMethod")));
		RendererText = FString::Printf(TEXT("GI: %s   AA: %s   virtual shadow maps: %s"),
			Gi >= 0 && Gi < UE_ARRAY_COUNT(GiNames) ? GiNames[Gi] : TEXT("?"),
			Aa >= 0 && Aa < UE_ARRAY_COUNT(AaNames) ? AaNames[Aa] : TEXT("?"),
			CVar(TEXT("r.Shadow.Virtual.Enable")) == TEXT("1") ? TEXT("on") : TEXT("off"));
		GrassText = FString::Printf(TEXT("density x%s   cull distance x%s"), *CVar(TEXT("grass.DensityScale")), *CVar(TEXT("grass.CullDistanceScale")));

		// --- lists (rebuilt only when their data changed) ---
		if (bStatsChanged)
		{
			bStatsChanged = false;
			RebuildRecent(Stats);
			RebuildUsage(Stats);
		}
		RebuildLog();
	}

	void RebuildRecent(const TSharedPtr<FJsonObject>& Stats)
	{
		if (!RecentBox.IsValid())
		{
			return;
		}
		RecentBox->ClearChildren();
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		if (!Stats.IsValid() || !Stats->TryGetArrayField(TEXT("recent"), Recent) || !Recent || Recent->Num() == 0)
		{
			RecentBox->AddSlot().AutoHeight()[ Label(TEXT("No requests yet. The log fills in as an agent calls the bridge."), OCDash::Muted) ];
			return;
		}
		const int32 First = FMath::Max(0, Recent->Num() - 12);
		for (int32 i = Recent->Num() - 1; i >= First; --i)
		{
			const TSharedPtr<FJsonObject> Call = (*Recent)[i]->AsObject();
			if (!Call.IsValid())
			{
				continue;
			}
			const bool bOk = Call->GetBoolField(TEXT("ok"));
			RecentBox->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					Glyph(bOk ? FEditorFontGlyphs::Check_Circle : FEditorFontGlyphs::Times_Circle, bOk ? OCDash::Accent : OCDash::Red, 11)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(62.0f)[ Label(Call->GetStringField(TEXT("time")), OCDash::Muted, TEXT("Mono"), 8) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ Label(Call->GetStringField(TEXT("tool")), OCDash::Text, TEXT("Mono"), 9) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Label(FString::Printf(TEXT("%.1f ms"), Call->GetNumberField(TEXT("ms"))), OCDash::Muted, TEXT("Mono"), 8)
				]
			];
		}
	}

	void RebuildUsage(const TSharedPtr<FJsonObject>& Stats)
	{
		if (!UsageBox.IsValid())
		{
			return;
		}
		UsageBox->ClearChildren();
		const TSharedPtr<FJsonObject>* Tools = nullptr;
		if (!Stats.IsValid() || !Stats->TryGetObjectField(TEXT("tools"), Tools) || !Tools || (*Tools)->Values.Num() == 0)
		{
			UsageBox->AddSlot().AutoHeight()[ Label(TEXT("Nothing called yet."), OCDash::Muted) ];
			return;
		}
		TArray<TPair<FString, int32>> Counts;
		int32 Most = 1;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Tools)->Values)
		{
			const int32 Count = (int32)Pair.Value->AsNumber();
			Counts.Emplace(Pair.Key, Count);
			Most = FMath::Max(Most, Count);
		}
		Counts.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
		for (int32 i = 0; i < FMath::Min(Counts.Num(), 8); ++i)
		{
			UsageBox->AddSlot().AutoHeight().Padding(0, 3)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(170.0f)[ Label(Counts[i].Key, OCDash::Text, TEXT("Mono"), 8) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SBox).HeightOverride(6.0f)
					[
						SNew(SProgressBar).Percent((float)Counts[i].Value / (float)Most).FillColorAndOpacity(OCDash::Accent)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(34.0f)[ Label(FString::FromInt(Counts[i].Value), OCDash::Muted, TEXT("Mono"), 8) ]
				]
			];
		}
	}

	void RebuildLog()
	{
		if (!LogBox.IsValid())
		{
			return;
		}
		TArray<OCDash::FLogTail::FLine> Lines;
		int32 Revision = 0;
		LogTail.Snapshot(Lines, 10, Revision);
		if (Revision == ShownLogRevision && LogBox->NumSlots() > 0)
		{
			return;
		}
		ShownLogRevision = Revision;
		LogBox->ClearChildren();
		if (Lines.Num() == 0)
		{
			LogBox->AddSlot().AutoHeight()[ Label(TEXT("Quiet so far. Compiles, Python output and errors show up here."), OCDash::Muted) ];
			return;
		}
		for (const OCDash::FLogTail::FLine& Line : Lines)
		{
			const FLinearColor Color = Line.Verbosity <= ELogVerbosity::Error ? OCDash::Red
				: (Line.Verbosity == ELogVerbosity::Warning ? OCDash::Amber : OCDash::Muted);
			LogBox->AddSlot().AutoHeight().Padding(0, 1)[ Label(Line.Text, Color, TEXT("Mono"), 8) ];
		}
	}

	// ---------------------------------------------------------------- state

	FSlateRoundedBoxBrush BackgroundBrush;
	FSlateRoundedBoxBrush CardBrush;
	FSlateRoundedBoxBrush TileBrush;
	FSlateRoundedBoxBrush PillBrush;

	OCDash::FLogTail LogTail;
	int32 ShownLogRevision = -1;

	TSharedPtr<SVerticalBox> RecentBox;
	TSharedPtr<SVerticalBox> UsageBox;
	TSharedPtr<SVerticalBox> LogBox;
	TSharedPtr<SEditableTextBox> AssetPathBox;

	bool bServerRunning = false;
	FString HeaderLine;
	FString LastStatsJson = TEXT("<unset>");
	bool bStatsChanged = true;
	int32 Total = 0;
	int32 Failed = 0;
	double AvgMs = 0.0;
	int32 RegisteredTools = 0;
	int32 ActorCount = 0;
	int32 SelectedCount = 0;
	FString LevelName;
	FString SelectionText;
	FString SunText;
	FString SkyText;
	FString FogText;
	FString PostText;
	FString RendererText;
	FString GrassText;
	FString LastNote = TEXT("Ready.");
	bool bLastNoteIsError = false;
};

// A named function, so a Live Coding patch of the tab's contents always takes:
// the spawner registered at startup only has to keep calling this.
static TSharedRef<SDockTab> SpawnOpenCodeTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SOpenCodeDashboard)
		];
}

// ===================== Module =====================

void FOpenCodeEditorModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ChatTabId,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			return SpawnOpenCodeTab(Args);
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
			LOCTEXT("ToolbarTooltip", "Open the OpenCode Bridge dashboard"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Outliner")
		));
	}));

	UE_LOG(LogOpenCodeEditor, Log, TEXT("OpenCodeEditor module started"));
}

void FOpenCodeEditorModule::ShutdownModule()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ChatTabId);
	UE_LOG(LogOpenCodeEditor, Log, TEXT("OpenCodeEditor module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenCodeEditorModule, OpenCodeEditor)
