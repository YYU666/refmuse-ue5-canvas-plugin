// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "RefmuseCanvasGlbImporter.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/NamePermissionList.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RefmuseCanvasAssetImporter.h"
#include "RefmuseCanvasAssetReceiver.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "Windows/WindowsHWrapper.h"

namespace RefmuseCanvas
{
namespace
{
uint32 ReadWord(const TArray<uint8>& Bytes, int32 Offset)
{
    uint32 Word;
    FMemory::Memcpy(&Word, Bytes.GetData() + Offset, sizeof(Word));
    return Word;
}

const TArray<TSharedPtr<FJsonValue>>& ArrayField(const FJsonObject& Object, const TCHAR* Name)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* Value;
    return Object.TryGetArrayField(Name, Value) ? *Value : Empty;
}

const FJsonObject* IndexedObject(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Index)
{
    const TSharedPtr<FJsonObject>* Object;
    return Values.IsValidIndex(Index) && Values[Index]->TryGetObject(Object) ? Object->Get() : nullptr;
}

bool ValidateImage(const FJsonObject& Image, const TArray<TSharedPtr<FJsonValue>>& Views, const uint8* Binary, int32 Size)
{
    int32 ViewIndex, Offset = 0, Length, Buffer;
    FString Mime;
    if (Image.HasField(TEXT("uri")) || !Image.TryGetNumberField(TEXT("bufferView"), ViewIndex)
        || !Image.TryGetStringField(TEXT("mimeType"), Mime)) return false;
    const FJsonObject* View = IndexedObject(Views, ViewIndex);
    if (!View || !View->TryGetNumberField(TEXT("buffer"), Buffer) || Buffer != 0
        || !View->TryGetNumberField(TEXT("byteLength"), Length)
        || (View->HasField(TEXT("byteOffset")) && !View->TryGetNumberField(TEXT("byteOffset"), Offset))
        || Offset < 0 || Length <= 0 || Offset > Size || Length > Size - Offset) return false;

    const EImageFormat Format = Mime == TEXT("image/png") ? EImageFormat::PNG
        : Mime == TEXT("image/jpeg") ? EImageFormat::JPEG : EImageFormat::Invalid;
    IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (Format == EImageFormat::Invalid || Wrappers.DetectImageFormat(Binary + Offset, Length) != Format) return false;
    const TSharedPtr<IImageWrapper> Wrapper = Wrappers.CreateImageWrapper(Format);
    if (!Wrapper || !Wrapper->SetCompressed(Binary + Offset, Length)
        || Wrapper->GetWidth() < 1 || Wrapper->GetWidth() > 4096
        || Wrapper->GetHeight() < 1 || Wrapper->GetHeight() > 4096) return false;
    TArray64<uint8> Pixels;
    return Wrapper->GetRaw(Pixels) && !Pixels.IsEmpty();
}

bool CheckDependencies(const TArray<UObject*>& Assets, int32 ExpectedImages, int32 ExpectedMaterials)
{
    TSet<UTexture*> Textures, BoundTextures;
    TSet<UMaterialInterface*> Materials, BoundMaterials;
    TArray<UStaticMesh*> Meshes;
    for (UObject* Asset : Assets)
    {
        if (UTexture2D* Texture = Cast<UTexture2D>(Asset)) Textures.Add(Texture);
        else if (UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(Asset)) Materials.Add(Material);
        else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset)) Meshes.Add(Mesh);
        else return false;
    }
    if (Meshes.IsEmpty() || Textures.Num() != ExpectedImages || Materials.Num() != ExpectedMaterials) return false;
    for (UStaticMesh* Mesh : Meshes)
    {
        if (Mesh->GetStaticMaterials().IsEmpty()) return false;
        for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
        {
            UMaterialInterface* Material = Slot.MaterialInterface.Get();
            if (!Materials.Contains(Material)) return false;
            BoundMaterials.Add(Material);
        }
    }
    for (UMaterialInterface* Material : BoundMaterials)
    {
        TArray<FMaterialParameterInfo> Parameters;
        TArray<FGuid> Ids;
        Material->GetAllTextureParameterInfo(Parameters, Ids);
        for (const FMaterialParameterInfo& Parameter : Parameters)
        {
            UTexture* Value = nullptr;
            if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter), Value) && Textures.Contains(Value))
            {
                BoundTextures.Add(Value);
            }
        }
    }
    return BoundMaterials.Num() == Materials.Num() && BoundTextures.Num() == Textures.Num();
}

void DiscardPackages(const FString& Prefix, const FString& Nonce)
{
    TArray<UPackage*> Packages;
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        if (It->GetName().StartsWith(Prefix, ESearchCase::IgnoreCase)) Packages.Add(*It);
    }
    for (UPackage* Package : Packages)
    {
        TArray<UObject*> Objects;
        GetObjectsWithPackage(Package, Objects, true);
        for (UObject* Object : Objects)
        {
            if (Object->IsAsset()) FAssetRegistryModule::AssetDeleted(Object);
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
        const FString Discarded = FString::Printf(TEXT("/Temp/RefmuseCanvasDiscarded/%s_%d"), *Nonce, Packages.Find(Package));
        Package->Rename(*Discarded, nullptr, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
        Package->SetDirtyFlag(false);
        Package->MarkAsGarbage();
    }
}
}

bool InspectCanvasGlb(FPreparedCanvasAsset& Source)
{
    const auto& Bytes = Source.Bytes;
    if (Bytes.Num() < 28 || ReadWord(Bytes, 0) != 0x46546c67 || ReadWord(Bytes, 4) != 2
        || ReadWord(Bytes, 8) != static_cast<uint32>(Bytes.Num()) || ReadWord(Bytes, 16) != 0x4e4f534a) return false;

    const uint32 JsonLength = ReadWord(Bytes, 12);
    if (JsonLength < 2 || JsonLength % 4 || JsonLength > static_cast<uint32>(Bytes.Num() - 28)) return false;
    const int32 BinaryHeader = 20 + static_cast<int32>(JsonLength);
    const int32 BinarySize = Bytes.Num() - BinaryHeader - 8;
    if (ReadWord(Bytes, BinaryHeader) != static_cast<uint32>(BinarySize)
        || ReadWord(Bytes, BinaryHeader + 4) != 0x004e4942) return false;

    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20), JsonLength);
    TSharedPtr<FJsonObject> Model;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Model)
        || !Model || Model->HasField(TEXT("extensionsRequired")) || Model->HasField(TEXT("extensionsUsed"))
        || Model->HasField(TEXT("extensions")) || Model->HasField(TEXT("skins")) || Model->HasField(TEXT("animations"))) return false;

    const auto& Buffers = ArrayField(*Model, TEXT("buffers"));
    const FJsonObject* Buffer = IndexedObject(Buffers, 0);
    int32 BufferLength;
    if (Buffers.Num() != 1 || !Buffer || Buffer->HasField(TEXT("uri"))
        || !Buffer->TryGetNumberField(TEXT("byteLength"), BufferLength)
        || BufferLength < 1 || BufferLength > BinarySize || BinarySize - BufferLength > 3) return false;

    const auto& Meshes = ArrayField(*Model, TEXT("meshes"));
    const auto& Materials = ArrayField(*Model, TEXT("materials"));
    const auto& Images = ArrayField(*Model, TEXT("images"));
    if (Meshes.IsEmpty() || Meshes.Num() > 16 || Materials.IsEmpty() || Materials.Num() > 16 || Images.Num() > 16) return false;

    const auto& Views = ArrayField(*Model, TEXT("bufferViews"));
    for (int32 Index = 0; Index < Images.Num(); ++Index)
    {
        const FJsonObject* Image = IndexedObject(Images, Index);
        if (!Image || !ValidateImage(*Image, Views, Bytes.GetData() + BinaryHeader + 8, BufferLength)) return false;
    }
    Source.GlbImageCount = Images.Num();
    Source.GlbMaterialCount = Materials.Num();
    return true;
}

bool ImportCanvasGlb(const FPreparedCanvasAsset& Source, const FString& Destination, FString& Result)
{
    if (!IsProjectAssetFolder(Destination))
    {
        Result = TEXT("The destination must be a valid folder under /Game.");
        return false;
    }
    const FString Nonce = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString RootPackage = Destination / (TEXT("GLB_Refmuse_") + Source.Digest.Left(16));
    const FString PackagePrefix = RootPackage + TEXT("/");
    FString FinalDirectory;
    if (!FPackageName::TryConvertLongPackageNameToFilename(RootPackage, FinalDirectory)) return false;
    FinalDirectory = FPaths::ConvertRelativePathToFull(FinalDirectory);
    const FString Stage = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RefmuseCanvas/Import") / Nonce);
    const FString Serialized = Stage / TEXT("packages");
    IFileManager& Files = IFileManager::Get();
    if (!FPaths::IsUnderDirectory(FinalDirectory, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))
        || !IsLocalProjectDirectory(FinalDirectory) || !IsLocalProjectDirectory(Stage)
        || Files.DirectoryExists(*FinalDirectory) || Files.FileExists(*FinalDirectory) || Files.DirectoryExists(*Stage))
    {
        Result = TEXT("The GLB destination already exists or is not a local project directory. No assets were replaced.");
        return false;
    }
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        if (It->GetName().StartsWith(PackagePrefix, ESearchCase::IgnoreCase))
        {
            Result = TEXT("The GLB destination already contains loaded packages. No assets were replaced.");
            return false;
        }
    }
    if (!Files.MakeDirectory(*Serialized, true))
    {
        Result = TEXT("UE could not create the GLB staging directory.");
        return false;
    }

    bool bCommitted = false;
    ON_SCOPE_EXIT
    {
        if (!bCommitted) DiscardPackages(PackagePrefix, Nonce);
        Files.DeleteDirectory(*Stage, false, true);
    };

    UAssetImportTask* Task = NewObject<UAssetImportTask>();
    Task->Filename = Stage / TEXT("Scene.glb");
    Task->DestinationPath = RootPackage;
    Task->bAutomated = true;
    Task->bReplaceExisting = false;
    Task->bReplaceExistingSettings = false;
    Task->bSave = false;
    Task->bAsync = false;
    if (!FFileHelper::SaveArrayToFile(Source.Bytes, *Task->Filename))
    {
        Result = TEXT("UE could not stage the GLB.");
        return false;
    }
    IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    Tools.ImportAssetTasks({Task});
    const TArray<UObject*>& Assets = Task->GetObjects();
    if (!CheckDependencies(Assets, Source.GlbImageCount, Source.GlbMaterialCount))
    {
        Result = TEXT("GLB import did not produce a complete mesh, material instance and texture set.");
        return false;
    }

    TSet<FString> PackageNames;
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    for (UObject* Asset : Assets)
    {
        UPackage* Package = Asset->GetOutermost();
        const FString PackageName = Package->GetName();
        if (!PackageName.StartsWith(PackagePrefix, ESearchCase::CaseSensitive) || PackageNames.Contains(PackageName))
        {
            Result = TEXT("Interchange returned an unexpected GLB package path.");
            return false;
        }
        PackageNames.Add(PackageName);

        FMetaData& Metadata = Package->GetMetaData();
        Metadata.SetValue(Asset, TEXT("RefmuseCanvas.ProjectId"), *Source.ProjectId);
        Metadata.SetValue(Asset, TEXT("RefmuseCanvas.NodeId"), *Source.NodeId);
        Metadata.SetValue(Asset, TEXT("RefmuseCanvas.AssetPath"), *(TEXT("assets/") + Source.Digest + TEXT(".glb")));
        Metadata.SetValue(Asset, TEXT("RefmuseCanvas.SHA256"), *Source.Digest);

        const FString Staged = Serialized / (PackageName.RightChop(PackagePrefix.Len()) + TEXT(".uasset"));
        if (!IsLocalProjectDirectory(FPaths::GetPath(Staged))
            || !Files.MakeDirectory(*FPaths::GetPath(Staged), true)
            || !UPackage::SavePackage(Package, Asset, *Staged, SaveArgs))
        {
            Result = TEXT("UE could not save the complete GLB asset set.");
            return false;
        }
    }
    TArray<FString> StagedFiles;
    Files.FindFilesRecursive(StagedFiles, *Serialized, TEXT("*"), true, false);
    if (StagedFiles.Num() != PackageNames.Num() || !IsLocalProjectDirectory(FinalDirectory)
        || !Files.MakeDirectory(*FPaths::GetPath(FinalDirectory), true)
        || !MoveFileW(*Serialized, *FinalDirectory))
    {
        Result = TEXT("UE could not publish the GLB asset set without replacing existing files.");
        return false;
    }
    bCommitted = true;
    for (UObject* Asset : Assets) Asset->GetOutermost()->SetDirtyFlag(false);
    Result = FString::Printf(TEXT("Saved StaticMesh asset set: %s (%d packages)"), *RootPackage, Assets.Num());
    return true;
}
}
