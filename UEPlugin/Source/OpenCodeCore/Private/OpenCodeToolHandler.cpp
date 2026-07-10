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
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
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
}

void FOpenCodeToolHandler::RegisterTool(const FString& ToolName, FOpenCodeToolDelegate Delegate)
{
	ToolDelegates.Add(ToolName, Delegate);
}

FOpenCodeResponse FOpenCodeToolHandler::Dispatch(const FOpenCodeRequest& Request)
{
	FOpenCodeToolDelegate* Found = ToolDelegates.Find(Request.Tool);
	if (!Found)
	{
		return FOpenCodeResponse::Failure(Request.Id, FString::Printf(TEXT("Unknown tool: %s"), *Request.Tool));
	}
	if (!Found->IsBound())
	{
		return FOpenCodeResponse::Failure(Request.Id, TEXT("Tool delegate not bound"));
	}
	return Found->Execute(Request);
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

	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	else
	{
		Filter.PackagePaths.Add(TEXT("/Game"));
	}

	if (!AssetTypeFilter.IsEmpty())
	{
		UClass* Class = FindObject<UClass>(nullptr, *(TEXT("/Script/Engine.") + AssetTypeFilter));
		if (!Class)
		{
			Class = FindObject<UClass>(nullptr, *(TEXT("/Script/CoreUObject.") + AssetTypeFilter));
		}
		if (Class)
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(Class));
		}
	}

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// Filter by query
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Skipped = 0;
	int32 Added = 0;
	for (const FAssetData& Asset : AssetList)
	{
		if (!Query.IsEmpty() && !Asset.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Skipped < Offset)
		{
			Skipped++;
			continue;
		}
		if (Added >= Limit) break;

		TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		Results.Add(MakeShareable(new FJsonValueObject(Item)));
		Added++;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetNumberField(TEXT("total"), AssetList.Num());
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

FOpenCodeResponse FOpenCodeToolHandler::HandleGetMaterialGraph(const FOpenCodeRequest& Request)
{
	FString AssetPath;
	Request.Args->TryGetStringField(TEXT("assetPath"), AssetPath);

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
		Data->SetStringField(TEXT("error"), TEXT("Asset must be a Material or MaterialFunction"));
		return FOpenCodeResponse::Success(Request.Id, Data);
	}

	TArray<UMaterialExpression*> AllExpressions;
	if (Mat)
	{
		Mat->GetAllExpressionsInMaterialAndFunctionsOfType<UMaterialExpression>(AllExpressions);
	}
	else
	{
		TArrayView<const TObjectPtr<UMaterialExpression>> Exprs = MF->GetExpressions();
		for (const TObjectPtr<UMaterialExpression>& E : Exprs)
		{
			AllExpressions.Add(E.Get());
		}
	}

	Data->SetStringField(TEXT("asset"), Asset->GetName());
	Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Data->SetNumberField(TEXT("expressionCount"), AllExpressions.Num());

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (int32 NodeIdx = 0; NodeIdx < AllExpressions.Num(); ++NodeIdx)
	{
		UMaterialExpression* Expr = AllExpressions[NodeIdx];
		if (!Expr) continue;

		TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject());
		Node->SetNumberField(TEXT("id"), NodeIdx);
		Node->SetStringField(TEXT("type"), Expr->GetClass()->GetName());
		Node->SetStringField(TEXT("desc"), Expr->Desc);

		// Function calls
		if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			if (FuncCall->MaterialFunction)
			{
				Node->SetStringField(TEXT("function"), FuncCall->MaterialFunction->GetPathName());
			}
		}

		// Scalar params
		if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
		{
			Node->SetStringField(TEXT("paramName"), Scalar->ParameterName.ToString());
			Node->SetStringField(TEXT("group"), Scalar->Group.ToString());
			Node->SetNumberField(TEXT("defaultValue"), Scalar->DefaultValue);
		}

		// Vector params
		if (UMaterialExpressionVectorParameter* Vec = Cast<UMaterialExpressionVectorParameter>(Expr))
		{
			Node->SetStringField(TEXT("paramName"), Vec->ParameterName.ToString());
			Node->SetStringField(TEXT("group"), Vec->Group.ToString());
			TSharedPtr<FJsonObject> Val = MakeShareable(new FJsonObject());
			Val->SetNumberField(TEXT("r"), Vec->DefaultValue.R);
			Val->SetNumberField(TEXT("g"), Vec->DefaultValue.G);
			Val->SetNumberField(TEXT("b"), Vec->DefaultValue.B);
			Node->SetObjectField(TEXT("defaultValue"), Val);
		}

		// Constants
		if (UMaterialExpressionConstant* Const = Cast<UMaterialExpressionConstant>(Expr))
		{
			Node->SetNumberField(TEXT("r"), Const->R);
		}

		// Constants 2, 3, 4 (for RGB, RGBA constants)
		if (UMaterialExpressionConstant2Vector* Const2 = Cast<UMaterialExpressionConstant2Vector>(Expr))
		{
			Node->SetNumberField(TEXT("r"), Const2->R);
			Node->SetNumberField(TEXT("g"), Const2->G);
		}
		if (UMaterialExpressionConstant3Vector* Const3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
		{
			Node->SetNumberField(TEXT("r"), Const3->Constant.R);
			Node->SetNumberField(TEXT("g"), Const3->Constant.G);
			Node->SetNumberField(TEXT("b"), Const3->Constant.B);
		}
		if (UMaterialExpressionConstant4Vector* Const4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
		{
			Node->SetNumberField(TEXT("r"), Const4->Constant.R);
			Node->SetNumberField(TEXT("g"), Const4->Constant.G);
			Node->SetNumberField(TEXT("b"), Const4->Constant.B);
			Node->SetNumberField(TEXT("a"), Const4->Constant.A);
		}

		// Static switches
		if (UMaterialExpressionStaticSwitchParameter* Switch = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
		{
			Node->SetStringField(TEXT("paramName"), Switch->ParameterName.ToString());
			Node->SetBoolField(TEXT("defaultValue"), Switch->DefaultValue);
		}

		// Texture samples
		if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
		{
			if (TexSample->Texture)
			{
				Node->SetStringField(TEXT("texture"), TexSample->Texture->GetPathName());
			}
		}

		// Function input (for material functions)
		if (UMaterialExpressionFunctionInput* FuncInput = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			Node->SetStringField(TEXT("inputName"), FuncInput->InputName.ToString());
			Node->SetStringField(TEXT("inputType"), FuncInput->InputType == FunctionInput_Scalar ? TEXT("Scalar") :
				FuncInput->InputType == FunctionInput_Vector4 ? TEXT("Vector4") :
				FuncInput->InputType == FunctionInput_Vector3 ? TEXT("Vector3") :
				FuncInput->InputType == FunctionInput_Vector2 ? TEXT("Vector2") :
				FuncInput->InputType == FunctionInput_StaticBool ? TEXT("StaticBool") :
				FuncInput->InputType == FunctionInput_Texture2D ? TEXT("Texture2D") : TEXT("Unknown"));
			Node->SetStringField(TEXT("preview"), FuncInput->PreviewValue.ToString());
		}

		// Function output
		if (UMaterialExpressionFunctionOutput* FuncOutput = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			Node->SetStringField(TEXT("outputName"), FuncOutput->OutputName.ToString());
		}

		// Common binary ops (Add, Multiply)
		if (UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expr))
		{
			Node->SetNumberField(TEXT("constA"), Add->ConstA);
			Node->SetNumberField(TEXT("constB"), Add->ConstB);
		}
		if (UMaterialExpressionMultiply* Mul = Cast<UMaterialExpressionMultiply>(Expr))
		{
			Node->SetNumberField(TEXT("constA"), Mul->ConstA);
			Node->SetNumberField(TEXT("constB"), Mul->ConstB);
		}

		Nodes.Add(MakeShareable(new FJsonValueObject(Node)));
	}

	Data->SetArrayField(TEXT("expressions"), Nodes);
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

	FString FullPath = FPaths::ProjectDir() / TEXT("Source") / FilePath;
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
