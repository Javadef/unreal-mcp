using UnrealBuildTool;

public class OpenCodeEditor : ModuleRules
{
	public OpenCodeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"EditorStyle",
			"EditorWidgets",
			"UnrealEd",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"HTTP",
			"Json",
			"JsonUtilities",
			"OpenCodeCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"ApplicationCore",
		});
	}
}
