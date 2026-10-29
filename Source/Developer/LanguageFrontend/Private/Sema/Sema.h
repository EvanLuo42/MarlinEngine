#pragma once

#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "LanguageFrontend/Compilation.h"
#include "LanguageFrontend/NameDependencies.h"
#include "LanguageFrontend/Parser.h"
#include "Memory/UniquePtr.h"
#include "Sema/BoundTree.h"
#include "Sema/SemaSymbols.h"

namespace Marlin
{
class Lowering;

struct SemaGenericBinding
{
    StringView Name;
    SemaGenericArgument Argument;
};

/// Where a name is being resolved: the file, the innermost declaration (whose parents give the namespaces and
/// their usings), the enclosing struct and the generic arguments in effect.
struct SemaContext
{
    const AstFile *File = nullptr;
    const AstDecl *Decl = nullptr;
    SemaSymbol *TypeSymbol = nullptr;
    SemaStructInstance *Owner = nullptr;
    Span<const SemaGenericBinding> Generics;
    /// What `this` in a type position and the associated types of an interface stand for when an interface's
    /// requirements are checked against an implementing type (§3.5); in an extension, its extended type is used.
    const SemaType *SelfType = nullptr;
    /// Whether the code is in a `.mcommon`, `.mshader` or `.mscript` file.
    LanguageDialect Dialect = LanguageDialect::Shader;
};

/// What a name resolves to outside local scopes.
struct SemaLookup
{
    List<SemaSymbol *> Symbols;
    const SemaGenericArgument *Generic = nullptr;
    /// A `using X = …;` alias and the file it is declared in.
    const AstUsingDecl *Alias = nullptr;
    const AstFile *AliasFile = nullptr;
    bool Ambiguous = false;

    [[nodiscard]] bool IsEmpty() const
    {
        return Symbols.IsEmpty() && Generic == nullptr && Alias == nullptr;
    }
};

/// The declarations a user-declared attribute applies to, as its struct's `[Attribute]` names them (§3.7).
enum class SemaAttributeTarget : uint32
{
    Struct = 1 << 0,
    Field = 1 << 1,
    Function = 1 << 2,
    Parameter = 1 << 3,
    GenericParameter = 1 << 4,
    Enum = 1 << 5,
    Interface = 1 << 6,
};

/// A field of a user-declared attribute and the constant an application gives it.
struct SemaAttributeField
{
    StringView Name;
    SemaConstant *Value = nullptr;
};

/// An application of a user-declared attribute: its declaration and the value of each of its fields.
struct SemaAttributeValue
{
    SemaSymbol *Declaration = nullptr;
    List<SemaAttributeField> Fields;
};

/// Implicit conversions from best to worst, then explicit-only and none (§4.6).
enum class SemaConversion : uint8
{
    Identity,
    Widening,
    ConstantNarrowing,
    Splat,
    UserDefined,
    Explicit,
    None,
};

/// The semantic analysis of a compilation: symbols, types, instantiation, binding and lowering.
class Sema
{
public:
    Sema(ISourceProvider &sources, const LanguageCompileOptions &options);
    ~Sema();
    MARLIN_NONCOPYABLE(Sema);

    bool Load();
    bool CompileEntryPoint(StringView qualifiedName, MirModule &module);
    bool CheckScriptPackage(StringView package);
    bool CompileScriptPackage(StringView package, MirModule &module);

    void GetDependencies(List<const SourceFile *> &files) const;
    void GetNameDependencies(LanguageNameDependencies &names) const;
    void GetSourceFiles(List<const SourceFile *> &files) const;
    void GetEntryPoints(StringView folder, List<String> &names) const;
    void GetMaterials(StringView folder, List<String> &names) const;
    bool ReflectMaterial(LanguageMaterialReflection &reflection);

    LanguageDiagnostics Diagnostics;
    Arena Memory;
    SemaTypeTable Types;
    const LanguageCompileOptions *Options;
    /// The features `Device.Supports` was asked about (§9.7).
    HashSet<String> QueriedFeatures;

    // Declarations and lookup (SemaDeclarations.cpp)

    SemaSymbol *GetGlobalNamespace() const
    {
        return _global;
    }

    SemaSymbol *GetMarlinNamespace() const
    {
        return _marlin;
    }

    SemaSymbol *NewSymbol(SemaSymbolKind kind, StringView name, SemaSymbol *parent);
    SemaSymbol *GetOrAddNamespace(SemaSymbol *parent, StringView name);
    /// The symbol of a namespace, type or member by its dotted name; null if absent.
    SemaSymbol *FindQualified(StringView name) const;
    /// The symbol a declaration made.
    SemaSymbol *GetSymbolOf(const AstDecl *decl) const;
    /// Looks *name* up from *context* through generics, the enclosing types and namespaces, their usings and
    /// `Marlin` (§7). Locals are the binder's business.
    void Lookup(const SemaContext &context, StringView name, SemaLookup &result);
    /// The members of *container* named *name*.
    Span<SemaSymbol *const> GetMembers(const SemaSymbol *container, StringView name) const;
    /// Whether code at *context* may use *symbol* by its visibility (§1.4).
    bool IsAccessible(const SemaContext &context, const SemaSymbol *symbol) const;
    /// Notes that *file* was used by the code being compiled.
    void UseFile(const AstFile *file);
    void EnsureBodyParsed(const AstFile *file, AstBody &body);
    SemaContext ContextOf(const SemaSymbol *symbol, Span<const SemaGenericBinding> generics = {},
                          SemaStructInstance *owner = nullptr);

    // Types and instances (SemaTypes.cpp)

    /// The type *node* names; in scripts a class, interface, delegate or `string` written without `?` cannot be null.
    const SemaType *ResolveType(AstNode *node, const SemaContext &context);
    /// ResolveType without the checks of the whole type, for its parts.
    const SemaType *ResolveTypeNode(AstNode *node, const SemaContext &context);
    /// The type of a field, parameter or variable declared as *node* with *attributes*: `[Format]` sets the storage
    /// format of a writable texture (§4.4).
    const SemaType *ResolveDeclaredType(AstNode *node, Span<AstAttribute> attributes, const SemaContext &context);
    /// The type a struct, interface, enum or alias symbol names with *genericArguments* written after it.
    const SemaType *ResolveSymbolType(SemaSymbol *symbol, Span<AstNode *> genericArguments,
                                      const SemaContext &context, SourceRange range);
    /// The default of constant generic parameter *index* of *symbol*, or null when it has none (§9.4).
    SemaConstant *GetGenericDefault(SemaSymbol *symbol, usize index);
    /// Resolves the generic arguments written for *symbol*'s declaration, values converted to their parameters;
    /// trailing parameters with defaults may be left out.
    bool ResolveGenericArguments(SemaSymbol *symbol, Span<AstNode *> nodes, const SemaContext &context,
                                 List<SemaGenericArgument> &arguments);
    /// A generic argument: a type, or a constant expression for a value parameter.
    bool ResolveGenericArgument(AstNode *node, const SemaContext &context, bool wantValue,
                                SemaGenericArgument &argument);
    const SemaType *GetTypeOfSymbol(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments,
                                    const SemaContext &context, SourceRange range);
    /// Checks the `where` clauses of *symbol*'s declaration against *bindings*, reporting at *range* when *report*.
    bool CheckConstraints(SemaSymbol *symbol, Span<const SemaGenericBinding> bindings, SourceRange range,
                          bool report = true);
    /// Infers the generic parameters in *bindings*, those of *symbol*'s declaration, from an argument of
    /// *argumentType* given for a parameter written *parameterType*; false when they contradict each other.
    bool InferGenerics(SemaSymbol *symbol, AstNode *parameterType, const SemaType *argumentType,
                       List<SemaGenericBinding> &bindings);
    /// Whether *type* satisfies the interface *interfaceSymbol*, built-in (§9.10) or declared.
    bool Satisfies(const SemaType *type, SemaSymbol *interfaceSymbol);
    /// Whether values of *type* have derivatives, so it conforms to IDifferentiable (§9.12): floating-point scalars,
    /// vectors and matrices, arrays of them, and structs with at least one field outside `[NoDiff]` ones and only
    /// differentiable fields there.
    bool IsDifferentiable(const SemaType *type);
    /// IsDifferentiable for the struct *type*; when it is not, *blocking* is the field outside `[NoDiff]` without
    /// derivatives, or null when there is no field outside them.
    bool IsDifferentiableStruct(const SemaType *type, const SemaField *&blocking);
    /// Whether *attributes* hold `[NoDiff]`: a parameter or field differentiation does not flow through (§9.12).
    static bool IsNoDiff(Span<AstAttribute> attributes);
    /// Whether *symbol* is `[Differentiable]` (§9.12): marked itself, or a method of *owner* implementing an interface
    /// member that is, such as a material's `Evaluate` (`ISurfaceMaterial.Evaluate`, §9.4).
    bool IsMarkedDifferentiable(const SemaSymbol *symbol, SemaStructInstance *owner);
    /// The structs `Compiler.TypeId` was asked about, by qualified name: the custom BSDFs a material's closures carry
    /// (07 §4.3).
    List<String> QueriedTypeIds;
    SemaStructInstance *InstantiateStruct(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments);
    bool ResolveFields(SemaStructInstance *instance);
    /// A function instance; *accessor* tells a property's setter from its getter.
    SemaFunction *InstantiateFunction(SemaSymbol *symbol, SemaStructInstance *owner,
                                      Span<const SemaGenericArgument> arguments,
                                      SemaAccessor accessor = SemaAccessor::None);
    bool ResolveSignature(SemaFunction *function);
    /// The type whose members *owner* declares: the struct itself, or the magic type of a `[Builtin]` struct.
    const SemaType *GetOwnerType(SemaStructInstance *owner);
    const SemaType *GetEnumType(SemaSymbol *symbol);
    SemaConstant *GetEnumMemberValue(SemaSymbol *member);
    /// The value of a `const` or `static readonly` field.
    SemaConstant *GetStaticValue(SemaSymbol *field, SemaStructInstance *owner);
    SemaGlobal *GetGlobal(SemaSymbol *field);
    /// `Namespace.Type.Member`.
    String GetQualifiedName(const SemaSymbol *symbol) const;
    /// The generic bindings of *instance*'s declaration.
    void GetBindings(SemaStructInstance *instance, List<SemaGenericBinding> &bindings);
    void GetBindings(SemaFunction *function, List<SemaGenericBinding> &bindings);

    // Script types (SemaClasses.cpp)

    /// The type values of an instance of *symbol* have: a struct, class, interface or delegate type.
    const SemaType *GetInstanceType(SemaStructInstance *instance);
    /// Resolves a class's base and interfaces, its virtual slots and interface maps, and a struct's or interface's
    /// interfaces (MarlinScript §3.4). False after a diagnostic.
    bool ResolveHierarchy(SemaStructInstance *instance);
    /// The instance of the class that declares *member*: *instance* or one of its bases.
    SemaStructInstance *FindDeclaringInstance(SemaStructInstance *instance, const SemaSymbol *member);
    /// The members named *name* of *instance* and its bases and interfaces, without those an override or a member
    /// of a derived class hides.
    void GetInstanceMembers(SemaStructInstance *instance, StringView name, List<SemaSymbol *> &members);
    /// Whether *derived* is *base* or a class derived from it.
    bool DerivesFrom(const SemaStructInstance *derived, const SemaStructInstance *base);
    /// Whether a reference of type *from* converts to *to* without a check: to a base class, an implemented
    /// interface or `object`.
    bool IsImplicitReference(const SemaType *from, const SemaType *to);
    /// Whether *type* implements *interfaceInstance*.
    bool ImplementsInterface(const SemaType *type, const SemaStructInstance *interfaceInstance);
    /// The class `object`; null without the script core library.
    SemaStructInstance *GetObjectClass();
    /// The parameters and return type of the delegate type *delegate*; false after a diagnostic.
    bool GetDelegateSignature(SemaStructInstance *delegate, SemaSignature &signature);
    /// The parameters (without `this`) and return type of *function*, whose signature is resolved.
    void GetFunctionSignature(SemaFunction *function, SemaSignature &signature);
    /// The storage of a script static field or auto-property of *owner*.
    SemaGlobal *GetStaticField(SemaSymbol *field, SemaStructInstance *owner);
    /// Whether *symbol* is a property whose accessors have no bodies, so a hidden field stores its value.
    static bool IsAutoProperty(const SemaSymbol *symbol);
    /// Whether *function* is called through a virtual slot: `virtual`, `abstract` or `override`, or a member of an
    /// interface.
    static bool IsVirtual(const SemaFunction *function);
    /// The constructor without parameters a class or struct without constructors has (MarlinScript §3.3).
    AstConstructorDecl *MakeDefaultConstructor(AstStructDecl *owner);

    // Interface requirements and extensions (SemaExtensions.cpp, §3.5)

    /// Whether the signature of *member*, a member of an interface, names the implementing type: `this` in a type
    /// position or an associated type. Such a member is called through type parameters only and has no virtual slot.
    static bool UsesImplementingType(const SemaSymbol *member);
    /// Every `extension` declaration of the source set.
    Span<SemaSymbol *const> GetExtensionDeclarations() const
    {
        return _extensions.AsSpan();
    }
    /// Whether *type* conforms to *interfaceType*: by its declaration, as a built-in (§9.10) or through an extension.
    bool ConformsTo(const SemaType *type, const SemaType *interfaceType);
    /// The extensions that apply to *type*, their generic arguments inferred from it. The first time, checks that no
    /// interface is implemented twice and that the extensions implement the requirements of their interfaces.
    Span<SemaStructInstance *const> GetExtensions(const SemaType *type);
    /// The members named *name* that the extensions of *type* visible from *context* add, and those extensions in
    /// *owners*. Extensions outside the core library and with fewer generic parameters hide the others; among equally
    /// specific ones, only functions and operators may come from more than one, others are ambiguous (reported at
    /// *range*).
    void FindExtensionMembers(const SemaType *type, StringView name, const SemaContext &context, SourceRange range,
                              List<SemaSymbol *> &members, List<SemaStructInstance *> &owners);
    /// Checks that *self* implements every requirement of *interfaceInstance* (instance and static members,
    /// operators and associated types) with public members of *implementer*, the struct or class declaring the
    /// interface or null, of the type *self* names or of its extensions. Reports at *range*; fills *map* with the
    /// implementations of the interface's instance methods when given.
    bool CheckConformance(const SemaType *self, SemaStructInstance *implementer, SemaStructInstance *interfaceInstance,
                          SourceRange range, SemaInterfaceMap *map);
    /// The type the associated type *member* stands for: what a binding declared in *owner* gives, or for an
    /// interface's requirement, the binding of the implementing type *self*.
    const SemaType *ResolveAssociatedType(SemaSymbol *member, SemaStructInstance *owner, const SemaType *self,
                                          SourceRange range);
    /// The binding of the associated type *name* that *type* or one of its extensions declares, with the instance
    /// declaring it in *owner*; null if there is none.
    SemaSymbol *FindAssociatedBinding(const SemaType *type, StringView name, SemaStructInstance *&owner);

    // Attributes

    /// The storage format of a writable texture of *texel* without `[Format]`: the format with the same components.
    static MirImageFormat InferImageFormat(const SemaType *texel);
    /// Whether writable texture format *format* holds *texel* values: floats in float, unorm and snorm formats (§4.4).
    static bool FormatHoldsTexel(MirImageFormat format, const SemaType *texel);
    /// Checks the `Vertices`, `Indices` and `Primitives` outputs of a mesh entry point and takes its limits and
    /// topology from them (§9.2).
    bool CheckMeshOutputs(SemaFunction *entry, const AstAttribute &stageAttribute, MirEntryPoint &settings);
    static const AstAttribute *FindAttribute(Span<AstAttribute> attributes, StringView name);
    /// The constant value of positional argument *index* or named argument *name* of *attribute*.
    SemaConstant *EvaluateAttributeArgument(const AstAttribute &attribute, usize index, StringView name,
                                            const SemaContext &context);

    // User-declared attributes and materials (SemaMaterials.cpp)

    /// The attribute struct an application names, `Name` or `NameAttribute`; null for compiler-known attributes.
    SemaSymbol *FindDeclaredAttribute(const AstAttribute &attribute, const SemaContext &context);
    /// Checks the applications of user-declared attributes on *decl*, once (§3.7).
    bool CheckDeclaredAttributes(const AstDecl *decl, SemaAttributeTarget target, const SemaContext &context);
    /// The field values one application of the attribute *declaration* gives.
    bool EvaluateDeclaredAttribute(const AstAttribute &attribute, SemaSymbol *declaration, const SemaContext &context,
                                   SemaAttributeValue &value);
    /// Adds the user-declared attributes on *decl*, a script struct lowered to *type* or a function lowered to
    /// *function*, to *module* for the engine to read (MarlinScript §11.2; SemaPackages.cpp).
    void RecordScriptAttributes(const AstDecl *decl, const SemaContext &context, MirModule &module, const MirType *type,
                                MirFunction *function);
    /// Checks a `[Material]` struct: its fields are the material's parameters and its generic parameters with
    /// defaults its static switches (§9.4). Once per struct.
    bool CheckMaterial(SemaSymbol *symbol);
    /// The instance of the material named by the options, with the variant's values for its constant generic
    /// parameters; null after a diagnostic at *range*.
    const SemaType *GetMaterialType(SourceRange range);

    // Conversions and constants (SemaConversions.cpp, SemaEvaluate.cpp)

    SemaConversion ClassifyConversion(const SemaType *from, const SemaType *to, const SemaConstant *constant);
    SemaConstant *NewScalar(const SemaType *type, uint64 bits);
    SemaConstant *NewFloat(const SemaType *type, double value);
    SemaConstant *NewBool(bool value);
    SemaConstant *NewString(StringView text, const SemaType *type = nullptr);
    SemaConstant *NewZero(const SemaType *type);
    SemaConstant *NewComposite(const SemaType *type, Span<SemaConstant *const> elements);
    /// Converts a constant, or null if the conversion cannot be done at compile time.
    SemaConstant *ConvertConstant(SemaConstant *value, const SemaType *to);
    /// Whether *value* survives conversion to *to* unchanged (§2.4).
    bool FitsExactly(const SemaConstant *value, const SemaType *to);
    /// Evaluates *expression* if it is constant (§6.1), calling `const` functions; null otherwise.
    SemaConstant *Evaluate(BoundExpr *expression, bool report);
    /// Binds and evaluates an expression outside any function body, such as an array size or default value.
    SemaConstant *EvaluateConstantExpression(AstNode *node, const SemaContext &context, const SemaType *type);

    // Functions and entry points (Binder*.cpp, SemaEntryPoints.cpp)

    bool BindBody(SemaFunction *function);
    /// Resolves and checks the `[ForwardDerivative]` and `[BackwardDerivative]` of *function* (§9.12); false after a
    /// diagnostic.
    bool ResolveCustomDerivatives(SemaFunction *function);
    /// Checks a struct that declares IDifferentiable, *self*, at *range*: the compiler implements it for structs
    /// that are differentiable.
    bool CheckDeclaredDifferentiable(const SemaType *self, SourceRange range);
    /// Reports locals read before they are assigned, `out` parameters left unassigned, a missing `return` and, in
    /// scripts, possible null dereferences (§3.2, §5.1, MarlinScript §3.6).
    void CheckFlow(SemaFunction *function);
    bool HasFeature(StringView feature) const;

    // Built-ins

    /// The core library struct declaring the magic type family *id*; null if the core library lacks it.
    SemaSymbol *GetMagicSymbol(uint32 id) const;
    /// The magic family of *type* and the generic arguments its declaration is instantiated with, so its members
    /// can be found; None for other types.
    uint32 GetMagicFamily(const SemaType *type, List<SemaGenericArgument> &arguments);
    /// The size of *type* in memory by the layout rules (§10); 0 if it cannot be stored.
    uint32 SizeOf(const SemaType *type);

    /// The type a magic type family makes from *arguments*, or null after a diagnostic.
    const SemaType *InstantiateBuiltinType(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments,
                                           SourceRange range);

    template <typename T> T *New()
    {
        return Memory.New<T>();
    }

    template <typename T> Span<T> Freeze(const List<T> &list)
    {
        if (list.IsEmpty())
        {
            return {};
        }
        T *data = Memory.AllocateArray<T>(list.Count());
        for (usize i = 0; i < list.Count(); ++i)
        {
            ConstructAt(data + i, list[i]);
        }
        return {data, list.Count()};
    }

    /// The text of *range*.
    StringView TextOf(SourceRange range) const;

private:
    struct StructKey
    {
        SemaSymbol *Symbol;
        Span<const SemaGenericArgument> Arguments;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const StructKey &other) const;
    };

    struct FunctionKey
    {
        SemaSymbol *Symbol;
        SemaStructInstance *Owner;
        Span<const SemaGenericArgument> Arguments;
        SemaAccessor Accessor = SemaAccessor::None;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const FunctionKey &other) const;
    };

    struct UsingScope
    {
        const AstFile *File = nullptr;
        List<AstUsingDecl *> Declarations;
        List<SemaSymbol *> Namespaces;
        List<SemaSymbol *> StaticTypes;
        HashMap<StringView, AstUsingDecl *> Aliases;
    };

    void DeclareMembers(SemaSymbol *container, Span<AstNode *> members, const AstFile *file, uint32 package);
    void DeclareTypeMembers(SemaSymbol *type, Span<AstDecl *> members, const AstFile *file, uint32 package);
    /// Reports a member *container* cannot declare: a field or constructor of an extension, an associated type
    /// bound in an interface or left unbound elsewhere. False when *decl* is not declared.
    bool CheckMemberPlacement(const SemaSymbol *container, const AstDecl *decl);
    void AddMember(SemaSymbol *container, SemaSymbol *member);
    UsingScope &GetUsingScope(const void *key);
    void ResolveUsings();
    uint32 GetPackage(StringView path);
    void LookupIn(SemaSymbol *container, StringView name, SemaLookup &result, const SemaContext &context);
    void LookupUsings(const void *key, StringView name, SemaLookup &result, const SemaContext &context);

    ISourceProvider *_sources;
    List<UniquePtr<SourceFile>> _files;
    List<AstFile *> _trees;
    List<UniquePtr<SemaSymbol>> _symbols;
    List<UniquePtr<SemaStructInstance>> _structs;
    List<UniquePtr<SemaFunction>> _functions;
    List<UniquePtr<SemaGlobal>> _globals;
    HashMap<const AstDecl *, SemaSymbol *> _declarations;
    HashMap<const void *, UniquePtr<UsingScope>> _usings;
    HashMap<StructKey, SemaStructInstance *> _structInstances;
    HashMap<FunctionKey, SemaFunction *> _functionInstances;
    HashMap<const SemaSymbol *, SemaGlobal *> _globalsBySymbol;
    HashMap<StringView, uint32> _packages;
    HashSet<const AstFile *> _usedFiles;
    /// The names looked up since Load, each with whether a namespace was among what it found (NameDependencies.h).
    /// Lookups during Load only declare, so they are not dependencies.
    mutable HashMap<uint64, bool> _queriedNames;
    /// The hash of each container's qualified name, for _queriedNames.
    mutable HashMap<const SemaSymbol *, uint64> _containerNames;
    bool _recordNames = false;
    bool _queriedExtensions = false;
    SemaSymbol *_global = nullptr;
    SemaSymbol *_marlin = nullptr;
    /// Bit `1 << LanguageDialect` for each dialect folder of the core library in the source set.
    uint32 _coreFolders = 0;
    SemaSymbol *_objectSymbol = nullptr;
    struct StaticKey
    {
        const SemaSymbol *Field;
        const SemaStructInstance *Owner;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const StaticKey &other) const = default;
    };
    HashMap<StaticKey, SemaGlobal *> _staticFields;
    /// Every `extension` of the source set, in file order.
    List<SemaSymbol *> _extensions;
    /// The extensions that apply to each type asked about. A set is added before it is filled, so a type that comes
    /// up again while its extensions are matched sees those found so far.
    HashMap<const SemaType *, UniquePtr<List<SemaStructInstance *>>> _extensionSets;

    /// Whether the extension *extension* applies to *type*, with the generic arguments that make it so.
    bool MatchExtension(SemaSymbol *extension, const SemaType *type, List<SemaGenericArgument> &arguments);
    /// Reports interfaces *type* implements twice and checks the conformances its *extensions* declare.
    void CheckExtensions(const SemaType *type, Span<SemaStructInstance *const> extensions);
    /// The signature the accessor *accessor* of the interface member *requirement* requires, its types read in
    /// *context*, where `this` and the associated types stand for the implementing type.
    bool ResolveRequirement(SemaSymbol *requirement, SemaAccessor accessor, const SemaContext &context,
                            SemaSignature &signature);
    HashMap<uint32, SemaSymbol *> _magicSymbols;
    HashMap<const AstDecl *, bool> _checkedAttributes;
    HashMap<const SemaSymbol *, bool> _checkedMaterials;
    UniquePtr<MirModule> _layoutModule;
    UniquePtr<Lowering> _layoutLowering;
};
} // namespace Marlin
