#include "OpenCodeTCPServer.h"
#include "Common/TcpSocketBuilder.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/RunnableThread.h"

FOpenCodeTCPServer::FOpenCodeTCPServer(int32 Port, FOpenCodeMessageDelegate InMessageHandler)
	: ListenPort(Port)
	, MessageHandler(InMessageHandler)
	, ListenerSocket(nullptr)
	, Thread(nullptr)
	, bIsRunning(false)
	, bStopRequested(false)
{
}

FOpenCodeTCPServer::~FOpenCodeTCPServer()
{
	Stop();
}

bool FOpenCodeTCPServer::Start()
{
	if (bIsRunning) return true;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem) return false;

	ListenerSocket = FTcpSocketBuilder(TEXT("OpenCodeListener"))
		.AsReusable()
		.BoundToAddress(FIPv4Address::Any)
		.BoundToPort(ListenPort)
		.Listening(8)
		.Build();

	if (!ListenerSocket)
	{
		UE_LOG(LogTemp, Error, TEXT("OpenCodeTCPServer: Failed to create listener socket on port %d"), ListenPort);
		return false;
	}

	bStopRequested = false;
	bIsRunning = true;

	Thread = FRunnableThread::Create(this, TEXT("OpenCodeTCPServerThread"), 0, TPri_Normal);
	if (!Thread)
	{
		UE_LOG(LogTemp, Error, TEXT("OpenCodeTCPServer: Failed to create thread"));
		bIsRunning = false;
		return false;
	}

	return true;
}

void FOpenCodeTCPServer::Stop()
{
	bStopRequested = true;

	if (ListenerSocket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	{
		FScopeLock Lock(&ConnectionsLock);
		for (FOpenCodeTCPConnection* Conn : ActiveConnections)
		{
			delete Conn;
		}
		ActiveConnections.Empty();
	}

	bIsRunning = false;
}

void FOpenCodeTCPServer::SendMessage(const FString& JsonMessage)
{
	FScopeLock Lock(&ConnectionsLock);
	for (int32 i = ActiveConnections.Num() - 1; i >= 0; --i)
	{
		ActiveConnections[i]->Send(JsonMessage);
	}
}

uint32 FOpenCodeTCPServer::Run()
{
	while (!bStopRequested)
	{
		if (!ListenerSocket) break;

		FSocket* ClientSocket = ListenerSocket->Accept(TEXT("OpenCodeClient"));

		if (ClientSocket)
		{
			UE_LOG(LogTemp, Log, TEXT("OpenCodeTCPServer: Client connected"));
			AcceptConnection(ClientSocket);
		}

		FPlatformProcess::Sleep(0.01f);
	}
	return 0;
}

void FOpenCodeTCPServer::Exit()
{
	Stop();
}

void FOpenCodeTCPServer::AcceptConnection(FSocket* ClientSocket)
{
	FOpenCodeTCPConnection* Connection = new FOpenCodeTCPConnection(ClientSocket, MessageHandler, this);
	FScopeLock Lock(&ConnectionsLock);
	ActiveConnections.Add(Connection);
}

void FOpenCodeTCPServer::RemoveConnection(FOpenCodeTCPConnection* Connection)
{
	FScopeLock Lock(&ConnectionsLock);
	ActiveConnections.Remove(Connection);
}

// ----- FOpenCodeTCPConnection -----

FOpenCodeTCPConnection::FOpenCodeTCPConnection(FSocket* InSocket, FOpenCodeMessageDelegate InMessageHandler, FOpenCodeTCPServer* InServer)
	: Socket(InSocket)
	, MessageHandler(InMessageHandler)
	, Server(InServer)
	, Thread(nullptr)
	, bIsRunning(false)
	, bStopRequested(false)
{
	ReadBuffer.SetNum(8192);
	bIsRunning = true;
	Thread = FRunnableThread::Create(this, TEXT("OpenCodeConnectionThread"), 0, TPri_Normal);
}

FOpenCodeTCPConnection::~FOpenCodeTCPConnection()
{
	bStopRequested = true;

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

uint32 FOpenCodeTCPConnection::Run()
{
	FString AccumulatedData;
	uint8 TempBuf[4096];

	while (!bStopRequested && Socket)
	{
		int32 BytesRead = 0;
		if (Socket->Recv(TempBuf, sizeof(TempBuf), BytesRead) && BytesRead > 0)
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(TempBuf), BytesRead);
			AccumulatedData.AppendChars(Converter.Get(), Converter.Length());

			int32 NewlineIdx;
			while ((NewlineIdx = AccumulatedData.Find(TEXT("\n"))) != INDEX_NONE)
			{
				FString Message = AccumulatedData.Left(NewlineIdx).TrimEnd();
				AccumulatedData.RightChopInline(NewlineIdx + 1);

				if (!Message.IsEmpty())
				{
					FString Response = MessageHandler.Execute(Message);
					if (!Response.IsEmpty())
					{
						Send(Response);
					}
				}
			}
		}
		else
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}

	if (Server)
	{
		Server->RemoveConnection(this);
	}

	return 0;
}

void FOpenCodeTCPConnection::Exit()
{
	bStopRequested = true;
	if (Socket)
	{
		Socket->Close();
	}
}

void FOpenCodeTCPConnection::Send(const FString& JsonMessage)
{
	if (!Socket || !bIsRunning) return;

	FString MessageWithNewline = JsonMessage + TEXT("\n");
	FTCHARToUTF8 Converter(*MessageWithNewline);
	const uint8* Data = reinterpret_cast<const uint8*>(Converter.Get());
	int32 TotalBytes = Converter.Length();
	int32 BytesSent = 0;

	while (BytesSent < TotalBytes)
	{
		int32 Sent = 0;
		if (!Socket->Send(Data + BytesSent, TotalBytes - BytesSent, Sent))
		{
			break;
		}
		BytesSent += Sent;
	}
}
