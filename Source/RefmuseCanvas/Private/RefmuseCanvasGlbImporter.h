// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

namespace RefmuseCanvas
{
struct FPreparedCanvasAsset;
bool InspectCanvasGlb(FPreparedCanvasAsset& Source);
bool ImportCanvasGlb(const FPreparedCanvasAsset& Source, const FString& Destination, FString& Result);
}
