// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
using UnrealBuildTool;

public class RefmuseCanvas : ModuleRules
{
    public RefmuseCanvas(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore",
            "UnrealEd", "ToolMenus", "WebBrowser", "Projects", "AssetRegistry", "ImageCore",
            "ImageWrapper", "AssetTools", "ContentBrowserData", "InputCore", "Json"
        });
        PublicSystemLibraries.Add("bcrypt.lib");
    }
}
