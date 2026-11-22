// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "RefmuseCanvasPcmWav.h"
#include "UObject/Object.h"
#include "RefmuseCanvasAssetReceiver.generated.h"

namespace RefmuseCanvas
{
struct FCanvasAssetSource
{
    FString ProjectId;
    FString NodeId;
    FString Digest;
    FString Extension = TEXT("png");

    bool IsWav() const { return Extension.Equals(TEXT("wav"), ESearchCase::CaseSensitive); }
};

struct FPreparedCanvasAsset : FCanvasAssetSource
{
    TArray<uint8> Bytes;
    int32 Width = 0;
    int32 Height = 0;
    FPcmWav Wave;
};
}

// This browser binding can only supply data to an active native request.
UCLASS()
class URefmuseCanvasAssetReceiver final : public UObject
{
    GENERATED_BODY()

public:
    void Begin(const FString& ProjectId, const FString& RequestId);
    void BeginCopy(const FString& RequestId, const FString& ProjectId = {}, const FString& NodeId = {});
    void BeginPaste(const RefmuseCanvas::FCanvasAssetSource& Source, const FString& RequestId);
    void Reset();
    bool IsReceiving() const;
    const FString& GetStatus() const;
    const RefmuseCanvas::FCanvasAssetSource* GetCopied() const;
    const RefmuseCanvas::FPreparedCanvasAsset* GetPrepared() const;
    RefmuseCanvas::FPreparedCanvasAsset TakePrepared();

    UFUNCTION() bool SubmitChunk(const FString& RequestId, int32 Offset, const FString& Base64);
    UFUNCTION() void CompleteCopy(const FString& RequestId, const FString& ProjectId, const FString& NodeId,
        const FString& Digest, const FString& Extension);
    UFUNCTION() void Complete(const FString& RequestId, const FString& NodeId, const FString& Digest);
    UFUNCTION() void Fail(const FString& RequestId, const FString& Code);

private:
    enum class ERequest { LegacyPng, Copy, Paste };

    void Start(ERequest Kind, const FString& RequestId);
    bool Accepts(const FString& RequestId) const;
    void Reject(const TCHAR* Message);

    RefmuseCanvas::FPreparedCanvasAsset Payload;
    ERequest RequestKind = ERequest::LegacyPng;
    FString ActiveRequest;
    FString Status;
    double Deadline = 0.0;
    double CopyExpiresAt = 0.0;
    bool bPrepared = false;
};
