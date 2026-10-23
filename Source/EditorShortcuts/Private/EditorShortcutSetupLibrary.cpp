#include "EditorShortcutSetupLibrary.h"

#include "EditorShortcutTagLibrary.h"
#include "EditorShortcutsLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "EditorUtilityBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
/**
 * The one class a shortcut Blueprint is parented on.
 *
 * UEditorFunctionLibrary is the engine's sentinel base for Blueprints holding editor
 * logic. Two things follow from it and nothing else provides both: it lives in an
 * editor module, which is what lets the graphs call editor-only functions, and the
 * Editor Utility Blueprint factory recognises it and switches the asset to
 * BPTYPE_FunctionLibrary, which is what lets the same asset be created by hand.
 *
 * Its header is private to Blutility, so it is resolved by path rather than included.
 */
UClass* FindShortcutBlueprintParentClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/Blutility.EditorFunctionLibrary"));
}

/** The pin type of a "Array of Actor" input, which is what the selection is fed into. */
FEdGraphPinType MakeActorArrayPinType()
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	PinType.PinSubCategoryObject = AActor::StaticClass();
	PinType.ContainerType = EPinContainerType::Array;
	return PinType;
}

/** The tag library call one operation wraps, and the prefix its generated functions carry. */
struct FTagOp
{
	FName Function;
	const TCHAR* Prefix;
};

FTagOp ResolveTagOp(EEditorShortcutTagOp Op)
{
	switch (Op)
	{
	case EEditorShortcutTagOp::Add:
		return { GET_FUNCTION_NAME_CHECKED(UEditorShortcutTagLibrary, AddTagToActors), TEXT("Add") };
	case EEditorShortcutTagOp::Remove:
		return { GET_FUNCTION_NAME_CHECKED(UEditorShortcutTagLibrary, RemoveTagFromActors), TEXT("Remove") };
	default:
		return { GET_FUNCTION_NAME_CHECKED(UEditorShortcutTagLibrary, ToggleTagOnActors), TEXT("Toggle") };
	}
}

/**
 * Clears out the function if it is already there and creates it empty, returning its entry node.
 * Rebuilding rather than editing in place keeps a regenerated function identical to a fresh one.
 */
UK2Node_FunctionEntry* RebuildFunctionGraph(UBlueprint* Blueprint, FName FunctionName, UEdGraph*& OutGraph, FString& OutError)
{
	OutGraph = nullptr;
	if (UEdGraph* Existing = FindObject<UEdGraph>(Blueprint, *FunctionName.ToString())) FBlueprintEditorUtils::RemoveGraph(Blueprint, Existing);

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("could not create a graph for %s"), *FunctionName.ToString());
		return nullptr;
	}

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no entry node"), *FunctionName.ToString());
		return nullptr;
	}

	OutGraph = Graph;
	return EntryNodes[0];
}

/**
 * Places a call to `Class::Function` right of the entry node and wires the exec pins.
 * Null when either side has no exec pin, which only happens for a pure or unresolved function.
 */
UK2Node_CallFunction* AddCallAfterEntry(UEdGraph* Graph, UK2Node_FunctionEntry* EntryNode, UClass* Class, FName Function)
{
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->FunctionReference.SetExternalMember(Function, Class);
	CallNode->CreateNewGuid();
	CallNode->PostPlacedNewNode();
	CallNode->SetFlags(RF_Transactional);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	CallNode->NodePosX = EntryNode->NodePosX + 320;
	CallNode->NodePosY = EntryNode->NodePosY;

	UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* CallExec = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!EntryThen || !CallExec) return nullptr;

	EntryThen->MakeLinkTo(CallExec);
	return CallNode;
}

/**
 * Builds one function: an Actors input wired straight into the tag library call,
 * with the tag written into the call's Tag pin.
 */
bool BuildTagFunction(UBlueprint* Blueprint, FName FunctionName, FName Tag, FName TagFunction, FString& OutError)
{
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* EntryNode = RebuildFunctionGraph(Blueprint, FunctionName, Graph, OutError);
	if (!EntryNode) return false;

	UEdGraphPin* ActorsOut = EntryNode->CreateUserDefinedPin(TEXT("Actors"), MakeActorArrayPinType(), EGPD_Output);
	if (!ActorsOut)
	{
		OutError = FString::Printf(TEXT("could not add the Actors input to %s"), *FunctionName.ToString());
		return false;
	}

	UK2Node_CallFunction* CallNode = AddCallAfterEntry(Graph, EntryNode, UEditorShortcutTagLibrary::StaticClass(), TagFunction);
	UEdGraphPin* CallActors = CallNode ? CallNode->FindPin(TEXT("Actors"), EGPD_Input) : nullptr;
	UEdGraphPin* CallTag = CallNode ? CallNode->FindPin(TEXT("Tag"), EGPD_Input) : nullptr;

	if (!CallActors || !CallTag)
	{
		OutError = FString::Printf(TEXT("%s: %s did not expose the expected pins"), *FunctionName.ToString(), *TagFunction.ToString());
		return false;
	}

	ActorsOut->MakeLinkTo(CallActors);
	CallTag->DefaultValue = Tag.ToString();

	return true;
}

/**
 * Builds one no-argument function whose body is a single Execute Python Command.
 *
 * Marked Call In Editor like a hand-made one, so it also shows up as a button on the
 * library. PythonScriptLibrary is resolved by path: the Python plugin is an editor plugin
 * this module does not link against, and a missing plugin should fail here, not at load.
 */
bool BuildPythonCommandFunction(UBlueprint* Blueprint, FName FunctionName, const FString& PythonCommand, FString& OutError)
{
	UClass* PythonLibrary = FindObject<UClass>(nullptr, TEXT("/Script/PythonScriptPlugin.PythonScriptLibrary"));
	if (!PythonLibrary)
	{
		OutError = TEXT("could not resolve /Script/PythonScriptPlugin.PythonScriptLibrary; is the Python Editor Script Plugin enabled?");
		return false;
	}

	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* EntryNode = RebuildFunctionGraph(Blueprint, FunctionName, Graph, OutError);
	if (!EntryNode) return false;
	EntryNode->MetaData.bCallInEditor = true;

	UK2Node_CallFunction* CallNode = AddCallAfterEntry(Graph, EntryNode, PythonLibrary, TEXT("ExecutePythonCommand"));
	UEdGraphPin* CallCommand = CallNode ? CallNode->FindPin(TEXT("PythonCommand"), EGPD_Input) : nullptr;
	if (!CallCommand)
	{
		OutError = FString::Printf(TEXT("%s: ExecutePythonCommand did not expose the expected pins"), *FunctionName.ToString());
		return false;
	}

	CallCommand->DefaultValue = PythonCommand;
	return true;
}

/**
 * Finds the shortcut library at PackageName or creates it. An asset with the wrong parent
 * is replaced: the parent class is what decides whether editor functions may be called,
 * so such an asset can never compile.
 */
UBlueprint* LoadOrCreateShortcutBlueprint(const FString& PackageName, UPackage*& OutPackage, bool& bOutIsNew)
{
	OutPackage = nullptr;
	bOutIsNew = false;

	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("'%s' is not a valid package name"), *PackageName);
		return nullptr;
	}

	const FString AssetName = FPackageName::GetShortName(PackageName);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not create package %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	UClass* ParentClass = FindShortcutBlueprintParentClass();
	if (!ParentClass)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not resolve /Script/Blutility.EditorFunctionLibrary; is the Blutility module loaded?"));
		return nullptr;
	}

	UBlueprint* Blueprint = FindObject<UBlueprint>(Package, *AssetName);

	if (Blueprint && (Blueprint->ParentClass != ParentClass || !Blueprint->IsA<UEditorUtilityBlueprint>()))
	{
		UE_LOG(LogEditorShortcuts, Warning, TEXT("%s cannot call editor functions (parent %s); replacing it"),
			*PackageName, Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("none"));
		Blueprint->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Blueprint->MarkAsGarbage();
		Blueprint = nullptr;
	}

	bOutIsNew = Blueprint == nullptr;
	if (bOutIsNew)
	{
		// Deliberately the same shape UEditorUtilityBlueprintFactory produces for this
		// parent, so a generated asset and a hand-made one are indistinguishable.
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			*AssetName,
			BPTYPE_FunctionLibrary,
			UEditorUtilityBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	if (!Blueprint)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not create blueprint %s"), *PackageName);
		return nullptr;
	}

	OutPackage = Package;
	return Blueprint;
}

/** Compiles, registers a new asset and optionally saves. Returns the generated class path, empty on failure. */
FString FinishShortcutBlueprint(UBlueprint* Blueprint, UPackage* Package, const FString& PackageName, bool bIsNew, bool bSave)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	if (bIsNew) FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->MarkPackageDirty();

	if (bSave)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			UE_LOG(LogEditorShortcuts, Error, TEXT("could not save %s"), *FileName);
			return FString();
		}
	}

	return FString::Printf(TEXT("%s.%s_C"), *PackageName, *FPackageName::GetShortName(PackageName));
}
} // namespace

FString UEditorShortcutSetupLibrary::CreateTagLibrary(const FString& PackageName, const TArray<FName>& Tags, EEditorShortcutTagOp Op, bool bSave)
{
	if (Tags.IsEmpty())
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("no tags given, nothing to generate"));
		return FString();
	}

	UPackage* Package = nullptr;
	bool bIsNew = false;
	UBlueprint* Blueprint = LoadOrCreateShortcutBlueprint(PackageName, Package, bIsNew);
	if (!Blueprint) return FString();

	const FTagOp TagOp = ResolveTagOp(Op);

	int32 Built = 0;
	for (FName Tag : Tags)
	{
		if (Tag.IsNone()) continue;

		const FName FunctionName(*FString::Printf(TEXT("%s%s"), TagOp.Prefix, *Tag.ToString()));

		FString Error;
		if (!BuildTagFunction(Blueprint, FunctionName, Tag, TagOp.Function, Error))
		{
			UE_LOG(LogEditorShortcuts, Error, TEXT("%s"), *Error);
			continue;
		}

		UE_LOG(LogEditorShortcuts, Log, TEXT("Generated %s -> %s(Actors, %s)"), *FunctionName.ToString(), *TagOp.Function.ToString(), *Tag.ToString());
		++Built;
	}

	const FString ClassPath = FinishShortcutBlueprint(Blueprint, Package, PackageName, bIsNew, bSave);
	if (ClassPath.IsEmpty()) return FString();

	UE_LOG(LogEditorShortcuts, Log, TEXT("%s %s with %d function(s)"), bIsNew ? TEXT("Created") : TEXT("Updated"), *PackageName, Built);
	return ClassPath;
}

FString UEditorShortcutSetupLibrary::SetPythonCommandFunction(const FString& PackageName, FName FunctionName, const FString& PythonCommand, bool bSave)
{
	if (FunctionName.IsNone() || PythonCommand.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("a function name and a Python command are both required"));
		return FString();
	}

	UPackage* Package = nullptr;
	bool bIsNew = false;
	UBlueprint* Blueprint = LoadOrCreateShortcutBlueprint(PackageName, Package, bIsNew);
	if (!Blueprint) return FString();

	FString Error;
	if (!BuildPythonCommandFunction(Blueprint, FunctionName, PythonCommand, Error))
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("%s"), *Error);
		return FString();
	}

	const FString ClassPath = FinishShortcutBlueprint(Blueprint, Package, PackageName, bIsNew, bSave);
	if (ClassPath.IsEmpty()) return FString();

	UE_LOG(LogEditorShortcuts, Log, TEXT("%s %s::%s -> ExecutePythonCommand"), bIsNew ? TEXT("Created") : TEXT("Updated"), *PackageName, *FunctionName.ToString());
	return ClassPath;
}
