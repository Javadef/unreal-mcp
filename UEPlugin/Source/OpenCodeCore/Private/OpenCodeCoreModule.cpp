#include "OpenCodeCoreModule.h"
#include "OpenCodeTCPServer.h"
#include "OpenCodeToolHandler.h"
#include "OpenCodeProtocol.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Async/TaskGraphInterfaces.h"

#define LOCTEXT_NAMESPACE "FOpenCodeCoreModule"

DEFINE_LOG_CATEGORY_STATIC(LogOpenCodeCore, Log, All);

void FOpenCodeCoreModule::StartupModule()
{
	ListenPort = 3099;
	UE_LOG(LogOpenCodeCore, Log, TEXT("OpenCodeCore starting on port %d"), ListenPort);

	GLog->AddOutputDevice(&FOpenCodeLogCapture::Get());

	FOpenCodeToolHandler::Get().RegisterTools();

	TCPServer = MakeUnique<FOpenCodeTCPServer>(ListenPort,
		FOpenCodeMessageDelegate::CreateRaw(this, &FOpenCodeCoreModule::HandleMessage));

	if (TCPServer->Start())
	{
		UE_LOG(LogOpenCodeCore, Log, TEXT("OpenCodeCore TCP server started on port %d"), ListenPort);
	}
	else
	{
		UE_LOG(LogOpenCodeCore, Error, TEXT("Failed to start OpenCodeCore TCP server"));
	}
}

void FOpenCodeCoreModule::ShutdownModule()
{
	if (TCPServer.IsValid())
	{
		TCPServer->Stop();
		TCPServer.Reset();
	}

	GLog->RemoveOutputDevice(&FOpenCodeLogCapture::Get());
	UE_LOG(LogOpenCodeCore, Log, TEXT("OpenCodeCore module shutdown"));
}

FOpenCodeCoreModule& FOpenCodeCoreModule::Get()
{
	return FModuleManager::LoadModuleChecked<FOpenCodeCoreModule>(TEXT("OpenCodeCore"));
}

FString FOpenCodeCoreModule::HandleMessage(const FString& JsonRequest)
{
	FOpenCodeRequest Request;
	if (!Request.ParseFromJson(JsonRequest))
	{
		return FOpenCodeResponse::Failure(TEXT(""), TEXT("Invalid request JSON")).ToJson();
	}

	FString ResponseJson;

	if (IsInGameThread())
	{
		FOpenCodeResponse Response = FOpenCodeToolHandler::Get().Dispatch(Request);
		ResponseJson = Response.ToJson();
	}
	else
	{
		FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([&]()
		{
			FOpenCodeResponse Response = FOpenCodeToolHandler::Get().Dispatch(Request);
			ResponseJson = Response.ToJson();
		}, TStatId(), nullptr, ENamedThreads::GameThread);

		FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
	}

	if (ResponseJson.IsEmpty())
	{
		ResponseJson = FOpenCodeResponse::Failure(Request.Id, TEXT("Internal error")).ToJson();
	}
	return ResponseJson;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenCodeCoreModule, OpenCodeCore)
