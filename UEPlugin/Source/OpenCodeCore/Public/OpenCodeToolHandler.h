#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "OpenCodeProtocol.h"
#include "Misc/OutputDevice.h"

DECLARE_DELEGATE_RetVal_OneParam(FOpenCodeResponse, FOpenCodeToolDelegate, const FOpenCodeRequest& /*Request*/);

struct FLogEntry
{
	FString Text;
	FString Category;
	ELogVerbosity::Type Verbosity;

	FLogEntry() : Verbosity(ELogVerbosity::Log) {}
	FLogEntry(const FString& InText, const FName& InCategory, ELogVerbosity::Type InVerbosity)
		: Text(InText)
		, Category(InCategory.ToString())
		, Verbosity(InVerbosity)
	{}
};

class FOpenCodeLogCapture : public FOutputDevice
{
public:
	static FOpenCodeLogCapture& Get()
	{
		static FOpenCodeLogCapture Instance;
		return Instance;
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FScopeLock Lock(&CriticalSection);
		if (Entries.Num() >= MaxEntries)
		{
			Entries.RemoveAt(0, Entries.Num() - MaxEntries + 1);
		}
		Entries.Add(FLogEntry(FString(V).TrimEnd(), Category, Verbosity));
	}

	void GetEntries(TArray<FLogEntry>& OutEntries) const
	{
		FScopeLock Lock(&CriticalSection);
		OutEntries = Entries;
	}

	void Clear()
	{
		FScopeLock Lock(&CriticalSection);
		Entries.Empty();
	}

private:
	FOpenCodeLogCapture()
	{
		MaxEntries = 2000;
	}

	int32 MaxEntries;
	TArray<FLogEntry> Entries;
	mutable FCriticalSection CriticalSection;
};

class FOpenCodeToolHandler
{
public:
	static FOpenCodeToolHandler& Get();

	void RegisterTools();

	FOpenCodeResponse Dispatch(const FOpenCodeRequest& Request);

private:
	TMap<FString, FOpenCodeToolDelegate> ToolDelegates;

	void RegisterTool(const FString& ToolName, FOpenCodeToolDelegate Delegate);

	// Tool implementations
	static FOpenCodeResponse HandlePing(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetProjectStructure(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetSceneHierarchy(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleExecuteConsoleCommand(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleSearchAssets(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetAssetDetails(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetClassDetails(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetModuleDependencies(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetPluginList(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetOutputLog(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetBuildLogs(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetCompilationStatus(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetBlueprintList(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetSelectedActors(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetActorDetails(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleSetActorProperty(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGenerateCode(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleSearchClasses(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetCppHierarchy(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetMaterialGraph(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleGetMaterialParameters(const FOpenCodeRequest& Request);
	static FOpenCodeResponse HandleRunPython(const FOpenCodeRequest& Request);
};
