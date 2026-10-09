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

	// Loopback only. Every tool this exposes (console commands, file writes)
	// is code execution — binding to Any made that reachable from the LAN.
	ListenerSocket = FTcpSocketBuilder(TEXT("OpenCodeListener"))
		.AsReusable()
		.BoundToAddress(FIPv4Address::InternalLoopback)
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

	// Close first (unblocks a thread waiting in WaitForPendingConnection),
	// join the thread, THEN destroy. Destroying while the accept thread is
	// still blocked inside the socket is a use-after-free race.
	if (ListenerSocket)
	{
		ListenerSocket->Close();
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	if (ListenerSocket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
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

		// Block (up to 100ms) instead of polling Accept at 100Hz. The timeout
		// doubles as the cadence for bStopRequested checks and reaping.
		bool bHasPending = false;
		if (ListenerSocket->WaitForPendingConnection(bHasPending, FTimespan::FromMilliseconds(100)) && bHasPending)
		{
			FSocket* ClientSocket = ListenerSocket->Accept(TEXT("OpenCodeClient"));
			if (ClientSocket)
			{
				UE_LOG(LogTemp, Log, TEXT("OpenCodeTCPServer: Client connected"));
				AcceptConnection(ClientSocket);
			}
		}

		ReapFinishedConnections();
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

void FOpenCodeTCPServer::ReapFinishedConnections()
{
	// Collect under the lock, delete outside it: ~FOpenCodeTCPConnection joins
	// its thread, and holding ConnectionsLock across that would stall Accept
	// and SendMessage for the duration of the join.
	TArray<FOpenCodeTCPConnection*> Finished;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (int32 i = ActiveConnections.Num() - 1; i >= 0; --i)
		{
			if (ActiveConnections[i]->IsFinished())
			{
				Finished.Add(ActiveConnections[i]);
				ActiveConnections.RemoveAt(i);
			}
		}
	}
	for (FOpenCodeTCPConnection* Conn : Finished)
	{
		delete Conn;
	}
}

// ----- FOpenCodeTCPConnection -----

FOpenCodeTCPConnection::FOpenCodeTCPConnection(FSocket* InSocket, FOpenCodeMessageDelegate InMessageHandler, FOpenCodeTCPServer* InServer)
	: Socket(InSocket)
	, MessageHandler(InMessageHandler)
	, Server(InServer)
	, Thread(nullptr)
	, bIsRunning(false)
	, bStopRequested(false)
	, bFinished(false)
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

	// Cap on buffered request data awaiting a newline. A peer streaming bytes
	// with no delimiter would otherwise grow AccumulatedData without bound.
	constexpr int32 MaxAccumulatedChars = 4 * 1024 * 1024;

	while (!bStopRequested && Socket)
	{
		// Wait until the socket is readable: that means data, or a close.
		//
		// A bare Recv cannot tell the two apart on this (non-blocking) socket.
		// FSocketBSD::Recv reports "nothing has arrived yet" as success with
		// zero bytes, and this loop used to read that as the peer hanging up —
		// so any connection whose request had not landed by the first Recv was
		// dropped (the client saw an aborted connection), and no connection
		// ever outlived one request. The timeout is the bStopRequested cadence.
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(100)))
		{
			continue;
		}

		int32 BytesRead = 0;
		if (Socket->Recv(TempBuf, sizeof(TempBuf), BytesRead))
		{
			if (BytesRead == 0)
			{
				// Readable but would block after all: nothing to do yet.
				continue;
			}

			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(TempBuf), BytesRead);
			AccumulatedData.AppendChars(Converter.Get(), Converter.Length());

			if (AccumulatedData.Len() > MaxAccumulatedChars)
			{
				UE_LOG(LogTemp, Warning, TEXT("OpenCodeTCPServer: Dropping connection — %d chars buffered with no message delimiter"), AccumulatedData.Len());
				break;
			}

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
			// Readable, and Recv failed: for a stream socket that is the peer
			// closing (recv returned 0) or the connection breaking.
			UE_LOG(LogTemp, Log, TEXT("OpenCodeTCPServer: Client disconnected"));
			break;
		}
	}

	// Flag for the server's reap pass — do NOT self-remove from the list here;
	// that left the object orphaned with no owner to delete it.
	bIsRunning = false;
	bFinished = true;

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
