// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "RefmuseCanvasAssetReceiver.h"

namespace RefmuseCanvas
{
struct FAssetCommand
{
    FGuid Id;
    FString Action;
    FString ProjectId;
    FString NodeId;
    FGuid CopyId;
    FString Destination;
};

class FAssetCommandInbox
{
public:
    bool Open();
    bool Read(FAssetCommand& Command);
    void Reply(const FAssetCommand& Command, bool bOk, const FString& Message,
        const TOptional<FCanvasAssetSource>& Source = {});
    const FString& GetDirectory() const { return Directory; }

private:
    FString Directory;
};
}
