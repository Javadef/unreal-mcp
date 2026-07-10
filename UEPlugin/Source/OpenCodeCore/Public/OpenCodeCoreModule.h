#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "OpenCodeTCPServer.h"

class OPENCODECORE_API FOpenCodeCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FOpenCodeCoreModule& Get();

	class FOpenCodeTCPServer* GetServer() const { return TCPServer.Get(); }

private:
	TUniquePtr<class FOpenCodeTCPServer> TCPServer;
	int32 ListenPort;

	FString HandleMessage(const FString& JsonRequest);
};
