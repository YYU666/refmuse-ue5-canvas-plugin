// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "RefmuseCanvasAssetImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Audio.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Memory/SharedBuffer.h"
#include "Misc/NamePermissionList.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RefmuseCanvasAssetReceiver.h"
#include "Sound/SoundWave.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace RefmuseCanvas
{
namespace
{
constexpr int64 MaxImageSide = 4096;

bool IsIdentifier(const FString& Value, int32 Maximum)
{
    if (Value.IsEmpty() || Value.Len() > Maximum) return false;
    for (TCHAR C : Value)
    {
        if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-'))
        {
            return false;
        }
    }
    return true;
}

bool IsSha256(const FString& Digest)
{
    if (Digest.Len() != 64) return false;
    for (TCHAR C : Digest)
    {
        if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
    }
    return true;
}

bool HashMatches(const TArray<uint8>& Bytes, const FString& Digest)
{
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    uint8 Hash[32];
    const bool bHasHash = BCryptHash(Algorithm, nullptr, 0, const_cast<uint8*>(Bytes.GetData()),
        static_cast<uint32>(Bytes.Num()), Hash, UE_ARRAY_COUNT(Hash)) == 0;
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return bHasHash && BytesToHex(Hash, UE_ARRAY_COUNT(Hash)).Equals(Digest, ESearchCase::IgnoreCase);
}

bool IsUnderProjectWithoutReparsePoints(FString Directory)
{
    FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FPaths::NormalizeDirectoryName(Root);
    FPaths::NormalizeDirectoryName(Directory);
    if (!FPaths::IsUnderDirectory(Directory, Root)) return false;

    // The package mapping is lexical. Reject any existing junction or symlink
    // between the project and its save/commit directories before writing.
    while (FPaths::IsUnderDirectory(Directory, Root))
    {
        const uint32 Attributes = GetFileAttributesW(*Directory);
        if (Attributes == INVALID_FILE_ATTRIBUTES)
        {
            const uint32 Error = GetLastError();
            if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND) return false;
        }
        else if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY) || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            return false;
        }
        if (Directory.Equals(Root, ESearchCase::IgnoreCase)) return true;
        Directory = FPaths::GetPath(Directory);
    }
    return false;
}

bool CanImportSoundWave(float Duration)
{
    IConsoleManager& Console = IConsoleManager::Get();
    const IConsoleVariable* Enabled = Console.FindConsoleVariable(TEXT("au.EnableUserSoundwaveImport"));
    const IConsoleVariable* Limit = Console.FindConsoleVariable(TEXT("au.SoundWaveImportLengthLimitInSeconds"));
    if ((Enabled && !Enabled->GetBool()) || (Limit && Limit->GetFloat() > 0.f && Duration > Limit->GetFloat()))
    {
        return false;
    }
    const IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    return Tools.GetAssetClassPathPermissionList(EAssetClassAction::ImportAsset)->PassesFilter(USoundWave::StaticClass()->GetPathName());
}
}

bool IsCanvasProjectId(const FString& ProjectId)
{
    // UE package names are case-insensitive. Reject a noncanonical project ID
    // so two identifiers that differ only by case cannot share a destination.
    return ProjectId.StartsWith(TEXT("prj_"), ESearchCase::CaseSensitive) && ProjectId.Len() > 4
        && IsIdentifier(ProjectId, 76) && ProjectId.Equals(ProjectId.ToLower(), ESearchCase::CaseSensitive);
}

bool IsCanvasNodeId(const FString& NodeId)
{
    return IsIdentifier(NodeId, 128);
}

bool IsLocalProjectDirectory(const FString& Directory)
{
    return IsUnderProjectWithoutReparsePoints(Directory);
}

bool IsCanvasAssetSource(const FCanvasAssetSource& Source)
{
    return IsCanvasProjectId(Source.ProjectId) && IsCanvasNodeId(Source.NodeId) && IsSha256(Source.Digest)
        && (Source.Extension.Equals(TEXT("png"), ESearchCase::CaseSensitive)
            || Source.Extension.Equals(TEXT("jpg"), ESearchCase::CaseSensitive)
            || Source.Extension.Equals(TEXT("jpeg"), ESearchCase::CaseSensitive) || Source.IsWav());
}

bool InspectCanvasAsset(FPreparedCanvasAsset& Source)
{
    if (!IsCanvasAssetSource(Source) || Source.Bytes.Num() < 4 || Source.Bytes.Num() > MaxCanvasAssetBytes
        || !HashMatches(Source.Bytes, Source.Digest))
    {
        return false;
    }
    if (Source.IsWav())
    {
        return ParsePcmWav(Source.Bytes.GetData(), Source.Bytes.Num(), Source.Wave);
    }

    const EImageFormat Format = Source.Extension == TEXT("png") ? EImageFormat::PNG : EImageFormat::JPEG;
    IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (Wrappers.DetectImageFormat(Source.Bytes.GetData(), Source.Bytes.Num()) != Format) return false;
    const TSharedPtr<IImageWrapper> Wrapper = Wrappers.CreateImageWrapper(Format);
    if (!Wrapper || !Wrapper->SetCompressed(Source.Bytes.GetData(), Source.Bytes.Num())) return false;
    const int64 Width = Wrapper->GetWidth();
    const int64 Height = Wrapper->GetHeight();
    if (Width <= 0 || Height <= 0 || Width > MaxImageSide || Height > MaxImageSide)
    {
        return false;
    }
    Source.Width = static_cast<int32>(Width);
    Source.Height = static_cast<int32>(Height);
    return true;
}

bool IsProjectAssetFolder(const FString& Destination)
{
    return (Destination == TEXT("/Game") || Destination.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
        && FPackageName::IsValidPath(Destination)
        && FPackageName::IsValidLongPackageName(Destination / TEXT("RefmuseCanvas"));
}

bool ImportCanvasAsset(const FPreparedCanvasAsset& Source, const FString& Destination, FString& Result)
{
    const bool bIsWav = Source.IsWav();
    const TCHAR* Prefix = bIsWav ? TEXT("SW_Refmuse_") : TEXT("T_Refmuse_");
    const FString AssetName = Prefix + Source.Digest.Left(16);
    const FString PackageName = Destination / AssetName;
    FString Filename;
    if (!IsProjectAssetFolder(Destination) || !FPackageName::IsValidLongPackageName(PackageName)
        || !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
    {
        Result = TEXT("The destination must be a valid folder under /Game.");
        return false;
    }
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    if (!FPaths::IsUnderDirectory(Filename, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))
        || !IsUnderProjectWithoutReparsePoints(FPaths::GetPath(Filename)))
    {
        Result = TEXT("The target is outside the project Content directory or uses a redirected directory.");
        return false;
    }
    IFileManager& Files = IFileManager::Get();
    if (FPackageName::DoesPackageExist(PackageName) || Files.FileExists(*Filename) || IsValid(FindPackage(nullptr, *PackageName)))
    {
        Result = TEXT("A package with this name already exists. No asset was replaced.");
        return false;
    }

    FImage Pixels;
    const float Duration = bIsWav ? static_cast<float>(Source.Wave.FrameCount()) / static_cast<float>(Source.Wave.SampleRate) : 0.f;
    if (bIsWav)
    {
        if (!CanImportSoundWave(Duration))
        {
            Result = TEXT("UE audio import settings or asset permissions rejected this SoundWave.");
            return false;
        }
    }
    else if (!FImageUtils::DecompressImage(Source.Bytes.GetData(), Source.Bytes.Num(), Pixels)
        || Pixels.SizeX != Source.Width || Pixels.SizeY != Source.Height || Pixels.NumSlices != 1)
    {
        Result = TEXT("UE could not decode the verified image.");
        return false;
    }

    const FString StageDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
        / TEXT("RefmuseCanvas/Import") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Staged = StageDirectory / (AssetName + FPackageName::GetAssetPackageExtension());
    if (!IsUnderProjectWithoutReparsePoints(StageDirectory) || Files.DirectoryExists(*StageDirectory)
        || !Files.MakeDirectory(*StageDirectory, true))
    {
        Result = TEXT("UE could not create the project import directory.");
        return false;
    }

    UPackage* Package = CreatePackage(*PackageName);
    UObject* Asset;
    UTexture2D* Texture = nullptr;
    if (bIsWav)
    {
        // Use the native WAV writer and SoundWave import lifecycle. The checked
        // samples are the only input, so no source or sidecar paths reach UE.
        const FPcmWav& Wave = Source.Wave;
        TArray<uint8> WaveFile;
        SerializeWaveFile(WaveFile, Source.Bytes.GetData() + Wave.DataOffset,
            static_cast<int32>(Wave.DataBytes), Wave.Channels, static_cast<int32>(Wave.SampleRate));
        USoundWave* Sound = NewObject<USoundWave>(Package, *AssetName, RF_Public | RF_Standalone);
        Sound->RawData.UpdatePayload(FSharedBuffer::Clone(WaveFile.GetData(), WaveFile.Num()));
        Sound->NumChannels = Wave.Channels;
        Sound->Duration = Duration;
        Sound->TotalSamples = static_cast<float>(Wave.FrameCount());
        Sound->SetImportedSampleRate(Wave.SampleRate);
        Sound->SetSampleRate(Wave.SampleRate);
        Sound->PostImport();
        Asset = Sound;
    }
    else
    {
        Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
        Texture->Source.Init(Pixels);
        Texture->SRGB = Pixels.GammaSpace == EGammaSpace::sRGB;
        Asset = Texture;
    }
    FMetaData& Metadata = Package->GetMetaData();
    Metadata.SetValue(Asset, TEXT("RefmuseCanvas.ProjectId"), *Source.ProjectId);
    Metadata.SetValue(Asset, TEXT("RefmuseCanvas.NodeId"), *Source.NodeId);
    Metadata.SetValue(Asset, TEXT("RefmuseCanvas.AssetPath"), *(TEXT("assets/") + Source.Digest + TEXT(".") + Source.Extension));
    Metadata.SetValue(Asset, TEXT("RefmuseCanvas.SHA256"), *Source.Digest);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Asset, *Staged, SaveArgs);
    TArray<FString> StagedFiles;
    Files.FindFiles(StagedFiles, *(StageDirectory / TEXT("*")), true, false);
    // This editor bridge expects one .uasset. Commit to Content only with a
    // no-replace move so a file created during save is never overwritten.
    const bool bMoved = bSaved && StagedFiles.Num() == 1 && IsUnderProjectWithoutReparsePoints(FPaths::GetPath(Filename))
        && !FPackageName::DoesPackageExist(PackageName)
        && Files.Move(*Filename, *Staged, false, false, false, true);
    for (const FString& File : StagedFiles)
    {
        Files.Delete(*(StageDirectory / File), false, false, true);
    }
    const bool bCleaned = Files.DeleteDirectory(*StageDirectory, false, false);
    if (!bMoved)
    {
        // Saving can make the in-memory asset visible even without a committed
        // file. Release the original package name immediately for a retry.
        if (!Files.FileExists(*Filename))
        {
            FAssetRegistryModule::AssetDeleted(Asset);
        }
        const FString DiscardedName = TEXT("/Temp/RefmuseCanvasDiscarded/") + FPaths::GetCleanFilename(StageDirectory);
        const bool bReleasedName = Package->Rename(*DiscardedName, nullptr,
            REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
        Asset->ClearFlags(RF_Public | RF_Standalone);
        Asset->MarkAsGarbage();
        Package->SetDirtyFlag(false);
        Package->MarkAsGarbage();
        Result = TEXT("Could not save the asset or commit it without replacing an existing package.");
        if (!bReleasedName) Result += TEXT(" Reopen the editor before retrying this asset.");
    }
    else
    {
        FAssetRegistryModule::AssetCreated(Asset);
        if (Texture) Texture->PostEditChange();
        Result = FString::Printf(TEXT("Saved %s: %s"), bIsWav ? TEXT("SoundWave") : TEXT("Texture2D"), *PackageName);
    }
    if (!bCleaned) Result += TEXT(" The project Saved staging directory could not be cleaned up.");
    return bMoved;
}
}
