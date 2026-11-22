// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "RefmuseCanvasAssetCommands.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RefmuseCanvasAssetImporter.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"

namespace RefmuseCanvas
{
namespace
{
constexpr TCHAR RequestSuffix[] = TEXT(".request.json");

bool IsPlainRequest(const FString& Path)
{
    const uint32 Attributes = GetFileAttributesW(*Path);
    return Attributes != INVALID_FILE_ATTRIBUTES
        && !(Attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
}

bool FAssetCommandInbox::Open()
{
    Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
        / TEXT("RefmuseCanvas/AssetCommands") / FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
    return IsLocalProjectDirectory(Directory) && IFileManager::Get().MakeDirectory(*Directory, true);
}

bool FAssetCommandInbox::Read(FAssetCommand& Command)
{
    if (!IsLocalProjectDirectory(Directory)) return false;

    auto& Files = IFileManager::Get();
    TArray<FString> Requests;
    Files.FindFiles(Requests, *(Directory / (FString(TEXT("*")) + RequestSuffix)), true, false);
    Requests.Sort();
    for (const FString& Name : Requests)
    {
        const FString Stem = Name.LeftChop(UE_ARRAY_COUNT(RequestSuffix) - 1);
        FGuid Id;
        if (!FGuid::ParseExact(Stem, EGuidFormats::Digits, Id) || !Id.IsValid()
            || !Stem.Equals(Id.ToString(EGuidFormats::Digits).ToLower(), ESearchCase::CaseSensitive))
        {
            continue;
        }

        const FString Request = Directory / Name;
        const FString Claimed = Directory / (Stem + TEXT(".claimed"));
        if (!IsPlainRequest(Request) || !Files.Move(*Claimed, *Request, false, false, false, true)) continue;
        if (Files.FileExists(*(Directory / (Stem + TEXT(".result.json")))))
        {
            Files.Delete(*Claimed, false, false, true);
            continue;
        }
        // Open the claimed file once and bound the reader itself, including if
        // the caller held a write handle when publishing the file.
        FString Contents;
        TSharedPtr<FJsonObject> Object;
        if (IsPlainRequest(Claimed))
        {
            TUniquePtr<FArchive> Reader(Files.CreateFileReader(*Claimed, FILEREAD_Silent));
            if (Reader && Reader->TotalSize() > 0 && Reader->TotalSize() <= 4096
                && FFileHelper::LoadFileToString(Contents, *Reader))
            {
                FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Contents), Object);
            }
        }
        Files.Delete(*Claimed, false, false, true);

        Command = FAssetCommand();
        Command.Id = Id;
        if (Object && Object->TryGetStringField(TEXT("action"), Command.Action)
            && Object->TryGetStringField(TEXT("projectId"), Command.ProjectId))
        {
            Object->TryGetStringField(TEXT("nodeId"), Command.NodeId);
            Object->TryGetStringField(TEXT("destination"), Command.Destination);
            FString CopyId;
            if (Object->TryGetStringField(TEXT("copyId"), CopyId))
            {
                FGuid::ParseExact(CopyId, EGuidFormats::Digits, Command.CopyId);
            }
        }
        return true;
    }
    return false;
}

void FAssetCommandInbox::Reply(const FAssetCommand& Command, bool bOk, const FString& Message,
    const TOptional<FCanvasAssetSource>& Source)
{
    if (!IsLocalProjectDirectory(Directory)) return;

    auto Object = MakeShared<FJsonObject>();
    const FString Id = Command.Id.ToString(EGuidFormats::Digits).ToLower();
    Object->SetStringField(TEXT("requestId"), Id);
    Object->SetBoolField(TEXT("ok"), bOk);
    Object->SetStringField(TEXT("message"), Message);
    if (Source.IsSet())
    {
        Object->SetStringField(TEXT("projectId"), Source->ProjectId);
        Object->SetStringField(TEXT("nodeId"), Source->NodeId);
        Object->SetStringField(TEXT("sha256"), Source->Digest);
        Object->SetStringField(TEXT("relativePath"), TEXT("assets/") + Source->Digest + TEXT(".") + Source->Extension);
        if (Command.Action == TEXT("copy")) Object->SetStringField(TEXT("copyId"), Id);
    }

    FString Json;
    if (!FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json))) return;
    const FString Temp = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp"));
    const FString Result = Directory / (Id + TEXT(".result.json"));
    auto& Files = IFileManager::Get();
    if (FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &Files, FILEWRITE_NoReplaceExisting))
    {
        Files.Move(*Result, *Temp, false, false, false, true);
    }
    Files.Delete(*Temp, false, false, true);
}
}
