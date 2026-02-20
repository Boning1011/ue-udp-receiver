// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Common/UdpSocketReceiver.h"
#include "UDPReceiverComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPointCloudReceived, int32, FrameId, const TArray<FVector4>&, Points);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPointCloudPositionsReceived, int32, FrameId, const TArray<FVector>&, Positions);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PCG_LEARN_API UUDPReceiverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUDPReceiverComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UDP Receiver")
	int32 ListenPort = 7000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UDP Receiver")
	bool bEnableDebugLog = false;

	/** Max seconds to wait for all chunks of a frame before discarding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UDP Receiver")
	float ChunkTimeoutSeconds = 0.3f;

	/** Full data: FVector4(X, Y, Z, Intensity) per point. */
	UPROPERTY(BlueprintAssignable, Category = "UDP Receiver")
	FOnPointCloudReceived OnPointCloudReceived;

	/** Positions only: FVector(X, Y, Z) — connects directly to Niagara. */
	UPROPERTY(BlueprintAssignable, Category = "UDP Receiver")
	FOnPointCloudPositionsReceived OnPointCloudPositionsReceived;

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	void StartListening();

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	void StopListening();

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	bool IsListening() const;

private:
	FSocket* UdpSocket = nullptr;
	FUdpSocketReceiver* UdpReceiver = nullptr;

	/** Per-frame reassembly buffer. */
	struct FFrameBuffer
	{
		uint32 TotalPoints = 0;
		uint16 TotalChunks = 0;
		uint16 ReceivedChunks = 0;
		double FirstChunkTime = 0.0;
		TMap<uint16, TArray<FVector4>> ChunkData;  // keyed by chunk_index for ordered reassembly
	};

	TMap<uint32, FFrameBuffer> PendingFrames;

	/** Protects PendingFrames (callback runs on receiver thread). */
	FCriticalSection FrameLock;

	/** Last frame_id delivered to game thread — skip older frames. */
	uint32 LastDeliveredFrameId = 0;
	bool bHasDeliveredAny = false;

	void OnDataReceivedCallback(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
	void FlushFrame(uint32 FrameId, FFrameBuffer& Buffer);
	void PurgeStaleFrames();
};
