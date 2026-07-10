#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

DECLARE_DELEGATE_RetVal_OneParam(FString, FOpenCodeMessageDelegate, const FString& /*JsonRequest*/);

class FOpenCodeTCPConnection;

class OPENCODECORE_API FOpenCodeTCPServer : public FRunnable
{
public:
	FOpenCodeTCPServer(int32 Port, FOpenCodeMessageDelegate InMessageHandler);
	virtual ~FOpenCodeTCPServer();

	bool Start();
	void Stop();

	void SendMessage(const FString& JsonMessage);

	virtual uint32 Run() override;
	virtual void Exit() override;

	bool IsRunning() const { return bIsRunning; }

private:
	int32 ListenPort;
	FOpenCodeMessageDelegate MessageHandler;
	FSocket* ListenerSocket;
	FRunnableThread* Thread;
	FThreadSafeBool bIsRunning;
	FThreadSafeBool bStopRequested;

	TArray<FOpenCodeTCPConnection*> ActiveConnections;
	FCriticalSection ConnectionsLock;

	void AcceptConnection(FSocket* ClientSocket);
	void RemoveConnection(FOpenCodeTCPConnection* Connection);

	friend class FOpenCodeTCPConnection;
};

class FOpenCodeTCPConnection : public FRunnable
{
public:
	FOpenCodeTCPConnection(FSocket* InSocket, FOpenCodeMessageDelegate InMessageHandler, FOpenCodeTCPServer* InServer);
	virtual ~FOpenCodeTCPConnection();

	virtual uint32 Run() override;
	virtual void Exit() override;

	void Send(const FString& JsonMessage);

private:
	FSocket* Socket;
	FOpenCodeMessageDelegate MessageHandler;
	FOpenCodeTCPServer* Server;
	FRunnableThread* Thread;
	FThreadSafeBool bIsRunning;
	FThreadSafeBool bStopRequested;

	TArray<uint8> ReadBuffer;
};
