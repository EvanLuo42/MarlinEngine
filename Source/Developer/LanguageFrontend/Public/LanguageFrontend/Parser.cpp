#include "LanguageFrontend/Parser.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformSync.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// The tokens of each file lexed so far, by path, with the hash of the text they came from. Every compilation of a
/// process declares the same library, so a file is lexed once until its text changes. Files with lexical errors are
/// lexed each time, so every compilation reports them.
class TokenCache
{
public:
    /// The tokens of *file*, in *arena*.
    Span<const Token> Tokenize(const SourceFile &file, Arena &arena, LanguageDiagnostics &diagnostics)
    {
        {
            const ScopeLock lock(_lock);
            if (const Entry *entry = _entries.Find(file.GetPath()); entry != nullptr && entry->Hash == file.GetHash())
            {
                return Store(entry->Tokens.AsSpan(), arena);
            }
        }
        const usize reported = diagnostics.GetAll().Count();
        List<Token> tokens;
        Marlin::Tokenize(file, diagnostics, tokens);
        if (diagnostics.GetAll().Count() == reported)
        {
            const ScopeLock lock(_lock);
            Entry &entry = _entries.FindOrAdd(String(file.GetPath()));
            entry.Hash = file.GetHash();
            entry.Tokens.Clear();
            entry.Tokens.AddRange(tokens.AsSpan());
        }
        return Store(tokens.AsSpan(), arena);
    }

private:
    struct Entry
    {
        Hash128 Hash;
        List<Token> Tokens;
    };

    static Span<const Token> Store(Span<const Token> tokens, Arena &arena)
    {
        Token *stored = arena.AllocateArray<Token>(tokens.Count());
        MemCopy(stored, tokens.Data(), tokens.Count() * sizeof(Token));
        return {stored, tokens.Count()};
    }

    Mutex _lock;
    HashMap<String, Entry> _entries;
};

TokenCache &GetTokenCache()
{
    static TokenCache cache;
    return cache;
}

class Parser
{
public:
    Parser(const SourceFile &file, Span<const Token> tokens, Arena &arena, LanguageDiagnostics &diagnostics,
           const ParseOptions &options)
        : _file(&file), _tokens(tokens), _arena(&arena), _diagnostics(&diagnostics), _options(options)
    {
    }

    Span<AstNode *> ParseFileMembers()
    {
        List<AstNode *> members;
        bool sawDeclaration = false;
        while (!At(TokenKind::EndOfFile))
        {
            const usize before = _pos;
            if (AstNode *member = ParseNamespaceMember(nullptr, !sawDeclaration))
            {
                sawDeclaration = sawDeclaration || member->Kind != AstKind::UsingDecl;
                AddMember(members, member);
            }
            if (_pos == before)
            {
                ++_pos;
            }
        }
        return Freeze(members);
    }

    void ParseBodyAt(AstBody &body)
    {
        _pos = body.BodyBegin;
        if (At(TokenKind::Arrow))
        {
            ++_pos;
            body.Expression = ParseExpression();
        }
        else
        {
            body.Block = ParseBlock();
        }
        body.Parsed = true;
    }

private:
    // Tokens

    [[nodiscard]] const Token &Current() const
    {
        return _tokens[_pos < _tokens.Count() ? _pos : _tokens.Count() - 1];
    }

    [[nodiscard]] const Token &PeekToken(usize offset) const
    {
        const usize index = _pos + offset;
        return _tokens[index < _tokens.Count() ? index : _tokens.Count() - 1];
    }

    [[nodiscard]] bool At(TokenKind kind) const
    {
        return Current().Kind == kind;
    }

    [[nodiscard]] bool AtContextual(StringView word) const
    {
        return Current().Kind == TokenKind::Identifier && !Current().Verbatim && Text(Current()) == word;
    }

    [[nodiscard]] bool AtContextualAt(usize offset, StringView word) const
    {
        const Token &token = PeekToken(offset);
        return token.Kind == TokenKind::Identifier && !token.Verbatim && Text(token) == word;
    }

    [[nodiscard]] StringView Text(const Token &token) const
    {
        return GetTokenText(*_file, token);
    }

    bool Accept(TokenKind kind)
    {
        if (At(kind))
        {
            ++_pos;
            return true;
        }
        return false;
    }

    bool Expect(TokenKind kind)
    {
        if (Accept(kind))
        {
            return true;
        }
        Error(DiagnosticCode::Expected, CurrentRange(), "expected {0}, found {1}", GetTokenKindName(kind),
              DescribeCurrent());
        return false;
    }

    StringView ExpectIdentifier(SourceRange *range = nullptr)
    {
        if (At(TokenKind::Identifier))
        {
            if (range != nullptr)
            {
                *range = CurrentRange();
            }
            return Text(_tokens[_pos++]);
        }
        Error(DiagnosticCode::Expected, CurrentRange(), "expected an identifier, found {0}", DescribeCurrent());
        return {};
    }

    [[nodiscard]] StringView DescribeCurrent() const
    {
        return At(TokenKind::Identifier) ? StringView("identifier") : GetTokenKindName(Current().Kind);
    }

    [[nodiscard]] SourceRange CurrentRange() const
    {
        return {_file->GetId(), Current().Begin, Current().End};
    }

    [[nodiscard]] SourceRange RangeFrom(usize beginToken) const
    {
        const uint32 begin = _tokens[beginToken < _tokens.Count() ? beginToken : _tokens.Count() - 1].Begin;
        const uint32 end = _pos > beginToken ? _tokens[_pos - 1].End : begin;
        return {_file->GetId(), begin, end};
    }

    template <typename... TArgs>
    void Error(DiagnosticCode code, SourceRange range, FormatString<TypeIdentity<TArgs>...> format, const TArgs &...args)
    {
        _failed = true;
        if (_speculating == 0)
        {
            _diagnostics->Error(code, range, format, args...);
        }
    }

    template <typename T> T *Make(usize beginToken)
    {
        T *node = _arena->New<T>();
        node->Kind = T::StaticKind;
        node->Range = {_file->GetId(), Current().Begin, Current().End};
        node->Range.Begin = _tokens[beginToken < _tokens.Count() ? beginToken : _tokens.Count() - 1].Begin;
        return node;
    }

    template <typename T> T *Finish(T *node, usize beginToken)
    {
        node->Range = RangeFrom(beginToken);
        return node;
    }

    template <typename T> Span<T> Freeze(const List<T> &list)
    {
        if (list.IsEmpty())
        {
            return {};
        }
        T *data = _arena->AllocateArray<T>(list.Count());
        for (usize i = 0; i < list.Count(); ++i)
        {
            ConstructAt(data + i, list[i]);
        }
        return {data, list.Count()};
    }

    /// Skips to just past the next `;` or to the next `}` at this nesting level.
    void Recover()
    {
        int32 depth = 0;
        while (!At(TokenKind::EndOfFile))
        {
            if (At(TokenKind::OpenBrace) || At(TokenKind::OpenParen) || At(TokenKind::OpenBracket))
            {
                ++depth;
            }
            else if (At(TokenKind::CloseBrace) || At(TokenKind::CloseParen) || At(TokenKind::CloseBracket))
            {
                if (depth == 0)
                {
                    return;
                }
                --depth;
                if (depth == 0 && At(TokenKind::CloseBrace))
                {
                    ++_pos;
                    return;
                }
            }
            else if (At(TokenKind::Semicolon) && depth == 0)
            {
                ++_pos;
                return;
            }
            ++_pos;
        }
    }

    // Declarations

    /// Adds *member* and the further declarators of a field declaration that ParseFields left behind.
    template <typename T> void AddMember(List<T *> &members, T *member)
    {
        members.Add(member);
        for (AstFieldDecl *field : _pendingFields)
        {
            members.Add(field);
        }
        _pendingFields.Clear();
    }

    AstNode *ParseNamespaceMember(AstDecl *parent, bool fileScopedAllowed)
    {
        const usize begin = _pos;
        if (At(TokenKind::KeywordUsing))
        {
            return ParseUsing(parent);
        }
        if (At(TokenKind::KeywordNamespace))
        {
            ++_pos;
            auto *ns = Make<AstNamespaceDecl>(begin);
            ns->Parent = parent;
            ns->Name = ParseDottedName(&ns->NameRange);
            if (Accept(TokenKind::Semicolon))
            {
                if (!fileScopedAllowed || parent != nullptr)
                {
                    Error(DiagnosticCode::FileScopedNamespaceNotFirst, ns->NameRange,
                          "a file-scoped namespace must come before every declaration of the file");
                }
                ns->FileScoped = true;
                List<AstNode *> members;
                while (!At(TokenKind::EndOfFile))
                {
                    const usize before = _pos;
                    if (AstNode *member = ParseNamespaceMember(ns, false))
                    {
                        AddMember(members, member);
                    }
                    if (_pos == before)
                    {
                        ++_pos;
                    }
                }
                ns->Members = Freeze(members);
                return Finish(ns, begin);
            }
            Expect(TokenKind::OpenBrace);
            List<AstNode *> members;
            while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
            {
                const usize before = _pos;
                if (AstNode *member = ParseNamespaceMember(ns, false))
                {
                    AddMember(members, member);
                }
                if (_pos == before)
                {
                    ++_pos;
                }
            }
            Expect(TokenKind::CloseBrace);
            ns->Members = Freeze(members);
            return Finish(ns, begin);
        }
        return ParseMemberDeclaration(parent, nullptr);
    }

    AstUsingDecl *ParseUsing(AstDecl *parent)
    {
        const usize begin = _pos++;
        auto *decl = Make<AstUsingDecl>(begin);
        decl->Parent = parent;
        decl->IsStatic = Accept(TokenKind::KeywordStatic);
        if (At(TokenKind::Identifier) && PeekToken(1).Is(TokenKind::Assign))
        {
            decl->Alias = ExpectIdentifier(&decl->NameRange);
            decl->Name = decl->Alias;
            ++_pos;
        }
        decl->Target = ParseType();
        if (!Expect(TokenKind::Semicolon))
        {
            Recover();
        }
        return Finish(decl, begin);
    }

    StringView ParseDottedName(SourceRange *range)
    {
        const usize begin = _pos;
        ExpectIdentifier();
        while (At(TokenKind::Dot) && PeekToken(1).Is(TokenKind::Identifier))
        {
            _pos += 2;
        }
        const SourceRange whole = RangeFrom(begin);
        if (range != nullptr)
        {
            *range = whole;
        }
        return _file->GetText().Substring(whole.Begin, whole.End - whole.Begin);
    }

    Span<AstAttribute> ParseAttributes()
    {
        List<AstAttribute> attributes;
        while (At(TokenKind::OpenBracket))
        {
            ++_pos;
            StringView target;
            if ((At(TokenKind::Identifier) || At(TokenKind::KeywordReturn)) && PeekToken(1).Is(TokenKind::Colon))
            {
                target = Text(Current());
                _pos += 2;
            }
            do
            {
                const usize begin = _pos;
                AstAttribute attribute;
                attribute.Target = target;
                attribute.Name = ParseDottedName(nullptr);
                if (Accept(TokenKind::OpenParen))
                {
                    attribute.HasArguments = true;
                    List<AstAttributeArgument> arguments;
                    while (!At(TokenKind::CloseParen) && !At(TokenKind::EndOfFile))
                    {
                        AstAttributeArgument argument;
                        if (At(TokenKind::Identifier) && PeekToken(1).Is(TokenKind::Assign))
                        {
                            argument.Name = Text(Current());
                            _pos += 2;
                        }
                        argument.Value = ParseExpression();
                        arguments.Add(argument);
                        if (!Accept(TokenKind::Comma))
                        {
                            break;
                        }
                    }
                    Expect(TokenKind::CloseParen);
                    attribute.Arguments = Freeze(arguments);
                }
                attribute.Range = RangeFrom(begin);
                attributes.Add(attribute);
            } while (Accept(TokenKind::Comma) && !At(TokenKind::CloseBracket));
            if (!Expect(TokenKind::CloseBracket))
            {
                Recover();
            }
        }
        return Freeze(attributes);
    }

    AstModifiers ParseModifiers()
    {
        AstModifiers modifiers = AstModifiers::None;
        while (true)
        {
            AstModifiers modifier = AstModifiers::None;
            switch (Current().Kind)
            {
            case TokenKind::KeywordPublic:
                modifier = AstModifiers::Public;
                break;
            case TokenKind::KeywordInternal:
                modifier = AstModifiers::Internal;
                break;
            case TokenKind::KeywordPrivate:
                modifier = AstModifiers::Private;
                break;
            case TokenKind::KeywordStatic:
                if (PeekToken(1).Is(TokenKind::KeywordIf))
                {
                    return modifiers;
                }
                modifier = AstModifiers::Static;
                break;
            case TokenKind::KeywordConst:
                modifier = AstModifiers::Const;
                break;
            case TokenKind::KeywordReadonly:
                modifier = AstModifiers::Readonly;
                break;
            case TokenKind::KeywordImplicit:
                modifier = AstModifiers::Implicit;
                break;
            case TokenKind::KeywordExplicit:
                modifier = AstModifiers::Explicit;
                break;
            case TokenKind::Identifier:
                if (!Current().Verbatim && (PeekToken(1).Is(TokenKind::Identifier) ||
                                            IsPredefinedTypeKeyword(PeekToken(1).Kind) ||
                                            PeekToken(1).Is(TokenKind::KeywordThis)))
                {
                    const StringView word = Text(Current());
                    modifier = word == "virtual"     ? AstModifiers::Virtual
                               : word == "override"  ? AstModifiers::Override
                               : word == "abstract"  ? AstModifiers::Abstract
                               : word == "async"     ? AstModifiers::Async
                               : word == "sealed"    ? AstModifiers::Sealed
                               : word == "protected" ? AstModifiers::Protected
                               : word == "event"     ? AstModifiers::Event
                                                     : AstModifiers::None;
                }
                break;
            default:
                break;
            }
            if (modifier == AstModifiers::None)
            {
                return modifiers;
            }
            if (HasAnyFlags(modifiers, modifier))
            {
                Error(DiagnosticCode::DuplicateModifier, CurrentRange(), "duplicate modifier");
            }
            modifiers |= modifier;
            ++_pos;
        }
    }

    template <typename T> T *StartDecl(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers, AstDecl *parent)
    {
        T *decl = Make<T>(begin);
        decl->Attributes = attributes;
        decl->Modifiers = modifiers;
        decl->Parent = parent;
        return decl;
    }

    /// A declaration inside a namespace, struct or interface; *owner* is the enclosing struct, whose name marks
    /// constructors.
    AstDecl *ParseMemberDeclaration(AstDecl *parent, const AstDecl *owner)
    {
        const usize begin = _pos;
        _failed = false;
        const Span<AstAttribute> attributes = ParseAttributes();
        const AstModifiers modifiers = ParseModifiers();
        AstDecl *decl = nullptr;
        if (At(TokenKind::KeywordUsing))
        {
            AstUsingDecl *alias = ParseUsing(parent);
            alias->Attributes = attributes;
            alias->Modifiers = modifiers;
            alias->Range = RangeFrom(begin);
            if (alias->Alias.IsEmpty())
            {
                Error(DiagnosticCode::InvalidModifier, alias->Range,
                      "only an alias `using Name = Type;` can have attributes or modifiers");
            }
            return alias;
        }
        if (At(TokenKind::KeywordStruct) || (AtContextual("class") && PeekToken(1).Is(TokenKind::Identifier)))
        {
            const bool isClass = !At(TokenKind::KeywordStruct);
            ++_pos;
            decl = ParseStruct(begin, attributes, isClass ? modifiers | AstModifiers::Class : modifiers, parent);
        }
        else if (At(TokenKind::KeywordInterface))
        {
            ++_pos;
            decl = ParseInterface(begin, attributes, modifiers, parent);
        }
        else if (At(TokenKind::KeywordEnum))
        {
            ++_pos;
            decl = ParseEnum(begin, attributes, modifiers, parent);
        }
        else if (owner == nullptr && AtContextual("extension") && LooksLikeExtension())
        {
            ++_pos;
            decl = ParseExtension(begin, attributes, modifiers, parent);
        }
        else if (owner != nullptr && AtContextual("type") && PeekToken(1).Is(TokenKind::Identifier) &&
                 (PeekToken(2).Is(TokenKind::Colon) || PeekToken(2).Is(TokenKind::Semicolon) ||
                  PeekToken(2).Is(TokenKind::Assign)))
        {
            ++_pos;
            auto *type = StartDecl<AstTypeDecl>(begin, attributes, modifiers, parent);
            type->Name = ExpectIdentifier(&type->NameRange);
            if (Accept(TokenKind::Colon))
            {
                List<AstNode *> bounds;
                do
                {
                    if (AstNode *bound = ParseType())
                    {
                        bounds.Add(bound);
                    }
                } while (Accept(TokenKind::Comma));
                type->Bounds = Freeze(bounds);
            }
            if (Accept(TokenKind::Assign))
            {
                type->Target = ParseType();
            }
            if (!Expect(TokenKind::Semicolon))
            {
                Recover();
            }
            decl = Finish(type, begin);
        }
        else if (AtContextual("delegate"))
        {
            ++_pos;
            auto *delegate = StartDecl<AstDelegateDecl>(begin, attributes, modifiers, parent);
            delegate->ReturnType = ParseType();
            delegate->Name = ExpectIdentifier(&delegate->NameRange);
            delegate->GenericParameters = ParseGenericParameters(delegate);
            delegate->Parameters = ParseParameters(delegate);
            delegate->Constraints = ParseConstraints();
            if (!Expect(TokenKind::Semicolon))
            {
                Recover();
            }
            decl = Finish(delegate, begin);
        }
        else if (At(TokenKind::KeywordOperator) &&
                 HasAnyFlags(modifiers, AstModifiers::Implicit | AstModifiers::Explicit))
        {
            ++_pos;
            auto *op = StartDecl<AstOperatorDecl>(begin, attributes, modifiers, parent);
            op->Operator = TokenKind::EndOfFile;
            op->ReturnType = ParseType();
            op->Name = "operator";
            op->Parameters = ParseParameters(op);
            ParseBodyOf(op->Body, true);
            decl = Finish(op, begin);
        }
        else if (owner != nullptr && At(TokenKind::Identifier) && Text(Current()) == owner->Name &&
                 PeekToken(1).Is(TokenKind::OpenParen))
        {
            auto *constructor = StartDecl<AstConstructorDecl>(begin, attributes, modifiers, parent);
            constructor->Name = ExpectIdentifier(&constructor->NameRange);
            constructor->Parameters = ParseParameters(constructor);
            if (At(TokenKind::Colon) && (AtContextualAt(1, "base") || PeekToken(1).Is(TokenKind::KeywordThis)))
            {
                ++_pos;
                const usize initializerBegin = _pos;
                constructor->HasInitializer = true;
                constructor->InitializerIsBase = !At(TokenKind::KeywordThis);
                ++_pos;
                Expect(TokenKind::OpenParen);
                constructor->InitializerArguments = ParseArguments(TokenKind::CloseParen);
                constructor->InitializerRange = RangeFrom(initializerBegin);
            }
            ParseBodyOf(constructor->Body, true);
            decl = Finish(constructor, begin);
        }
        else
        {
            AstNode *type = ParseType();
            if (type == nullptr)
            {
                Recover();
                return nullptr;
            }
            if (At(TokenKind::KeywordThis) && PeekToken(1).Is(TokenKind::OpenBracket))
            {
                auto *indexer = StartDecl<AstPropertyDecl>(begin, attributes, modifiers, parent);
                indexer->Type = type;
                indexer->Name = "this";
                indexer->NameRange = CurrentRange();
                indexer->IsIndexer = true;
                ++_pos;
                indexer->Parameters = ParseParameters(indexer, TokenKind::OpenBracket, TokenKind::CloseBracket);
                ParseProperty(*indexer);
                decl = Finish(indexer, begin);
            }
            else if (Accept(TokenKind::KeywordOperator))
            {
                auto *op = StartDecl<AstOperatorDecl>(begin, attributes, modifiers, parent);
                op->ReturnType = type;
                op->Operator = ParseOverloadableOperator();
                op->Name = "operator";
                op->Parameters = ParseParameters(op);
                ParseBodyOf(op->Body, true);
                decl = Finish(op, begin);
            }
            else
            {
                SourceRange nameRange;
                const StringView name = ExpectIdentifier(&nameRange);
                if (At(TokenKind::OpenParen) || At(TokenKind::Less))
                {
                    decl = ParseFunction(begin, attributes, modifiers, parent, type, name, nameRange);
                }
                else if (At(TokenKind::OpenBrace) || At(TokenKind::Arrow))
                {
                    auto *property = StartDecl<AstPropertyDecl>(begin, attributes, modifiers, parent);
                    property->Type = type;
                    property->Name = name;
                    property->NameRange = nameRange;
                    ParseProperty(*property);
                    decl = Finish(property, begin);
                }
                else
                {
                    decl = ParseFields(begin, attributes, modifiers, parent, type, name, nameRange);
                }
            }
        }
        if (_failed && decl == nullptr)
        {
            Recover();
        }
        return decl;
    }

    TokenKind ParseOverloadableOperator()
    {
        const TokenKind kind = Current().Kind;
        switch (kind)
        {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
        case TokenKind::Caret:
        case TokenKind::Tilde:
        case TokenKind::Exclamation:
        case TokenKind::Equal:
        case TokenKind::NotEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:
        case TokenKind::LessLess:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
            ++_pos;
            return kind;
        case TokenKind::Greater:
            ++_pos;
            if (At(TokenKind::Greater) && Current().Begin == _tokens[_pos - 1].End)
            {
                ++_pos;
                return TokenKind::GreaterGreater;
            }
            return kind;
        default:
            Error(DiagnosticCode::Expected, CurrentRange(), "expected an overloadable operator, found {0}",
                  DescribeCurrent());
            return TokenKind::Plus;
        }
    }

    Span<AstGenericParameterDecl *> ParseGenericParameters(AstDecl *parent)
    {
        List<AstGenericParameterDecl *> parameters;
        if (!Accept(TokenKind::Less))
        {
            return {};
        }
        bool sawDefault = false;
        do
        {
            const usize begin = _pos;
            const Span<AstAttribute> attributes = ParseAttributes();
            auto *parameter = StartDecl<AstGenericParameterDecl>(begin, attributes, AstModifiers::None, parent);
            if (At(TokenKind::Identifier) &&
                (PeekToken(1).Is(TokenKind::Comma) || PeekToken(1).Is(TokenKind::Greater) ||
                 PeekToken(1).Is(TokenKind::Assign)))
            {
                parameter->Name = ExpectIdentifier(&parameter->NameRange);
            }
            else
            {
                parameter->ValueType = ParseType();
                parameter->Name = ExpectIdentifier(&parameter->NameRange);
            }
            if (Accept(TokenKind::Assign))
            {
                parameter->Default = ParseShift();
                sawDefault = true;
            }
            else if (sawDefault)
            {
                Error(DiagnosticCode::InvalidGenericArguments, parameter->NameRange,
                      "a generic parameter after one with a default value needs a default value too");
            }
            parameters.Add(Finish(parameter, begin));
        } while (Accept(TokenKind::Comma));
        Expect(TokenKind::Greater);
        return Freeze(parameters);
    }

    Span<AstConstraint> ParseConstraints()
    {
        List<AstConstraint> constraints;
        while (Accept(TokenKind::KeywordWhere))
        {
            const usize begin = _pos;
            AstConstraint constraint;
            constraint.Parameter = ExpectIdentifier();
            Expect(TokenKind::Colon);
            List<AstNode *> bounds;
            do
            {
                if (Accept(TokenKind::KeywordStruct))
                {
                    constraint.Struct = true;
                }
                else if (AtContextual("unmanaged"))
                {
                    ++_pos;
                    constraint.Unmanaged = true;
                }
                else if (AtContextual("class") && !PeekToken(1).Is(TokenKind::Dot))
                {
                    ++_pos;
                    constraint.Class = true;
                }
                else if (At(TokenKind::KeywordNew) && PeekToken(1).Is(TokenKind::OpenParen))
                {
                    _pos += 2;
                    Expect(TokenKind::CloseParen);
                    constraint.New = true;
                }
                else if (AstNode *bound = ParseType())
                {
                    bounds.Add(bound);
                }
            } while (Accept(TokenKind::Comma));
            constraint.Bounds = Freeze(bounds);
            constraint.Range = RangeFrom(begin);
            constraints.Add(constraint);
        }
        return Freeze(constraints);
    }

    Span<AstParameterDecl *> ParseParameters(AstDecl *parent, TokenKind open = TokenKind::OpenParen,
                                             TokenKind close = TokenKind::CloseParen)
    {
        List<AstParameterDecl *> parameters;
        Expect(open);
        while (!At(close) && !At(TokenKind::EndOfFile))
        {
            const usize begin = _pos;
            const Span<AstAttribute> attributes = ParseAttributes();
            AstModifiers modifiers = AstModifiers::None;
            while (true)
            {
                // `this a` is a parameter of the implementing type (§3.5); `this T a` an extension method's receiver.
                const bool selfType = At(TokenKind::KeywordThis) && PeekToken(1).Is(TokenKind::Identifier) &&
                                      (PeekToken(2).Is(TokenKind::Comma) || PeekToken(2).Is(close) ||
                                       PeekToken(2).Is(TokenKind::Assign));
                const AstModifiers modifier = At(TokenKind::KeywordIn)                  ? AstModifiers::In
                                              : At(TokenKind::KeywordOut)               ? AstModifiers::Out
                                              : At(TokenKind::KeywordRef)               ? AstModifiers::Ref
                                              : At(TokenKind::KeywordThis) && !selfType ? AstModifiers::This
                                                                                        : AstModifiers::None;
                if (modifier == AstModifiers::None)
                {
                    break;
                }
                if (HasAnyFlags(modifiers, modifier))
                {
                    Error(DiagnosticCode::DuplicateModifier, CurrentRange(), "duplicate modifier");
                }
                modifiers |= modifier;
                ++_pos;
            }
            auto *parameter = StartDecl<AstParameterDecl>(begin, attributes, modifiers, parent);
            parameter->Type = ParseType();
            parameter->Name = ExpectIdentifier(&parameter->NameRange);
            if (Accept(TokenKind::Assign))
            {
                parameter->Default = ParseExpression();
            }
            parameters.Add(Finish(parameter, begin));
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
        }
        if (!Expect(close))
        {
            while (!At(close) && !At(TokenKind::OpenBrace) && !At(TokenKind::EndOfFile))
            {
                ++_pos;
            }
            Accept(close);
        }
        return Freeze(parameters);
    }

    /// A block, `=> expr;` or, where *optional*, `;` for a declaration without a body: an interface member or a
    /// built-in of the prelude. The checker decides where that is allowed.
    void ParseBodyOf(AstBody &body, bool optional)
    {
        if (optional && Accept(TokenKind::Semicolon))
        {
            return;
        }
        body.BodyBegin = static_cast<uint32>(_pos);
        if (At(TokenKind::Arrow))
        {
            if (_options.SkipBodies)
            {
                SkipPast(TokenKind::Semicolon);
                body.BodyEnd = static_cast<uint32>(_pos);
                return;
            }
            ++_pos;
            body.Expression = ParseExpression();
            body.BodyEnd = static_cast<uint32>(_pos);
            body.Parsed = true;
            Expect(TokenKind::Semicolon);
            return;
        }
        if (!At(TokenKind::OpenBrace))
        {
            Error(DiagnosticCode::Expected, CurrentRange(), "expected a body, found {0}", DescribeCurrent());
            Recover();
            body.BodyBegin = 0;
            return;
        }
        if (_options.SkipBodies)
        {
            SkipBalanced();
            body.BodyEnd = static_cast<uint32>(_pos);
            return;
        }
        body.Block = ParseBlock();
        body.BodyEnd = static_cast<uint32>(_pos);
        body.Parsed = true;
    }

    void SkipBalanced()
    {
        int32 depth = 0;
        do
        {
            if (At(TokenKind::OpenBrace))
            {
                ++depth;
            }
            else if (At(TokenKind::CloseBrace))
            {
                --depth;
            }
            ++_pos;
        } while (depth > 0 && !At(TokenKind::EndOfFile));
    }

    void SkipPast(TokenKind kind)
    {
        int32 depth = 0;
        while (!At(TokenKind::EndOfFile))
        {
            if (At(TokenKind::OpenBrace) || At(TokenKind::OpenParen) || At(TokenKind::OpenBracket))
            {
                ++depth;
            }
            else if (At(TokenKind::CloseBrace) || At(TokenKind::CloseParen) || At(TokenKind::CloseBracket))
            {
                --depth;
            }
            else if (depth == 0 && At(kind))
            {
                ++_pos;
                return;
            }
            ++_pos;
        }
    }

    AstFunctionDecl *ParseFunction(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers,
                                   AstDecl *parent, AstNode *returnType, StringView name, SourceRange nameRange)
    {
        auto *function = StartDecl<AstFunctionDecl>(begin, attributes, modifiers, parent);
        function->ReturnType = returnType;
        function->Name = name;
        function->NameRange = nameRange;
        function->GenericParameters = ParseGenericParameters(function);
        function->Parameters = ParseParameters(function);
        function->Constraints = ParseConstraints();
        ParseBodyOf(function->Body, true);
        return Finish(function, begin);
    }

    /// `=> expr;`, or `{ get ...; set ...; }` with `init` in place of `set` and an optional `= value;` after the
    /// braces. An accessor written `get;` has no body, which makes an auto-property.
    void ParseProperty(AstPropertyDecl &property)
    {
        if (At(TokenKind::Arrow))
        {
            property.HasGetter = true;
            ParseBodyOf(property.Getter, false);
            return;
        }
        Expect(TokenKind::OpenBrace);
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize begin = _pos;
            const AstModifiers modifiers = ParseAccessorModifiers();
            if (AtContextual("get"))
            {
                ++_pos;
                property.HasGetter = true;
                ParseBodyOf(property.Getter, true);
            }
            else if (AtContextual("set") || AtContextual("init"))
            {
                property.SetterIsInit = AtContextual("init");
                ++_pos;
                property.Setter.Present = true;
                property.Setter.Modifiers = modifiers;
                ParseBodyOf(property.Setter.Body, true);
                property.Setter.Range = RangeFrom(begin);
            }
            else
            {
                Error(DiagnosticCode::Expected, CurrentRange(), "expected 'get', 'set' or 'init', found {0}",
                      DescribeCurrent());
                break;
            }
        }
        if (!Expect(TokenKind::CloseBrace))
        {
            Recover();
            return;
        }
        if (Accept(TokenKind::Assign))
        {
            property.Initializer = ParseExpression();
            Expect(TokenKind::Semicolon);
        }
    }

    /// The visibility written before an accessor: `private set`.
    AstModifiers ParseAccessorModifiers()
    {
        AstModifiers modifiers = AstModifiers::None;
        while (true)
        {
            const AstModifiers modifier = At(TokenKind::KeywordPublic)     ? AstModifiers::Public
                                          : At(TokenKind::KeywordInternal) ? AstModifiers::Internal
                                          : At(TokenKind::KeywordPrivate)  ? AstModifiers::Private
                                          : AtContextual("protected")      ? AstModifiers::Protected
                                                                           : AstModifiers::None;
            if (modifier == AstModifiers::None)
            {
                return modifiers;
            }
            if (HasAnyFlags(modifiers, modifier))
            {
                Error(DiagnosticCode::DuplicateModifier, CurrentRange(), "duplicate modifier");
            }
            modifiers |= modifier;
            ++_pos;
        }
    }

    AstDecl *ParseFields(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers, AstDecl *parent,
                         AstNode *type, StringView name, SourceRange nameRange)
    {
        AstFieldDecl *first = nullptr;
        while (true)
        {
            auto *field = StartDecl<AstFieldDecl>(begin, attributes, modifiers, parent);
            field->Type = type;
            field->Name = name;
            field->NameRange = nameRange;
            if (Accept(TokenKind::Assign))
            {
                field->Initializer = ParseExpression();
            }
            Finish(field, begin);
            if (first == nullptr)
            {
                first = field;
            }
            else
            {
                _pendingFields.Add(field);
            }
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
            name = ExpectIdentifier(&nameRange);
        }
        if (!Expect(TokenKind::Semicolon))
        {
            Recover();
        }
        return first;
    }

    AstStructDecl *ParseStruct(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers, AstDecl *parent)
    {
        auto *decl = StartDecl<AstStructDecl>(begin, attributes, modifiers, parent);
        decl->Name = ExpectIdentifier(&decl->NameRange);
        decl->GenericParameters = ParseGenericParameters(decl);
        if (Accept(TokenKind::Colon))
        {
            List<AstNode *> interfaces;
            do
            {
                if (AstNode *type = ParseType())
                {
                    interfaces.Add(type);
                }
            } while (Accept(TokenKind::Comma));
            decl->Interfaces = Freeze(interfaces);
        }
        decl->Constraints = ParseConstraints();
        decl->Members = ParseTypeMembers(decl);
        return Finish(decl, begin);
    }

    AstInterfaceDecl *ParseInterface(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers,
                                     AstDecl *parent)
    {
        auto *decl = StartDecl<AstInterfaceDecl>(begin, attributes, modifiers, parent);
        decl->Name = ExpectIdentifier(&decl->NameRange);
        decl->GenericParameters = ParseGenericParameters(decl);
        if (Accept(TokenKind::Colon))
        {
            List<AstNode *> bases;
            do
            {
                if (AstNode *type = ParseType())
                {
                    bases.Add(type);
                }
            } while (Accept(TokenKind::Comma));
            decl->Bases = Freeze(bases);
        }
        decl->Constraints = ParseConstraints();
        decl->Members = ParseTypeMembers(decl);
        return Finish(decl, begin);
    }

    /// Whether the contextual `extension` at the cursor starts an extension: generic parameters, or a type followed
    /// by `:`, `where` or the body, rather than a member whose type is named `extension`.
    bool LooksLikeExtension()
    {
        if (PeekToken(1).Is(TokenKind::Less))
        {
            return true;
        }
        const usize save = _pos;
        const bool failed = _failed;
        ++_speculating;
        _failed = false;
        ++_pos;
        const bool result = ParseType() != nullptr && !_failed &&
                            (At(TokenKind::Colon) || At(TokenKind::KeywordWhere) || At(TokenKind::OpenBrace));
        --_speculating;
        _failed = failed;
        _pos = save;
        return result;
    }

    AstExtensionDecl *ParseExtension(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers,
                                     AstDecl *parent)
    {
        auto *decl = StartDecl<AstExtensionDecl>(begin, attributes, modifiers, parent);
        decl->GenericParameters = ParseGenericParameters(decl);
        const usize typeBegin = _pos;
        decl->ExtendedType = ParseType();
        decl->NameRange = RangeFrom(typeBegin);
        decl->Name = _file->GetText().Substring(decl->NameRange.Begin, decl->NameRange.End - decl->NameRange.Begin);
        if (Accept(TokenKind::Colon))
        {
            List<AstNode *> interfaces;
            do
            {
                if (AstNode *type = ParseType())
                {
                    interfaces.Add(type);
                }
            } while (Accept(TokenKind::Comma));
            decl->Interfaces = Freeze(interfaces);
        }
        decl->Constraints = ParseConstraints();
        decl->Members = ParseTypeMembers(decl);
        return Finish(decl, begin);
    }

    Span<AstDecl *> ParseTypeMembers(AstDecl *owner)
    {
        List<AstDecl *> members;
        if (!Expect(TokenKind::OpenBrace))
        {
            Recover();
            return {};
        }
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize before = _pos;
            if (AstDecl *member = ParseMemberDeclaration(owner, owner))
            {
                AddMember(members, member);
            }
            _pendingFields.Clear();
            if (_pos == before)
            {
                ++_pos;
            }
        }
        Expect(TokenKind::CloseBrace);
        Accept(TokenKind::Semicolon);
        return Freeze(members);
    }

    AstEnumDecl *ParseEnum(usize begin, Span<AstAttribute> attributes, AstModifiers modifiers, AstDecl *parent)
    {
        auto *decl = StartDecl<AstEnumDecl>(begin, attributes, modifiers, parent);
        decl->Name = ExpectIdentifier(&decl->NameRange);
        if (Accept(TokenKind::Colon))
        {
            decl->UnderlyingType = ParseType();
        }
        List<AstEnumMemberDecl *> members;
        Expect(TokenKind::OpenBrace);
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize memberBegin = _pos;
            const Span<AstAttribute> memberAttributes = ParseAttributes();
            auto *member = StartDecl<AstEnumMemberDecl>(memberBegin, memberAttributes, AstModifiers::None, decl);
            member->Name = ExpectIdentifier(&member->NameRange);
            if (Accept(TokenKind::Assign))
            {
                member->Value = ParseExpression();
            }
            members.Add(Finish(member, memberBegin));
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
        }
        if (!Expect(TokenKind::CloseBrace))
        {
            Recover();
        }
        Accept(TokenKind::Semicolon);
        decl->Members = Freeze(members);
        return Finish(decl, begin);
    }

    // Types

    /// A type with its `[n]` and `[]` array suffixes, and `?` for a nullable type where *allowNullable*: a type in a
    /// pattern takes no `?`, so that `x is T ? a : b` stays a conditional.
    AstNode *ParseType(bool allowNullable = true)
    {
        const usize begin = _pos;
        AstNode *type = ParseNonArrayType();
        while (type != nullptr)
        {
            if (At(TokenKind::OpenBracket))
            {
                ++_pos;
                auto *array = Make<AstArrayType>(begin);
                array->Element = type;
                if (!At(TokenKind::CloseBracket))
                {
                    array->Size = ParseExpression();
                }
                Expect(TokenKind::CloseBracket);
                type = Finish(array, begin);
            }
            else if (allowNullable && At(TokenKind::Question))
            {
                ++_pos;
                auto *nullable = Make<AstNullableType>(begin);
                nullable->Element = type;
                type = Finish(nullable, begin);
            }
            else
            {
                break;
            }
        }
        return type;
    }

    /// A keyword type or a possibly qualified named type, without array suffixes: the type of `new T[n]`.
    AstNode *ParseNonArrayType()
    {
        const usize begin = _pos;
        if (IsPredefinedTypeKeyword(Current().Kind))
        {
            auto *predefined = Make<AstPredefinedType>(begin);
            predefined->Keyword = Current().Kind;
            ++_pos;
            return Finish(predefined, begin);
        }
        if (At(TokenKind::KeywordThis))
        {
            // Where a type is expected, `this` is the implementing or extended type (§3.5).
            auto *self = Make<AstSelfType>(begin);
            ++_pos;
            return Finish(self, begin);
        }
        if (!At(TokenKind::Identifier))
        {
            Error(DiagnosticCode::Expected, CurrentRange(), "expected a type, found {0}", DescribeCurrent());
            return nullptr;
        }
        AstNode *type = ParseNamedType(nullptr, begin);
        while (type != nullptr && At(TokenKind::Dot) && PeekToken(1).Is(TokenKind::Identifier))
        {
            ++_pos;
            type = ParseNamedType(type, begin);
        }
        return type;
    }

    AstNamedType *ParseNamedType(AstNode *qualifier, usize begin)
    {
        auto *named = Make<AstNamedType>(begin);
        named->Qualifier = qualifier;
        named->Name = ExpectIdentifier();
        if (At(TokenKind::Less))
        {
            named->HasGenericArguments = true;
            named->GenericArguments = ParseGenericArguments();
        }
        return Finish(named, begin);
    }

    /// `<A, 3, B[2]>`; arguments that start like an expression are constant expressions, the rest are types.
    Span<AstNode *> ParseGenericArguments()
    {
        List<AstNode *> arguments;
        Expect(TokenKind::Less);
        do
        {
            AstNode *argument = nullptr;
            if (At(TokenKind::IntegerLiteral) || At(TokenKind::FloatLiteral) || At(TokenKind::KeywordTrue) ||
                At(TokenKind::KeywordFalse) || At(TokenKind::OpenParen) || At(TokenKind::Minus) ||
                At(TokenKind::KeywordSizeof) || At(TokenKind::KeywordNameof))
            {
                argument = ParseShift();
            }
            else
            {
                argument = ParseType();
            }
            if (argument == nullptr)
            {
                break;
            }
            arguments.Add(argument);
        } while (Accept(TokenKind::Comma));
        if (At(TokenKind::GreaterEqual) || !Expect(TokenKind::Greater))
        {
            _failed = true;
        }
        return Freeze(arguments);
    }

    /// Whether `<` at the cursor starts generic arguments: they must parse and be followed by a token that cannot
    /// continue a comparison (C# §12.8.9.2).
    bool LooksLikeGenericArguments()
    {
        const usize save = _pos;
        const bool failed = _failed;
        ++_speculating;
        _failed = false;
        (void)ParseGenericArguments();
        bool result = !_failed;
        if (result)
        {
            switch (Current().Kind)
            {
            case TokenKind::OpenParen:
            case TokenKind::CloseParen:
            case TokenKind::CloseBracket:
            case TokenKind::CloseBrace:
            case TokenKind::Colon:
            case TokenKind::Semicolon:
            case TokenKind::Comma:
            case TokenKind::Dot:
            case TokenKind::Question:
            case TokenKind::QuestionQuestion:
            case TokenKind::QuestionDot:
            case TokenKind::Equal:
            case TokenKind::NotEqual:
            case TokenKind::Pipe:
            case TokenKind::Caret:
            case TokenKind::AmpersandAmpersand:
            case TokenKind::PipePipe:
            case TokenKind::Ampersand:
            case TokenKind::OpenBracket:
            case TokenKind::EndOfFile:
                break;
            case TokenKind::Greater:
                break;
            default:
                result = false;
                break;
            }
        }
        --_speculating;
        _failed = failed;
        _pos = save;
        return result;
    }

    /// Parses a type if one is at the cursor and is followed by something *accept* takes; restores the cursor
    /// otherwise.
    template <typename TAccept> AstNode *TryParseType(TAccept accept, bool allowNullable = true)
    {
        const usize save = _pos;
        const bool failed = _failed;
        ++_speculating;
        _failed = false;
        // `this x` declares a local of the implementing or extended type; `this` alone stays the current instance.
        const bool selfType = At(TokenKind::KeywordThis) && PeekToken(1).Is(TokenKind::Identifier);
        AstNode *type = (At(TokenKind::Identifier) || IsPredefinedTypeKeyword(Current().Kind) || selfType)
                            ? ParseType(allowNullable)
                            : nullptr;
        const bool ok = type != nullptr && !_failed && accept();
        --_speculating;
        _failed = failed;
        if (!ok)
        {
            _pos = save;
            return nullptr;
        }
        return type;
    }

    // Statements

    AstBlockStmt *ParseBlock()
    {
        const usize begin = _pos;
        auto *block = Make<AstBlockStmt>(begin);
        Expect(TokenKind::OpenBrace);
        List<AstNode *> statements;
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize before = _pos;
            if (AstNode *statement = ParseStatement())
            {
                statements.Add(statement);
            }
            if (_pos == before)
            {
                ++_pos;
            }
        }
        Expect(TokenKind::CloseBrace);
        block->Statements = Freeze(statements);
        return Finish(block, begin);
    }

    AstNode *ParseEmbeddedStatement()
    {
        AstNode *statement = ParseStatement();
        if (statement == nullptr)
        {
            auto *empty = Make<AstEmptyStmt>(_pos);
            return Finish(empty, _pos);
        }
        return statement;
    }

    AstNode *ParseStatement()
    {
        _failed = false;
        const usize begin = _pos;
        AstNode *statement = ParseStatementInner();
        if (_failed)
        {
            if (_pos == begin || !(_tokens[_pos - 1].Is(TokenKind::Semicolon) || _tokens[_pos - 1].Is(TokenKind::CloseBrace)))
            {
                Recover();
            }
        }
        return statement;
    }

    AstNode *ParseStatementInner()
    {
        if (At(TokenKind::OpenBracket))
        {
            const Span<AstAttribute> attributes = ParseAttributes();
            AstNode *statement = ParseStatementInner();
            if (statement == nullptr)
            {
                return nullptr;
            }
            switch (statement->Kind)
            {
            case AstKind::VarStmt:
                statement->As<AstVarStmt>()->Attributes = attributes;
                break;
            case AstKind::ForStmt:
                statement->As<AstForStmt>()->Attributes = attributes;
                break;
            case AstKind::ForeachStmt:
                statement->As<AstForeachStmt>()->Attributes = attributes;
                break;
            case AstKind::WhileStmt:
                statement->As<AstWhileStmt>()->Attributes = attributes;
                break;
            case AstKind::DoStmt:
                statement->As<AstDoStmt>()->Attributes = attributes;
                break;
            default:
                if (!attributes.IsEmpty())
                {
                    SourceRange range = attributes[0].Range;
                    range.End = attributes[attributes.Count() - 1].Range.End;
                    Error(DiagnosticCode::Expected, range, "attributes apply only to local declarations and loops");
                }
                break;
            }
            return statement;
        }
        const usize begin = _pos;
        switch (Current().Kind)
        {
        case TokenKind::OpenBrace:
            return ParseBlock();
        case TokenKind::Semicolon:
            ++_pos;
            return Finish(Make<AstEmptyStmt>(begin), begin);
        case TokenKind::KeywordStatic:
            if (PeekToken(1).Is(TokenKind::KeywordIf))
            {
                ++_pos;
                return ParseIf(begin, true);
            }
            break;
        case TokenKind::KeywordIf:
            return ParseIf(begin, false);
        case TokenKind::KeywordWhile:
        {
            ++_pos;
            auto *statement = Make<AstWhileStmt>(begin);
            Expect(TokenKind::OpenParen);
            statement->Condition = ParseExpression();
            Expect(TokenKind::CloseParen);
            statement->Body = ParseEmbeddedStatement();
            return Finish(statement, begin);
        }
        case TokenKind::KeywordDo:
        {
            ++_pos;
            auto *statement = Make<AstDoStmt>(begin);
            statement->Body = ParseEmbeddedStatement();
            Expect(TokenKind::KeywordWhile);
            Expect(TokenKind::OpenParen);
            statement->Condition = ParseExpression();
            Expect(TokenKind::CloseParen);
            Expect(TokenKind::Semicolon);
            return Finish(statement, begin);
        }
        case TokenKind::KeywordFor:
            return ParseFor(begin);
        case TokenKind::KeywordForeach:
            return ParseForeach(begin);
        case TokenKind::KeywordSwitch:
            return ParseSwitch(begin);
        case TokenKind::KeywordBreak:
            ++_pos;
            Expect(TokenKind::Semicolon);
            return Finish(Make<AstBreakStmt>(begin), begin);
        case TokenKind::KeywordContinue:
            ++_pos;
            Expect(TokenKind::Semicolon);
            return Finish(Make<AstContinueStmt>(begin), begin);
        case TokenKind::KeywordReturn:
        {
            ++_pos;
            auto *statement = Make<AstReturnStmt>(begin);
            if (!At(TokenKind::Semicolon))
            {
                statement->Value = ParseExpression();
            }
            Expect(TokenKind::Semicolon);
            return Finish(statement, begin);
        }
        case TokenKind::KeywordConst:
        {
            ++_pos;
            AstVarStmt *statement = ParseVarDeclarators(begin, ParseType());
            statement->IsConst = true;
            Expect(TokenKind::Semicolon);
            return statement;
        }
        case TokenKind::KeywordRef:
        {
            ++_pos;
            AstVarStmt *statement = TryParseLocalDeclaration(begin);
            if (statement == nullptr)
            {
                Error(DiagnosticCode::Expected, CurrentRange(), "expected a `ref` local declaration, found {0}",
                      DescribeCurrent());
                return nullptr;
            }
            statement->IsRef = true;
            Expect(TokenKind::Semicolon);
            return statement;
        }
        default:
            break;
        }
        if (AtContextual("yield") &&
            (PeekToken(1).Is(TokenKind::KeywordReturn) || PeekToken(1).Is(TokenKind::KeywordBreak)))
        {
            auto *statement = Make<AstYieldStmt>(begin);
            statement->IsBreak = PeekToken(1).Is(TokenKind::KeywordBreak);
            _pos += 2;
            if (!statement->IsBreak)
            {
                statement->Value = ParseExpression();
            }
            Expect(TokenKind::Semicolon);
            return Finish(statement, begin);
        }
        if (AtContextual("discard") && PeekToken(1).Is(TokenKind::Semicolon))
        {
            _pos += 2;
            return Finish(Make<AstDiscardStmt>(begin), begin);
        }
        if (AstVarStmt *declaration = TryParseLocalDeclaration(begin))
        {
            Expect(TokenKind::Semicolon);
            return declaration;
        }
        auto *statement = Make<AstExprStmt>(begin);
        statement->Expression = ParseExpression();
        if (statement->Expression == nullptr)
        {
            return nullptr;
        }
        Expect(TokenKind::Semicolon);
        return Finish(statement, begin);
    }

    /// `T name ...` or `var name ...`, without the closing `;`. `await x;` is an expression even though it looks
    /// like a declaration of `x` with the type `await`.
    AstVarStmt *TryParseLocalDeclaration(usize begin)
    {
        if (AtContextual("var") && PeekToken(1).Is(TokenKind::Identifier))
        {
            ++_pos;
            return ParseVarDeclarators(begin, nullptr);
        }
        if (AtContextual("await") && StartsOperand(PeekToken(1)))
        {
            return nullptr;
        }
        AstNode *type = TryParseType([this] { return At(TokenKind::Identifier); });
        if (type == nullptr)
        {
            return nullptr;
        }
        return ParseVarDeclarators(begin, type);
    }

    AstVarStmt *ParseVarDeclarators(usize begin, AstNode *type)
    {
        auto *statement = Make<AstVarStmt>(begin);
        statement->Type = type;
        List<AstVariableDeclarator> declarators;
        do
        {
            const usize declaratorBegin = _pos;
            AstVariableDeclarator declarator;
            declarator.Name = ExpectIdentifier();
            if (Accept(TokenKind::Assign))
            {
                if (At(TokenKind::KeywordRef))
                {
                    const usize refBegin = _pos++;
                    auto *reference = Make<AstRefExpr>(refBegin);
                    reference->Operand = ParseExpression();
                    declarator.Initializer = Finish(reference, refBegin);
                }
                else
                {
                    declarator.Initializer = ParseExpression();
                }
            }
            declarator.Range = RangeFrom(declaratorBegin);
            declarators.Add(declarator);
        } while (Accept(TokenKind::Comma));
        statement->Declarators = Freeze(declarators);
        return Finish(statement, begin);
    }

    AstIfStmt *ParseIf(usize begin, bool isStatic)
    {
        ++_pos;
        auto *statement = Make<AstIfStmt>(begin);
        statement->IsStatic = isStatic;
        Expect(TokenKind::OpenParen);
        statement->Condition = ParseExpression();
        Expect(TokenKind::CloseParen);
        statement->Then = ParseEmbeddedStatement();
        if (Accept(TokenKind::KeywordElse))
        {
            statement->Else = ParseEmbeddedStatement();
        }
        return Finish(statement, begin);
    }

    AstForStmt *ParseFor(usize begin)
    {
        ++_pos;
        auto *statement = Make<AstForStmt>(begin);
        Expect(TokenKind::OpenParen);
        List<AstNode *> initializers;
        if (!At(TokenKind::Semicolon))
        {
            if (AstVarStmt *declaration = TryParseLocalDeclaration(_pos))
            {
                initializers.Add(declaration);
            }
            else
            {
                do
                {
                    const usize expressionBegin = _pos;
                    auto *expression = Make<AstExprStmt>(expressionBegin);
                    expression->Expression = ParseExpression();
                    initializers.Add(Finish(expression, expressionBegin));
                } while (Accept(TokenKind::Comma));
            }
        }
        statement->Initializers = Freeze(initializers);
        Expect(TokenKind::Semicolon);
        if (!At(TokenKind::Semicolon))
        {
            statement->Condition = ParseExpression();
        }
        Expect(TokenKind::Semicolon);
        List<AstNode *> increments;
        if (!At(TokenKind::CloseParen))
        {
            do
            {
                increments.Add(ParseExpression());
            } while (Accept(TokenKind::Comma));
        }
        statement->Increments = Freeze(increments);
        Expect(TokenKind::CloseParen);
        statement->Body = ParseEmbeddedStatement();
        return Finish(statement, begin);
    }

    AstForeachStmt *ParseForeach(usize begin)
    {
        ++_pos;
        auto *statement = Make<AstForeachStmt>(begin);
        Expect(TokenKind::OpenParen);
        if (AtContextual("var") && PeekToken(1).Is(TokenKind::Identifier))
        {
            ++_pos;
        }
        else
        {
            statement->Type = ParseType();
        }
        statement->Name = ExpectIdentifier(&statement->NameRange);
        Expect(TokenKind::KeywordIn);
        statement->Collection = ParseExpression();
        Expect(TokenKind::CloseParen);
        statement->Body = ParseEmbeddedStatement();
        return Finish(statement, begin);
    }

    /// `case` labels are patterns, optionally with `when`; a constant label is a constant pattern.
    AstSwitchStmt *ParseSwitch(usize begin)
    {
        ++_pos;
        auto *statement = Make<AstSwitchStmt>(begin);
        Expect(TokenKind::OpenParen);
        statement->Selector = ParseExpression();
        Expect(TokenKind::CloseParen);
        Expect(TokenKind::OpenBrace);
        List<AstSwitchSection> sections;
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize sectionBegin = _pos;
            AstSwitchSection section;
            List<AstCaseLabel> labels;
            while (At(TokenKind::KeywordCase) || (At(TokenKind::KeywordDefault) && PeekToken(1).Is(TokenKind::Colon)))
            {
                if (Accept(TokenKind::KeywordDefault))
                {
                    section.HasDefault = true;
                }
                else
                {
                    ++_pos;
                    const usize labelBegin = _pos;
                    AstCaseLabel label;
                    label.Pattern = ParsePattern();
                    if (AtContextual("when"))
                    {
                        ++_pos;
                        label.When = ParseExpression();
                    }
                    label.Range = RangeFrom(labelBegin);
                    labels.Add(label);
                }
                Expect(TokenKind::Colon);
            }
            if (labels.IsEmpty() && !section.HasDefault)
            {
                Error(DiagnosticCode::Expected, CurrentRange(), "expected 'case' or 'default', found {0}",
                      DescribeCurrent());
                Recover();
                continue;
            }
            List<AstNode *> statements;
            while (!At(TokenKind::KeywordCase) && !(At(TokenKind::KeywordDefault) && PeekToken(1).Is(TokenKind::Colon)) &&
                   !At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
            {
                const usize before = _pos;
                if (AstNode *inner = ParseStatement())
                {
                    statements.Add(inner);
                }
                if (_pos == before)
                {
                    ++_pos;
                }
            }
            section.Labels = Freeze(labels);
            section.Statements = Freeze(statements);
            section.Range = RangeFrom(sectionBegin);
            sections.Add(section);
        }
        Expect(TokenKind::CloseBrace);
        statement->Sections = Freeze(sections);
        return Finish(statement, begin);
    }

    // Expressions

    AstNode *ParseExpression()
    {
        return ParseAssignment();
    }

    TokenKind ReadAssignmentOperator()
    {
        switch (Current().Kind)
        {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::AmpersandAssign:
        case TokenKind::PipeAssign:
        case TokenKind::CaretAssign:
        case TokenKind::LessLessAssign:
        case TokenKind::QuestionQuestionAssign:
            return Current().Kind;
        case TokenKind::Greater:
            if (PeekToken(1).Is(TokenKind::GreaterEqual) && PeekToken(1).Begin == Current().End)
            {
                return TokenKind::GreaterGreaterAssign;
            }
            return TokenKind::EndOfFile;
        default:
            return TokenKind::EndOfFile;
        }
    }

    /// Whether a lambda starts at the cursor: `x =>` or `(...) =>`, after any `async` and `static`.
    [[nodiscard]] bool AtLambda() const
    {
        usize offset = 0;
        while (AtContextualAt(offset, "async") || PeekToken(offset).Is(TokenKind::KeywordStatic))
        {
            ++offset;
        }
        if (PeekToken(offset).Is(TokenKind::Identifier))
        {
            return PeekToken(offset + 1).Is(TokenKind::Arrow);
        }
        if (!PeekToken(offset).Is(TokenKind::OpenParen))
        {
            return false;
        }
        int32 depth = 0;
        for (usize i = offset;; ++i)
        {
            const Token &token = PeekToken(i);
            if (token.Is(TokenKind::EndOfFile))
            {
                return false;
            }
            if (token.Is(TokenKind::OpenParen))
            {
                ++depth;
            }
            else if (token.Is(TokenKind::CloseParen) && --depth == 0)
            {
                return PeekToken(i + 1).Is(TokenKind::Arrow);
            }
        }
    }

    AstLambdaExpr *ParseLambda()
    {
        const usize begin = _pos;
        auto *lambda = Make<AstLambdaExpr>(begin);
        while (true)
        {
            if (AtContextual("async"))
            {
                lambda->IsAsync = true;
            }
            else if (At(TokenKind::KeywordStatic))
            {
                lambda->IsStatic = true;
            }
            else
            {
                break;
            }
            ++_pos;
        }
        List<AstLambdaParameter> parameters;
        if (At(TokenKind::Identifier))
        {
            AstLambdaParameter parameter;
            parameter.Name = ExpectIdentifier(&parameter.Range);
            parameters.Add(parameter);
        }
        else
        {
            Expect(TokenKind::OpenParen);
            while (!At(TokenKind::CloseParen) && !At(TokenKind::EndOfFile))
            {
                const usize parameterBegin = _pos;
                AstLambdaParameter parameter;
                parameter.Modifier = At(TokenKind::KeywordRef)   ? AstModifiers::Ref
                                     : At(TokenKind::KeywordOut) ? AstModifiers::Out
                                     : At(TokenKind::KeywordIn)  ? AstModifiers::In
                                                                 : AstModifiers::None;
                if (parameter.Modifier != AstModifiers::None)
                {
                    ++_pos;
                }
                if (!(At(TokenKind::Identifier) &&
                      (PeekToken(1).Is(TokenKind::Comma) || PeekToken(1).Is(TokenKind::CloseParen))))
                {
                    parameter.Type = ParseType();
                }
                parameter.Name = ExpectIdentifier();
                parameter.Range = RangeFrom(parameterBegin);
                parameters.Add(parameter);
                if (!Accept(TokenKind::Comma))
                {
                    break;
                }
            }
            Expect(TokenKind::CloseParen);
        }
        lambda->Parameters = Freeze(parameters);
        Expect(TokenKind::Arrow);
        lambda->Body = At(TokenKind::OpenBrace) ? static_cast<AstNode *>(ParseBlock()) : ParseExpression();
        return Finish(lambda, begin);
    }

    AstNode *ParseAssignment()
    {
        const usize begin = _pos;
        if (AtLambda())
        {
            return ParseLambda();
        }
        AstNode *target = ParseConditional();
        if (target == nullptr)
        {
            return nullptr;
        }
        const TokenKind op = ReadAssignmentOperator();
        if (op == TokenKind::EndOfFile)
        {
            return target;
        }
        _pos += op == TokenKind::GreaterGreaterAssign ? 2 : 1;
        auto *assign = Make<AstAssignExpr>(begin);
        assign->Operator = op;
        assign->Target = target;
        assign->Value = ParseAssignment();
        return Finish(assign, begin);
    }

    /// `a ?? b`, right-associative, below `||`.
    AstNode *ParseCoalesce()
    {
        const usize begin = _pos;
        AstNode *left = ParseBinary(0);
        if (left == nullptr || !Accept(TokenKind::QuestionQuestion))
        {
            return left;
        }
        auto *binary = Make<AstBinaryExpr>(begin);
        binary->Operator = TokenKind::QuestionQuestion;
        binary->Left = left;
        binary->Right = ParseCoalesce();
        return Finish(binary, begin);
    }

    AstNode *ParseConditional()
    {
        const usize begin = _pos;
        AstNode *condition = ParseCoalesce();
        if (condition == nullptr || !Accept(TokenKind::Question))
        {
            return condition;
        }
        auto *conditional = Make<AstConditionalExpr>(begin);
        conditional->Condition = condition;
        conditional->WhenTrue = ParseExpression();
        Expect(TokenKind::Colon);
        conditional->WhenFalse = ParseExpression();
        return Finish(conditional, begin);
    }

    /// The binary operator at the cursor and its precedence level, 0 lowest; -1 if none.
    int32 ReadBinaryOperator(TokenKind &op) const
    {
        op = Current().Kind;
        switch (op)
        {
        case TokenKind::PipePipe:
            return 0;
        case TokenKind::AmpersandAmpersand:
            return 1;
        case TokenKind::Pipe:
            return 2;
        case TokenKind::Caret:
            return 3;
        case TokenKind::Ampersand:
            return 4;
        case TokenKind::Equal:
        case TokenKind::NotEqual:
            return 5;
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:
        case TokenKind::KeywordIs:
        case TokenKind::KeywordAs:
            return 6;
        case TokenKind::Greater:
            if (PeekToken(1).Is(TokenKind::Greater) && PeekToken(1).Begin == Current().End)
            {
                op = TokenKind::GreaterGreater;
                return 7;
            }
            if (PeekToken(1).Is(TokenKind::GreaterEqual) && PeekToken(1).Begin == Current().End)
            {
                return -1;
            }
            return 6;
        case TokenKind::LessLess:
            return 7;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 8;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            return 9;
        default:
            return -1;
        }
    }

    AstNode *ParseShift()
    {
        return ParseBinary(7);
    }

    AstNode *ParseBinary(int32 minimum)
    {
        const usize begin = _pos;
        AstNode *left = ParseWith();
        while (left != nullptr)
        {
            TokenKind op;
            const int32 level = ReadBinaryOperator(op);
            if (level < minimum)
            {
                return left;
            }
            _pos += op == TokenKind::GreaterGreater ? 2 : 1;
            if (op == TokenKind::KeywordIs)
            {
                auto *is = Make<AstIsExpr>(begin);
                is->Operand = left;
                is->Pattern = ParsePattern();
                left = Finish(is, begin);
                continue;
            }
            if (op == TokenKind::KeywordAs)
            {
                auto *as = Make<AstAsExpr>(begin);
                as->Operand = left;
                as->Type = ParseType();
                left = Finish(as, begin);
                continue;
            }
            auto *binary = Make<AstBinaryExpr>(begin);
            binary->Operator = op;
            binary->Left = left;
            binary->Right = ParseBinary(level + 1);
            left = Finish(binary, begin);
        }
        return left;
    }

    AstNode *ParseWith()
    {
        const usize begin = _pos;
        AstNode *target = ParseUnary();
        while (target != nullptr)
        {
            if (Accept(TokenKind::KeywordWith))
            {
                auto *with = Make<AstWithExpr>(begin);
                with->Target = target;
                with->Initializers = ParseMemberInitializers();
                target = Finish(with, begin);
            }
            else if (At(TokenKind::KeywordSwitch) && PeekToken(1).Is(TokenKind::OpenBrace))
            {
                target = ParseSwitchExpression(target, begin);
            }
            else
            {
                break;
            }
        }
        return target;
    }

    AstSwitchExpr *ParseSwitchExpression(AstNode *selector, usize begin)
    {
        _pos += 2;
        auto *node = Make<AstSwitchExpr>(begin);
        node->Selector = selector;
        List<AstSwitchArm> arms;
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize armBegin = _pos;
            AstSwitchArm arm;
            arm.Pattern = ParsePattern();
            if (AtContextual("when"))
            {
                ++_pos;
                arm.When = ParseExpression();
            }
            Expect(TokenKind::Arrow);
            arm.Value = ParseExpression();
            arm.Range = RangeFrom(armBegin);
            arms.Add(arm);
            if (arm.Pattern == nullptr || arm.Value == nullptr || !Accept(TokenKind::Comma))
            {
                break;
            }
        }
        Expect(TokenKind::CloseBrace);
        node->Arms = Freeze(arms);
        return Finish(node, begin);
    }

    // Patterns

    AstNode *ParsePattern()
    {
        return ParseBinaryPattern(false);
    }

    /// `p or q` over `p and q` over the rest.
    AstNode *ParseBinaryPattern(bool andLevel)
    {
        const usize begin = _pos;
        AstNode *left = andLevel ? ParsePrimaryPattern() : ParseBinaryPattern(true);
        const StringView word = andLevel ? StringView("and") : StringView("or");
        while (left != nullptr && AtContextual(word))
        {
            ++_pos;
            auto *binary = Make<AstBinaryPattern>(begin);
            binary->IsAnd = andLevel;
            binary->Left = left;
            binary->Right = andLevel ? ParsePrimaryPattern() : ParseBinaryPattern(true);
            left = Finish(binary, begin);
        }
        return left;
    }

    [[nodiscard]] bool AtDesignation() const
    {
        return At(TokenKind::Identifier) && !AtContextual("and") && !AtContextual("or") && !AtContextual("when");
    }

    AstNode *ParsePrimaryPattern()
    {
        const usize begin = _pos;
        if (AtContextual("not"))
        {
            ++_pos;
            auto *node = Make<AstNotPattern>(begin);
            node->Operand = ParsePrimaryPattern();
            return Finish(node, begin);
        }
        if (Accept(TokenKind::OpenParen))
        {
            AstNode *inner = ParsePattern();
            Expect(TokenKind::CloseParen);
            return inner;
        }
        if (AtContextual("_") && !PeekToken(1).Is(TokenKind::Dot) && !PeekToken(1).Is(TokenKind::OpenParen))
        {
            ++_pos;
            return Finish(Make<AstDiscardPattern>(begin), begin);
        }
        if (AtContextual("var") && PeekToken(1).Is(TokenKind::Identifier))
        {
            ++_pos;
            auto *node = Make<AstVarPattern>(begin);
            node->Name = ExpectIdentifier(&node->NameRange);
            return Finish(node, begin);
        }
        if (At(TokenKind::Less) || At(TokenKind::LessEqual) || At(TokenKind::Greater) || At(TokenKind::GreaterEqual))
        {
            auto *node = Make<AstRelationalPattern>(begin);
            node->Operator = Current().Kind;
            ++_pos;
            node->Value = ParseShift();
            return Finish(node, begin);
        }
        if (At(TokenKind::OpenBrace))
        {
            return ParsePropertyPattern(nullptr, begin);
        }
        if (At(TokenKind::Identifier) || IsPredefinedTypeKeyword(Current().Kind))
        {
            AstNode *type = TryParseType(
                [this] {
                    return At(TokenKind::OpenBrace) || AtDesignation();
                },
                false);
            if (type != nullptr)
            {
                if (At(TokenKind::OpenBrace))
                {
                    return ParsePropertyPattern(type, begin);
                }
                auto *node = Make<AstDeclarationPattern>(begin);
                node->Type = type;
                node->Name = ExpectIdentifier(&node->NameRange);
                return Finish(node, begin);
            }
        }
        auto *node = Make<AstConstantPattern>(begin);
        node->Value = ParseShift();
        if (node->Value == nullptr)
        {
            return nullptr;
        }
        return Finish(node, begin);
    }

    AstPropertyPattern *ParsePropertyPattern(AstNode *type, usize begin)
    {
        auto *node = Make<AstPropertyPattern>(begin);
        node->Type = type;
        Expect(TokenKind::OpenBrace);
        List<AstSubpattern> subpatterns;
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize subBegin = _pos;
            AstSubpattern subpattern;
            subpattern.Name = ExpectIdentifier();
            Expect(TokenKind::Colon);
            subpattern.Pattern = ParsePattern();
            subpattern.Range = RangeFrom(subBegin);
            subpatterns.Add(subpattern);
            if (subpattern.Pattern == nullptr || !Accept(TokenKind::Comma))
            {
                break;
            }
        }
        Expect(TokenKind::CloseBrace);
        node->Subpatterns = Freeze(subpatterns);
        if (AtDesignation())
        {
            node->Name = ExpectIdentifier(&node->NameRange);
        }
        return Finish(node, begin);
    }

    Span<AstMemberInitializer> ParseMemberInitializers()
    {
        List<AstMemberInitializer> initializers;
        Expect(TokenKind::OpenBrace);
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            const usize begin = _pos;
            AstMemberInitializer initializer;
            initializer.Name = ExpectIdentifier();
            Expect(TokenKind::Assign);
            initializer.Value = ParseExpression();
            initializer.Range = RangeFrom(begin);
            initializers.Add(initializer);
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
        }
        Expect(TokenKind::CloseBrace);
        return Freeze(initializers);
    }

    AstNode *ParseUnary()
    {
        const usize begin = _pos;
        switch (Current().Kind)
        {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Exclamation:
        case TokenKind::Tilde:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
        {
            auto *unary = Make<AstUnaryExpr>(begin);
            unary->Operator = Current().Kind;
            ++_pos;
            unary->Operand = ParseUnary();
            return Finish(unary, begin);
        }
        case TokenKind::OpenParen:
            if (AstNode *cast = TryParseCast(begin))
            {
                return cast;
            }
            break;
        default:
            break;
        }
        if (AtContextual("await") && StartsOperand(PeekToken(1)))
        {
            ++_pos;
            auto *node = Make<AstAwaitExpr>(begin);
            node->Operand = ParseUnary();
            return Finish(node, begin);
        }
        return ParsePostfix(ParsePrimary());
    }

    /// Whether *token* can begin an operand, so that a contextual keyword before it is an operator.
    [[nodiscard]] static bool StartsOperand(const Token &token)
    {
        switch (token.Kind)
        {
        case TokenKind::Identifier:
        case TokenKind::IntegerLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::RuneLiteral:
        case TokenKind::InterpolatedStringLiteral:
        case TokenKind::OpenParen:
        case TokenKind::Exclamation:
        case TokenKind::Tilde:
        case TokenKind::Minus:
        case TokenKind::KeywordThis:
        case TokenKind::KeywordNew:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
        case TokenKind::KeywordNull:
        case TokenKind::KeywordDefault:
        case TokenKind::KeywordTypeof:
            return true;
        default:
            return IsPredefinedTypeKeyword(token.Kind);
        }
    }

    /// `(T)x`: taken as a cast when T is a keyword type, or when the `)` is followed by a token that can only
    /// start an operand (C# §12.9.7).
    AstNode *TryParseCast(usize begin)
    {
        const usize save = _pos;
        ++_pos;
        bool predefined = IsPredefinedTypeKeyword(Current().Kind);
        AstNode *type = TryParseType([this] { return At(TokenKind::CloseParen); });
        if (type == nullptr)
        {
            _pos = save;
            return nullptr;
        }
        predefined = predefined && type->Kind == AstKind::PredefinedType;
        ++_pos;
        bool cast = predefined;
        switch (Current().Kind)
        {
        case TokenKind::Identifier:
        case TokenKind::IntegerLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::OpenParen:
        case TokenKind::Exclamation:
        case TokenKind::Tilde:
        case TokenKind::KeywordThis:
        case TokenKind::KeywordNew:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
        case TokenKind::KeywordDefault:
        case TokenKind::KeywordSizeof:
        case TokenKind::KeywordNameof:
            cast = true;
            break;
        default:
            cast = cast || IsPredefinedTypeKeyword(Current().Kind);
            break;
        }
        if (!cast)
        {
            _pos = save;
            return nullptr;
        }
        auto *node = Make<AstCastExpr>(begin);
        node->Type = type;
        node->Operand = ParseUnary();
        return Finish(node, begin);
    }

    Span<AstArgument> ParseArguments(TokenKind close)
    {
        List<AstArgument> arguments;
        while (!At(close) && !At(TokenKind::EndOfFile))
        {
            const usize begin = _pos;
            AstArgument argument;
            if (At(TokenKind::Identifier) && PeekToken(1).Is(TokenKind::Colon))
            {
                argument.Name = Text(Current());
                _pos += 2;
            }
            argument.Modifier = At(TokenKind::KeywordRef)   ? AstModifiers::Ref
                                : At(TokenKind::KeywordOut) ? AstModifiers::Out
                                : At(TokenKind::KeywordIn)  ? AstModifiers::In
                                                            : AstModifiers::None;
            if (argument.Modifier != AstModifiers::None)
            {
                ++_pos;
            }
            argument.Value = ParseExpression();
            if (argument.Value == nullptr)
            {
                break;
            }
            argument.Range = RangeFrom(begin);
            arguments.Add(argument);
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
        }
        Expect(close);
        return Freeze(arguments);
    }

    AstNode *ParsePostfix(AstNode *expression)
    {
        if (expression == nullptr)
        {
            return nullptr;
        }
        const uint32 beginOffset = expression->Range.Begin;
        while (true)
        {
            const usize begin = _pos;
            if (At(TokenKind::Dot))
            {
                ++_pos;
                auto *member = Make<AstMemberExpr>(begin);
                member->Target = expression;
                member->Name = ExpectIdentifier(&member->NameRange);
                if (At(TokenKind::Less) && LooksLikeGenericArguments())
                {
                    member->HasGenericArguments = true;
                    member->GenericArguments = ParseGenericArguments();
                }
                expression = Finish(member, begin);
            }
            else if (At(TokenKind::OpenParen))
            {
                ++_pos;
                auto *call = Make<AstCallExpr>(begin);
                call->Callee = expression;
                call->Arguments = ParseArguments(TokenKind::CloseParen);
                expression = Finish(call, begin);
            }
            else if (At(TokenKind::OpenBracket))
            {
                ++_pos;
                auto *index = Make<AstIndexExpr>(begin);
                index->Target = expression;
                index->Index = ParseExpression();
                Expect(TokenKind::CloseBracket);
                expression = Finish(index, begin);
            }
            else if (At(TokenKind::PlusPlus) || At(TokenKind::MinusMinus))
            {
                auto *unary = Make<AstUnaryExpr>(begin);
                unary->Operator = Current().Kind;
                unary->Postfix = true;
                unary->Operand = expression;
                ++_pos;
                expression = Finish(unary, begin);
            }
            else if (At(TokenKind::Exclamation))
            {
                ++_pos;
                auto *node = Make<AstNullForgivingExpr>(begin);
                node->Operand = expression;
                expression = Finish(node, begin);
            }
            else if (At(TokenKind::QuestionDot) || (At(TokenKind::Question) && PeekToken(1).Is(TokenKind::OpenBracket)))
            {
                expression = ParseConditionalAccess(expression, beginOffset);
            }
            else
            {
                return expression;
            }
            expression->Range.Begin = beginOffset;
        }
    }

    /// `target?.rest` or `target?[i]rest`: the rest of the postfix chain applies to the non-null target.
    AstNode *ParseConditionalAccess(AstNode *target, uint32 beginOffset)
    {
        const usize begin = _pos;
        auto *receiver = Make<AstConditionalReceiverExpr>(begin);
        receiver->Range = CurrentRange();
        AstNode *first = nullptr;
        if (Accept(TokenKind::QuestionDot))
        {
            auto *member = Make<AstMemberExpr>(begin);
            member->Target = receiver;
            member->Name = ExpectIdentifier(&member->NameRange);
            if (At(TokenKind::Less) && LooksLikeGenericArguments())
            {
                member->HasGenericArguments = true;
                member->GenericArguments = ParseGenericArguments();
            }
            first = Finish(member, begin);
        }
        else
        {
            _pos += 2;
            auto *index = Make<AstIndexExpr>(begin);
            index->Target = receiver;
            index->Index = ParseExpression();
            Expect(TokenKind::CloseBracket);
            first = Finish(index, begin);
        }
        auto *node = Make<AstConditionalAccessExpr>(begin);
        node->Target = target;
        node->Access = ParsePostfix(first);
        Finish(node, begin);
        node->Range.Begin = beginOffset;
        return node;
    }

    AstNode *ParsePrimary()
    {
        const usize begin = _pos;
        switch (Current().Kind)
        {
        case TokenKind::IntegerLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::RuneLiteral:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
        case TokenKind::KeywordNull:
        {
            auto *literal = Make<AstLiteralExpr>(begin);
            literal->Literal = Current();
            ++_pos;
            return Finish(literal, begin);
        }
        case TokenKind::InterpolatedStringLiteral:
            return ParseInterpolatedString(_tokens[_pos++], begin);
        case TokenKind::Identifier:
        {
            if (AtContextual("base") && (PeekToken(1).Is(TokenKind::Dot) || PeekToken(1).Is(TokenKind::OpenBracket)))
            {
                ++_pos;
                return Finish(Make<AstBaseExpr>(begin), begin);
            }
            if (AtContextual("checked") && PeekToken(1).Is(TokenKind::OpenParen))
            {
                _pos += 2;
                auto *node = Make<AstCheckedExpr>(begin);
                node->Operand = ParseExpression();
                Expect(TokenKind::CloseParen);
                return Finish(node, begin);
            }
            auto *name = Make<AstNameExpr>(begin);
            name->Name = Text(Current());
            ++_pos;
            if (At(TokenKind::Less) && LooksLikeGenericArguments())
            {
                name->HasGenericArguments = true;
                name->GenericArguments = ParseGenericArguments();
            }
            return Finish(name, begin);
        }
        case TokenKind::OpenParen:
        {
            ++_pos;
            AstNode *first = ParseExpression();
            if (!Accept(TokenKind::Comma))
            {
                Expect(TokenKind::CloseParen);
                return first;
            }
            auto *tuple = Make<AstTupleExpr>(begin);
            List<AstNode *> elements;
            elements.Add(first);
            do
            {
                elements.Add(ParseExpression());
            } while (Accept(TokenKind::Comma));
            Expect(TokenKind::CloseParen);
            tuple->Elements = Freeze(elements);
            return Finish(tuple, begin);
        }
        case TokenKind::KeywordNew:
        {
            ++_pos;
            if (At(TokenKind::OpenBracket) && PeekToken(1).Is(TokenKind::CloseBracket))
            {
                _pos += 2;
                auto *array = Make<AstArrayCreationExpr>(begin);
                array->HasElements = true;
                array->Elements = ParseCollectionElements();
                return Finish(array, begin);
            }
            const usize typeBegin = _pos;
            AstNode *type = ParseNonArrayType();
            if (type != nullptr && At(TokenKind::OpenBracket))
            {
                if (AstNode *array = TryParseArrayCreation(type, begin, typeBegin))
                {
                    return array;
                }
                _pos = typeBegin;
                type = ParseType();
            }
            auto *node = Make<AstNewExpr>(begin);
            node->Type = type;
            if (Accept(TokenKind::OpenParen))
            {
                node->Arguments = ParseArguments(TokenKind::CloseParen);
            }
            if (At(TokenKind::OpenBrace))
            {
                if (PeekToken(1).Is(TokenKind::CloseBrace) ||
                    (PeekToken(1).Is(TokenKind::Identifier) && PeekToken(2).Is(TokenKind::Assign)))
                {
                    node->HasInitializer = true;
                    node->Initializers = ParseMemberInitializers();
                }
                else
                {
                    node->HasCollectionInitializer = true;
                    node->Elements = ParseCollectionElements();
                }
            }
            return Finish(node, begin);
        }
        case TokenKind::KeywordThis:
            ++_pos;
            return Finish(Make<AstThisExpr>(begin), begin);
        case TokenKind::KeywordDefault:
        {
            ++_pos;
            auto *node = Make<AstDefaultExpr>(begin);
            if (Accept(TokenKind::OpenParen))
            {
                node->Type = ParseType();
                Expect(TokenKind::CloseParen);
            }
            return Finish(node, begin);
        }
        case TokenKind::KeywordSizeof:
        {
            ++_pos;
            auto *node = Make<AstSizeofExpr>(begin);
            Expect(TokenKind::OpenParen);
            node->Type = ParseType();
            Expect(TokenKind::CloseParen);
            return Finish(node, begin);
        }
        case TokenKind::KeywordTypeof:
        {
            ++_pos;
            auto *node = Make<AstTypeofExpr>(begin);
            Expect(TokenKind::OpenParen);
            node->Type = ParseType();
            Expect(TokenKind::CloseParen);
            return Finish(node, begin);
        }
        case TokenKind::KeywordNameof:
        {
            ++_pos;
            auto *node = Make<AstNameofExpr>(begin);
            Expect(TokenKind::OpenParen);
            node->Operand = ParseExpression();
            Expect(TokenKind::CloseParen);
            return Finish(node, begin);
        }
        default:
            if (IsPredefinedTypeKeyword(Current().Kind))
            {
                auto *predefined = Make<AstPredefinedType>(begin);
                predefined->Keyword = Current().Kind;
                ++_pos;
                return Finish(predefined, begin);
            }
            Error(DiagnosticCode::UnexpectedToken, CurrentRange(), "expected an expression, found {0}",
                  DescribeCurrent());
            return nullptr;
        }
    }

    /// After `new T` at `[`: `new T[n]`, `new T[] { ... }` or `new T[n] { ... }`. Null, with the cursor restored, when
    /// the brackets are part of the type being constructed, as in `new float[4](...)`.
    AstNode *TryParseArrayCreation(AstNode *element, usize begin, usize typeBegin)
    {
        ++_pos;
        AstNode *length = nullptr;
        if (!At(TokenKind::CloseBracket))
        {
            length = ParseExpression();
        }
        const bool closed = Expect(TokenKind::CloseBracket);
        if (closed && (At(TokenKind::OpenParen) || At(TokenKind::OpenBracket) ||
                       (length == nullptr && !At(TokenKind::OpenBrace))))
        {
            _pos = typeBegin;
            return nullptr;
        }
        auto *array = Make<AstArrayCreationExpr>(begin);
        array->Element = element;
        array->Length = length;
        if (At(TokenKind::OpenBrace))
        {
            array->HasElements = true;
            array->Elements = ParseCollectionElements();
        }
        return Finish(array, begin);
    }

    /// `{ a, b, c }`, with an optional trailing comma.
    Span<AstNode *> ParseCollectionElements()
    {
        List<AstNode *> elements;
        Expect(TokenKind::OpenBrace);
        while (!At(TokenKind::CloseBrace) && !At(TokenKind::EndOfFile))
        {
            AstNode *element = ParseExpression();
            if (element == nullptr)
            {
                break;
            }
            elements.Add(element);
            if (!Accept(TokenKind::Comma))
            {
                break;
            }
        }
        Expect(TokenKind::CloseBrace);
        return Freeze(elements);
    }

    /// Copies *text*, which is not a view of the source, into the arena.
    StringView StoreText(StringView text)
    {
        if (text.IsEmpty())
        {
            return {};
        }
        char *data = _arena->AllocateArray<char>(text.Length());
        MemCopy(data, text.Data(), text.Length());
        return {data, text.Length()};
    }

    /// Splits an interpolated string token into its parts. Text between holes is decoded like the text of a string,
    /// with `{{` and `}}` as braces. A hole `{expression}` or `{expression:format}` ends at its `}`: brackets nest in
    /// it, strings in it are skipped whole, and a `:` outside brackets starts the format.
    AstInterpolatedStringExpr *ParseInterpolatedString(const Token &token, usize begin)
    {
        auto *node = Make<AstInterpolatedStringExpr>(begin);
        const StringView source = _file->GetText();
        uint32 pos = token.Begin;
        while (pos < token.End && source[pos] != '"')
        {
            ++pos;
        }
        ++pos;
        const uint32 end = token.End > pos && source[token.End - 1] == '"' ? token.End - 1 : token.End;
        List<AstInterpolationPart> parts;
        StringBuilder<256> text;
        uint32 textBegin = pos;
        auto flushText = [&](uint32 textEnd) {
            if (!text.IsEmpty())
            {
                AstInterpolationPart part;
                part.Range = {_file->GetId(), textBegin, textEnd};
                part.Text = StoreText(DecodeStringText(text.AsView(), token.Verbatim).AsView());
                parts.Add(part);
                text.Clear();
            }
        };
        while (pos < end)
        {
            const char c = source[pos];
            if ((c == '{' || c == '}') && pos + 1 < end && source[pos + 1] == c)
            {
                text.Append(c);
                pos += 2;
                continue;
            }
            if (c != '{')
            {
                const uint32 length = (c == '\\' && !token.Verbatim) || (c == '"' && token.Verbatim) ? 2 : 1;
                text.Append(source.Substring(pos, pos + length <= end ? length : 1));
                pos += length;
                continue;
            }
            flushText(pos);
            const uint32 holeBegin = pos + 1;
            uint32 holeEnd = holeBegin;
            uint32 formatBegin = 0;
            int32 depth = 0;
            while (holeEnd < end && !(depth == 0 && source[holeEnd] == '}'))
            {
                const char h = source[holeEnd];
                if (h == '"' || h == '\'')
                {
                    const bool verbatim = h == '"' && holeEnd > holeBegin && source[holeEnd - 1] == '@';
                    ++holeEnd;
                    while (holeEnd < end && source[holeEnd] != h)
                    {
                        holeEnd += source[holeEnd] == '\\' && !verbatim ? 2 : 1;
                    }
                }
                else if (h == '(' || h == '[' || h == '{')
                {
                    ++depth;
                }
                else if (h == ')' || h == ']' || h == '}')
                {
                    --depth;
                }
                else if (h == ':' && depth == 0 && formatBegin == 0)
                {
                    formatBegin = holeEnd + 1;
                    while (holeEnd + 1 < end && source[holeEnd + 1] != '}')
                    {
                        ++holeEnd;
                    }
                }
                ++holeEnd;
            }
            AstInterpolationPart part;
            part.Range = {_file->GetId(), pos, holeEnd < end ? holeEnd + 1 : end};
            const uint32 expressionEnd = formatBegin != 0 ? formatBegin - 1 : holeEnd;
            part.Expression = ParseHole(holeBegin, expressionEnd);
            if (formatBegin != 0)
            {
                part.Format = source.Substring(formatBegin, holeEnd - formatBegin);
            }
            if (part.Expression != nullptr)
            {
                parts.Add(part);
            }
            pos = holeEnd < end ? holeEnd + 1 : end;
            textBegin = pos;
        }
        flushText(pos);
        node->Parts = Freeze(parts);
        return Finish(node, begin);
    }

    /// Parses the expression of an interpolation hole, the source range [begin, end), with a parser of its own over
    /// the hole's tokens.
    AstNode *ParseHole(uint32 begin, uint32 end)
    {
        List<Token> tokens;
        LanguageDiagnostics ignored;
        TokenizeRange(*_file, begin, end, _speculating == 0 ? *_diagnostics : ignored, tokens);
        Parser parser(*_file, Span<const Token>(tokens.Data(), tokens.Count()), *_arena, *_diagnostics, _options);
        parser._speculating = _speculating;
        AstNode *expression = parser.ParseExpression();
        if (expression != nullptr && !parser.At(TokenKind::EndOfFile))
        {
            Error(DiagnosticCode::Expected, parser.CurrentRange(), "expected one expression in the interpolation hole");
        }
        _failed = _failed || parser._failed;
        return expression;
    }

    const SourceFile *_file;
    Span<const Token> _tokens;
    Arena *_arena;
    LanguageDiagnostics *_diagnostics;
    ParseOptions _options;
    List<AstFieldDecl *> _pendingFields;
    usize _pos = 0;
    uint32 _speculating = 0;
    bool _failed = false;
};
} // namespace

AstFile *ParseFile(const SourceFile &file, Arena &arena, LanguageDiagnostics &diagnostics, const ParseOptions &options)
{
    AstFile *result = arena.New<AstFile>();
    result->File = &file;
    result->Tokens = GetTokenCache().Tokenize(file, arena, diagnostics);
    Parser parser(file, result->Tokens, arena, diagnostics, options);
    result->Members = parser.ParseFileMembers();
    return result;
}

void ParseBody(const AstFile &file, AstBody &body, Arena &arena, LanguageDiagnostics &diagnostics)
{
    if (body.Parsed || body.IsMissing())
    {
        return;
    }
    Parser parser(*file.File, file.Tokens, arena, diagnostics, {});
    parser.ParseBodyAt(body);
}
} // namespace Marlin
