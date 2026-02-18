// Fill out your copyright notice in the Description page of Project Settings.

#include "UDPReceiverComponent.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDPReceiver, Log, All);

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
		.WithReceiveBufferSize(2 * 1024 * 1024)
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

	UE_LOG(LogUDPReceiver, Log, TEXT("UDP listener stopped."));
}

bool UUDPReceiverComponent::IsListening() const
{
	return UdpSocket != nullptr;
}

void UUDPReceiverComponent::OnDataReceivedCallback(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint)
{
	// Convert raw bytes to FString (expected UTF-8 JSON)
	TArray<uint8> NullTerminated;
	NullTerminated.Append(Data->GetData(), Data->Num());
	NullTerminated.Add(0);

	const FString JsonString = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(NullTerminated.GetData())));

	// Parse JSON
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUDPReceiver, Warning, TEXT("Failed to parse JSON from %s."), *Endpoint.ToString());
		return;
	}

	// Extract fields
	FEmbeddingData ParsedData;
	Root->TryGetStringField(TEXT("type"), ParsedData.Type);

	double CountAsDouble = 0.0;
	Root->TryGetNumberField(TEXT("count"), CountAsDouble);
	ParsedData.Count = static_cast<int32>(CountAsDouble);

	const TArray<TSharedPtr<FJsonValue>>* PointsArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("points"), PointsArray) || PointsArray == nullptr)
	{
		UE_LOG(LogUDPReceiver, Warning, TEXT("No 'points' array in packet from %s."), *Endpoint.ToString());
		return;
	}

	ParsedData.Points.Reserve(PointsArray->Num());
	for (const TSharedPtr<FJsonValue>& Entry : *PointsArray)
	{
		const TArray<TSharedPtr<FJsonValue>>* Comps = nullptr;
		if (!Entry->TryGetArray(Comps) || Comps == nullptr || Comps->Num() < 4)
		{
			continue;
		}
		FVector4 P;
		P.X = (*Comps)[0]->AsNumber();
		P.Y = (*Comps)[1]->AsNumber();
		P.Z = (*Comps)[2]->AsNumber();
		P.W = (*Comps)[3]->AsNumber();
		ParsedData.Points.Add(P);
	}

	// Log a preview of the received packet for quick debugging
	{
		FString Preview;
		const int32 PreviewCount = FMath::Min(ParsedData.Points.Num(), 3);
		for (int32 i = 0; i < PreviewCount; ++i)
		{
			const FVector4& P = ParsedData.Points[i];
			Preview += FString::Printf(TEXT("[%.2f, %.2f, %.2f, %.2f] "), P.X, P.Y, P.Z, P.W);
		}
		UE_LOG(LogUDPReceiver, Log,
			TEXT("Received from %s | type=%s | count=%d | first %d point(s): %s"),
			*Endpoint.ToString(), *ParsedData.Type, ParsedData.Count,
			PreviewCount, Preview.IsEmpty() ? TEXT("(none)") : *Preview);
	}

	// Dispatch to game thread — Blueprint delegates must not be called from background threads
	TWeakObjectPtr<UUDPReceiverComponent> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedData = MoveTemp(ParsedData)]() mutable
	{
		if (UUDPReceiverComponent* Comp = WeakThis.Get())
		{
			Comp->OnEmbeddingReceived.Broadcast(CapturedData);

			TArray<FVector> XYZPoints;
			XYZPoints.Reserve(CapturedData.Points.Num());
			for (const FVector4& P : CapturedData.Points)
			{
				XYZPoints.Add(FVector(P.X, P.Y, P.Z));
			}
			Comp->OnPointsReceived.Broadcast(CapturedData.Type, XYZPoints);
		}
	});
}
