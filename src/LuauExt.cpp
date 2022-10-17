#include "Luau/BuiltinDefinitions.h"
#include "Luau/ToString.h"
#include "Luau/Transpiler.h"
#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"

LUAU_FASTFLAG(LuauTypeMismatchModuleNameResolution)

namespace types
{
std::optional<Luau::TypeId> getTypeIdForClass(const Luau::ScopePtr& globalScope, std::optional<std::string> className)
{
    std::optional<Luau::TypeFun> baseType;
    if (className.has_value())
    {
        baseType = globalScope->lookupType(className.value());
    }
    if (!baseType.has_value())
    {
        baseType = globalScope->lookupType("Instance");
    }

    if (baseType.has_value())
    {
        return baseType->type;
    }
    else
    {
        // If we reach this stage, we couldn't find the class name nor the "Instance" type
        // This most likely means a valid definitions file was not provided
        return std::nullopt;
    }
}

std::optional<std::string> getTypeName(Luau::TypeId typeId)
{
    auto ty = Luau::follow(typeId);
    if (auto typeName = Luau::getName(ty))
    {
        return *typeName;
    }
    else if (auto mtv = Luau::get<Luau::MetatableTypeVar>(ty))
    {
        if (auto mtvName = Luau::getName(mtv->metatable))
            return *mtvName;
    }
    else if (auto parentClass = Luau::get<Luau::ClassTypeVar>(ty))
    {
        return parentClass->name;
    }
    // if (auto parentUnion = Luau::get<UnionTypeVar>(ty))
    // {
    //     return returnFirstNonnullOptionOfType<ClassTypeVar>(parentUnion);
    // }
    return std::nullopt;
}

// Retrieves the corresponding Luau type for a Sourcemap node
// If it does not yet exist, the type is produced
Luau::TypeId getSourcemapType(
    const Luau::TypeChecker& typeChecker, Luau::TypeArena& arena, const Luau::ScopePtr& globalScope, const SourceNodePtr& node)
{
    // Gets the type corresponding to the sourcemap node if it exists
    if (node->ty)
        return node->ty;

    Luau::LazyTypeVar ltv;
    ltv.thunk = [&typeChecker, &arena, globalScope, node]()
    {
        // Handle if the node is no longer valid
        if (!node)
            return typeChecker.singletonTypes->anyType;

        auto instanceTy = globalScope->lookupType("Instance");
        if (!instanceTy)
            return typeChecker.singletonTypes->anyType;

        // Look up the base class instance
        Luau::TypeId baseTypeId;
        if (auto foundId = getTypeIdForClass(globalScope, node->className))
            baseTypeId = *foundId;
        else
            return typeChecker.singletonTypes->anyType;

        // Point the metatable to the metatable of "Instance" so that we allow equality
        std::optional<Luau::TypeId> instanceMetaIdentity;
        if (auto* ctv = Luau::get<Luau::ClassTypeVar>(instanceTy->type))
            instanceMetaIdentity = ctv->metatable;

        // Create the ClassTypeVar representing the instance
        std::string typeName = getTypeName(baseTypeId).value_or(node->name);
        Luau::ClassTypeVar ctv{typeName, {}, baseTypeId, instanceMetaIdentity, {}, {}, "@roblox"};
        auto typeId = arena.addType(std::move(ctv));

        // Attach Parent and Children info
        // Get the mutable version of the type var
        if (Luau::ClassTypeVar* ctv = Luau::getMutable<Luau::ClassTypeVar>(typeId))
        {
            if (auto parentNode = node->parent.lock())
                ctv->props["Parent"] = Luau::makeProperty(getSourcemapType(typeChecker, arena, globalScope, parentNode));

            // Add children as properties
            for (const auto& child : node->children)
                ctv->props[child->name] = Luau::makeProperty(getSourcemapType(typeChecker, arena, globalScope, child));

            // Add FindFirstAncestor and FindFirstChild
            if (auto instanceType = getTypeIdForClass(globalScope, "Instance"))
            {
                auto findFirstAncestorFunction =
                    Luau::makeFunction(arena, typeId, {typeChecker.singletonTypes->stringType}, {"name"}, {*instanceType});

                Luau::attachMagicFunction(findFirstAncestorFunction,
                    [&arena, node](Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr,
                        Luau::WithPredicate<Luau::TypePackId> withPredicate) -> std::optional<Luau::WithPredicate<Luau::TypePackId>>
                    {
                        if (expr.args.size < 1)
                            return std::nullopt;

                        auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
                        if (!str)
                            return std::nullopt;

                        // This is a O(n) search, not great!
                        if (auto ancestor = node->findAncestor(std::string(str->value.data, str->value.size)))
                        {
                            return Luau::WithPredicate<Luau::TypePackId>{arena.addTypePack({getSourcemapType(typeChecker, arena, scope, *ancestor)})};
                        }

                        return std::nullopt;
                    });
                ctv->props["FindFirstAncestor"] = Luau::makeProperty(findFirstAncestorFunction, "@roblox/globaltype/Instance.FindFirstAncestor");

                auto findFirstChildFunction = Luau::makeFunction(arena, typeId, {typeChecker.singletonTypes->stringType}, {"name"}, {*instanceType});
                Luau::attachMagicFunction(findFirstChildFunction,
                    [node, &arena](Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr,
                        Luau::WithPredicate<Luau::TypePackId> withPredicate) -> std::optional<Luau::WithPredicate<Luau::TypePackId>>
                    {
                        if (expr.args.size < 1)
                            return std::nullopt;

                        auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
                        if (!str)
                            return std::nullopt;

                        if (auto child = node->findChild(std::string(str->value.data, str->value.size)))
                            return Luau::WithPredicate<Luau::TypePackId>{arena.addTypePack({getSourcemapType(typeChecker, arena, scope, *child)})};

                        return std::nullopt;
                    });
                ctv->props["FindFirstChild"] = Luau::makeProperty(findFirstChildFunction, "@roblox/globaltype/Instance.FindFirstChild");
            }
        }
        return typeId;
    };

    auto ty = arena.addType(std::move(ltv));
    node->ty = ty;

    return ty;
}

// Magic function for `Instance:IsA("ClassName")` predicate
std::optional<Luau::WithPredicate<Luau::TypePackId>> magicFunctionInstanceIsA(
    Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr, Luau::WithPredicate<Luau::TypePackId> withPredicate)
{
    if (expr.args.size != 1)
        return std::nullopt;

    auto index = expr.func->as<Luau::AstExprIndexName>();
    auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
    if (!index || !str)
        return std::nullopt;

    std::optional<Luau::LValue> lvalue = tryGetLValue(*index->expr);
    if (!lvalue)
        return std::nullopt;

    std::string className(str->value.data, str->value.size);
    std::optional<Luau::TypeFun> tfun = scope->lookupType(className);
    if (!tfun)
    {
        typeChecker.reportError(Luau::TypeError{expr.args.data[0]->location, Luau::UnknownSymbol{className, Luau::UnknownSymbol::Type}});
        return std::nullopt;
    }

    Luau::TypePackId booleanPack = typeChecker.globalTypes.addTypePack({typeChecker.booleanType});
    return Luau::WithPredicate<Luau::TypePackId>{booleanPack, {Luau::IsAPredicate{std::move(*lvalue), expr.location, tfun->type}}};
}

// Magic function for `instance:Clone()`, so that we return the exact subclass that `instance` is, rather than just a generic Instance
std::optional<Luau::WithPredicate<Luau::TypePackId>> magicFunctionInstanceClone(
    Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr, Luau::WithPredicate<Luau::TypePackId> withPredicate)
{
    auto index = expr.func->as<Luau::AstExprIndexName>();
    if (!index)
        return std::nullopt;

    Luau::TypeArena& arena = typeChecker.currentModule->internalTypes;
    auto instanceType = typeChecker.checkExpr(scope, *index->expr);
    return Luau::WithPredicate<Luau::TypePackId>{arena.addTypePack({instanceType.type})};
}

// Magic function for `Instance:FindFirstChildWhichIsA("ClassName")` and friends
std::optional<Luau::WithPredicate<Luau::TypePackId>> magicFunctionFindFirstXWhichIsA(
    Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr, Luau::WithPredicate<Luau::TypePackId> withPredicate)
{
    if (expr.args.size < 1)
        return std::nullopt;

    auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
    if (!str)
        return std::nullopt;

    std::optional<Luau::TypeFun> tfun = scope->lookupType(std::string(str->value.data, str->value.size));
    if (!tfun)
        return std::nullopt;

    Luau::TypeId nillableClass = Luau::makeOption(typeChecker, typeChecker.globalTypes, tfun->type);
    return Luau::WithPredicate<Luau::TypePackId>{typeChecker.globalTypes.addTypePack({nillableClass})};
}

// Magic function for `EnumItem:IsA("EnumType")` predicate
std::optional<Luau::WithPredicate<Luau::TypePackId>> magicFunctionEnumItemIsA(
    Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr, Luau::WithPredicate<Luau::TypePackId> withPredicate)
{
    if (expr.args.size != 1)
        return std::nullopt;

    auto index = expr.func->as<Luau::AstExprIndexName>();
    auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
    if (!index || !str)
        return std::nullopt;

    std::optional<Luau::LValue> lvalue = tryGetLValue(*index->expr);
    if (!lvalue)
        return std::nullopt;

    std::string enumItem(str->value.data, str->value.size);
    std::optional<Luau::TypeFun> tfun = scope->lookupImportedType("Enum", enumItem);
    if (!tfun) {
        typeChecker.reportError(Luau::TypeError{expr.args.data[0]->location, Luau::UnknownSymbol{enumItem, Luau::UnknownSymbol::Type}});
        return std::nullopt;
    }

    Luau::TypePackId booleanPack = typeChecker.globalTypes.addTypePack({typeChecker.booleanType});
    return Luau::WithPredicate<Luau::TypePackId>{booleanPack, {Luau::IsAPredicate{std::move(*lvalue), expr.location, tfun->type}}};
}

// Magic function for `instance:GetPropertyChangedSignal()`, so that we can perform type checking on the provided property
static std::optional<Luau::WithPredicate<Luau::TypePackId>> magicFunctionGetPropertyChangedSignal(
    Luau::TypeChecker& typeChecker, const Luau::ScopePtr& scope, const Luau::AstExprCall& expr, Luau::WithPredicate<Luau::TypePackId> withPredicate)
{
    if (expr.args.size != 1)
        return std::nullopt;

    auto index = expr.func->as<Luau::AstExprIndexName>();
    auto str = expr.args.data[0]->as<Luau::AstExprConstantString>();
    if (!index || !str)
        return std::nullopt;


    auto instanceType = typeChecker.checkExpr(scope, *index->expr);
    auto ctv = Luau::get<Luau::ClassTypeVar>(Luau::follow(instanceType.type));
    if (!ctv)
        return std::nullopt;

    std::string property(str->value.data, str->value.size);
    if (!Luau::lookupClassProp(ctv, property))
    {
        typeChecker.reportError(Luau::TypeError{expr.args.data[0]->location, Luau::UnknownProperty{instanceType.type, property}});
        return std::nullopt;
    }

    return std::nullopt;
}

void addChildrenToCTV(Luau::TypeChecker& typeChecker, Luau::TypeArena& arena, const Luau::TypeId& ty, const SourceNodePtr& node)
{
    if (Luau::ClassTypeVar* ctv = Luau::getMutable<Luau::ClassTypeVar>(ty))
    {
        // Clear out all the old registered children
        for (auto it = ctv->props.begin(); it != ctv->props.end();)
        {
            if (hasTag(it->second, "@sourcemap-generated"))
                it = ctv->props.erase(it);
            else
                ++it;
        }


        // Extend the props to include the children
        for (const auto& child : node->children)
        {
            ctv->props[child->name] = Luau::Property{
                getSourcemapType(typeChecker, arena, typeChecker.globalScope, child),
                /* deprecated */ false,
                /* deprecatedSuggestion */ {},
                /* location */ std::nullopt,
                /* tags */ {"@sourcemap-generated"},
                /* documentationSymbol*/ std::nullopt,
            };
        }
    }
}

// TODO: expressiveTypes is used because of a Luau issue where we can't cast a most specific Instance type (which we create here)
// to another type. For the time being, we therefore make all our DataModel instance types marked as "any".
// Remove this once Luau has improved
void registerInstanceTypes(Luau::TypeChecker& typeChecker, Luau::TypeArena& arena, const WorkspaceFileResolver& fileResolver, bool expressiveTypes)
{
    if (!fileResolver.rootSourceNode)
        return;

    // Create a type for the root source node
    getSourcemapType(typeChecker, arena, typeChecker.globalScope, fileResolver.rootSourceNode);

    // Modify sourcemap types
    if (fileResolver.rootSourceNode->className == "DataModel")
    {
        // Mutate DataModel with its children
        if (auto dataModelType = typeChecker.globalScope->lookupType("DataModel"))
            addChildrenToCTV(typeChecker, arena, dataModelType->type, fileResolver.rootSourceNode);

        // Mutate globally-registered Services to include children information (so its available through :GetService)
        for (const auto& service : fileResolver.rootSourceNode->children)
        {
            auto serviceName = service->className; // We know it must be a service of the same class name
            if (auto serviceType = typeChecker.globalScope->lookupType(serviceName))
                addChildrenToCTV(typeChecker, arena, serviceType->type, service);
        }

        // Add containers to player and copy over instances
        // TODO: Player.Character should contain StarterCharacter instances
        if (auto playerType = typeChecker.globalScope->lookupType("Player"))
        {
            if (auto* ctv = Luau::getMutable<Luau::ClassTypeVar>(playerType->type))
            {
                // Player.Backpack should be defined
                if (auto backpackType = typeChecker.globalScope->lookupType("Backpack"))
                {
                    ctv->props["Backpack"] = Luau::makeProperty(backpackType->type);
                    // TODO: should we duplicate StarterPack into here as well? Is that a reasonable assumption to make?
                }

                // Player.PlayerGui should contain StarterGui instances
                if (auto playerGuiType = typeChecker.globalScope->lookupType("PlayerGui"))
                {
                    if (auto starterGui = fileResolver.rootSourceNode->findChild("StarterGui"))
                        addChildrenToCTV(typeChecker, arena, playerGuiType->type, *starterGui);
                    ctv->props["PlayerGui"] = Luau::makeProperty(playerGuiType->type);
                }

                // Player.StarterGear should contain StarterPack instances
                if (auto starterGearType = typeChecker.globalScope->lookupType("StarterGear"))
                {
                    if (auto starterPack = fileResolver.rootSourceNode->findChild("StarterPack"))
                        addChildrenToCTV(typeChecker, arena, starterGearType->type, *starterPack);

                    ctv->props["StarterGear"] = Luau::makeProperty(starterGearType->type);
                }

                // Player.PlayerScripts should contain StarterPlayerScripts instances
                if (auto playerScriptsType = typeChecker.globalScope->lookupType("PlayerScripts"))
                {
                    if (auto starterPlayer = fileResolver.rootSourceNode->findChild("StarterPlayer"))
                    {
                        if (auto starterPlayerScripts = starterPlayer.value()->findChild("StarterPlayerScripts"))
                        {
                            addChildrenToCTV(typeChecker, arena, playerScriptsType->type, *starterPlayerScripts);
                        }
                    }
                    ctv->props["PlayerScripts"] = Luau::makeProperty(playerScriptsType->type);
                }
            }
        }
    }

    // Prepare module scope so that we can dynamically reassign the type of "script" to retrieve instance info
    typeChecker.prepareModuleScope = [&, expressiveTypes](const Luau::ModuleName& name, const Luau::ScopePtr& scope)
    {
        // TODO: we hope to remove these in future!
        if (!expressiveTypes)
        {
            scope->bindings[Luau::AstName("script")] = Luau::Binding{typeChecker.singletonTypes->anyType};
            scope->bindings[Luau::AstName("workspace")] = Luau::Binding{typeChecker.singletonTypes->anyType};
            scope->bindings[Luau::AstName("game")] = Luau::Binding{typeChecker.singletonTypes->anyType};
        }

        if (auto node =
                fileResolver.isVirtualPath(name) ? fileResolver.getSourceNodeFromVirtualPath(name) : fileResolver.getSourceNodeFromRealPath(name))
        {
            if (expressiveTypes)
                scope->bindings[Luau::AstName("script")] = Luau::Binding{getSourcemapType(typeChecker, arena, typeChecker.globalScope, node.value())};
        }
    };
}

Luau::LoadDefinitionFileResult registerDefinitions(Luau::TypeChecker& typeChecker, const std::string& definitions)
{
    auto loadResult = Luau::loadDefinitionFile(typeChecker, typeChecker.globalScope, definitions, "@roblox");
    if (!loadResult.success)
    {
        return loadResult;
    }

    // Extend Instance types
    if (auto instanceType = typeChecker.globalScope->lookupType("Instance"))
    {
        if (auto* ctv = Luau::getMutable<Luau::ClassTypeVar>(instanceType->type))
        {
            Luau::attachMagicFunction(ctv->props["IsA"].type, types::magicFunctionInstanceIsA);
            Luau::attachMagicFunction(ctv->props["FindFirstChildWhichIsA"].type, types::magicFunctionFindFirstXWhichIsA);
            Luau::attachMagicFunction(ctv->props["FindFirstChildOfClass"].type, types::magicFunctionFindFirstXWhichIsA);
            Luau::attachMagicFunction(ctv->props["FindFirstAncestorWhichIsA"].type, types::magicFunctionFindFirstXWhichIsA);
            Luau::attachMagicFunction(ctv->props["FindFirstAncestorOfClass"].type, types::magicFunctionFindFirstXWhichIsA);
            Luau::attachMagicFunction(ctv->props["Clone"].type, types::magicFunctionInstanceClone);
            Luau::attachMagicFunction(ctv->props["GetPropertyChangedSignal"].type, magicFunctionGetPropertyChangedSignal);

            // Autocomplete ClassNames for :IsA("") and counterparts
            Luau::attachTag(ctv->props["IsA"].type, "ClassNames");
            Luau::attachTag(ctv->props["FindFirstChildWhichIsA"].type, "ClassNames");
            Luau::attachTag(ctv->props["FindFirstChildOfClass"].type, "ClassNames");
            Luau::attachTag(ctv->props["FindFirstAncestorWhichIsA"].type, "ClassNames");
            Luau::attachTag(ctv->props["FindFirstAncestorOfClass"].type, "ClassNames");

            // Autocomplete Properties for :GetPropertyChangedSignal("")
            Luau::attachTag(ctv->props["GetPropertyChangedSignal"].type, "Properties");

            // Go through all the defined classes and if they are a subclass of Instance then give them the
            // same metatable identity as Instance so that equality comparison works.
            // NOTE: This will OVERWRITE any metatables set on these classes!
            // We assume that all subclasses of instance don't have any metamethaods
            for (auto& [_, ty] : typeChecker.globalScope->exportedTypeBindings)
            {
                if (auto* c = Luau::getMutable<Luau::ClassTypeVar>(ty.type))
                {
                    // Check if the ctv is a subclass of instance
                    if (Luau::isSubclass(c, ctv))
                    {
                        c->metatable = ctv->metatable;
                    }
                }
            }
        }
    }

    // Move Enums over as imported type bindings
    std::unordered_map<Luau::Name, Luau::TypeFun> enumTypes;
    for (auto it = typeChecker.globalScope->exportedTypeBindings.begin(); it != typeChecker.globalScope->exportedTypeBindings.end();)
    {
        auto erase = false;
        auto ty = it->second.type;
        if (auto* ctv = Luau::getMutable<Luau::ClassTypeVar>(ty))
        {
            if (Luau::startsWith(ctv->name, "Enum"))
            {
                if (ctv->name == "EnumItem")
                {
                    Luau::attachMagicFunction(ctv->props["IsA"].type, types::magicFunctionEnumItemIsA);
                    Luau::attachTag(ctv->props["IsA"].type, "Enums");
                }
                else if (ctv->name != "Enum" && ctv->name != "Enums")
                {
                    // Erase the "Enum" at the start
                    ctv->name = ctv->name.substr(4);

                    // Move the enum over to the imported types if it is not internal, otherwise rename the type
                    if (endsWith(ctv->name, "_INTERNAL"))
                    {
                        ctv->name.erase(ctv->name.rfind("_INTERNAL"), 9);
                    }
                    else
                    {
                        enumTypes.emplace(ctv->name, it->second);
                        // Erase the metatable for the type so it can be used in comparison
                    }

                    // Update the documentation symbol
                    Luau::asMutable(ty)->documentationSymbol = "@roblox/enum/" + ctv->name;
                    for (auto& [name, prop] : ctv->props)
                    {
                        prop.documentationSymbol = "@roblox/enum/" + ctv->name + "." + name;
                        Luau::attachTag(prop, "EnumItem");
                    }

                    // Prefix the name (after its been placed into enumTypes) with "Enum."
                    ctv->name = "Enum." + ctv->name;

                    erase = true;
                }

                // Erase the metatable from the type to allow comparison
                ctv->metatable = std::nullopt;
            }
        };

        if (erase)
            it = typeChecker.globalScope->exportedTypeBindings.erase(it);
        else
            ++it;
    }
    typeChecker.globalScope->importedTypeBindings.emplace("Enum", enumTypes);

    return loadResult;
}

Luau::LoadDefinitionFileResult registerDefinitions(Luau::TypeChecker& typeChecker, const std::filesystem::path& definitionsFile)
{
    if (auto definitions = readFile(definitionsFile))
    {
        return registerDefinitions(typeChecker, definitions.value());
    }
    else
    {
        return {false};
    }
}

using NameOrExpr = std::variant<std::string, Luau::AstExpr*>;

// Converts a FTV and function call to a nice string
// In the format "function NAME(args): ret"
std::string toStringNamedFunction(Luau::ModulePtr module, const Luau::FunctionTypeVar* ftv, const NameOrExpr nameOrFuncExpr,
    std::optional<Luau::ScopePtr> scope, ToStringNamedFunctionOpts stringOpts)
{
    Luau::ToStringOptions opts;
    opts.functionTypeArguments = true;
    opts.hideNamedFunctionTypeParameters = false;
    opts.hideTableKind = stringOpts.hideTableKind;
    opts.useLineBreaks = stringOpts.multiline;
    opts.indent = stringOpts.multiline;
    if (scope)
        opts.scope = *scope;
    auto functionString = Luau::toStringNamedFunction("", *ftv, opts);

    // HACK: remove all instances of "_: " from the function string
    // They don't look great, maybe we should upstream this as an option?
    replaceAll(functionString, "_: ", "");

    // If a name has already been provided, just use that
    if (auto name = std::get_if<std::string>(&nameOrFuncExpr))
    {
        return "function " + *name + functionString;
    }
    auto funcExprPtr = std::get_if<Luau::AstExpr*>(&nameOrFuncExpr);

    // TODO: error here?
    if (!funcExprPtr)
        return "function" + functionString;
    auto funcExpr = *funcExprPtr;

    // See if its just in the form `func(args)`
    if (auto local = funcExpr->as<Luau::AstExprLocal>())
    {
        return "function " + std::string(local->local->name.value) + functionString;
    }
    else if (auto global = funcExpr->as<Luau::AstExprGlobal>())
    {
        return "function " + std::string(global->name.value) + functionString;
    }
    else if (funcExpr->as<Luau::AstExprGroup>() || funcExpr->as<Luau::AstExprFunction>())
    {
        // In the form (expr)(args), which implies thats its probably a IIFE
        return "function" + functionString;
    }

    // See if the name belongs to a ClassTypeVar
    Luau::TypeId* parentIt = nullptr;
    std::string methodName;
    std::string baseName;

    if (auto indexName = funcExpr->as<Luau::AstExprIndexName>())
    {
        parentIt = module->astTypes.find(indexName->expr);
        methodName = std::string(1, indexName->op) + indexName->index.value;
        // If we are calling this as a method ':', we should implicitly hide self, and recompute the functionString
        opts.hideFunctionSelfArgument = indexName->op == ':';
        functionString = Luau::toStringNamedFunction("", *ftv, opts);
        replaceAll(functionString, "_: ", "");
        // We can try and give a temporary base name from what we can infer by the index, and then attempt to improve it with proper information
        baseName = Luau::toString(indexName->expr);
        trim(baseName); // Trim it, because toString is probably not meant to be used in this context (it has whitespace)
    }
    else if (auto indexExpr = funcExpr->as<Luau::AstExprIndexExpr>())
    {
        parentIt = module->astTypes.find(indexExpr->expr);
        methodName = "[" + Luau::toString(indexExpr->index) + "]";
        // We can try and give a temporary base name from what we can infer by the index, and then attempt to improve it with proper information
        baseName = Luau::toString(indexExpr->expr);
        // Trim it, because toString is probably not meant to be used in this context (it has whitespace)
        trim(baseName);
    }

    if (!parentIt)
        return "function" + methodName + functionString;

    if (auto name = getTypeName(*parentIt))
        baseName = *name;

    return "function " + baseName + methodName + functionString;
}

std::string toStringReturnType(Luau::TypePackId retTypes, Luau::ToStringOptions options)
{
    return toStringReturnTypeDetailed(retTypes, options).name;
}

Luau::ToStringResult toStringReturnTypeDetailed(Luau::TypePackId retTypes, Luau::ToStringOptions options)
{
    size_t retSize = Luau::size(retTypes);
    bool hasTail = !Luau::finite(retTypes);
    bool wrap = Luau::get<Luau::TypePack>(Luau::follow(retTypes)) && (hasTail ? retSize != 0 : retSize != 1);

    auto result = Luau::toStringDetailed(retTypes, options);
    if (wrap)
        result.name = "(" + result.name + ")";
    return result;
}

// Duplicated from Luau/TypeInfer.h, since its static
std::optional<Luau::AstExpr*> matchRequire(const Luau::AstExprCall& call)
{
    const char* require = "require";

    if (call.args.size != 1)
        return std::nullopt;

    const Luau::AstExprGlobal* funcAsGlobal = call.func->as<Luau::AstExprGlobal>();
    if (!funcAsGlobal || funcAsGlobal->name != require)
        return std::nullopt;

    if (call.args.size != 1)
        return std::nullopt;

    return call.args.data[0];
}
} // namespace types

Luau::AstNode* findNodeOrTypeAtPosition(const Luau::SourceModule& source, Luau::Position pos)
{
    const Luau::Position end = source.root->location.end;
    if (pos < source.root->location.begin)
        return source.root;

    if (pos > end)
        pos = end;

    FindNodeType findNode{pos, end};
    findNode.visit(source.root);
    return findNode.best;
}

std::optional<Luau::Location> lookupTypeLocation(const Luau::Scope& deepScope, const Luau::Name& name)
{
    const Luau::Scope* scope = &deepScope;
    while (true)
    {
        auto it = scope->typeAliasLocations.find(name);
        if (it != scope->typeAliasLocations.end())
            return it->second;

        if (scope->parent)
            scope = scope->parent.get();
        else
            return std::nullopt;
    }
}

std::optional<Luau::Property> lookupProp(const Luau::TypeId& parentType, const Luau::Name& name)
{
    if (auto ctv = Luau::get<Luau::ClassTypeVar>(parentType))
    {
        if (auto prop = Luau::lookupClassProp(ctv, name))
            return *prop;
    }
    else if (auto tbl = Luau::get<Luau::TableTypeVar>(parentType))
    {
        if (tbl->props.find(name) != tbl->props.end())
        {
            return tbl->props.at(name);
        }
    }
    else if (auto mt = Luau::get<Luau::MetatableTypeVar>(parentType))
    {
        if (auto mtable = Luau::get<Luau::TableTypeVar>(Luau::follow(mt->metatable)))
        {
            auto indexIt = mtable->props.find("__index");
            if (indexIt != mtable->props.end())
            {
                Luau::TypeId followed = Luau::follow(indexIt->second.type);
                if (Luau::get<Luau::TableTypeVar>(followed) || Luau::get<Luau::MetatableTypeVar>(followed))
                {
                    return lookupProp(followed, name);
                }
                else if (Luau::get<Luau::FunctionTypeVar>(followed))
                {
                    // TODO: can we handle an index function...?
                    return std::nullopt;
                }
            }
        }

        if (auto tbl = Luau::get<Luau::TableTypeVar>(Luau::follow(mt->table)))
        {
            if (tbl->props.find(name) != tbl->props.end())
            {
                return tbl->props.at(name);
            }
        }
    }
    // else if (auto i = get<Luau::IntersectionTypeVar>(parentType))
    // {
    //     for (Luau::TypeId ty : i->parts)
    //     {
    //         // TODO: find the corresponding ty
    //     }
    // }
    // else if (auto u = get<Luau::UnionTypeVar>(parentType))
    // {
    //     // Find the corresponding ty
    // }
    return std::nullopt;
}

bool types::isMetamethod(const Luau::Name& name)
{
    return name == "__index" || name == "__newindex" || name == "__call" || name == "__concat" || name == "__unm" || name == "__add" ||
           name == "__sub" || name == "__mul" || name == "__div" || name == "__mod" || name == "__pow" || name == "__tostring" ||
           name == "__metatable" || name == "__eq" || name == "__lt" || name == "__le" || name == "__mode" || name == "__iter" || name == "__len";
}

lsp::Position toUTF16(const TextDocument* textDocument, const Luau::Position& position)
{
    if (textDocument)
        return textDocument->convertPosition(position);
    else
        return lsp::Position{static_cast<size_t>(position.line), static_cast<size_t>(position.column)};
}

lsp::Diagnostic createTypeErrorDiagnostic(const Luau::TypeError& error, Luau::FileResolver* fileResolver, const TextDocument* textDocument)
{
    std::string message;
    if (const Luau::SyntaxError* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
        message = "SyntaxError: " + syntaxError->message;
    else if (FFlag::LuauTypeMismatchModuleNameResolution)
        message = "TypeError: " + Luau::toString(error, Luau::TypeErrorToStringOptions{fileResolver});
    else
        message = "TypeError: " + Luau::toString(error);

    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luau";
    diagnostic.code = error.code();
    diagnostic.message = message;
    diagnostic.severity = lsp::DiagnosticSeverity::Error;
    diagnostic.range = {toUTF16(textDocument, error.location.begin), toUTF16(textDocument, error.location.end)};
    diagnostic.codeDescription = {Uri::parse("https://luau-lang.org/typecheck")};
    return diagnostic;
}

lsp::Diagnostic createLintDiagnostic(const Luau::LintWarning& lint, const TextDocument* textDocument)
{
    std::string lintName = Luau::LintWarning::getName(lint.code);

    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luau";
    diagnostic.code = lint.code;
    diagnostic.message = lintName + ": " + lint.text;
    diagnostic.severity = lsp::DiagnosticSeverity::Warning; // Configuration can convert this to an error
    diagnostic.range = {toUTF16(textDocument, lint.location.begin), toUTF16(textDocument, lint.location.end)};
    diagnostic.codeDescription = {Uri::parse("https://luau-lang.org/lint#" + toLower(lintName) + "-" + std::to_string(static_cast<int>(lint.code)))};

    if (lint.code == Luau::LintWarning::Code::Code_LocalUnused || lint.code == Luau::LintWarning::Code::Code_ImportUnused ||
        lint.code == Luau::LintWarning::Code::Code_FunctionUnused)
    {
        diagnostic.tags.emplace_back(lsp::DiagnosticTag::Unnecessary);
    }
    else if (lint.code == Luau::LintWarning::Code::Code_DeprecatedApi || lint.code == Luau::LintWarning::Code::Code_DeprecatedGlobal)
    {
        diagnostic.tags.emplace_back(lsp::DiagnosticTag::Deprecated);
    }

    return diagnostic;
}

lsp::Diagnostic createParseErrorDiagnostic(const Luau::ParseError& error, const TextDocument* textDocument)
{
    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luau";
    diagnostic.code = "SyntaxError";
    diagnostic.message = "SyntaxError: " + error.getMessage();
    diagnostic.severity = lsp::DiagnosticSeverity::Error;
    diagnostic.range = {toUTF16(textDocument, error.getLocation().begin), toUTF16(textDocument, error.getLocation().end)};
    diagnostic.codeDescription = {Uri::parse("https://luau-lang.org/syntax")};
    return diagnostic;
}

struct FindSymbolReferences : public Luau::AstVisitor
{
    const Luau::Symbol symbol;
    std::vector<Luau::Location> result;

    explicit FindSymbolReferences(Luau::Symbol symbol)
        : symbol(symbol)
    {
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            stat->visit(this);
        }

        return false;
    }

    bool visitLocal(Luau::AstLocal* local)
    {
        if (Luau::Symbol(local) == symbol)
        {
            return true;
        }
        return false;
    }

    bool visit(Luau::AstStatLocalFunction* function) override
    {
        if (visitLocal(function->name))
        {
            result.push_back(function->name->location);
        }
        return true;
    }

    bool visit(Luau::AstStatLocal* al) override
    {
        for (size_t i = 0; i < al->vars.size; ++i)
        {
            if (visitLocal(al->vars.data[i]))
            {
                result.push_back(al->vars.data[i]->location);
            }
        }
        return true;
    }

    virtual bool visit(Luau::AstExprLocal* local) override
    {
        if (visitLocal(local->local))
        {
            result.push_back(local->location);
        }
        return true;
    }

    virtual bool visit(Luau::AstExprFunction* fn) override
    {
        for (size_t i = 0; i < fn->args.size; ++i)
        {
            if (visitLocal(fn->args.data[i]))
            {
                result.push_back(fn->args.data[i]->location);
            }
        }
        return true;
    }

    virtual bool visit(Luau::AstStatFor* forStat) override
    {
        if (visitLocal(forStat->var))
        {
            result.push_back(forStat->var->location);
        }
        return true;
    }

    virtual bool visit(Luau::AstStatForIn* forIn) override
    {
        for (auto var : forIn->vars)
        {
            if (visitLocal(var))
            {
                result.push_back(var->location);
            }
        }
        return true;
    }
};

std::vector<Luau::Location> findSymbolReferences(const Luau::SourceModule& source, Luau::Symbol symbol)
{
    FindSymbolReferences finder(symbol);
    source.root->visit(&finder);
    return std::move(finder.result);
}

std::optional<Luau::Location> getLocation(Luau::TypeId type)
{
    type = follow(type);

    if (auto ftv = Luau::get<Luau::FunctionTypeVar>(type))
    {
        if (ftv->definition)
            return ftv->definition->originalNameLocation;
    }

    return std::nullopt;
}