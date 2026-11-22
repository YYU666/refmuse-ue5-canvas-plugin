// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

namespace RefmuseCanvas
{
struct FCanvasAssetSource;
struct FPreparedCanvasAsset;

constexpr int32 MaxCanvasAssetBytes = 16 * 1024 * 1024;
bool IsCanvasProjectId(const FString& ProjectId);
bool IsCanvasNodeId(const FString& NodeId);
bool IsLocalProjectDirectory(const FString& Directory);
bool IsCanvasAssetSource(const FCanvasAssetSource& Source);
bool InspectCanvasAsset(FPreparedCanvasAsset& Image);
bool IsProjectAssetFolder(const FString& Destination);
bool ImportCanvasAsset(const FPreparedCanvasAsset& Image, const FString& Destination, FString& Result);
}
