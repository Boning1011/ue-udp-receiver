// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Common/UdpSocketReceiver.h"
#include "UDPReceiverComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPointCloudReceived, uint32, FrameId, const TArray<FVector4>&, Points);

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
	float ChunkTimeoutSeconds = 2.0f;

	UPROPERTY(BlueprintAssignable, Category = "UDP Receiver")
	FOnPointCloudReceived OnPointCloudReceived;

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
		TArray<FVector4> Points;
		TSet<uint16> ReceivedIndices;
	};

	TMap<uint32, FFrameBuffer> PendingFrames;

	/** Protects PendingFrames (callback runs on receiver thread). */
	FCriticalSection FrameLock;

	void OnDataReceivedCallback(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
	void FlushFrame(uint32 FrameId, FFrameBuffer& Buffer);
	void PurgeStaleFrames();
};
