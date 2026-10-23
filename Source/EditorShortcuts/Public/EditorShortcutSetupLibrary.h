#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EditorShortcutSetupLibrary.generated.h"

/** Which UEditorShortcutTagLibrary call a generated function forwards the selection to. */
UENUM(BlueprintType)
enum class EEditorShortcutTagOp : uint8
{
	/** Add<Tag>: adds the tag where it is missing. */
	Add,
	/** Remove<Tag>: removes the tag where it is present. */
	Remove,
	/** Toggle<Tag>: adds it where missing and removes it where present. */
	Toggle,
};

/**
 * Generates the Blueprint function library a tag shortcut binds to.
 *
 * A binding cannot pass a tag, so every tag needs its own no-argument function.
 * Writing those by hand is tedious and easy to get subtly wrong, so this builds
 * them: one function per tag and operation, each forwarding the selection to the
 * matching UEditorShortcutTagLibrary call with the tag baked in.
 *
 * The asset is an Editor Utility Blueprint parented on EditorFunctionLibrary, which
 * is the only Blueprint shape this system uses. It is exactly what the editor makes
 * for Editor Utilities > Editor Utility Blueprint with that parent picked, so a
 * generated library and a hand-made one behave identically. A plain Blueprint
 * function library cannot be used instead: the tag library lives in an editor module,
 * and a runtime Blueprint is not allowed to call it.
 *
 * Run it again with a longer tag list or another operation to add more; existing
 * functions with the same names are rebuilt, anything else in the asset is left alone.
 */
UCLASS()
class EDITORSHORTCUTS_API UEditorShortcutSetupLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Creates or updates a Blueprint function library holding one <Op><Tag> function per tag.
	 *
	 * @param PackageName  where to put it, e.g. /PCGPlugins/EditorShortcuts_BPFL/BPFL_Pick
	 * @param Tags         tags to generate functions for
	 * @param Op           what the generated functions do to the tag
	 * @param bSave        write the package to disk
	 * @return             the generated class path to put in a binding's TargetClass, empty on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Setup")
	static FString CreateTagLibrary(const FString& PackageName, const TArray<FName>& Tags, EEditorShortcutTagOp Op = EEditorShortcutTagOp::Toggle, bool bSave = true);

	/**
	 * Creates or rebuilds one no-argument function whose body is a single Execute Python Command.
	 *
	 * For shortcuts that are already a Python script: the function is marked Call In Editor,
	 * so it is both a binding target and a button on the library. Only the named function is
	 * rebuilt; everything else in the asset is left alone.
	 *
	 * @param PackageName    library to put it in, created when missing
	 * @param FunctionName   function to create or rebuild
	 * @param PythonCommand  the command, as typed into Execute Python Command's pin
	 * @param bSave          write the package to disk
	 * @return               the generated class path, empty on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "Editor Shortcuts|Setup")
	static FString SetPythonCommandFunction(const FString& PackageName, FName FunctionName, const FString& PythonCommand, bool bSave = true);
};
