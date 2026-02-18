// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Common/UdpSocketReceiver.h"
#include "UDPReceiverComponent.generated.h"

USTRUCT(BlueprintType)
struct FEmbeddingData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UDP Embedding")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "UDP Embedding")
	int32 Count = 0;

	// X, Y, Z = position; W = intensity
	UPROPERTY(BlueprintReadOnly, Category = "UDP Embedding")
	TArray<FVector4> Points;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEmbeddingReceived, const FEmbeddingData&, EmbeddingData);

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

	UPROPERTY(BlueprintAssignable, Category = "UDP Receiver")
	FOnEmbeddingReceived OnEmbeddingReceived;

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	void StartListening();

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	void StopListening();

	UFUNCTION(BlueprintCallable, Category = "UDP Receiver")
	bool IsListening() const;

private:
	FSocket* UdpSocket = nullptr;
	FUdpSocketReceiver* UdpReceiver = nullptr;

	void OnDataReceivedCallback(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
};
