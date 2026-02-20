// Fill out your copyright notice in the Description page of Project Settings.

#include "UDPReceiverComponent.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDPReceiver, Log, All);

// ── Binary protocol constants ───────────────────────────────────────────────
static constexpr int32 HEADER_SIZE      = 16;
static constexpr int32 BYTES_PER_POINT  = 16;  // 4 × float32

// Header offsets (little-endian)
//  0: uint8  msg_type
//  1: uint8  flags
//  2: uint32 frame_id
//  6: uint16 chunk_index
//  8: uint16 total_chunks
// 10: uint16 points_in_chunk
// 12: uint32 total_points
// ─────────────────────────────────────────────────────────────────────────────

UUDPReceiverComponent::UUDPReceiverComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UUDPReceiverComponent::BeginPlay()
{
	Super::BeginPlay();
	StartListening();
}

void UUDPReceiverComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopListening();
	Super::EndPlay(EndPlayReason);
}

void UUDPReceiverComponent::StartListening()
{
	if (UdpSocket != nullptr)
	{
		UE_LOG(LogUDPReceiver, Warning, TEXT("Already listening on port %d."), ListenPort);
		return;
	}

	UdpSocket = FUdpSocketBuilder(TEXT("UDPReceiverComponent"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToPort(ListenPort)
		.WithReceiveBufferSize(16 * 1024 * 1024)
		.Build();

	if (UdpSocket == nullptr)
	{
		UE_LOG(LogUDPReceiver, Error, TEXT("Failed to create UDP socket on port %d."), ListenPort);
		return;
	}

	UdpReceiver = new FUdpSocketReceiver(UdpSocket, FTimespan::FromMilliseconds(100), TEXT("UDPReceiverThread"));
	UdpReceiver->OnDataReceived().BindUObject(this, &UUDPReceiverComponent::OnDataReceivedCallback);
	UdpReceiver->Start();

	UE_LOG(LogUDPReceiver, Log, TEXT("UDP listener started on port %d."), ListenPort);
}

void UUDPReceiverComponent::StopListening()
{
	if (UdpReceiver != nullptr)
	{
		UdpReceiver->Stop();
		delete UdpReceiver;
		UdpReceiver = nullptr;
	}

	if (UdpSocket != nullptr)
	{
		UdpSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(UdpSocket);
		UdpSocket = nullptr;
	}

	FScopeLock Lock(&FrameLock);
	PendingFrames.Empty();
	LastDeliveredFrameId = 0;
	bHasDeliveredAny = false;

	UE_LOG(LogUDPReceiver, Log, TEXT("UDP listener stopped."));
}

bool UUDPReceiverComponent::IsListening() const
{
	return UdpSocket != nullptr;
}

void UUDPReceiverComponent::OnDataReceivedCallback(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint)
{
	const int32 DataSize = Data->Num();

	// Minimum: header only (no points is acceptable for total_chunks bookkeeping)
	if (DataSize < HEADER_SIZE)
	{
		UE_LOG(LogUDPReceiver, Warning, TEXT("Packet too small (%d bytes) from %s, ignoring."), DataSize, *Endpoint.ToString());
		return;
	}

	const uint8* Raw = Data->GetData();

	// ── Parse header (little-endian, matching Python struct "<2BI3HI") ──────
	const uint8  MsgType        = Raw[0];
	// const uint8  Flags        = Raw[1];  // reserved
	uint32 FrameId;
	FMemory::Memcpy(&FrameId, Raw + 2, 4);

	uint16 ChunkIndex;
	FMemory::Memcpy(&ChunkIndex, Raw + 6, 2);

	uint16 TotalChunks;
	FMemory::Memcpy(&TotalChunks, Raw + 8, 2);

	uint16 PointsInChunk;
	FMemory::Memcpy(&PointsInChunk, Raw + 10, 2);

	uint32 TotalPoints;
	FMemory::Memcpy(&TotalPoints, Raw + 12, 4);

	// Only handle point-cloud messages
	if (MsgType != 0)
	{
		UE_LOG(LogUDPReceiver, Warning, TEXT("Unknown msg_type %d from %s, ignoring."), MsgType, *Endpoint.ToString());
		return;
	}

	// Validate payload size
	const int32 ExpectedPayload = static_cast<int32>(PointsInChunk) * BYTES_PER_POINT;
	if (DataSize - HEADER_SIZE < ExpectedPayload)
	{
		UE_LOG(LogUDPReceiver, Warning,
			TEXT("Payload too small: expected %d bytes for %d points, got %d bytes. frame=%u chunk=%u/%u"),
			ExpectedPayload, PointsInChunk, DataSize - HEADER_SIZE, FrameId, ChunkIndex, TotalChunks);
		return;
	}

	// ── Parse point payload ─────────────────────────────────────────────────
	const float* FloatData = reinterpret_cast<const float*>(Raw + HEADER_SIZE);
	TArray<FVector4> ChunkPoints;
	ChunkPoints.Reserve(PointsInChunk);

	for (uint16 i = 0; i < PointsInChunk; ++i)
	{
		const int32 Base = i * 4;
		ChunkPoints.Add(FVector4(
			FloatData[Base + 0],  // x
			FloatData[Base + 1],  // y
			FloatData[Base + 2],  // z
			FloatData[Base + 3]   // intensity
		));
	}

	if (bEnableDebugLog)
	{
		UE_LOG(LogUDPReceiver, Log,
			TEXT("Chunk %u/%u  frame=%u  pts_in_chunk=%d  total_pts=%u  from %s"),
			ChunkIndex + 1, TotalChunks, FrameId, PointsInChunk, TotalPoints, *Endpoint.ToString());
	}

	// ── Reassemble frame ────────────────────────────────────────────────────
	FScopeLock Lock(&FrameLock);

	// Purge stale incomplete frames
	PurgeStaleFrames();

	FFrameBuffer& Frame = PendingFrames.FindOrAdd(FrameId);

	// Initialize on first chunk
	if (Frame.ReceivedChunks == 0)
	{
		Frame.TotalPoints  = TotalPoints;
		Frame.TotalChunks  = TotalChunks;
		Frame.FirstChunkTime = FPlatformTime::Seconds();
	}

	// Deduplicate (in case of retransmit)
	if (Frame.ChunkData.Contains(ChunkIndex))
	{
		return;
	}
	Frame.ChunkData.Add(ChunkIndex, MoveTemp(ChunkPoints));
	Frame.ReceivedChunks++;

	// All chunks received — flush
	if (Frame.ReceivedChunks >= Frame.TotalChunks)
	{
		FlushFrame(FrameId, Frame);
		PendingFrames.Remove(FrameId);
	}
}

void UUDPReceiverComponent::FlushFrame(uint32 FrameId, FFrameBuffer& Buffer)
{
	// Drop frames older than what we've already delivered
	if (bHasDeliveredAny && static_cast<int32>(FrameId - LastDeliveredFrameId) <= 0)
	{
		if (bEnableDebugLog)
		{
			UE_LOG(LogUDPReceiver, Log,
				TEXT("Dropping stale frame %u (last delivered: %u)"), FrameId, LastDeliveredFrameId);
		}
		return;
	}
	LastDeliveredFrameId = FrameId;
	bHasDeliveredAny = true;

	// Reassemble points in chunk_index order
	TArray<FVector4> OrderedPoints;
	OrderedPoints.Reserve(Buffer.TotalPoints);
	for (uint16 i = 0; i < Buffer.TotalChunks; ++i)
	{
		if (TArray<FVector4>* Chunk = Buffer.ChunkData.Find(i))
		{
			OrderedPoints.Append(MoveTemp(*Chunk));
		}
	}

	if (bEnableDebugLog)
	{
		FString Preview;
		const int32 PreviewCount = FMath::Min(OrderedPoints.Num(), 3);
		for (int32 i = 0; i < PreviewCount; ++i)
		{
			const FVector4& P = OrderedPoints[i];
			Preview += FString::Printf(TEXT("[%.2f, %.2f, %.2f, %.2f] "), P.X, P.Y, P.Z, P.W);
		}
		UE_LOG(LogUDPReceiver, Log,
			TEXT("Frame %u complete: %d points | first %d: %s"),
			FrameId, OrderedPoints.Num(), PreviewCount, *Preview);
	}

	// Build FVector positions array for Niagara-friendly delegate
	TArray<FVector> Positions;
	Positions.Reserve(OrderedPoints.Num());
	for (const FVector4& P : OrderedPoints)
	{
		Positions.Add(FVector(P.X, P.Y, P.Z));
	}

	// Dispatch to game thread
	TWeakObjectPtr<UUDPReceiverComponent> WeakThis(this);
	int32 CapturedFrameId = static_cast<int32>(FrameId);

	AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedFrameId,
		FullPoints = MoveTemp(OrderedPoints), Pos = MoveTemp(Positions)]()
	{
		if (UUDPReceiverComponent* Comp = WeakThis.Get())
		{
			Comp->OnPointCloudReceived.Broadcast(CapturedFrameId, FullPoints);
			Comp->OnPointCloudPositionsReceived.Broadcast(CapturedFrameId, Pos);
		}
	});
}

void UUDPReceiverComponent::PurgeStaleFrames()
{
	const double Now = FPlatformTime::Seconds();
	TArray<uint32> ToRemove;

	for (auto& Pair : PendingFrames)
	{
		if (Now - Pair.Value.FirstChunkTime > static_cast<double>(ChunkTimeoutSeconds))
		{
			UE_LOG(LogUDPReceiver, Warning,
				TEXT("Frame %u timed out (%d/%d chunks received). Discarding."),
				Pair.Key, Pair.Value.ReceivedChunks, Pair.Value.TotalChunks);
			ToRemove.Add(Pair.Key);
		}
	}

	for (uint32 Key : ToRemove)
	{
		PendingFrames.Remove(Key);
	}
}
