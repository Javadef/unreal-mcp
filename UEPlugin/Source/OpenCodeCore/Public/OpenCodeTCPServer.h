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
	void ReapFinishedConnections();

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

	/// True once Run() has returned — the server's accept loop reaps (deletes)
	/// finished connections. Previously a disconnecting connection removed
	/// itself from the server's list without anyone deleting it, leaking the
	/// thread object + socket on every client disconnect.
	bool IsFinished() const { return bFinished; }

private:
	FSocket* Socket;
	FOpenCodeMessageDelegate MessageHandler;
	FOpenCodeTCPServer* Server;
	FRunnableThread* Thread;
	FThreadSafeBool bIsRunning;
	FThreadSafeBool bStopRequested;
	FThreadSafeBool bFinished;

	TArray<uint8> ReadBuffer;
};
