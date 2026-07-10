using UnrealBuildTool;

public class OpenCodeCore : ModuleRules
{
	public OpenCodeCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",
			"Json",
			"JsonUtilities",
			"Sockets",
			"Networking",
			"AssetRegistry",
			"Landscape",
			"Foliage",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"EditorSubsystem",
				"BlueprintGraph",
			});
		}
	}
}
