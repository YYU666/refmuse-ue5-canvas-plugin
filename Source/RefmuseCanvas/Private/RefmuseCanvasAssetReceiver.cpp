// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "RefmuseCanvasAssetReceiver.h"

#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"
#include "RefmuseCanvasAssetImporter.h"

void URefmuseCanvasAssetReceiver::Begin(const FString& ProjectId, const FString& RequestId)
{
    Start(ERequest::LegacyPng, RequestId);
    Payload.ProjectId = ProjectId;
    Status = TEXT("Reading the selected saved PNG from the local canvas...");
}

void URefmuseCanvasAssetReceiver::BeginCopy(const FString& RequestId, const FString& ProjectId, const FString& NodeId)
{
    Start(ERequest::Copy, RequestId);
    Payload.ProjectId = ProjectId;
    Payload.NodeId = NodeId;
    Status = TEXT("Checking the selected canvas asset...");
}

void URefmuseCanvasAssetReceiver::BeginPaste(const RefmuseCanvas::FCanvasAssetSource& Source, const FString& RequestId)
{
    Start(ERequest::Paste, RequestId);
    static_cast<RefmuseCanvas::FCanvasAssetSource&>(Payload) = Source;
    Status = TEXT("Rechecking and reading the copied canvas asset...");
}

void URefmuseCanvasAssetReceiver::Start(ERequest Kind, const FString& RequestId)
{
    Reset();
    RequestKind = Kind;
    ActiveRequest = RequestId;
    Deadline = FPlatformTime::Seconds() + 30.0;
}

void URefmuseCanvasAssetReceiver::Reset()
{
    Payload = RefmuseCanvas::FPreparedCanvasAsset();
    ActiveRequest.Reset();
    Status.Reset();
    Deadline = 0.0;
    CopyExpiresAt = 0.0;
    bPrepared = false;
}

bool URefmuseCanvasAssetReceiver::IsReceiving() const
{
    return !ActiveRequest.IsEmpty() && FPlatformTime::Seconds() < Deadline;
}

const FString& URefmuseCanvasAssetReceiver::GetStatus() const
{
    static const FString TimedOut(TEXT("The asset request timed out. Select the node and try again."));
    static const FString Expired(TEXT("The copied asset has expired. Copy the node again."));
    if (CopyExpiresAt > 0.0 && !GetCopied()) return Expired;
    return !ActiveRequest.IsEmpty() && !IsReceiving() ? TimedOut : Status;
}

const RefmuseCanvas::FCanvasAssetSource* URefmuseCanvasAssetReceiver::GetCopied() const
{
    return CopyExpiresAt > FPlatformTime::Seconds() ? &Payload : nullptr;
}

const RefmuseCanvas::FPreparedCanvasAsset* URefmuseCanvasAssetReceiver::GetPrepared() const
{
    return bPrepared ? &Payload : nullptr;
}

RefmuseCanvas::FPreparedCanvasAsset URefmuseCanvasAssetReceiver::TakePrepared()
{
    RefmuseCanvas::FPreparedCanvasAsset Source = MoveTemp(Payload);
    Reset();
    return Source;
}

bool URefmuseCanvasAssetReceiver::Accepts(const FString& RequestId) const
{
    return IsInGameThread() && IsReceiving() && RequestId.Equals(ActiveRequest, ESearchCase::CaseSensitive);
}

void URefmuseCanvasAssetReceiver::Reject(const TCHAR* Message)
{
    Reset();
    Status = Message;
}

bool URefmuseCanvasAssetReceiver::SubmitChunk(const FString& RequestId, int32 Offset, const FString& Base64)
{
    if (!Accepts(RequestId) || RequestKind == ERequest::Copy) return false;
    constexpr int32 MaxEncodedChunk = 64 * 1024;
    TArray<uint8> Chunk;
    if (Offset != Payload.Bytes.Num() || Base64.IsEmpty() || Base64.Len() > MaxEncodedChunk
        || !FBase64::Decode(Base64, Chunk) || Chunk.IsEmpty() || Chunk.Num() > RefmuseCanvas::MaxCanvasAssetBytes - Offset)
    {
        Reject(TEXT("Invalid or oversized asset data was rejected."));
        return false;
    }
    Payload.Bytes.Append(Chunk);
    return true;
}

void URefmuseCanvasAssetReceiver::CompleteCopy(const FString& RequestId, const FString& ProjectId, const FString& NodeId,
    const FString& Digest, const FString& Extension)
{
    if (!Accepts(RequestId) || RequestKind != ERequest::Copy) return;
    const RefmuseCanvas::FCanvasAssetSource Source{ProjectId, NodeId, Digest, Extension};
    if (!RefmuseCanvas::IsCanvasAssetSource(Source)
        || (!Payload.ProjectId.IsEmpty() && !Payload.ProjectId.Equals(ProjectId, ESearchCase::CaseSensitive))
        || (!Payload.NodeId.IsEmpty() && !Payload.NodeId.Equals(NodeId, ESearchCase::CaseSensitive)))
    {
        Reject(TEXT("The canvas asset identity could not be verified."));
        return;
    }
    static_cast<RefmuseCanvas::FCanvasAssetSource&>(Payload) = Source;
    ActiveRequest.Reset();
    CopyExpiresAt = FPlatformTime::Seconds() + 120.0;
    Status = TEXT("Asset copied. In the destination Content Browser folder, right-click empty space and choose Paste Canvas Asset.");
}

void URefmuseCanvasAssetReceiver::Complete(const FString& RequestId, const FString& NodeId, const FString& Digest)
{
    if (!Accepts(RequestId) || RequestKind == ERequest::Copy) return;
    if (RequestKind == ERequest::Paste && (!Payload.NodeId.Equals(NodeId, ESearchCase::CaseSensitive)
        || !Payload.Digest.Equals(Digest, ESearchCase::CaseSensitive)))
    {
        Reject(TEXT("The fetched file does not match the copied asset."));
        return;
    }
    Payload.NodeId = NodeId;
    Payload.Digest = Digest;
    if (!RefmuseCanvas::InspectCanvasAsset(Payload))
    {
        Reject(Payload.IsWav()
            ? TEXT("The WAV must be standard 16-bit PCM, mono or stereo, 8-192 kHz, with a matching SHA-256.")
            : Payload.IsGlb() ? TEXT("The GLB, embedded textures, project identity or SHA-256 could not be verified.")
            : TEXT("Image format, dimensions, project identity or SHA-256 could not be verified."));
        return;
    }
    ActiveRequest.Reset();
    bPrepared = true;
    const FString DigestPrefix = Payload.Digest.Left(16);
    if (Payload.IsGlb())
    {
        Status = FString::Printf(TEXT("Ready: verified self-contained GLB | SHA-256 %s"), *DigestPrefix);
    }
    else if (Payload.IsWav())
    {
        Status = FString::Printf(TEXT("Ready: %s / %s  |  WAV %u Hz, %u channel(s)  |  SHA-256 %s"),
            *Payload.ProjectId, *Payload.NodeId, Payload.Wave.SampleRate,
            static_cast<uint32>(Payload.Wave.Channels), *DigestPrefix);
    }
    else
    {
        Status = FString::Printf(TEXT("Ready: %s / %s  |  %d x %d  |  SHA-256 %s"),
            *Payload.ProjectId, *Payload.NodeId, Payload.Width, Payload.Height, *DigestPrefix);
    }
}

void URefmuseCanvasAssetReceiver::Fail(const FString& RequestId, const FString& Code)
{
    if (!Accepts(RequestId)) return;
    if (Code == TEXT("selection")) Reject(TEXT("Select exactly one image, audio, or model node in the visible canvas."));
    else if (Code == TEXT("saved-png")) Reject(TEXT("The selected node must refer to a saved PNG in the specified canvas project."));
    else if (Code == TEXT("saved-asset")) Reject(TEXT("Save one PNG, JPEG, PCM WAV, or GLB in the current project before copying it."));
    else if (Code == TEXT("project")) Reject(TEXT("The current canvas project differs from the copied asset. Copy the node again."));
    else if (Code == TEXT("changed")) Reject(TEXT("The saved node or file changed or is no longer available. Copy the node again."));
    else if (Code == TEXT("size")) Reject(TEXT("The canvas project or asset exceeded the 16 MiB read limit."));
    else if (Code == TEXT("png")) Reject(TEXT("The selected file has an invalid image signature."));
    else if (Code == TEXT("glb")) Reject(TEXT("The selected file is not a GLB 2.0 file."));
    else if (Code == TEXT("wav")) Reject(TEXT("The selected file is not a valid RIFF/WAVE file."));
    else Reject(TEXT("Could not read the local canvas project or asset. Verify the connection and project ID."));
}
