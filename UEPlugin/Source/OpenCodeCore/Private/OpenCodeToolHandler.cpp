#include "OpenCodeToolHandler.h"
#include "OpenCodeCoreModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Selection.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "Misc/PackageName.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "LandscapeGrassType.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "InstancedFoliageActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/LocalLightComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Exporters/Exporter.h"
#include "MaterialShared.h"
#include "PixelFormat.h"
#include "RHIFeatureLevel.h"
#include "Math/Float16.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#if PLATFORM_WINDOWS
// Header-only use (the interface is pure virtual), so no module dependency.
#include "Developer/Windows/LiveCoding/Public/ILiveCodingModule.h"
#endif
#if WITH_EDITOR
#include "IPythonScriptPlugin.h"
#endif

// Tools implemented at the end of this file as free functions (see there).
static FOpenCodeResponse OCHandleGetStaticMeshData(const FOpenCodeRequest& Request);
static FOpenCodeResponse OCHandleGetTextureInfo(const FOpenCodeRequest& Request);
static FOpenCodeResponse OCHandleGetLevelLighting(const FOpenCodeRequest& Request);
static FOpenCodeResponse OCHandleGetMaterialHlsl(const FOpenCodeRequest& Request);
static FOpenCodeResponse OCHandleExportAssetText(const FOpenCodeRequest& Request);
static FOpenCodeResponse OCHandleLiveCompile(const FOpenCodeRequest& Request);

// ======================= REQUEST LOG + CONSOLE HOOKS =======================
// The editor dashboard lives in another module, and a Live Coding patch cannot
// add symbols for another module to link against. Both can already reach the
// console manager, so that is the channel:
//   OpenCodeBridge.Stats          string variable: a JSON snapshot of the request log
//   OpenCodeBridge.Run <tool> ..  command: run a tool, result to a file or the log
namespace OCBridge
{
	struct FCall
	{
		FString Tool;
		FString Time;
		double Ms = 0.0;
		bool bOk = false;
	};

	static TArray<FCall> RecentCalls;
	static TMap<FString, int32> ToolCounts;
	static int32 TotalCalls = 0;
	static int32 FailedCalls = 0;
	static double TotalMs = 0.0;

	static FString OutputDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OpenCodeBridge"));
	}

	// Relative paths land in <Project>/Saved/OpenCodeBridge.
	static FString ResolveOutputPath(const FString& Path)
	{
		FString Full = FPaths::IsRelative(Path) ? OutputDir() / Path : Path;
		FPaths::NormalizeFilename(Full);
		return Full;
	}

	static void RecordCall(const FString& Tool, double Ms, bool bOk, int32 RegisteredTools)
	{
		++TotalCalls;
		if (!bOk)
		{
			++FailedCalls;
		}
		TotalMs += Ms;
		ToolCounts.FindOrAdd(Tool)++;

		FCall Call;
		Call.Tool = Tool;
		Call.Time = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
		Call.Ms = Ms;
		Call.bOk = bOk;
		RecentCalls.Add(Call);
		if (RecentCalls.Num() > 40)
		{
			RecentCalls.RemoveAt(0, RecentCalls.Num() - 40);
		}

		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("OpenCodeBridge.Stats"));
		if (!Var)
		{
			return;
		}
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
		Root->SetNumberField(TEXT("total"), TotalCalls);
		Root->SetNumberField(TEXT("failed"), FailedCalls);
		Root->SetNumberField(TEXT("avgMs"), TotalCalls > 0 ? TotalMs / TotalCalls : 0.0);
		Root->SetNumberField(TEXT("registeredTools"), RegisteredTools);
		Root->SetStringField(TEXT("build"), FString(TEXT(__DATE__)) + TEXT(" ") + TEXT(__TIME__));
		TSharedPtr<FJsonObject> Counts = MakeShareable(new FJsonObject());
		for (const TPair<FString, int32>& Pair : ToolCounts)
		{
			Counts->SetNumberField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("tools"), Counts);
		TArray<TSharedPtr<FJsonValue>> Recent;
		for (const FCall& Entry : RecentCalls)
		{
			TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
			Obj->SetStringField(TEXT("tool"), Entry.Tool);
			Obj->SetStringField(TEXT("time"), Entry.Time);
			Obj->SetNumberField(TEXT("ms"), Entry.Ms);
			Obj->SetBoolField(TEXT("ok"), Entry.bOk);
			Recent.Add(MakeShareable(new FJsonValueObject(Obj)));
		}
		Root->SetArrayField(TEXT("recent"), Recent);
		Var->Set(*JSON_OBJ_TO_STRING(Root), ECVF_SetByCode);
	}

	// OpenCodeBridge.Run <tool> [out=<file>] [<json args, no spaces needed>]
	static void RunCommand(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("Usage: OpenCodeBridge.Run <tool> [out=<file>] [<json args>]"));
			return;
		}
		FOpenCodeRequest Request;
		Request.Id = TEXT("console");
		Request.Tool = Args[0];
		Request.Args = MakeShareable(new FJsonObject());

		FString OutFile;
		FString JsonText;
		for (int32 i = 1; i < Args.Num(); ++i)
		{
			if (Args[i].StartsWith(TEXT("out=")))
			{
				OutFile = Args[i].RightChop(4);
			}
			else
			{
				if (!JsonText.IsEmpty())
				{
					JsonText += TEXT(" ");
				}
				JsonText += Args[i];
			}
		}
		if (!JsonText.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				Request.Args = Parsed;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("OpenCodeBridge.Run: could not parse the arguments as JSON: %s"), *JsonText);
				return;
			}
		}
		if (!OutFile.IsEmpty())
		{
			Request.Args->SetStringField(TEXT("outputFile"), OutFile);
		}

		const FOpenCodeResponse Response = FOpenCodeToolHandler::Get().Dispatch(Request);
		UE_LOG(LogTemp, Display, TEXT("OpenCodeBridge.Run %s -> %s"), *Request.Tool, *Response.ToJson().Left(1500));
	}

	static void EnsureConsoleHooks()
	{
		IConsoleManager& Console = IConsoleManager::Get();
		if (!Console.FindConsoleVariable(TEXT("OpenCodeBridge.Stats")))
		{
			Console.RegisterConsoleVariable(TEXT("OpenCodeBridge.Stats"), FString(TEXT("{}")),
				TEXT("JSON snapshot of the OpenCode bridge's request log. Read by the dashboard tab."), ECVF_Default);
		}
		if (!Console.FindConsoleVariable(TEXT("OpenCodeBridge.Client")))
		{
			Console.RegisterConsoleVariable(TEXT("OpenCodeBridge.Client"), FString(),
				TEXT("Path of the MCP server script that last connected to the bridge (it says so when it connects)."), ECVF_Default);
		}
		if (!Console.IsNameRegistered(TEXT("OpenCodeBridge.Run")))
		{
			Console.RegisterConsoleCommand(TEXT("OpenCodeBridge.Run"),
				TEXT("OpenCodeBridge.Run <tool> [out=<file>] [<json args>] - run a bridge tool. The result goes to the file (relative paths: Saved/OpenCodeBridge) or the log."),
				FConsoleCommandWithArgsDelegate::CreateStatic(&RunCommand), ECVF_Default);
		}
	}
}

FOpenCodeToolHandler& FOpenCodeToolHandler::Get()
{
	static FOpenCodeToolHandler Instance;
	return Instance;
}

void FOpenCodeToolHandler::RegisterTools()
{
	RegisterTool(TEXT("ping"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandlePing));
	RegisterTool(TEXT("get_project_structure"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetProjectStructure));
	RegisterTool(TEXT("get_scene_hierarchy"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetSceneHierarchy));
	RegisterTool(TEXT("execute_console_command"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleExecuteConsoleCommand));
	RegisterTool(TEXT("search_assets"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleSearchAssets));
	RegisterTool(TEXT("get_asset_details"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetAssetDetails));
	RegisterTool(TEXT("get_class_details"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetClassDetails));
	RegisterTool(TEXT("get_module_dependencies"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetModuleDependencies));
	RegisterTool(TEXT("get_plugin_list"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetPluginList));
	RegisterTool(TEXT("get_output_log"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetOutputLog));
	RegisterTool(TEXT("get_build_logs"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetBuildLogs));
	RegisterTool(TEXT("get_compilation_status"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetCompilationStatus));
	RegisterTool(TEXT("get_blueprint_list"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetBlueprintList));
	RegisterTool(TEXT("get_selected_actors"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetSelectedActors));
	RegisterTool(TEXT("get_actor_details"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetActorDetails));
	RegisterTool(TEXT("set_actor_property"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleSetActorProperty));
	RegisterTool(TEXT("generate_code"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGenerateCode));
	RegisterTool(TEXT("search_classes"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleSearchClasses));
	RegisterTool(TEXT("get_cpp_hierarchy"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetCppHierarchy));
	RegisterTool(TEXT("get_material_graph"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetMaterialGraph));
	RegisterTool(TEXT("get_material_parameters"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleGetMaterialParameters));
	RegisterTool(TEXT("run_python"), FOpenCodeToolDelegate::CreateStatic(&FOpenCodeToolHandler::HandleRunPython));
	RegisterTool(TEXT("get_static_mesh_data"), FOpenCodeToolDelegate::CreateStatic(&OCHandleGetStaticMeshData));
	RegisterTool(TEXT("get_texture_info"), FOpenCodeToolDelegate::CreateStatic(&OCHandleGetTextureInfo));
	RegisterTool(TEXT("get_level_lighting"), FOpenCodeToolDelegate::CreateStatic(&OCHandleGetLevelLighting));
	RegisterTool(TEXT("get_material_hlsl"), FOpenCodeToolDelegate::CreateStatic(&OCHandleGetMaterialHlsl));
	RegisterTool(TEXT("export_asset_text"), FOpenCodeToolDelegate::CreateStatic(&OCHandleExportAssetText));
	RegisterTool(TEXT("live_compile"), FOpenCodeToolDelegate::CreateStatic(&OCHandleLiveCompile));

	OCBridge::EnsureConsoleHooks();
}

void FOpenCodeToolHandler::RegisterTool(const FString& ToolName, FOpenCodeToolDelegate Delegate)
{
	ToolDelegates.Add(ToolName, Delegate);
}

FOpenCodeResponse FOpenCodeToolHandler::Dispatch(const FOpenCodeRequest& Request)
{
	OCBridge::EnsureConsoleHooks();

	// The MCP server introduces itself on connect, so the dashboard can show
	// (and hand out) the copy that is actually in use. Not a tool; not counted.
	if (Request.Tool == TEXT("hello"))
	{
		FString Server;
		if (Request.Args.IsValid() && Request.Args->TryGetStringField(TEXT("server"), Server))
		{
			if (IConsoleVariable* Client = IConsoleManager::Get().FindConsoleVariable(TEXT("OpenCodeBridge.Client")))
			{
				Client->Set(*Server, ECVF_SetByCode);
			}
		}
		return FOpenCodeResponse::Success(Request.Id, MakeShareable(new FJsonObject()));
	}

	FOpenCodeToolDelegate* Found = ToolDelegates.Find(Request.Tool);
	if (!Found)
	{
		// A Live Coding patch can add tools after startup: pick them up.
		RegisterTools();
		Found = ToolDelegates.Find(Request.Tool);
	}

	const double StartSeconds = FPlatformTime::Seconds();
	FOpenCodeResponse Response;
	if (!Found)
	{
		Response = FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Unknown tool: %s"), *Request.Tool));
	}
	else if (!Found->IsBound())
	{
		Response = FOpenCodeResponse::Failure(Request.Id, TEXT("Tool delegate not bound"));
	}
	else
	{
		Response = Found->Execute(Request);
	}

	// Any tool: "outputFile" sends the result to disk and returns only where it
	// went. A result carrying a `text` field (HLSL, T3D) is written as that text;
	// anything else as its JSON.
	FString OutputFile;
	if (Response.bSuccess && Response.Data.IsValid() && Request.Args.IsValid()
		&& Request.Args->TryGetStringField(TEXT("outputFile"), OutputFile) && !OutputFile.IsEmpty())
	{
		const FString FullPath = OCBridge::ResolveOutputPath(OutputFile);
		FString Payload;
		TSharedPtr<FJsonObject> Summary = MakeShareable(new FJsonObject());
		if (Response.Data->TryGetStringField(TEXT("text"), Payload))
		{
			// Keep the small fields around the text as the summary.
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Response.Data->Values)
			{
				if (Field.Key != TEXT("text"))
				{
					Summary->SetField(Field.Key, Field.Value);
				}
			}
		}
		else
		{
			Payload = JSON_OBJ_TO_STRING(Response.Data);
		}
		if (FFileHelper::SaveStringToFile(Payload, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Summary->SetStringField(TEXT("savedTo"), FullPath);
			Summary->SetNumberField(TEXT("chars"), Payload.Len());
			Response.Data = Summary;
		}
		else
		{
			Response = FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Could not write %s"), *FullPath));
		}
	}

	OCBridge::RecordCall(Request.Tool, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, Response.bSuccess, ToolDelegates.Num());
	return Response;
}

// ======================= TOOL IMPLEMENTATIONS =======================

static TSharedPtr<FJsonObject> ActorToJson(AActor* Actor)
{
	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	if (!Actor) return Obj;

	Obj->SetStringField(TEXT("name"), Actor->GetName());
	Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	FVector Loc = Actor->GetActorLocation();
	FRotator Rot = Actor->GetActorRotation();
	FVector Scale = Actor->GetActorScale3D();
	TSharedPtr<FJsonObject> Transform = MakeShareable(new FJsonObject());
	Transform->SetStringField(TEXT("location"), FString::Printf(TEXT("%.2f, %.2f, %.2f"), Loc.X, Loc.Y, Loc.Z));
	Transform->SetStringField(TEXT("rotation"), FString::Printf(TEXT("%.2f, %.2f, %.2f"), Rot.Pitch, Rot.Yaw, Rot.Roll));
	Transform->SetStringField(TEXT("scale"), FString::Printf(TEXT("%.2f, %.2f, %.2f"), Scale.X, Scale.Y, Scale.Z));
	Obj->SetObjectField(TEXT("transform"), Transform);

	AActor* Parent = Actor->GetAttachParentActor();
	if (Parent)
	{
		Obj->SetStringField(TEXT("parent"), Parent->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> Components;
	TArray<UActorComponent*> ActorComponents;
	Actor->GetComponents(ActorComponents);
	for (UActorComponent* Comp : ActorComponents)
	{
		if (Comp)
		{
			Components.Add(MakeShareable(new FJsonValueString(Comp->GetClass()->GetName())));
		}
	}
	Obj->SetArrayField(TEXT("components"), Components);

	TArray<TSharedPtr<FJsonValue>> Tags;
	for (const FName& Tag : Actor->Tags)
	{
		Tags.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
	}
	Obj->SetArrayField(TEXT("tags"), Tags);

	return Obj;
}

FOpenCodeResponse FOpenCodeToolHandler::HandlePing(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("status"), TEXT("pong"));
	Data->SetStringField(TEXT("pluginVersion"), TEXT("1.0.0"));
	// Build provenance: the compiled DLL can lag (or lead) whichever source
	// checkout you're reading. These answer "which source, compiled when?"
	// before you spend an hour editing a file the editor never loaded.
	Data->SetStringField(TEXT("buildTimestamp"), TEXT(__DATE__ " " __TIME__));
	Data->SetStringField(TEXT("compiledFrom"), ANSI_TO_TCHAR(__FILE__));
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetProjectStructure(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());

	const FString ProjectDir = FPaths::ProjectDir();
	Data->SetStringField(TEXT("projectRoot"), ProjectDir);

	// .uproject path
	TArray<FString> UProjectFiles;
	IFileManager::Get().FindFiles(UProjectFiles, *(ProjectDir / TEXT("*.uproject")), true, false);
	FString UProjectFile = (UProjectFiles.Num() > 0) ? (ProjectDir / UProjectFiles[0]) : TEXT("");
	Data->SetStringField(TEXT("uprojectPath"), UProjectFile);

	// Modules
	TArray<TSharedPtr<FJsonValue>> Modules;
	FModuleManager& ModuleManager = FModuleManager::Get();
	TArray<FModuleStatus> ModuleStatuses;
	ModuleManager.QueryModules(ModuleStatuses);
	for (const FModuleStatus& Status : ModuleStatuses)
	{
		TSharedPtr<FJsonObject> Mod = MakeShareable(new FJsonObject());
		Mod->SetStringField(TEXT("name"), Status.Name);
		Mod->SetBoolField(TEXT("isLoaded"), Status.bIsLoaded);
		Modules.Add(MakeShareable(new FJsonValueObject(Mod)));
	}
	Data->SetArrayField(TEXT("modules"), Modules);

	// Plugins
	TArray<TSharedPtr<FJsonValue>> Plugins;
	TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetStringField(TEXT("name"), Plugin->GetName());
		P->SetStringField(TEXT("friendlyName"), Plugin->GetDescriptor().FriendlyName);
		P->SetStringField(TEXT("version"), Plugin->GetDescriptor().VersionName);
		Plugins.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Data->SetArrayField(TEXT("plugins"), Plugins);

	// Content directories (top level)
	TArray<TSharedPtr<FJsonValue>> ContentDirs;
	const FString ContentPath = FPaths::ProjectContentDir();
	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *(ContentPath / TEXT("*")), false, true);
	for (const FString& Dir : Directories)
	{
		ContentDirs.Add(MakeShareable(new FJsonValueString(Dir)));
	}
	Data->SetArrayField(TEXT("contentDirectories"), ContentDirs);

	// Source modules
	TArray<TSharedPtr<FJsonValue>> SourceModules;
	const FString SourcePath = FPaths::ProjectDir() / TEXT("Source");
	TArray<FString> SourceDirs;
	IFileManager::Get().FindFiles(SourceDirs, *(SourcePath / TEXT("*")), false, true);
	for (const FString& Dir : SourceDirs)
	{
		SourceModules.Add(MakeShareable(new FJsonValueString(Dir)));
	}
	Data->SetArrayField(TEXT("sourceModules"), SourceModules);

	Data->SetStringField(TEXT("projectName"), FString(FApp::GetProjectName()));

	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetSceneHierarchy(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Actors;
	TArray<TSharedPtr<FJsonValue>> LandscapeActors;
	TArray<TSharedPtr<FJsonValue>> FoliageActors;

	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !IsValid(Actor)) continue;

			TSharedPtr<FJsonObject> ActorObj = ActorToJson(Actor);

			if (ALandscape* Landscape = Cast<ALandscape>(Actor))
			{
				ActorObj->SetStringField(TEXT("type"), TEXT("Landscape"));

				int32 ComponentCount = Landscape->LandscapeComponents.Num();
				ActorObj->SetNumberField(TEXT("componentCount"), ComponentCount);

				if (Landscape->LandscapeComponents.Num() > 0)
				{
					ULandscapeComponent* FirstComp = Landscape->LandscapeComponents[0];
					if (FirstComp)
					{
						ActorObj->SetNumberField(TEXT("componentSizeQuads"), FirstComp->ComponentSizeQuads);
						ActorObj->SetNumberField(TEXT("subsectionSizeQuads"), FirstComp->SubsectionSizeQuads);
						ActorObj->SetNumberField(TEXT("numSubsections"), FirstComp->NumSubsections);
					}
				}

				UMaterialInterface* LandscapeMat = Landscape->GetLandscapeMaterial();
				ActorObj->SetStringField(TEXT("material"), LandscapeMat ? LandscapeMat->GetPathName() : TEXT("None"));

				FVector Scale = Landscape->GetActorScale3D();
				ActorObj->SetStringField(TEXT("scale"), Scale.ToString());

				LandscapeActors.Add(MakeShareable(new FJsonValueObject(ActorObj)));
			}
			else if (AInstancedFoliageActor* Foliage = Cast<AInstancedFoliageActor>(Actor))
			{
				ActorObj->SetStringField(TEXT("type"), TEXT("InstancedFoliageActor"));

				int32 TotalInstances = 0;
				TArray<TSharedPtr<FJsonValue>> MeshEntries;

				for (const auto& FoliagePair : Foliage->GetFoliageInfos())
				{
					UFoliageType* FoliageType = FoliagePair.Key;
					const FFoliageInfo& FoliageInfo = FoliagePair.Value.Get();

					if (!FoliageType) continue;

					TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject());
					Entry->SetStringField(TEXT("mesh"), FoliageType->GetSource() ? FoliageType->GetSource()->GetPathName() : TEXT("None"));
					Entry->SetStringField(TEXT("foliageType"), FoliageType->GetClass()->GetName());

					int32 InstanceCount = FoliageInfo.Instances.Num();
					Entry->SetNumberField(TEXT("instanceCount"), InstanceCount);
					TotalInstances += InstanceCount;

				// Sample all instance transforms (no limit)
				TArray<TSharedPtr<FJsonValue>> Samples;
				for (int32 i = 0; i < InstanceCount; ++i)
					{
						const FFoliageInstance& Inst = FoliageInfo.Instances[i];
						TSharedPtr<FJsonObject> S = MakeShareable(new FJsonObject());
						S->SetStringField(TEXT("location"), Inst.Location.ToString());
						S->SetStringField(TEXT("rotation"), Inst.Rotation.ToString());
						S->SetStringField(TEXT("drawScale3D"), Inst.DrawScale3D.ToString());
						Samples.Add(MakeShareable(new FJsonValueObject(S)));
					}
				Entry->SetArrayField(TEXT("instances"), Samples);
				MeshEntries.Add(MakeShareable(new FJsonValueObject(Entry)));
				}

				ActorObj->SetNumberField(TEXT("totalInstances"), TotalInstances);
				ActorObj->SetArrayField(TEXT("foliageMeshes"), MeshEntries);
				FoliageActors.Add(MakeShareable(new FJsonValueObject(ActorObj)));
			}
			else
			{
				Actors.Add(MakeShareable(new FJsonValueObject(ActorObj)));
			}
		}
	}
	Data->SetArrayField(TEXT("actors"), Actors);
	Data->SetArrayField(TEXT("landscapes"), LandscapeActors);
	Data->SetArrayField(TEXT("foliage"), FoliageActors);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleExecuteConsoleCommand(const FOpenCodeRequest& Request)
{
	FString Command;
	Request.Args->TryGetStringField(TEXT("command"), Command);

	if (Command.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No command provided"));
	}

	if (GEngine)
	{
		GEngine->Exec(GWorld, *Command);
	}
	else if (GEditor)
	{
		GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command);
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("command"), Command);
	Data->SetBoolField(TEXT("executed"), true);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleSearchAssets(const FOpenCodeRequest& Request)
{
	FString Query;
	int32 Limit = 50;
	int32 Offset = 0;
	FString AssetTypeFilter;
	FString PathPrefix;

	Request.Args->TryGetStringField(TEXT("query"), Query);
	Request.Args->TryGetNumberField(TEXT("limit"), Limit);
	Request.Args->TryGetNumberField(TEXT("offset"), Offset);
	Request.Args->TryGetStringField(TEXT("assetType"), AssetTypeFilter);
	Request.Args->TryGetStringField(TEXT("pathPrefix"), PathPrefix);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	bool bIncludeInstances = false;
	Request.Args->TryGetBoolField(TEXT("includeInstances"), bIncludeInstances);

	TArray<FString> SearchedRoots;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	else
	{
		// No prefix: search every mounted content root (project + plugins) except
		// /Engine, so projects whose content lives under a plugin mount (not /Game)
		// are found without the caller knowing the mount name.
		TArray<FString> RootPaths;
		FPackageName::QueryRootContentPaths(RootPaths);
		for (FString Root : RootPaths)
		{
			Root.RemoveFromEnd(TEXT("/"));
			if (Root.StartsWith(TEXT("/Engine")))
			{
				continue;
			}
			Filter.PackagePaths.Add(*Root);
			SearchedRoots.Add(Root);
		}
	}

	// assetType handling. When the string resolves to a real UClass we use the
	// registry's (recursive) class filter — fast and includes subclasses. When
	// it does NOT resolve (typo, or a class outside these two modules), we fall
	// back to a class-NAME substring match applied in the loop below instead of
	// silently dropping the filter (the old behaviour, which returned every
	// asset and looked like the filter was ignored).
	bool bAssetTypeResolved = false;
	if (!AssetTypeFilter.IsEmpty())
	{
		// Resolve by bare name across all loaded modules, native classes first.
		UClass* Class = FindFirstObject<UClass>(*AssetTypeFilter, EFindFirstObjectOptions::NativeFirst);
		if (Class)
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(Class));
			bAssetTypeResolved = true;
			// "Material" is UMaterial only; instances are a sibling class tree.
			if (bIncludeInstances && Class == UMaterial::StaticClass())
			{
				Filter.ClassPaths.Add(FTopLevelAssetPath(UMaterialInstance::StaticClass()));
			}
		}
	}

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// Filter by query. `TotalMatches` counts everything that passes the query
	// filter (for pagination), independent of the Offset/Limit slice that
	// actually lands in `Results` — these used to be conflated, which made
	// `total` report the pre-query package count instead of the real match
	// count (confusing when it disagreed with an empty `results`).
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 TotalMatches = 0;
	int32 Skipped = 0;
	int32 Added = 0;
	for (const FAssetData& Asset : AssetList)
	{
		if (!Query.IsEmpty() && !Asset.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase))
		{
			continue;
		}
		// Fallback assetType filter: only runs when the class string didn't
		// resolve to a UClass above. Substring, case-insensitive, against the
		// asset's class name (e.g. "MaterialInstance" matches
		// "MaterialInstanceConstant").
		if (!AssetTypeFilter.IsEmpty() && !bAssetTypeResolved &&
			!Asset.AssetClassPath.GetAssetName().ToString().Contains(AssetTypeFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		TotalMatches++;

		if (Skipped < Offset)
		{
			Skipped++;
			continue;
		}
		if (Added >= Limit) continue;

		TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		Results.Add(MakeShareable(new FJsonValueObject(Item)));
		Added++;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetNumberField(TEXT("total"), TotalMatches);
	if (PathPrefix.IsEmpty())
	{
		Data->SetStringField(TEXT("note"), FString::Printf(TEXT("No pathPrefix given — searched all content roots except /Engine: %s"), *FString::Join(SearchedRoots, TEXT(", "))));
	}
	else if (!AssetTypeFilter.IsEmpty() && !bAssetTypeResolved)
	{
		Data->SetStringField(TEXT("note"), FString::Printf(
			TEXT("assetType '%s' is not a UClass in /Script/Engine or /Script/CoreUObject — matched by class-name substring instead. For an exact class filter, pass the C++ class name (e.g. StaticMesh, Texture2D, MaterialInstanceConstant)."),
			*AssetTypeFilter));
	}
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetAssetDetails(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);

	if (AssetPath.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No assetPath provided"));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());

	if (!Asset)
	{
		Data->SetStringField(TEXT("error"), TEXT("Asset not found"));
		Data->SetStringField(TEXT("path"), AssetPath);
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	Data->SetStringField(TEXT("name"), Asset->GetName());
	Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Data->SetStringField(TEXT("path"), Asset->GetPathName());
	Data->SetStringField(TEXT("outer"), Asset->GetOutermost()->GetName());

	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("Blueprint"));
		Data->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None"));
		Data->SetStringField(TEXT("blueprintType"), StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(BP->BlueprintType)));
	}
	else if (UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("MaterialInstanceConstant"));
		if (MI->Parent)
		{
			Data->SetStringField(TEXT("parent"), MI->Parent->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ScalarParams;
		for (const FScalarParameterValue& Val : MI->ScalarParameterValues)
		{
			TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetStringField(TEXT("name"), Val.ParameterInfo.Name.ToString());
			P->SetNumberField(TEXT("value"), Val.ParameterValue);
			ScalarParams.Add(MakeShareable(new FJsonValueObject(P)));
		}
		Data->SetArrayField(TEXT("scalarParameters"), ScalarParams);

		TArray<TSharedPtr<FJsonValue>> VectorParams;
		for (const FVectorParameterValue& Val : MI->VectorParameterValues)
		{
			TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetStringField(TEXT("name"), Val.ParameterInfo.Name.ToString());
			P->SetStringField(TEXT("value"), FString::Printf(TEXT("(%f, %f, %f, %f)"),
				Val.ParameterValue.R, Val.ParameterValue.G, Val.ParameterValue.B, Val.ParameterValue.A));
			VectorParams.Add(MakeShareable(new FJsonValueObject(P)));
		}
		Data->SetArrayField(TEXT("vectorParameters"), VectorParams);

		TArray<TSharedPtr<FJsonValue>> TextureParams;
		for (const FTextureParameterValue& Val : MI->TextureParameterValues)
		{
			TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetStringField(TEXT("name"), Val.ParameterInfo.Name.ToString());
			P->SetStringField(TEXT("value"), Val.ParameterValue ? Val.ParameterValue->GetPathName() : TEXT("None"));
			TextureParams.Add(MakeShareable(new FJsonValueObject(P)));
		}
		Data->SetArrayField(TEXT("textureParameters"), TextureParams);

		TArray<TSharedPtr<FJsonValue>> StaticSwitches;
		if (UMaterial* ParentMat = Cast<UMaterial>(MI->Parent))
		{
			TArray<UMaterialExpression*> AllExpressions;
			ParentMat->GetAllExpressionsInMaterialAndFunctionsOfType<UMaterialExpression>(AllExpressions);
			for (UMaterialExpression* Expr : AllExpressions)
			{
				if (UMaterialExpressionStaticSwitchParameter* SwitchParam = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
					P->SetStringField(TEXT("name"), SwitchParam->ParameterName.ToString());
					P->SetBoolField(TEXT("value"), SwitchParam->DefaultValue);
					StaticSwitches.Add(MakeShareable(new FJsonValueObject(P)));
				}
			}
		}
		Data->SetArrayField(TEXT("staticSwitchParameters"), StaticSwitches);
	}
	else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("StaticMesh"));
		Data->SetNumberField(TEXT("numLODs"), Mesh->GetNumLODs());
		Data->SetNumberField(TEXT("totalTriangles"), Mesh->GetNumTriangles(0));

		TArray<TSharedPtr<FJsonValue>> LODs;
		if (Mesh->GetRenderData())
		{
			for (int32 LODIdx = 0; LODIdx < Mesh->GetRenderData()->LODResources.Num(); ++LODIdx)
			{
				const FStaticMeshLODResources& LODRes = Mesh->GetRenderData()->LODResources[LODIdx];
				TSharedPtr<FJsonObject> LOD = MakeShareable(new FJsonObject());
				LOD->SetNumberField(TEXT("index"), LODIdx);
				LOD->SetNumberField(TEXT("triangles"), LODRes.GetNumTriangles());
				LOD->SetNumberField(TEXT("vertices"), LODRes.GetNumVertices());
				LOD->SetNumberField(TEXT("sections"), LODRes.Sections.Num());

				float ScreenSize = Mesh->GetSourceModel(LODIdx).ScreenSize.Default;
				LOD->SetNumberField(TEXT("screenSize"), ScreenSize);
				LODs.Add(MakeShareable(new FJsonValueObject(LOD)));
			}
		}
		Data->SetArrayField(TEXT("lods"), LODs);
	}
	else if (UTexture2D* Tex = Cast<UTexture2D>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("Texture2D"));
		Data->SetNumberField(TEXT("width"), Tex->GetSizeX());
		Data->SetNumberField(TEXT("height"), Tex->GetSizeY());
	}
	else if (UMaterial* Mat = Cast<UMaterial>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("Material"));
		Data->SetStringField(TEXT("blendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Mat->BlendMode)));
		Data->SetStringField(TEXT("shadingModel"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(Mat->GetShadingModels().GetFirstShadingModel())));
		Data->SetBoolField(TEXT("twoSided"), Mat->TwoSided != 0);
		Data->SetNumberField(TEXT("opacityMaskClipValue"), Mat->OpacityMaskClipValue);

		TArray<TSharedPtr<FJsonValue>> StaticSwitches;
		TArray<TSharedPtr<FJsonValue>> ScalarParams;
		TArray<TSharedPtr<FJsonValue>> VectorParams;

		TArray<UMaterialExpression*> AllExpressions;
		Mat->GetAllExpressionsInMaterialAndFunctionsOfType<UMaterialExpression>(AllExpressions);
		for (UMaterialExpression* Expr : AllExpressions)
		{
			if (UMaterialExpressionStaticSwitchParameter* SwitchParam = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
			{
				TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
				P->SetStringField(TEXT("name"), SwitchParam->ParameterName.ToString());
				P->SetBoolField(TEXT("value"), SwitchParam->DefaultValue);
				StaticSwitches.Add(MakeShareable(new FJsonValueObject(P)));
			}
			else if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
				P->SetStringField(TEXT("name"), Scalar->ParameterName.ToString());
				P->SetNumberField(TEXT("value"), Scalar->DefaultValue);
				P->SetStringField(TEXT("group"), Scalar->Group.ToString());
				ScalarParams.Add(MakeShareable(new FJsonValueObject(P)));
			}
			else if (UMaterialExpressionVectorParameter* Vec = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
				P->SetStringField(TEXT("name"), Vec->ParameterName.ToString());
				P->SetStringField(TEXT("value"), FString::Printf(TEXT("(%f, %f, %f)"),
					Vec->DefaultValue.R, Vec->DefaultValue.G, Vec->DefaultValue.B));
				P->SetStringField(TEXT("group"), Vec->Group.ToString());
				VectorParams.Add(MakeShareable(new FJsonValueObject(P)));
			}
			else if (UMaterialExpressionConstant* Const = Cast<UMaterialExpressionConstant>(Expr))
			{
				// Only report if it has a non-default description (hides internal constants)
				if (!Const->Desc.IsEmpty())
				{
					TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
					P->SetStringField(TEXT("desc"), Const->Desc);
					P->SetNumberField(TEXT("value"), Const->R);
					ScalarParams.Add(MakeShareable(new FJsonValueObject(P)));
				}
			}
		}
		Data->SetArrayField(TEXT("staticSwitchParameters"), StaticSwitches);
		Data->SetArrayField(TEXT("scalarParameters"), ScalarParams);
		Data->SetArrayField(TEXT("vectorParameters"), VectorParams);
	}
	else if (UMaterialFunction* MF = Cast<UMaterialFunction>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
		TArray<TSharedPtr<FJsonValue>> Inputs;

		TArrayView<const TObjectPtr<UMaterialExpression>> Expressions = MF->GetExpressions();
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Expressions)
		{
				UMaterialExpression* Expr = ExprPtr.Get();
				if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expr))
				{
					TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
					P->SetStringField(TEXT("name"), Input->InputName.ToString());
					P->SetStringField(TEXT("type"), Input->InputType == FunctionInput_Vector4 ? TEXT("Vector4") :
						Input->InputType == FunctionInput_Vector3 ? TEXT("Vector3") :
						Input->InputType == FunctionInput_Vector2 ? TEXT("Vector2") :
						Input->InputType == FunctionInput_Scalar ? TEXT("Scalar") :
						Input->InputType == FunctionInput_StaticBool ? TEXT("StaticBool") :
						Input->InputType == FunctionInput_Texture2D ? TEXT("Texture2D") : TEXT("Unknown"));
					P->SetStringField(TEXT("previewValue"), Input->PreviewValue.ToString());
					P->SetStringField(TEXT("description"), Input->Desc);
					P->SetNumberField(TEXT("sortPriority"), Input->SortPriority);
					Inputs.Add(MakeShareable(new FJsonValueObject(P)));
				}
			}
			Data->SetArrayField(TEXT("functionInputs"), Inputs);
	}
	else if (ULandscapeGrassType* GrassType = Cast<ULandscapeGrassType>(Asset))
	{
		Data->SetStringField(TEXT("type"), TEXT("LandscapeGrassType"));
		TArray<TSharedPtr<FJsonValue>> Varieties;
		for (const FGrassVariety& Var : GrassType->GrassVarieties)
		{
			TSharedPtr<FJsonObject> V = MakeShareable(new FJsonObject());
			V->SetStringField(TEXT("mesh"), Var.GrassMesh ? Var.GrassMesh->GetPathName() : TEXT("None"));
			V->SetNumberField(TEXT("grassDensity"), Var.GrassDensity.GetValue());
			V->SetNumberField(TEXT("startCullDistance"), (double)Var.StartCullDistance.GetValue());
			V->SetNumberField(TEXT("endCullDistance"), (double)Var.EndCullDistance.GetValue());
			V->SetBoolField(TEXT("randomRotation"), Var.RandomRotation);
			V->SetBoolField(TEXT("alignToSurface"), Var.AlignToSurface);
			V->SetNumberField(TEXT("minLOD"), Var.MinLOD);
			V->SetStringField(TEXT("scaling"), FString::Printf(TEXT("X=(%.2f,%.2f) Y=(%.2f,%.2f) Z=(%.2f,%.2f)"),
				Var.ScaleX.Min, Var.ScaleX.Max,
				Var.ScaleY.Min, Var.ScaleY.Max,
				Var.ScaleZ.Min, Var.ScaleZ.Max));
			Varieties.Add(MakeShareable(new FJsonValueObject(V)));
		}
		Data->SetArrayField(TEXT("grassVarieties"), Varieties);
	}

	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetClassDetails(const FOpenCodeRequest& Request)
{
	FString ClassName;
	Request.Args->TryGetStringField(TEXT("className"), ClassName);

	if (ClassName.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No className provided"));
	}

	UClass* Class = FindObject<UClass>(nullptr, *ClassName, true);
	if (!Class)
	{
		Class = FindObject<UClass>(nullptr, *(TEXT("/Script/Engine.") + ClassName));
	}
	if (!Class)
	{
		Class = FindObject<UClass>(nullptr, *(TEXT("/Script/CoreUObject.") + ClassName));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());

	if (!Class)
	{
		Data->SetStringField(TEXT("error"), FString::Printf(TEXT("Class %s not found"), *ClassName));
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	Data->SetStringField(TEXT("name"), Class->GetName());
	Data->SetStringField(TEXT("pathName"), Class->GetPathName());
	if (UClass* Super = Class->GetSuperClass())
	{
		Data->SetStringField(TEXT("superClass"), Super->GetName());
	}

	// Properties
	TArray<TSharedPtr<FJsonValue>> Properties;
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		TSharedPtr<FJsonObject> PropObj = MakeShareable(new FJsonObject());
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
		Properties.Add(MakeShareable(new FJsonValueObject(PropObj)));
	}
	Data->SetArrayField(TEXT("properties"), Properties);

	// Functions
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		UFunction* Func = *It;
		if (Func->HasAnyFunctionFlags(FUNC_Delegate)) continue;

		TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject());
		FuncObj->SetStringField(TEXT("name"), Func->GetName());

		TArray<TSharedPtr<FJsonValue>> Params;
		for (TFieldIterator<FProperty> PIt(Func); PIt; ++PIt)
		{
			Params.Add(MakeShareable(new FJsonValueString((*PIt)->GetCPPType() + TEXT(" ") + (*PIt)->GetName())));
		}
		FuncObj->SetArrayField(TEXT("parameters"), Params);
		Functions.Add(MakeShareable(new FJsonValueObject(FuncObj)));
	}
	Data->SetArrayField(TEXT("functions"), Functions);

	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetModuleDependencies(const FOpenCodeRequest& Request)
{
	FString ModuleName;
	Request.Args->TryGetStringField(TEXT("moduleName"), ModuleName);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Modules;

	TArray<FModuleStatus> ModuleStatuses;
	FModuleManager::Get().QueryModules(ModuleStatuses);

	for (const FModuleStatus& Status : ModuleStatuses)
	{
		if (!ModuleName.IsEmpty() && !Status.Name.Contains(ModuleName))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Mod = MakeShareable(new FJsonObject());
		Mod->SetStringField(TEXT("name"), Status.Name);
		Mod->SetBoolField(TEXT("isLoaded"), Status.bIsLoaded);
		Mod->SetBoolField(TEXT("isGameModule"), Status.bIsGameModule);

		// Look for Build.cs and parse dependencies
		FString BuildCsPath = FPaths::ProjectDir() / TEXT("Source") / Status.Name / (Status.Name + TEXT(".Build.cs"));
		if (!FPaths::FileExists(BuildCsPath))
		{
			BuildCsPath = FPaths::EngineDir() / TEXT("Source") / TEXT("Runtime") / Status.Name / (Status.Name + TEXT(".Build.cs"));
		}
		if (FPaths::FileExists(BuildCsPath))
		{
			FString BuildCsContent;
			if (FFileHelper::LoadFileToString(BuildCsContent, *BuildCsPath))
			{
				TArray<TSharedPtr<FJsonValue>> Deps;
				TArray<FString> DepLines;
				FString Pattern = TEXT("\"");
				int32 StartPos = BuildCsContent.Find(TEXT("AddRange"));
				if (StartPos == INDEX_NONE)
				{
					StartPos = BuildCsContent.Find(TEXT("PublicDependencyModuleNames"));
				}
				if (StartPos != INDEX_NONE)
				{
					int32 EndPos = BuildCsContent.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromStart, StartPos);
					if (EndPos != INDEX_NONE)
					{
						FString DepSection = BuildCsContent.Mid(StartPos, EndPos - StartPos);
						int32 QuoteStart = 0;
						while ((QuoteStart = DepSection.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart)) != INDEX_NONE)
						{
							int32 QuoteEnd = DepSection.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
							if (QuoteEnd != INDEX_NONE)
							{
								FString Dep = DepSection.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
								if (!Dep.IsEmpty() && Dep != TEXT(","))
								{
									Deps.Add(MakeShareable(new FJsonValueString(Dep)));
								}
								QuoteStart = QuoteEnd + 1;
							}
							else
							{
								break;
							}
						}
					}
				}
				Mod->SetArrayField(TEXT("dependencies"), Deps);
			}
		}
		Modules.Add(MakeShareable(new FJsonValueObject(Mod)));
	}

	Data->SetArrayField(TEXT("modules"), Modules);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetPluginList(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Plugins;

	TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : AllPlugins)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetStringField(TEXT("name"), Plugin->GetName());
		P->SetStringField(TEXT("friendlyName"), Plugin->GetDescriptor().FriendlyName);
		P->SetStringField(TEXT("version"), Plugin->GetDescriptor().VersionName);
		P->SetStringField(TEXT("createdBy"), Plugin->GetDescriptor().CreatedBy);
		P->SetStringField(TEXT("description"), Plugin->GetDescriptor().Description);
		P->SetStringField(TEXT("baseDir"), Plugin->GetBaseDir());
		P->SetBoolField(TEXT("isEnabled"), Plugin->IsEnabled());
		Plugins.Add(MakeShareable(new FJsonValueObject(P)));
	}

	Data->SetArrayField(TEXT("plugins"), Plugins);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetOutputLog(const FOpenCodeRequest& Request)
{
	int32 Limit = 100;
	FString CategoryFilter;
	Request.Args->TryGetNumberField(TEXT("limit"), Limit);
	Request.Args->TryGetStringField(TEXT("category"), CategoryFilter);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Messages;

	TArray<FLogEntry> Entries;
	FOpenCodeLogCapture::Get().GetEntries(Entries);

	int32 StartIndex = FMath::Max(0, Entries.Num() - Limit);
	for (int32 i = StartIndex; i < Entries.Num(); ++i)
	{
		const FLogEntry& Entry = Entries[i];

		if (!CategoryFilter.IsEmpty() && !Entry.Category.Contains(CategoryFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject());
		Msg->SetStringField(TEXT("text"), Entry.Text);
		Msg->SetStringField(TEXT("category"), Entry.Category);
		Msg->SetNumberField(TEXT("verbosity"), static_cast<int32>(Entry.Verbosity));
		Messages.Add(MakeShareable(new FJsonValueObject(Msg)));
	}

	Data->SetArrayField(TEXT("messages"), Messages);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetBuildLogs(const FOpenCodeRequest& Request)
{
	int32 Limit = 50;
	FString CategoryFilter;
	Request.Args->TryGetNumberField(TEXT("limit"), Limit);
	Request.Args->TryGetStringField(TEXT("category"), CategoryFilter);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Messages;

	TArray<FLogEntry> Entries;
	FOpenCodeLogCapture::Get().GetEntries(Entries);

	int32 StartIndex = FMath::Max(0, Entries.Num() - Limit);
	for (int32 i = StartIndex; i < Entries.Num(); ++i)
	{
		const FLogEntry& Entry = Entries[i];

		if (!CategoryFilter.IsEmpty() && !Entry.Category.StartsWith(CategoryFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Msg = MakeShareable(new FJsonObject());
		Msg->SetStringField(TEXT("text"), Entry.Text);
		Msg->SetStringField(TEXT("category"), Entry.Category);
		Messages.Add(MakeShareable(new FJsonValueObject(Msg)));
	}

	Data->SetArrayField(TEXT("messages"), Messages);
	Data->SetStringField(TEXT("note"), TEXT("Build log data is derived from the output log. For full build output, check the Saved/Logs directory."));
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetCompilationStatus(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());

	bool bIsCompiling = false;

#if WITH_EDITOR
	if (GEditor)
	{
		bIsCompiling = GEditor->IsPlayingSessionInEditor();
	}
#endif

	Data->SetBoolField(TEXT("isCompiling"), bIsCompiling);
	Data->SetBoolField(TEXT("hotReloading"), false);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetBlueprintList(const FOpenCodeRequest& Request)
{
	int32 Limit = 100;
	FString PathPrefix;
	Request.Args->TryGetNumberField(TEXT("limit"), Limit);
	Request.Args->TryGetStringField(TEXT("pathPrefix"), PathPrefix);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(FTopLevelAssetPath(UBlueprint::StaticClass()));

	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	else
	{
		Filter.PackagePaths.Add(TEXT("/Game"));
	}

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (int32 i = 0; i < FMath::Min(AssetList.Num(), Limit); ++i)
	{
		const FAssetData& Asset = AssetList[i];
		TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());

		FString ParentClassName = TEXT("Unknown");
		for (const auto& Pair : Asset.TagsAndValues)
		{
			if (Pair.Key == FName(TEXT("ParentClass")))
			{
				ParentClassName = FString(Pair.Value.AsString());
				break;
			}
		}
		Item->SetStringField(TEXT("parentClass"), ParentClassName);
		Results.Add(MakeShareable(new FJsonValueObject(Item)));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("results"), Results);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetSelectedActors(const FOpenCodeRequest& Request)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Selected;

	if (GEditor)
	{
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection)
		{
			for (int32 i = 0; i < Selection->Num(); ++i)
			{
				AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
				if (Actor)
				{
					Selected.Add(MakeShareable(new FJsonValueObject(ActorToJson(Actor))));
				}
			}
		}
	}
	Data->SetArrayField(TEXT("selected"), Selected);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetActorDetails(const FOpenCodeRequest& Request)
{
	FString ActorName;
	Request.Args->TryGetStringField(TEXT("name"), ActorName);

	if (ActorName.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No actor name provided"));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());

	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && Actor->GetName() == ActorName)
			{
				Data->SetStringField(TEXT("name"), Actor->GetName());
				Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				Data->SetStringField(TEXT("label"), Actor->GetActorLabel());

				bool bIsLandscape = Actor->IsA<ALandscape>();

				// Components detail — skip full property reflection for landscape (heightmap/weightmap below)
				if (!bIsLandscape)
				{
					TArray<TSharedPtr<FJsonValue>> Components;
					TArray<UActorComponent*> ActorComponents;
					Actor->GetComponents(ActorComponents);
					for (UActorComponent* Comp : ActorComponents)
					{
						if (!Comp) continue;
						TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject());
						CompObj->SetStringField(TEXT("name"), Comp->GetName());
						CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

						TArray<TSharedPtr<FJsonValue>> Props;
						for (TFieldIterator<FProperty> PIt(Comp->GetClass()); PIt; ++PIt)
						{
							FProperty* Prop = *PIt;
							FString ValueStr;
							const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
							if (ValuePtr)
							{
								Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Comp, PPF_None);
								TSharedPtr<FJsonObject> PropObj = MakeShareable(new FJsonObject());
								PropObj->SetStringField(TEXT("name"), Prop->GetName());
								PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
								PropObj->SetStringField(TEXT("value"), ValueStr);
								Props.Add(MakeShareable(new FJsonValueObject(PropObj)));
							}
						}
						CompObj->SetArrayField(TEXT("properties"), Props);
						Components.Add(MakeShareable(new FJsonValueObject(CompObj)));
					}
					Data->SetArrayField(TEXT("components"), Components);
				}

				if (ALandscape* Landscape = Cast<ALandscape>(Actor))
				{
					int32 MaxX = 0, MaxY = 0;
					for (ULandscapeComponent* Comp : Landscape->LandscapeComponents)
					{
						if (Comp)
						{
							MaxX = FMath::Max(MaxX, Comp->SectionBaseX / Comp->ComponentSizeQuads);
							MaxY = FMath::Max(MaxY, Comp->SectionBaseY / Comp->ComponentSizeQuads);
						}
					}
					int32 GridW = MaxX + 1;
					int32 GridH = MaxY + 1;
					Data->SetNumberField(TEXT("componentGridWidth"), GridW);
					Data->SetNumberField(TEXT("componentGridHeight"), GridH);

					ULandscapeComponent* FirstComp = Landscape->LandscapeComponents.Num() > 0 ? Landscape->LandscapeComponents[0] : nullptr;
					int32 CompVerts = FirstComp ? (FirstComp->ComponentSizeQuads + 1) : 0;
					Data->SetNumberField(TEXT("verticesPerComponentSide"), CompVerts);

					float MinZ = FLT_MAX, MaxZ = -FLT_MAX;

					TArray<TSharedPtr<FJsonValue>> CompData;
					for (ULandscapeComponent* Comp : Landscape->LandscapeComponents)
					{
						if (!Comp) continue;
						int32 GridX = Comp->SectionBaseX / Comp->ComponentSizeQuads;
						int32 GridY = Comp->SectionBaseY / Comp->ComponentSizeQuads;

						TArray<TSharedPtr<FJsonValue>> Heights;
						int32 Size = Comp->ComponentSizeQuads + 1;

						FLandscapeComponentDataInterface CDI(Comp, 0);
						for (int32 Y = 0; Y < Size; ++Y)
						{
							for (int32 X = 0; X < Size; ++X)
							{
								float Height = CDI.GetHeight(X, Y);
								Heights.Add(MakeShareable(new FJsonValueNumber(Height)));
								MinZ = FMath::Min(MinZ, Height);
								MaxZ = FMath::Max(MaxZ, Height);
							}
						}

						TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject());
						CompObj->SetNumberField(TEXT("gridX"), GridX);
						CompObj->SetNumberField(TEXT("gridY"), GridY);
						CompObj->SetArrayField(TEXT("heights"), Heights);
						CompData.Add(MakeShareable(new FJsonValueObject(CompObj)));
					}

					Data->SetNumberField(TEXT("heightMin"), MinZ);
					Data->SetNumberField(TEXT("heightMax"), MaxZ);
					Data->SetArrayField(TEXT("heightmapComponents"), CompData);

					// Weightmap export
					TArray<FName> LayerNames;
					TArray<TSharedPtr<FJsonValue>> WeightmapData;

					ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
					if (LandscapeInfo)
					{
						for (const FLandscapeInfoLayerSettings& Layer : LandscapeInfo->Layers)
						{
							LayerNames.Add(Layer.LayerName);
						}
					}

					TArray<TSharedPtr<FJsonValue>> LayerNameJson;
					for (const FName& Name : LayerNames)
					{
						LayerNameJson.Add(MakeShareable(new FJsonValueString(Name.ToString())));
					}
					Data->SetArrayField(TEXT("layerNames"), LayerNameJson);
					Data->SetNumberField(TEXT("layerCount"), LayerNames.Num());

					if (LayerNames.Num() > 0)
					{
						for (ULandscapeComponent* Comp : Landscape->LandscapeComponents)
						{
							if (!Comp) continue;
							int32 GridX = Comp->SectionBaseX / Comp->ComponentSizeQuads;
							int32 GridY = Comp->SectionBaseY / Comp->ComponentSizeQuads;
							int32 Size = Comp->ComponentSizeQuads + 1;

							TArray<TSharedPtr<FJsonValue>> LayerWeights;

							FLandscapeComponentDataInterface CDI(Comp, 0);

							for (int32 LayerIdx = 0; LayerIdx < LayerNames.Num(); ++LayerIdx)
							{
								TArray<TSharedPtr<FJsonValue>> WeightGrid;

								ULandscapeLayerInfoObject* LayerInfo = nullptr;
								const TArray<FWeightmapLayerAllocationInfo>& Allocs = Comp->GetWeightmapLayerAllocations();
								for (const FWeightmapLayerAllocationInfo& Alloc : Allocs)
								{
									if (Alloc.LayerInfo && Alloc.LayerInfo->LayerName == LayerNames[LayerIdx])
									{
										LayerInfo = Alloc.LayerInfo;
										break;
									}
								}

								if (LayerInfo)
								{
									TArray<uint8> WeightData;
									if (CDI.GetWeightmapTextureData(LayerInfo, WeightData))
									{
										for (uint8 W : WeightData)
										{
											WeightGrid.Add(MakeShareable(new FJsonValueNumber(W)));
										}
									}
								}

								TSharedPtr<FJsonObject> LayerEntry = MakeShareable(new FJsonObject());
								LayerEntry->SetNumberField(TEXT("layerIndex"), LayerIdx);
								LayerEntry->SetArrayField(TEXT("weights"), WeightGrid);
								LayerWeights.Add(MakeShareable(new FJsonValueObject(LayerEntry)));
							}

							TSharedPtr<FJsonObject> CompWeightObj = MakeShareable(new FJsonObject());
							CompWeightObj->SetNumberField(TEXT("gridX"), GridX);
							CompWeightObj->SetNumberField(TEXT("gridY"), GridY);
							CompWeightObj->SetArrayField(TEXT("layers"), LayerWeights);
							WeightmapData.Add(MakeShareable(new FJsonValueObject(CompWeightObj)));
						}
					}

					Data->SetArrayField(TEXT("weightmapComponents"), WeightmapData);
				}

				break;
			}
		}
	}

	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleSetActorProperty(const FOpenCodeRequest& Request)
{
	FString ActorName, PropertyName, Value, ComponentName;
	Request.Args->TryGetStringField(TEXT("actorName"), ActorName);
	Request.Args->TryGetStringField(TEXT("propertyName"), PropertyName);
	Request.Args->TryGetStringField(TEXT("value"), Value);
	Request.Args->TryGetStringField(TEXT("componentName"), ComponentName);

	if (ActorName.IsEmpty() || PropertyName.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("actorName and propertyName are required"));
	}

	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->GetName() != ActorName) continue;

			UObject* TargetObj = Actor;
			if (!ComponentName.IsEmpty())
			{
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				for (UActorComponent* Comp : Components)
				{
					if (Comp && Comp->GetName() == ComponentName)
					{
						TargetObj = Comp;
						break;
					}
				}
			}

			FProperty* Prop = FindFProperty<FProperty>(TargetObj->GetClass(), *PropertyName);
			if (!Prop)
			{
				return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Property %s not found"), *PropertyName));
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObj);
			if (ValuePtr)
			{
				Prop->ImportText_Direct(*Value, ValuePtr, TargetObj, PPF_None);
				TargetObj->PostEditChange();

				TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
				Data->SetStringField(TEXT("actor"), ActorName);
				Data->SetStringField(TEXT("property"), PropertyName);
				Data->SetStringField(TEXT("value"), Value);
				Data->SetBoolField(TEXT("success"), true);
				return FOpenCodeResponse::Success(Request.Id, Data);
			}
		}
	}

	return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Actor %s not found"), *ActorName));
}

// ---- get_material_graph helpers -------------------------------------------

// "MaterialExpressionAdd_3" -> "Add_3". Object names are unique within an asset,
// so the short name doubles as the node id.
static FString OCShortExprName(const UObject* Obj)
{
	FString Name = Obj ? Obj->GetName() : FString(TEXT("None"));
	Name.RemoveFromStart(TEXT("MaterialExpression"));
	return Name;
}

// "Source.Output" (or "Source[RG]" for a masked unnamed output) for a connected input.
static FString OCDescribeSource(const FExpressionInput& Input)
{
	UMaterialExpression* Source = Input.Expression;
	if (!Source)
	{
		return FString();
	}
	FString Out = OCShortExprName(Source);
	const TArray<FExpressionOutput>& Outputs = Source->GetOutputs();
	if (Outputs.IsValidIndex(Input.OutputIndex) && !Outputs[Input.OutputIndex].OutputName.IsNone())
	{
		Out += TEXT(".");
		Out += Outputs[Input.OutputIndex].OutputName.ToString();
	}
	else
	{
		if (Input.OutputIndex != 0)
		{
			Out += FString::Printf(TEXT(".%d"), Input.OutputIndex);
		}
		if (Input.Mask != 0)
		{
			Out += TEXT("[");
			if (Input.MaskR != 0) Out += TEXT("R");
			if (Input.MaskG != 0) Out += TEXT("G");
			if (Input.MaskB != 0) Out += TEXT("B");
			if (Input.MaskA != 0) Out += TEXT("A");
			Out += TEXT("]");
		}
	}
	return Out;
}

// Input/output pin structs: wiring, reported through the input iterator instead.
static bool OCIsWiringStruct(const UScriptStruct* Struct)
{
	if (!Struct)
	{
		return false;
	}
	const FString Name = Struct->GetName();
	return Name.EndsWith(TEXT("Input")) || Name.EndsWith(TEXT("Output"));
}

// Every property the expression's own class hierarchy adds on top of
// UMaterialExpression whose value differs from the class default: component
// masks, UV index, constants, transform spaces, parameter names, textures,
// reroute declarations... Parameter names and defaults are always listed.
static void OCCollectExprProps(UMaterialExpression* Expr, TArray<TPair<FString, FString>>& OutProps)
{
	static const TSet<FName> Skip = {
		FName(TEXT("ExpressionGUID")), FName(TEXT("Group")), FName(TEXT("SortPriority")),
		FName(TEXT("ChannelNames")), FName(TEXT("Id")), FName(TEXT("VariableGuid")),
		FName(TEXT("DeclarationGuid")), FName(TEXT("NodeColor")), FName(TEXT("AttributeSetTypes")),
		FName(TEXT("AttributeGetTypes")), FName(TEXT("PreAttributeSetTypes")), FName(TEXT("PreAttributeGetTypes")),
		FName(TEXT("ParameterCustomization")), FName(TEXT("Description")), FName(TEXT("bLastPreviewed")),
	};
	static const TSet<FName> Always = {
		FName(TEXT("ParameterName")), FName(TEXT("DefaultValue")), FName(TEXT("InputName")), FName(TEXT("OutputName")),
	};

	const UObject* CDO = Expr->GetClass()->GetDefaultObject();
	for (TFieldIterator<FProperty> It(Expr->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop || Prop->GetOwnerClass() == UMaterialExpression::StaticClass())
		{
			continue;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}
		const FName PropName = Prop->GetFName();
		if (Skip.Contains(PropName))
		{
			continue;
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (OCIsWiringStruct(StructProp->Struct))
			{
				continue;
			}
		}
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			const FStructProperty* Inner = CastField<FStructProperty>(ArrayProp->Inner);
			if (Inner && OCIsWiringStruct(Inner->Struct))
			{
				continue;
			}
		}

		const bool bAlways = Always.Contains(PropName);
		if (!bAlways && CDO && Prop->Identical_InContainer(Expr, CDO))
		{
			continue;
		}

		FString Value;
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			const UObject* Obj = ObjProp->GetObjectPropertyValue_InContainer(Expr);
			if (!Obj)
			{
				continue;
			}
			if (Obj->IsA<UMaterialExpression>())
			{
				Value = TEXT("@") + OCShortExprName(Obj);
			}
			else
			{
				// Assets by package path: enough to load them, shorter than an export path.
				Value = Obj->GetOutermost()->GetName();
			}
		}
		else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			// Spelled out: an always-listed false (a static switch's default) must not read as blank.
			Value = BoolProp->GetPropertyValue_InContainer(Expr) ? TEXT("True") : TEXT("False");
		}
		else
		{
			Prop->ExportText_InContainer(0, Value, Expr, Expr, Expr, PPF_None);
		}
		if (Value.Len() > 200)
		{
			Value = Value.Left(200) + TEXT("...");
		}
		OutProps.Emplace(PropName.ToString(), MoveTemp(Value));
	}
}

// Every connected input, by the pin name the editor shows. Unlike reflecting
// over FExpressionInput members this reaches function-call inputs and the
// attribute pins of Set/GetMaterialAttributes.
static void OCCollectExprInputs(UMaterialExpression* Expr, TArray<TPair<FString, FString>>& OutInputs)
{
	for (FExpressionInputIterator It{ Expr }; It; ++It)
	{
		const FExpressionInput* Input = It.Input;
		if (!Input || !Input->Expression)
		{
			continue;
		}
		const FName PinName = Expr->GetInputName(It.Index);
		OutInputs.Emplace(PinName.IsNone() ? FString(TEXT("in")) : PinName.ToString(), OCDescribeSource(*Input));
	}
}

// Material or MaterialFunction graph.
//
// args: assetPath, format ("compact" default | "json").
//   compact: one string per node, "Name | Prop=Value ... | Pin<-Source.Output, ... // desc"
//   json:    one object per node {name, type, desc, x, y, props{}, inputs{}}
// Only the asset's own expressions are listed; called functions are named in
// `functions` and dumped by a call of their own.
FOpenCodeResponse FOpenCodeToolHandler::HandleGetMaterialGraph(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);
	FString Format;
	Request.Args->TryGetStringField(TEXT("format"), Format);
	const bool bJson = Format.Equals(TEXT("json"), ESearchCase::IgnoreCase);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Asset)
	{
		Data->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	UMaterial* Mat = Cast<UMaterial>(Asset);
	UMaterialFunction* MF = Cast<UMaterialFunction>(Asset);
	if (!Mat && !MF)
	{
		if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset))
		{
			const UMaterial* Base = Instance->GetMaterial();
			Data->SetStringField(TEXT("error"), FString::Printf(
				TEXT("%s is a MaterialInstance and has no graph of its own. Dump its base material %s, and read the instance's overrides with get_material_parameters."),
				*Asset->GetName(), Base ? *Base->GetPathName() : TEXT("(none)")));
		}
		else
		{
			Data->SetStringField(TEXT("error"), TEXT("Asset must be a Material or MaterialFunction"));
		}
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	TArray<UMaterialExpression*> Expressions;
	TArray<FString> CommentTexts;
	if (Mat)
	{
		for (const TObjectPtr<UMaterialExpression>& E : Mat->GetExpressions())
		{
			if (E) Expressions.Add(E.Get());
		}
		for (const TObjectPtr<UMaterialExpressionComment>& Comment : Mat->GetEditorComments())
		{
			if (Comment) CommentTexts.Add(Comment->Text);
		}
	}
	else
	{
		for (const TObjectPtr<UMaterialExpression>& E : MF->GetExpressions())
		{
			if (E) Expressions.Add(E.Get());
		}
		for (const TObjectPtr<UMaterialExpressionComment>& Comment : MF->GetEditorComments())
		{
			if (Comment) CommentTexts.Add(Comment->Text);
		}
	}
	Expressions.Sort([](const UMaterialExpression& A, const UMaterialExpression& B)
	{
		return A.GetName() < B.GetName();
	});

	Data->SetStringField(TEXT("asset"), Asset->GetPathName());
	Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Data->SetNumberField(TEXT("expressionCount"), Expressions.Num());

	// Root: what the material is, and what feeds its output pins.
	if (Mat)
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
		if (const UEnum* BlendEnum = StaticEnum<EBlendMode>())
		{
			Root->SetStringField(TEXT("blendMode"), BlendEnum->GetNameStringByValue((int64)Mat->GetBlendMode()));
		}
		FString ShadingModels;
		if (const UEnum* ShadingEnum = StaticEnum<EMaterialShadingModel>())
		{
			const FMaterialShadingModelField Field = Mat->GetShadingModels();
			for (int32 Model = 0; Model < MSM_NUM; ++Model)
			{
				if (Field.HasShadingModel((EMaterialShadingModel)Model))
				{
					if (!ShadingModels.IsEmpty()) ShadingModels += TEXT(",");
					ShadingModels += ShadingEnum->GetNameStringByValue(Model);
				}
			}
		}
		Root->SetStringField(TEXT("shadingModels"), ShadingModels);
		Root->SetBoolField(TEXT("twoSided"), Mat->IsTwoSided());
		Root->SetNumberField(TEXT("opacityMaskClipValue"), Mat->GetOpacityMaskClipValue());
		Root->SetBoolField(TEXT("tangentSpaceNormal"), Mat->bTangentSpaceNormal != 0);
		Root->SetBoolField(TEXT("useMaterialAttributes"), Mat->bUseMaterialAttributes != 0);

		struct FRootPin { EMaterialProperty Property; const TCHAR* Name; };
		static const FRootPin RootPins[] = {
			{ MP_MaterialAttributes, TEXT("MaterialAttributes") }, { MP_BaseColor, TEXT("BaseColor") },
			{ MP_Metallic, TEXT("Metallic") }, { MP_Specular, TEXT("Specular") }, { MP_Roughness, TEXT("Roughness") },
			{ MP_Anisotropy, TEXT("Anisotropy") }, { MP_EmissiveColor, TEXT("EmissiveColor") },
			{ MP_Opacity, TEXT("Opacity") }, { MP_OpacityMask, TEXT("OpacityMask") }, { MP_Normal, TEXT("Normal") },
			{ MP_Tangent, TEXT("Tangent") }, { MP_WorldPositionOffset, TEXT("WorldPositionOffset") },
			{ MP_SubsurfaceColor, TEXT("SubsurfaceColor") }, { MP_AmbientOcclusion, TEXT("AmbientOcclusion") },
			{ MP_Refraction, TEXT("Refraction") }, { MP_PixelDepthOffset, TEXT("PixelDepthOffset") },
			{ MP_ShadingModel, TEXT("ShadingModel") },
		};
		TArray<TSharedPtr<FJsonValue>> RootInputs;
		for (const FRootPin& Pin : RootPins)
		{
			const FExpressionInput* Input = Mat->GetExpressionInputForProperty(Pin.Property);
			if (Input && Input->Expression)
			{
				RootInputs.Add(MakeShareable(new FJsonValueString(
					FString::Printf(TEXT("%s<-%s"), Pin.Name, *OCDescribeSource(*Input)))));
			}
		}
		Root->SetArrayField(TEXT("inputs"), RootInputs);
		Data->SetObjectField(TEXT("material"), Root);
	}

	TSet<FString> FunctionPaths;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UMaterialExpression* Expr : Expressions)
	{
		if (Expr->IsA<UMaterialExpressionComment>())
		{
			continue;
		}
		if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			if (Call->MaterialFunction)
			{
				FunctionPaths.Add(Call->MaterialFunction->GetOutermost()->GetName());
			}
		}

		TArray<TPair<FString, FString>> Props;
		TArray<TPair<FString, FString>> Inputs;
		OCCollectExprProps(Expr, Props);
		OCCollectExprInputs(Expr, Inputs);
		const FString Name = OCShortExprName(Expr);
		const FString Desc = Expr->Desc.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));

		if (bJson)
		{
			TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject());
			Node->SetStringField(TEXT("name"), Name);
			Node->SetStringField(TEXT("type"), Expr->GetClass()->GetName());
			if (!Desc.IsEmpty()) Node->SetStringField(TEXT("desc"), Desc);
			Node->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
			Node->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
			TSharedPtr<FJsonObject> PropsObj = MakeShareable(new FJsonObject());
			for (const TPair<FString, FString>& P : Props) PropsObj->SetStringField(P.Key, P.Value);
			Node->SetObjectField(TEXT("props"), PropsObj);
			TSharedPtr<FJsonObject> InputsObj = MakeShareable(new FJsonObject());
			for (const TPair<FString, FString>& In : Inputs) InputsObj->SetStringField(In.Key, In.Value);
			Node->SetObjectField(TEXT("inputs"), InputsObj);
			Nodes.Add(MakeShareable(new FJsonValueObject(Node)));
		}
		else
		{
			FString Line = Name + TEXT(" | ");
			for (int32 i = 0; i < Props.Num(); ++i)
			{
				if (i > 0) Line += TEXT(" ");
				Line += Props[i].Key + TEXT("=") + Props[i].Value;
			}
			Line += TEXT(" | ");
			for (int32 i = 0; i < Inputs.Num(); ++i)
			{
				if (i > 0) Line += TEXT(", ");
				Line += Inputs[i].Key + TEXT("<-") + Inputs[i].Value;
			}
			if (!Desc.IsEmpty())
			{
				Line += TEXT(" // ") + Desc;
			}
			Nodes.Add(MakeShareable(new FJsonValueString(Line)));
		}
	}
	Data->SetStringField(TEXT("nodeFormat"), bJson
		? TEXT("json")
		: TEXT("Name | Prop=Value (non-default only) | Pin<-Source.Output // desc"));
	Data->SetArrayField(TEXT("nodes"), Nodes);

	TArray<FString> SortedFunctions = FunctionPaths.Array();
	SortedFunctions.Sort();
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (const FString& Path : SortedFunctions)
	{
		Functions.Add(MakeShareable(new FJsonValueString(Path)));
	}
	Data->SetArrayField(TEXT("functions"), Functions);

	TArray<TSharedPtr<FJsonValue>> Comments;
	for (const FString& Text : CommentTexts)
	{
		Comments.Add(MakeShareable(new FJsonValueString(Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")))));
	}
	Data->SetArrayField(TEXT("comments"), Comments);

	return FOpenCodeResponse::Success(Request.Id, Data);
}

// Effective value of every scalar/vector/texture/static-switch parameter of a
// Material or MaterialInstance (resolved through the parent chain), with
// `overridden` marking values set on the asset itself rather than inherited.
FOpenCodeResponse FOpenCodeToolHandler::HandleGetMaterialParameters(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);

	UMaterialInterface* Mat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *AssetPath));
	if (!Mat)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Material or MaterialInstance not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset"), Mat->GetPathName());
	Data->SetStringField(TEXT("class"), Mat->GetClass()->GetName());

	// Parent chain, nearest first, ending at the base UMaterial.
	TArray<TSharedPtr<FJsonValue>> Chain;
	for (const UMaterialInterface* Cur = Mat; Cur; )
	{
		Chain.Add(MakeShareable(new FJsonValueString(Cur->GetPathName())));
		const UMaterialInstance* Inst = Cast<UMaterialInstance>(Cur);
		Cur = Inst ? Inst->Parent.Get() : nullptr;
	}
	Data->SetArrayField(TEXT("parentChain"), Chain);

	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;

	auto ParamBase = [](const FMaterialParameterInfo& Info, bool bOverridden)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetStringField(TEXT("name"), Info.Name.ToString());
		P->SetBoolField(TEXT("overridden"), bOverridden);
		return P;
	};

	TArray<TSharedPtr<FJsonValue>> Scalars;
	Mat->GetAllScalarParameterInfo(Infos, Ids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		float Value = 0.f, Dummy = 0.f;
		Mat->GetScalarParameterValue(Info, Value);
		const bool bOver = Mat->GetScalarParameterValue(Info, Dummy, true);
		TSharedPtr<FJsonObject> P = ParamBase(Info, bOver);
		P->SetNumberField(TEXT("value"), Value);
		Scalars.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Data->SetArrayField(TEXT("scalars"), Scalars);

	TArray<TSharedPtr<FJsonValue>> Vectors;
	Infos.Reset(); Ids.Reset();
	Mat->GetAllVectorParameterInfo(Infos, Ids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		FLinearColor Value = FLinearColor::Black, Dummy;
		Mat->GetVectorParameterValue(Info, Value);
		const bool bOver = Mat->GetVectorParameterValue(Info, Dummy, true);
		TSharedPtr<FJsonObject> P = ParamBase(Info, bOver);
		P->SetNumberField(TEXT("r"), Value.R);
		P->SetNumberField(TEXT("g"), Value.G);
		P->SetNumberField(TEXT("b"), Value.B);
		P->SetNumberField(TEXT("a"), Value.A);
		Vectors.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Data->SetArrayField(TEXT("vectors"), Vectors);

	TArray<TSharedPtr<FJsonValue>> Textures;
	Infos.Reset(); Ids.Reset();
	Mat->GetAllTextureParameterInfo(Infos, Ids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		UTexture* Value = nullptr;
		UTexture* Dummy = nullptr;
		Mat->GetTextureParameterValue(Info, Value);
		const bool bOver = Mat->GetTextureParameterValue(Info, Dummy, true);
		TSharedPtr<FJsonObject> P = ParamBase(Info, bOver);
		P->SetStringField(TEXT("texture"), Value ? Value->GetPathName() : FString());
		Textures.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Data->SetArrayField(TEXT("textures"), Textures);

	TArray<TSharedPtr<FJsonValue>> Switches;
	Infos.Reset(); Ids.Reset();
	Mat->GetAllStaticSwitchParameterInfo(Infos, Ids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		bool Value = false, Dummy = false;
		FGuid Guid;
		Mat->GetStaticSwitchParameterValue(Info, Value, Guid);
		const bool bOver = Mat->GetStaticSwitchParameterValue(Info, Dummy, Guid, true);
		TSharedPtr<FJsonObject> P = ParamBase(Info, bOver);
		P->SetBoolField(TEXT("value"), Value);
		Switches.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Data->SetArrayField(TEXT("staticSwitches"), Switches);

	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGenerateCode(const FOpenCodeRequest& Request)
{
	FString FilePath, Content, Description;
	Request.Args->TryGetStringField(TEXT("filePath"), FilePath);
	Request.Args->TryGetStringField(TEXT("content"), Content);
	Request.Args->TryGetStringField(TEXT("description"), Description);

	if (FilePath.IsEmpty() || Content.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("filePath and content are required"));
	}

	// Containment check: resolve ../ etc. and require the final absolute path
	// to stay under Source/. Without this, a filePath of "..\\..\\x" writes
	// anywhere on disk — combined with a network-reachable listener that was
	// remote arbitrary file write.
	FString FullPath = FPaths::ProjectDir() / TEXT("Source") / FilePath;
	FPaths::CollapseRelativeDirectories(FullPath);
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source/"));
	const FString FullAbs = FPaths::ConvertRelativePathToFull(FullPath);
	if (!FullAbs.StartsWith(SourceRoot))
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("filePath must resolve inside the project Source/ directory"));
	}
	FString Dir = FPaths::GetPath(FullPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	if (FFileHelper::SaveStringToFile(Content, *FullPath))
	{
		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetStringField(TEXT("filePath"), FilePath);
		Data->SetStringField(TEXT("fullPath"), FullPath);
		Data->SetStringField(TEXT("description"), Description);
		Data->SetBoolField(TEXT("written"), true);
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Failed to write file: %s"), *FullPath));
}

FOpenCodeResponse FOpenCodeToolHandler::HandleSearchClasses(const FOpenCodeRequest& Request)
{
	FString Query;
	int32 Limit = 50;
	Request.Args->TryGetStringField(TEXT("query"), Query);
	Request.Args->TryGetNumberField(TEXT("limit"), Limit);

	TArray<TSharedPtr<FJsonValue>> Results;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class) continue;
		FString Name = Class->GetName();
		if (!Query.IsEmpty() && !Name.Contains(Query, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Results.Num() >= Limit) break;

		TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
		Item->SetStringField(TEXT("name"), Name);
		Item->SetStringField(TEXT("pathName"), Class->GetPathName());
		if (UClass* Super = Class->GetSuperClass())
		{
			Item->SetStringField(TEXT("superClass"), Super->GetName());
		}
		Results.Add(MakeShareable(new FJsonValueObject(Item)));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("results"), Results);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleGetCppHierarchy(const FOpenCodeRequest& Request)
{
	FString BaseClass;
	int32 MaxDepth = 5;
	Request.Args->TryGetStringField(TEXT("baseClass"), BaseClass);
	Request.Args->TryGetNumberField(TEXT("maxDepth"), MaxDepth);

	UClass* Base = nullptr;
	if (!BaseClass.IsEmpty())
	{
		Base = FindObject<UClass>(nullptr, *BaseClass, true);
		if (!Base)
		{
			Base = FindObject<UClass>(nullptr, *(TEXT("/Script/Engine.") + BaseClass));
		}
		if (!Base)
		{
			Base = FindObject<UClass>(nullptr, *(TEXT("/Script/CoreUObject.") + BaseClass));
		}
	}

	if (!Base)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Base class %s not found"), *BaseClass));
	}

	TFunction<TSharedPtr<FJsonObject>(UClass*, int32)> BuildHierarchy;
	BuildHierarchy = [&BuildHierarchy, MaxDepth](UClass* Class, int32 Depth) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject());
		Node->SetStringField(TEXT("name"), Class->GetName());

		if (Depth < MaxDepth)
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Check = *It;
				if (Check && Check->GetSuperClass() == Class && Check != Class)
				{
					Children.Add(MakeShareable(new FJsonValueObject(BuildHierarchy(Check, Depth + 1))));
				}
			}
			Node->SetArrayField(TEXT("children"), Children);
		}
		return Node;
	};

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetObjectField(TEXT("hierarchy"), BuildHierarchy(Base, 0));
	return FOpenCodeResponse::Success(Request.Id, Data);
}

FOpenCodeResponse FOpenCodeToolHandler::HandleRunPython(const FOpenCodeRequest& Request)
{
#if WITH_EDITOR
	FString Code, Mode;
	Request.Args->TryGetStringField(TEXT("code"), Code);
	Request.Args->TryGetStringField(TEXT("mode"), Mode);

	if (Code.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("code is required"));
	}

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python || !Python->IsPythonAvailable())
	{
		return FOpenCodeResponse::Failure(Request.Id,
			TEXT("Python is not available — enable the 'Python Editor Script Plugin' (PythonScriptPlugin) in Edit > Plugins and restart the editor"));
	}

	// Generic escape hatch: anything the `unreal` Python API can reach is
	// scriptable without adding a bespoke C++ tool for it. Runs on the game
	// thread (module dispatch guarantees that), which Python requires anyway.
	FPythonCommandEx Cmd;
	Cmd.Command = Code;
	// exec (default) → ExecuteFile: compiles the command as a literal script
	// (Py_file_input), so multi-line / multi-statement code works. The older
	// ExecuteStatement mode uses single_input and rejects anything past one
	// statement ("multiple statements found while compiling a single statement").
	// eval → EvaluateStatement: single expression, repr() returned in `result`.
	Cmd.ExecutionMode = (Mode == TEXT("eval"))
		? EPythonCommandExecutionMode::EvaluateStatement
		: EPythonCommandExecutionMode::ExecuteFile;

	const bool bOk = Python->ExecPythonCommandEx(Cmd);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetBoolField(TEXT("ok"), bOk);
	// eval mode: repr() of the expression. On failure: the Python exception text.
	if (!Cmd.CommandResult.IsEmpty())
	{
		Data->SetStringField(TEXT("result"), Cmd.CommandResult);
	}

	TArray<TSharedPtr<FJsonValue>> Log;
	for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
	{
		TSharedPtr<FJsonObject> E = MakeShareable(new FJsonObject());
		E->SetStringField(TEXT("type"),
			Entry.Type == EPythonLogOutputType::Error   ? TEXT("error")
			: Entry.Type == EPythonLogOutputType::Warning ? TEXT("warning")
			: TEXT("info"));
		E->SetStringField(TEXT("output"), Entry.Output);
		Log.Add(MakeShareable(new FJsonValueObject(E)));
	}
	Data->SetArrayField(TEXT("log"), Log);

	return FOpenCodeResponse::Success(Request.Id, Data);
#else
	return FOpenCodeResponse::Failure(Request.Id, TEXT("run_python requires an editor build"));
#endif
}

// ======================= TOOLS ADDED AFTER THE HEADER =======================
// Free functions rather than class members, so the header (and every other
// translation unit) stays untouched and a Live Coding patch can add them.

// Reflected properties of an object as strings: those that differ from the
// class default, plus any named in `Always`. Properties declared by `StopAt`
// or its bases are skipped (pass USceneComponent to drop transform noise).
static void OCExportObjectProps(const UObject* Obj, const UClass* StopAt, const TSet<FName>& Always, const TSharedPtr<FJsonObject>& Out)
{
	if (!Obj)
	{
		return;
	}
	const UObject* CDO = Obj->GetClass()->GetDefaultObject();
	for (TFieldIterator<FProperty> It(Obj->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		const UClass* Owner = Prop ? Prop->GetOwnerClass() : nullptr;
		if (!Owner || (StopAt && StopAt->IsChildOf(Owner)))
		{
			continue;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
		{
			continue;
		}
		if (CastField<FDelegateProperty>(Prop) || CastField<FMulticastDelegateProperty>(Prop))
		{
			continue;
		}
		// Bookkeeping that says nothing about how the asset behaves.
		static const TSet<FName> Noise = {
			FName(TEXT("Source")), FName(TEXT("LightingGuid")), FName(TEXT("AssetImportData")),
			FName(TEXT("AssetUserData")), FName(TEXT("ImportedSize")),
		};
		if (Noise.Contains(Prop->GetFName()))
		{
			continue;
		}
		const bool bAlways = Always.Contains(Prop->GetFName());
		if (!bAlways && CDO && Prop->Identical_InContainer(Obj, CDO))
		{
			continue;
		}

		FString Value;
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			Value = BoolProp->GetPropertyValue_InContainer(Obj) ? TEXT("true") : TEXT("false");
		}
		else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			const UObject* Ref = ObjProp->GetObjectPropertyValue_InContainer(Obj);
			Value = Ref ? Ref->GetPathName() : FString(TEXT("None"));
		}
		else
		{
			Prop->ExportText_InContainer(0, Value, Obj, Obj, const_cast<UObject*>(Obj), PPF_None);
		}
		if (Value.Len() > 300)
		{
			Value = Value.Left(300) + TEXT("...");
		}
		Out->SetStringField(Prop->GetName(), Value);
	}
}

static double OCRound(double Value, double Scale)
{
	return FMath::RoundToDouble(Value * Scale) / Scale;
}

static TSharedPtr<FJsonValue> OCNum(double Value, double Scale)
{
	return MakeShareable(new FJsonValueNumber(OCRound(Value, Scale)));
}

// ---- get_static_mesh_data ----------------------------------------------------
// args: assetPath, lod (default 0), includeData (default false), includeNormals.
// Without includeData: bounds, material slots and per-LOD counts / screen sizes.
// With it: the LOD's render vertices (positions, every UV channel, optionally
// normals) and triangle indices, as flat arrays — pair with outputFile.
static FOpenCodeResponse OCHandleGetStaticMeshData(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);
	int32 Lod = 0;
	Request.Args->TryGetNumberField(TEXT("lod"), Lod);
	bool bIncludeData = false;
	Request.Args->TryGetBoolField(TEXT("includeData"), bIncludeData);
	bool bIncludeNormals = false;
	Request.Args->TryGetBoolField(TEXT("includeNormals"), bIncludeNormals);

	UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *AssetPath));
	if (!Mesh)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("The mesh has no render data"));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset"), Mesh->GetPathName());
	Data->SetStringField(TEXT("units"), TEXT("UE centimeters, Z-up, local mesh space"));

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	TSharedPtr<FJsonObject> BoundsObj = MakeShareable(new FJsonObject());
	BoundsObj->SetStringField(TEXT("origin"), Bounds.Origin.ToString());
	BoundsObj->SetStringField(TEXT("boxExtent"), Bounds.BoxExtent.ToString());
	BoundsObj->SetNumberField(TEXT("sphereRadius"), Bounds.SphereRadius);
	Data->SetObjectField(TEXT("bounds"), BoundsObj);

	TArray<TSharedPtr<FJsonValue>> Materials;
	for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject());
		SlotObj->SetStringField(TEXT("slot"), Slot.MaterialSlotName.ToString());
		SlotObj->SetStringField(TEXT("material"), Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
		Materials.Add(MakeShareable(new FJsonValueObject(SlotObj)));
	}
	Data->SetArrayField(TEXT("materials"), Materials);

	TArray<TSharedPtr<FJsonValue>> Lods;
	for (int32 LodIndex = 0; LodIndex < RenderData->LODResources.Num(); ++LodIndex)
	{
		const FStaticMeshLODResources& LodRes = RenderData->LODResources[LodIndex];
		TSharedPtr<FJsonObject> LodObj = MakeShareable(new FJsonObject());
		LodObj->SetNumberField(TEXT("lod"), LodIndex);
		LodObj->SetNumberField(TEXT("vertices"), LodRes.GetNumVertices());
		LodObj->SetNumberField(TEXT("triangles"), LodRes.GetNumTriangles());
		LodObj->SetNumberField(TEXT("uvChannels"), LodRes.GetNumTexCoords());
		LodObj->SetNumberField(TEXT("screenSize"), RenderData->ScreenSize[LodIndex].GetValue());
		TArray<TSharedPtr<FJsonValue>> Sections;
		for (const FStaticMeshSection& Section : LodRes.Sections)
		{
			TSharedPtr<FJsonObject> SectionObj = MakeShareable(new FJsonObject());
			SectionObj->SetNumberField(TEXT("materialIndex"), Section.MaterialIndex);
			SectionObj->SetNumberField(TEXT("triangles"), Section.NumTriangles);
			SectionObj->SetNumberField(TEXT("firstIndex"), Section.FirstIndex);
			Sections.Add(MakeShareable(new FJsonValueObject(SectionObj)));
		}
		LodObj->SetArrayField(TEXT("sections"), Sections);
		Lods.Add(MakeShareable(new FJsonValueObject(LodObj)));
	}
	Data->SetArrayField(TEXT("lods"), Lods);

	if (bIncludeData)
	{
		if (!RenderData->LODResources.IsValidIndex(Lod))
		{
			return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("LOD %d does not exist (the mesh has %d)"), Lod, RenderData->LODResources.Num()));
		}
		const FStaticMeshLODResources& LodRes = RenderData->LODResources[Lod];
		const FPositionVertexBuffer& Positions = LodRes.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& Vertices = LodRes.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumVerts = (int32)Positions.GetNumVertices();
		if (NumVerts == 0 || Positions.GetVertexData() == nullptr || (int32)Vertices.GetNumVertices() != NumVerts)
		{
			return FOpenCodeResponse::Failure(Request.Id, TEXT("The LOD's vertex data is not available on the CPU"));
		}

		TSharedPtr<FJsonObject> Geo = MakeShareable(new FJsonObject());
		Geo->SetNumberField(TEXT("lod"), Lod);
		Geo->SetStringField(TEXT("layout"), TEXT("positions: xyz per vertex; uvs[channel]: uv per vertex; normals: xyz per vertex; indices: 3 per triangle"));

		TArray<TSharedPtr<FJsonValue>> PosArray;
		PosArray.Reserve(NumVerts * 3);
		for (int32 i = 0; i < NumVerts; ++i)
		{
			const FVector3f& P = Positions.VertexPosition(i);
			PosArray.Add(OCNum(P.X, 10000.0));
			PosArray.Add(OCNum(P.Y, 10000.0));
			PosArray.Add(OCNum(P.Z, 10000.0));
		}
		Geo->SetArrayField(TEXT("positions"), PosArray);

		TArray<TSharedPtr<FJsonValue>> UvChannels;
		const int32 NumUVs = (int32)Vertices.GetNumTexCoords();
		for (int32 Channel = 0; Channel < NumUVs; ++Channel)
		{
			TArray<TSharedPtr<FJsonValue>> UvArray;
			UvArray.Reserve(NumVerts * 2);
			for (int32 i = 0; i < NumVerts; ++i)
			{
				const FVector2f UV = Vertices.GetVertexUV(i, Channel);
				UvArray.Add(OCNum(UV.X, 1000000.0));
				UvArray.Add(OCNum(UV.Y, 1000000.0));
			}
			UvChannels.Add(MakeShareable(new FJsonValueArray(UvArray)));
		}
		Geo->SetArrayField(TEXT("uvs"), UvChannels);

		if (bIncludeNormals)
		{
			TArray<TSharedPtr<FJsonValue>> NormalArray;
			NormalArray.Reserve(NumVerts * 3);
			for (int32 i = 0; i < NumVerts; ++i)
			{
				const FVector4f N = Vertices.VertexTangentZ(i);
				NormalArray.Add(OCNum(N.X, 10000.0));
				NormalArray.Add(OCNum(N.Y, 10000.0));
				NormalArray.Add(OCNum(N.Z, 10000.0));
			}
			Geo->SetArrayField(TEXT("normals"), NormalArray);
		}

		TArray<uint32> Indices;
		LodRes.IndexBuffer.GetCopy(Indices);
		TArray<TSharedPtr<FJsonValue>> IndexArray;
		IndexArray.Reserve(Indices.Num());
		for (uint32 Index : Indices)
		{
			IndexArray.Add(MakeShareable(new FJsonValueNumber((double)Index)));
		}
		Geo->SetArrayField(TEXT("indices"), IndexArray);
		Data->SetObjectField(TEXT("geometry"), Geo);
	}

	return FOpenCodeResponse::Success(Request.Id, Data);
}

// ---- get_texture_info --------------------------------------------------------
// What the GPU actually samples (format, size, mips, alpha), the import
// settings that decide it, and per-channel min/max/mean of the source image —
// so "is the alpha channel real data?" is a lookup, not a guess.
static FOpenCodeResponse OCHandleGetTextureInfo(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);
	bool bStats = true;
	Request.Args->TryGetBoolField(TEXT("channelStats"), bStats);

	UTexture* Texture = Cast<UTexture>(StaticLoadObject(UTexture::StaticClass(), nullptr, *AssetPath));
	if (!Texture)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Texture not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset"), Texture->GetPathName());
	Data->SetStringField(TEXT("class"), Texture->GetClass()->GetName());

	if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
	{
		TSharedPtr<FJsonObject> Runtime = MakeShareable(new FJsonObject());
		Runtime->SetNumberField(TEXT("width"), Texture2D->GetSizeX());
		Runtime->SetNumberField(TEXT("height"), Texture2D->GetSizeY());
		Runtime->SetNumberField(TEXT("mips"), Texture2D->GetNumMips());
		Runtime->SetStringField(TEXT("pixelFormat"), GetPixelFormatString(Texture2D->GetPixelFormat()));
		Runtime->SetBoolField(TEXT("hasAlphaChannel"), Texture2D->HasAlphaChannel());
		Data->SetObjectField(TEXT("runtime"), Runtime);
	}

	static const TSet<FName> Always = {
		FName(TEXT("CompressionSettings")), FName(TEXT("SRGB")), FName(TEXT("LODGroup")), FName(TEXT("MaxTextureSize")),
		FName(TEXT("LODBias")), FName(TEXT("Filter")), FName(TEXT("MipGenSettings")), FName(TEXT("CompressionNoAlpha")),
		FName(TEXT("AddressX")), FName(TEXT("AddressY")), FName(TEXT("NeverStream")), FName(TEXT("VirtualTextureStreaming")),
		FName(TEXT("bFlipGreenChannel")),
	};
	TSharedPtr<FJsonObject> Settings = MakeShareable(new FJsonObject());
	OCExportObjectProps(Texture, UObject::StaticClass(), Always, Settings);
	Data->SetObjectField(TEXT("settings"), Settings);

	const FTextureSource& Source = Texture->Source;
	if (Source.IsValid())
	{
		TSharedPtr<FJsonObject> SourceObj = MakeShareable(new FJsonObject());
		const ETextureSourceFormat Format = Source.GetFormat();
		const int64 Width = Source.GetSizeX();
		const int64 Height = Source.GetSizeY();
		SourceObj->SetNumberField(TEXT("width"), (double)Width);
		SourceObj->SetNumberField(TEXT("height"), (double)Height);
		SourceObj->SetNumberField(TEXT("mips"), Source.GetNumMips());
		if (const UEnum* FormatEnum = StaticEnum<ETextureSourceFormat>())
		{
			SourceObj->SetStringField(TEXT("format"), FormatEnum->GetNameStringByValue((int64)Format));
		}

		// Channel layout of the formats worth reading; anything else is skipped.
		int32 NumChannels = 0;
		int32 BytesPerChannel = 0;
		bool bFloat = false;
		bool bBgra = false;
		switch (Format)
		{
		case TSF_G8:      NumChannels = 1; BytesPerChannel = 1; break;
		case TSF_BGRA8:   NumChannels = 4; BytesPerChannel = 1; bBgra = true; break;
		case TSF_G16:     NumChannels = 1; BytesPerChannel = 2; break;
		case TSF_RGBA16:  NumChannels = 4; BytesPerChannel = 2; break;
		case TSF_RGBA16F: NumChannels = 4; BytesPerChannel = 2; bFloat = true; break;
		case TSF_RGBA32F: NumChannels = 4; BytesPerChannel = 4; bFloat = true; break;
		default: break;
		}

		TArray64<uint8> Mip;
		if (bStats && NumChannels > 0 && Width > 0 && Height > 0
			&& const_cast<FTextureSource&>(Source).GetMipData(Mip, 0)
			&& Mip.Num() >= Width * Height * NumChannels * BytesPerChannel)
		{
			double Min[4] = { DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX };
			double Max[4] = { -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
			double Sum[4] = { 0.0, 0.0, 0.0, 0.0 };
			const int64 NumPixels = Width * Height;
			const uint8* Bytes = Mip.GetData();
			for (int64 Pixel = 0; Pixel < NumPixels; ++Pixel)
			{
				for (int32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					const uint8* At = Bytes + (Pixel * NumChannels + Channel) * BytesPerChannel;
					double Value = 0.0;
					if (bFloat && BytesPerChannel == 2)
					{
						FFloat16 Half;
						FMemory::Memcpy(&Half.Encoded, At, 2);
						Value = Half.GetFloat();
					}
					else if (bFloat)
					{
						float F;
						FMemory::Memcpy(&F, At, 4);
						Value = F;
					}
					else if (BytesPerChannel == 2)
					{
						uint16 V;
						FMemory::Memcpy(&V, At, 2);
						Value = V / 65535.0;
					}
					else
					{
						Value = *At / 255.0;
					}
					// Report in RGBA order whatever the storage order.
					const int32 Out = (bBgra && Channel < 3) ? 2 - Channel : Channel;
					Min[Out] = FMath::Min(Min[Out], Value);
					Max[Out] = FMath::Max(Max[Out], Value);
					Sum[Out] += Value;
				}
			}
			static const TCHAR* Names[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			TSharedPtr<FJsonObject> Stats = MakeShareable(new FJsonObject());
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				TSharedPtr<FJsonObject> ChannelObj = MakeShareable(new FJsonObject());
				ChannelObj->SetNumberField(TEXT("min"), OCRound(Min[Channel], 10000.0));
				ChannelObj->SetNumberField(TEXT("max"), OCRound(Max[Channel], 10000.0));
				ChannelObj->SetNumberField(TEXT("mean"), OCRound(Sum[Channel] / (double)NumPixels, 10000.0));
				Stats->SetObjectField(NumChannels == 1 ? TEXT("grey") : Names[Channel], ChannelObj);
			}
			SourceObj->SetObjectField(TEXT("channelStats"), Stats);
			SourceObj->SetStringField(TEXT("channelStatsNote"), TEXT("source values, 0..1 for integer formats, before sRGB decode"));
		}
		Data->SetObjectField(TEXT("source"), SourceObj);
	}

	return FOpenCodeResponse::Success(Request.Id, Data);
}

// ---- get_level_lighting ------------------------------------------------------
// Everything that decides how the level is lit and graded, in one reply: the
// directional/sky lights, height fog, sky atmosphere, post-process volumes
// (only the settings each one overrides) and the renderer switches that matter.
static FOpenCodeResponse OCHandleGetLevelLighting(const FOpenCodeRequest& Request)
{
	UWorld* World = nullptr;
#if WITH_EDITOR
	if (GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
#endif
	if (!World)
	{
		World = GWorld;
	}
	if (!World)
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No world is loaded"));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("level"), World->GetOutermost()->GetName());

	static const TSet<FName> LightAlways = {
		FName(TEXT("Intensity")), FName(TEXT("LightColor")), FName(TEXT("bUseTemperature")), FName(TEXT("Temperature")),
		FName(TEXT("CastShadows")), FName(TEXT("IndirectLightingIntensity")), FName(TEXT("LightSourceAngle")),
		FName(TEXT("DynamicShadowDistanceMovableLight")), FName(TEXT("DynamicShadowCascades")),
		FName(TEXT("bAtmosphereSunLight")), FName(TEXT("SourceType")), FName(TEXT("bRealTimeCapture")),
		FName(TEXT("bLowerHemisphereIsBlack")), FName(TEXT("LowerHemisphereColor")),
	};
	static const TSet<FName> FogAlways = {
		FName(TEXT("FogDensity")), FName(TEXT("FogHeightFalloff")), FName(TEXT("FogInscatteringLuminance")),
		FName(TEXT("FogMaxOpacity")), FName(TEXT("StartDistance")), FName(TEXT("bEnableVolumetricFog")),
	};
	static const TSet<FName> None;

	TArray<TSharedPtr<FJsonValue>> DirectionalLights, SkyLights, Fogs, Atmospheres, LocalLights, Volumes;
	int32 LocalLightCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
#if WITH_EDITOR
		const FString Label = Actor->GetActorLabel();
#else
		const FString Label = Actor->GetName();
#endif

		TInlineComponentArray<USceneComponent*> Components(Actor);
		for (USceneComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject());
			Entry->SetStringField(TEXT("actor"), Label);
			Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
			const TSet<FName>* Always = nullptr;
			TArray<TSharedPtr<FJsonValue>>* Bucket = nullptr;

			if (Component->IsA<UDirectionalLightComponent>())
			{
				const FRotator Rotation = Component->GetComponentRotation();
				const FVector Forward = Component->GetForwardVector();
				Entry->SetStringField(TEXT("rotation"), FString::Printf(TEXT("pitch=%.2f yaw=%.2f roll=%.2f"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
				Entry->SetStringField(TEXT("lightTravelDirection"), FString::Printf(TEXT("%.4f, %.4f, %.4f"), Forward.X, Forward.Y, Forward.Z));
				Always = &LightAlways;
				Bucket = &DirectionalLights;
			}
			else if (Component->IsA<USkyLightComponent>())
			{
				Always = &LightAlways;
				Bucket = &SkyLights;
			}
			else if (Component->IsA<UExponentialHeightFogComponent>())
			{
				Entry->SetNumberField(TEXT("heightZ"), Component->GetComponentLocation().Z);
				Always = &FogAlways;
				Bucket = &Fogs;
			}
			else if (Component->IsA<USkyAtmosphereComponent>())
			{
				Always = &None;
				Bucket = &Atmospheres;
			}
			else if (Component->IsA<ULocalLightComponent>())
			{
				++LocalLightCount;
				if (LocalLights.Num() < 32)
				{
					const FVector Location = Component->GetComponentLocation();
					Entry->SetStringField(TEXT("location"), FString::Printf(TEXT("%.1f, %.1f, %.1f"), Location.X, Location.Y, Location.Z));
					Always = &LightAlways;
					Bucket = &LocalLights;
				}
			}

			if (Bucket && Always)
			{
				TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());
				OCExportObjectProps(Component, USceneComponent::StaticClass(), *Always, Props);
				Entry->SetObjectField(TEXT("settings"), Props);
				Bucket->Add(MakeShareable(new FJsonValueObject(Entry)));
			}
		}

		if (APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
		{
			TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject());
			Entry->SetStringField(TEXT("actor"), Label);
			Entry->SetBoolField(TEXT("enabled"), Volume->bEnabled != 0);
			Entry->SetBoolField(TEXT("unbound"), Volume->bUnbound != 0);
			Entry->SetNumberField(TEXT("blendWeight"), Volume->BlendWeight);
			Entry->SetNumberField(TEXT("priority"), Volume->Priority);

			// Only what this volume overrides: bOverride_X set means X is live.
			TSharedPtr<FJsonObject> Overrides = MakeShareable(new FJsonObject());
			const UScriptStruct* SettingsStruct = FPostProcessSettings::StaticStruct();
			for (TFieldIterator<FProperty> PropIt(SettingsStruct); PropIt; ++PropIt)
			{
				const FBoolProperty* Flag = CastField<FBoolProperty>(*PropIt);
				if (!Flag || !Flag->GetName().StartsWith(TEXT("bOverride_")) || !Flag->GetPropertyValue_InContainer(&Volume->Settings))
				{
					continue;
				}
				const FString SettingName = Flag->GetName().RightChop(10);
				if (const FProperty* Setting = SettingsStruct->FindPropertyByName(FName(*SettingName)))
				{
					FString Value;
					Setting->ExportText_InContainer(0, Value, &Volume->Settings, &Volume->Settings, nullptr, PPF_None);
					Overrides->SetStringField(SettingName, Value);
				}
			}
			Entry->SetObjectField(TEXT("overrides"), Overrides);
			Volumes.Add(MakeShareable(new FJsonValueObject(Entry)));
		}
	}

	Data->SetArrayField(TEXT("directionalLights"), DirectionalLights);
	Data->SetArrayField(TEXT("skyLights"), SkyLights);
	Data->SetArrayField(TEXT("heightFog"), Fogs);
	Data->SetArrayField(TEXT("skyAtmosphere"), Atmospheres);
	Data->SetArrayField(TEXT("postProcessVolumes"), Volumes);
	Data->SetNumberField(TEXT("localLightCount"), LocalLightCount);
	Data->SetArrayField(TEXT("localLights"), LocalLights);

	static const TCHAR* CVarNames[] = {
		TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ReflectionMethod"), TEXT("r.Shadow.Virtual.Enable"),
		TEXT("r.AntiAliasingMethod"), TEXT("r.DefaultFeature.AutoExposure"), TEXT("r.DefaultFeature.AutoExposure.Method"),
		TEXT("r.DefaultFeature.AutoExposure.Bias"), TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"),
		TEXT("r.DefaultFeature.Bloom"), TEXT("r.DefaultFeature.AmbientOcclusion"), TEXT("r.Tonemapper.Sharpen"),
		TEXT("r.SkyAtmosphere"), TEXT("r.VolumetricFog"), TEXT("r.GenerateMeshDistanceFields"), TEXT("r.ForwardShading"),
		TEXT("grass.DensityScale"), TEXT("grass.CullDistanceScale"), TEXT("foliage.DensityScale"),
		TEXT("sg.ShadowQuality"), TEXT("sg.PostProcessQuality"), TEXT("sg.FoliageQuality"), TEXT("sg.GlobalIlluminationQuality"),
	};
	TSharedPtr<FJsonObject> CVars = MakeShareable(new FJsonObject());
	for (const TCHAR* Name : CVarNames)
	{
		if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVars->SetStringField(Name, Var->GetString());
		}
	}
	Data->SetObjectField(TEXT("cvars"), CVars);

	return FOpenCodeResponse::Success(Request.Id, Data);
}

// ---- get_material_hlsl -------------------------------------------------------
// The HLSL Unreal generates for a Material or MaterialInstance, with every
// static switch already resolved for that asset — the exact shader, where the
// node graph is only the recipe. section "generated" (default) returns just the
// functions the translator writes; "full" the whole material template.
static FString OCExtractHlslFunction(const FString& Source, const TCHAR* Signature)
{
	int32 SearchFrom = 0;
	for (;;)
	{
		const int32 At = Source.Find(Signature, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (At == INDEX_NONE)
		{
			return FString();
		}
		const int32 Open = Source.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		const int32 Semicolon = Source.Find(TEXT(";"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		if (Open == INDEX_NONE)
		{
			return FString();
		}
		if (Semicolon != INDEX_NONE && Semicolon < Open)
		{
			SearchFrom = Semicolon + 1;   // a forward declaration; keep looking for the body
			continue;
		}
		int32 LineStart = At;
		while (LineStart > 0 && Source[LineStart - 1] != TEXT('\n'))
		{
			--LineStart;
		}
		int32 Depth = 0;
		for (int32 i = Open; i < Source.Len(); ++i)
		{
			if (Source[i] == TEXT('{'))
			{
				++Depth;
			}
			else if (Source[i] == TEXT('}') && --Depth == 0)
			{
				return Source.Mid(LineStart, i - LineStart + 1);
			}
		}
		return FString();
	}
}

static FOpenCodeResponse OCHandleGetMaterialHlsl(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);
	FString Section = TEXT("generated");
	Request.Args->TryGetStringField(TEXT("section"), Section);

	UMaterialInterface* Material = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *AssetPath));
	if (!Material)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Material or MaterialInstance not found: %s"), *AssetPath));
	}
#if WITH_EDITOR
	// The editor world's feature level (asking the RHI global would need a
	// module this one does not link).
	ERHIFeatureLevel::Type FeatureLevel = ERHIFeatureLevel::SM5;
	if (GEditor)
	{
		if (const UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			FeatureLevel = World->GetFeatureLevel();
		}
	}
	FMaterialResource* Resource = Material->GetMaterialResource(FeatureLevel);
	FString Source;
	if (!Resource || !Resource->GetMaterialExpressionSource(Source) || Source.IsEmpty())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("Unreal could not translate this material to HLSL (does it compile?)"));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset"), Material->GetPathName());
	Data->SetNumberField(TEXT("fullSourceChars"), Source.Len());

	FString Text;
	if (!Section.Equals(TEXT("full"), ESearchCase::IgnoreCase))
	{
		static const TCHAR* Signatures[] = {
			TEXT("float3 GetMaterialWorldPositionOffsetRaw("),
			TEXT("float3 GetMaterialPreviousWorldPositionOffsetRaw("),
			TEXT("void GetMaterialCustomizedUVs("),
			TEXT("void GetCustomInterpolators("),
			TEXT("void CalcPixelMaterialInputs("),
		};
		for (const TCHAR* Signature : Signatures)
		{
			const FString Function = OCExtractHlslFunction(Source, Signature);
			if (!Function.IsEmpty())
			{
				Text += Function;
				Text += TEXT("\n\n");
			}
		}
	}
	if (Text.IsEmpty())
	{
		Text = Source;
		Section = TEXT("full");
	}
	Data->SetStringField(TEXT("section"), Section);
	Data->SetStringField(TEXT("note"), TEXT("Parameters appear as Material.PreshaderBuffer[n] slots; get_material_parameters has their values. Static switches are already resolved."));
	Data->SetStringField(TEXT("text"), Text);
	return FOpenCodeResponse::Success(Request.Id, Data);
#else
	return FOpenCodeResponse::Failure(Request.Id, TEXT("Editor only"));
#endif
}

// ---- export_asset_text -------------------------------------------------------
// Any asset as T3D text: every sub-object with every non-default property.
// For what no dedicated tool covers (material instances, grass types, ...).
static FOpenCodeResponse OCHandleExportAssetText(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Asset)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}
	UExporter* Exporter = UExporter::FindExporter(Asset, TEXT("T3D"));
	if (!Exporter)
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("No T3D exporter accepts this asset"));
	}
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OpenCodeBridge"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString File = Directory / (Asset->GetName() + TEXT(".t3d"));
	FString Text;
	if (UExporter::ExportToFile(Asset, Exporter, *File, false, false, false) != 1 || !FFileHelper::LoadFileToString(Text, *File))
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("T3D export failed"));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset"), Asset->GetPathName());
	Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Data->SetStringField(TEXT("exportedTo"), File);
	Data->SetNumberField(TEXT("chars"), Text.Len());
	Data->SetStringField(TEXT("text"), Text);
	return FOpenCodeResponse::Success(Request.Id, Data);
}

// ---- live_compile ------------------------------------------------------------
// Runs a Live Coding compile and waits for it.
//
// When it fails, Live Coding shows the compiler's errors only in its own
// console window: neither log file gets them. So on failure this re-runs the
// compiler on each source file that is newer than its Live Coding object, with
// the same response file Live Coding uses, and returns what it prints.
static void OCDiagnoseFailedCompile(TArray<TSharedPtr<FJsonValue>>& OutDiagnostics)
{
	TArray<FString> SearchRoots;
	SearchRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()));
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		if (Plugin->GetType() == EPluginType::Project)
		{
			SearchRoots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Intermediate")));
		}
	}

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::EngineSourceDir());
	const FString ScratchDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OpenCodeBridge"));
	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*ScratchDir, true);

	int32 Checked = 0;
	for (const FString& Root : SearchRoots)
	{
		TArray<FString> ResponseFiles;
		Files.FindFilesRecursive(ResponseFiles, *Root, TEXT("*.rsp.lc"), true, false);
		for (const FString& ResponseFile : ResponseFiles)
		{
			if (Checked >= 3 || !ResponseFile.EndsWith(TEXT(".obj.rsp.lc")))
			{
				continue;
			}
			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *ResponseFile) || Lines.Num() == 0)
			{
				continue;
			}
			const FString SourceFile = Lines[0].TrimStartAndEnd().TrimQuotes();
			FString ObjectFile;
			FString SharedResponse;
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("/Fo")))
				{
					ObjectFile = Line.RightChop(3).TrimQuotes();
				}
				else if (Line.StartsWith(TEXT("@")))
				{
					SharedResponse = Line.RightChop(1).TrimQuotes();
				}
			}
			// Only files Live Coding would have had to rebuild.
			const FDateTime SourceTime = Files.GetTimeStamp(*SourceFile);
			if (SourceTime == FDateTime::MinValue() || Files.GetTimeStamp(*ObjectFile) >= SourceTime)
			{
				continue;
			}

			// The compiler sits next to the standard library the build includes.
			FString Compiler;
			TArray<FString> SharedLines;
			FFileHelper::LoadFileToStringArray(SharedLines, *SharedResponse);
			for (const FString& Line : SharedLines)
			{
				const int32 At = Line.Find(TEXT("\\VC\\Tools\\MSVC\\"));
				if (Line.StartsWith(TEXT("/external:I")) && At != INDEX_NONE)
				{
					const FString IncludeDir = Line.RightChop(11).TrimStartAndEnd().TrimQuotes();
					Compiler = FPaths::GetPath(IncludeDir) / TEXT("bin/Hostx64/x64/cl.exe");
					break;
				}
			}
			if (Compiler.IsEmpty() || !Files.FileExists(*Compiler))
			{
				continue;
			}

			FString CheckResponse;
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("/experimental:log")) || Line.StartsWith(TEXT("/sourceDependencies")))
				{
					continue;
				}
				CheckResponse += Line.StartsWith(TEXT("/Fo"))
					? FString::Printf(TEXT("/Fo\"%s\""), *(ScratchDir / TEXT("compile_check.obj")))
					: Line;
				CheckResponse += TEXT("\n");
			}
			const FString CheckFile = ScratchDir / TEXT("compile_check.rsp");
			if (!FFileHelper::SaveStringToFile(CheckResponse, *CheckFile))
			{
				continue;
			}

			++Checked;
			int32 ReturnCode = 0;
			FString StdOut, StdErr;
			FPlatformProcess::ExecProcess(*Compiler, *FString::Printf(TEXT("@\"%s\""), *CheckFile), &ReturnCode, &StdOut, &StdErr, *WorkingDir);

			TArray<FString> Output;
			(StdOut + TEXT("\n") + StdErr).ParseIntoArrayLines(Output);
			int32 Reported = 0;
			for (const FString& Line : Output)
			{
				if (Reported < 40 && (Line.Contains(TEXT("error")) || Line.Contains(TEXT("warning C"))))
				{
					OutDiagnostics.Add(MakeShareable(new FJsonValueString(Line.Left(500))));
					++Reported;
				}
			}
			if (ReturnCode != 0 && Reported == 0)
			{
				OutDiagnostics.Add(MakeShareable(new FJsonValueString(
					FString::Printf(TEXT("%s: the compiler exited with %d but printed no error lines"), *FPaths::GetCleanFilename(SourceFile), ReturnCode))));
			}
		}
	}
	Files.Delete(*(ScratchDir / TEXT("compile_check.obj")), false, true, true);
}

static FOpenCodeResponse OCHandleLiveCompile(const FOpenCodeRequest& Request)
{
#if PLATFORM_WINDOWS && WITH_EDITOR
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("The LiveCoding module is not loaded (enable Live Coding in Editor Preferences)"));
	}

	const FString ConsoleLogPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Programs/LiveCodingConsole/Saved/Logs/LiveCodingConsole.log"));
	FString Before;
	FFileHelper::LoadFileToString(Before, *ConsoleLogPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite);

	ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
	LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

	const TCHAR* ResultName = TEXT("Unknown");
	switch (Result)
	{
	case ELiveCodingCompileResult::Success:            ResultName = TEXT("Success"); break;
	case ELiveCodingCompileResult::NoChanges:          ResultName = TEXT("NoChanges"); break;
	case ELiveCodingCompileResult::InProgress:         ResultName = TEXT("InProgress"); break;
	case ELiveCodingCompileResult::CompileStillActive: ResultName = TEXT("CompileStillActive"); break;
	case ELiveCodingCompileResult::NotStarted:         ResultName = TEXT("NotStarted"); break;
	case ELiveCodingCompileResult::Failure:            ResultName = TEXT("Failure"); break;
	case ELiveCodingCompileResult::Cancelled:          ResultName = TEXT("Cancelled"); break;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("result"), ResultName);
	Data->SetBoolField(TEXT("succeeded"), Result == ELiveCodingCompileResult::Success || Result == ELiveCodingCompileResult::NoChanges);
	Data->SetStringField(TEXT("bridgeBuildBeforePatch"), FString(TEXT(__DATE__)) + TEXT(" ") + TEXT(__TIME__));

	FString After;
	TArray<TSharedPtr<FJsonValue>> Lines;
	if (FFileHelper::LoadFileToString(After, *ConsoleLogPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
	{
		const FString Added = (After.Len() >= Before.Len() && After.StartsWith(Before, ESearchCase::CaseSensitive)) ? After.Mid(Before.Len()) : After;
		TArray<FString> Raw;
		Added.ParseIntoArrayLines(Raw);
		for (FString& Line : Raw)
		{
			// Benign linker noise that would otherwise bury the real messages.
			if (Line.Contains(TEXT("Cannot find image section .voltbl")))
			{
				continue;
			}
			const int32 Tag = Line.Find(TEXT("LogLiveCodingServer: "));
			if (Tag != INDEX_NONE)
			{
				Line = Line.Mid(Tag + 21);
			}
			if (Line.Len() > 400)
			{
				Line = Line.Left(400) + TEXT("...");
			}
			Lines.Add(MakeShareable(new FJsonValueString(Line)));
		}
		if (Lines.Num() > 200)
		{
			Lines.RemoveAt(0, Lines.Num() - 200);
		}
	}
	Data->SetArrayField(TEXT("log"), Lines);

	if (Result == ELiveCodingCompileResult::Failure)
	{
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		OCDiagnoseFailedCompile(Diagnostics);
		Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	}
	return FOpenCodeResponse::Success(Request.Id, Data);
#else
	return FOpenCodeResponse::Failure(Request.Id, TEXT("Live Coding is only available in the Windows editor"));
#endif
}
