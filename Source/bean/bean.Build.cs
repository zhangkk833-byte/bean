// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class bean : ModuleRules
{
	public bean(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"EnhancedInput", "UMG", "Slate", "SlateCore",
			"ImageCore", "Json", "JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		if (Target.bBuildEditor)
		{
			// Only needed for the editor-only material baking utility (BeanBakeAssets).
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
