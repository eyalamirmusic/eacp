// clang-tidy plugin enforcing the eacp CLAUDE.md rules. Loaded into the
// pinned clang-tidy with --load (see ../README.md); the checks then behave
// like built-in ones: configured via .clang-tidy, suppressed with NOLINT,
// and fixable with -fix.

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"

#include <set>
#include <string>
#include <vector>

namespace eacp::tidy
{
using namespace clang;
using namespace clang::ast_matchers;
using clang::tidy::ClangTidyCheck;
using clang::tidy::ClangTidyContext;

// ---------------------------------------------------------------------------
// eacp-use-auto: a variable that spells out a type its initializer already
// determines. Mirrors CLAUDE.md "Use auto for variables and whenever
// possible". Fix-its only where dropping the type is a pure substitution:
// copy-init ('T x = ...') and range-for variables, single declarator.
// ---------------------------------------------------------------------------
class UseAutoCheck : public ClangTidyCheck
{
public:
    using ClangTidyCheck::ClangTidyCheck;

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override
    {
        return langOpts.CPlusPlus11;
    }

    void registerMatchers(MatchFinder* finder) override
    {
        finder->addMatcher(varDecl(hasInitializer(expr()),
                                   unless(parmVarDecl()),
                                   unless(isImplicit()),
                                   unless(isTemplateInstantiation()),
                                   unless(isExpansionInSystemHeader()))
                               .bind("var"),
                           this);
    }

    void check(const MatchFinder::MatchResult& result) override
    {
        const auto* var = result.Nodes.getNodeAs<VarDecl>("var");

        if (var->isExceptionVariable() || var->getType()->isDependentType())
            return;
        if (var->getType()->isObjCIdType()) // auto-deducing 'id' is a
            return; // clang warning (-Wauto-var-id)

        const auto* typeInfo = var->getTypeSourceInfo();
        if (typeInfo == nullptr || typeInfo->getTypeLoc().getContainedAutoTypeLoc())
            return;
        if (typeInfo->getType()->getAs<DecltypeType>() != nullptr)
            return;

        const auto* init = var->getInit()->IgnoreImplicit();
        if (isa<InitListExpr>(init))
            return;
        if (const auto* construct = dyn_cast<CXXConstructExpr>(init))
        {
            auto defaultConstructed =
                construct->getNumArgs() == 0
                && construct->getParenOrBraceRange().isInvalid();
            if (defaultConstructed) // 'T x;' — no initializer was written
                return;
            if (construct->isListInitialization()
                && var->getInitStyle() == VarDecl::CInit)
                return; // 'T x = {...}' — auto would deduce initializer_list
        }

        auto declared = var->getType().getNonReferenceType().getUnqualifiedType();
        auto deduced = init->getType().getNonReferenceType().getUnqualifiedType();
        if (declared.isNull() || deduced.isNull()
            || !result.Context->hasSameType(declared.getCanonicalType(),
                                            deduced.getCanonicalType()))
            return;

        auto diagnostic =
            diag(var->getLocation(),
                 "variable %0 spells out its type; use auto (CLAUDE.md: use "
                 "auto for variables whenever possible)")
            << var;

        if (auto range = fixableTypeRange(*var, *result.Context))
            diagnostic << FixItHint::CreateReplacement(*range, "auto");
    }

private:
    static std::optional<SourceRange> fixableTypeRange(const VarDecl& var,
                                                       ASTContext& context)
    {
        auto copyInit = var.getInitStyle() == VarDecl::CInit;
        if (!(copyInit || var.isCXXForRangeDecl()))
            return std::nullopt;

        auto parents = context.getParents(var);
        if (!parents.empty())
        {
            if (const auto* stmt = parents[0].get<DeclStmt>();
                stmt != nullptr && !stmt->isSingleDecl())
                return std::nullopt;
        }

        auto typeLoc = var.getTypeSourceInfo()->getTypeLoc();
        while (true)
        {
            if (auto qualified = typeLoc.getAs<QualifiedTypeLoc>())
            {
                typeLoc = qualified.getUnqualifiedLoc();
                continue;
            }
            if (auto reference = typeLoc.getAs<ReferenceTypeLoc>())
            {
                typeLoc = reference.getPointeeLoc();
                continue;
            }
            if (auto pointer = typeLoc.getAs<PointerTypeLoc>())
            {
                typeLoc = pointer.getPointeeLoc();
                continue;
            }
            break;
        }

        if (typeLoc.getBeginLoc().isMacroID() || typeLoc.getEndLoc().isMacroID())
            return std::nullopt;

        return typeLoc.getSourceRange();
    }
};

// ---------------------------------------------------------------------------
// eacp-no-auto-function-return: functions and member functions must spell
// out their return type (CLAUDE.md: "Don't use auto for functions and member
// functions"). Lambdas are exempt. The fix-it substitutes the deduced type
// when clang has already resolved it.
// ---------------------------------------------------------------------------
class NoAutoFunctionReturnCheck : public ClangTidyCheck
{
public:
    using ClangTidyCheck::ClangTidyCheck;

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override
    {
        return langOpts.CPlusPlus14;
    }

    void registerMatchers(MatchFinder* finder) override
    {
        finder->addMatcher(functionDecl(unless(isImplicit()),
                                        unless(isTemplateInstantiation()),
                                        unless(isExpansionInSystemHeader()))
                               .bind("fn"),
                           this);
    }

    void check(const MatchFinder::MatchResult& result) override
    {
        const auto* fn = result.Nodes.getNodeAs<FunctionDecl>("fn");

        if (const auto* method = dyn_cast<CXXMethodDecl>(fn);
            method != nullptr && method->getParent()->isLambda())
            return;
        if (isa<CXXConversionDecl>(fn) || fn != fn->getCanonicalDecl())
            return;

        const auto* proto = fn->getType()->getAs<FunctionProtoType>();
        auto trailingReturn = proto != nullptr && proto->hasTrailingReturn();
        auto writtenAuto =
            fn->getDeclaredReturnType()->getContainedAutoType() != nullptr;
        if (!trailingReturn && !writtenAuto)
            return;

        auto diagnostic =
            diag(fn->getLocation(),
                 "function %0 uses an auto return type; spell out the return "
                 "type (CLAUDE.md: don't use auto for functions)")
            << fn;

        if (writtenAuto && !trailingReturn)
            addDeducedTypeFixIt(diagnostic, *fn, *result.Context);
    }

private:
    void addDeducedTypeFixIt(DiagnosticBuilder& diagnostic,
                             const FunctionDecl& fn,
                             ASTContext& context)
    {
        auto returnType = fn.getReturnType();
        if (returnType.isNull() || returnType->isUndeducedAutoType()
            || returnType->isDependentType())
            return;

        auto printed = returnType.getAsString(context.getPrintingPolicy());
        if (printed.find("(lambda") != std::string::npos
            || printed.find("(unnamed") != std::string::npos)
            return;

        auto typeLoc = fn.getTypeSourceInfo()->getTypeLoc().IgnoreParens();
        if (auto functionLoc = typeLoc.getAs<FunctionTypeLoc>())
        {
            auto returnLoc = functionLoc.getReturnLoc();
            if (!returnLoc.getBeginLoc().isMacroID())
                diagnostic << FixItHint::CreateReplacement(
                    returnLoc.getSourceRange(), printed);
        }
    }
};

// ---------------------------------------------------------------------------
// eacp-std-function-member-default: std::function data members need a
// non-null default (CLAUDE.md: "Give std::function members a non-null
// default — a no-op lambda, or one returning an empty value — so call sites
// invoke them directly without null checks").
// ---------------------------------------------------------------------------
class StdFunctionMemberDefaultCheck : public ClangTidyCheck
{
public:
    using ClangTidyCheck::ClangTidyCheck;

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override
    {
        return langOpts.CPlusPlus11;
    }

    void registerMatchers(MatchFinder* finder) override
    {
        finder->addMatcher(
            fieldDecl(unless(isExpansionInSystemHeader())).bind("field"), this);
    }

    void check(const MatchFinder::MatchResult& result) override
    {
        const auto* field = result.Nodes.getNodeAs<FieldDecl>("field");

        // Lambda captures are compiler-generated fields, and Objective-C
        // ivars are routinely branched on for emptiness (e.g. provider
        // dispatch), where a no-op default would change behaviour.
        if (isa<ObjCIvarDecl>(field))
            return;
        if (const auto* parent = dyn_cast<CXXRecordDecl>(field->getParent());
            parent != nullptr && parent->isLambda())
            return;

        const auto* record =
            field->getType().getCanonicalType()->getAsCXXRecordDecl();
        if (record == nullptr || !record->isInStdNamespace()
            || record->getName() != "function")
            return;

        auto lambda = noOpLambda(*record, *result.Context);

        if (!field->hasInClassInitializer())
        {
            auto diagnostic =
                diag(field->getLocation(),
                     "std::function member %0 has no default; give it a "
                     "no-op lambda so call sites don't need null checks")
                << field;
            if (!lambda.empty())
                diagnostic << FixItHint::CreateInsertion(afterName(*field, result),
                                                         " = " + lambda);
            return;
        }

        const auto* init = field->getInClassInitializer();
        if (init == nullptr || !isNullDefault(*init))
            return;

        auto diagnostic =
            diag(field->getLocation(),
                 "std::function member %0 defaults to null; use a no-op "
                 "lambda (e.g. [] {}) so call sites don't need null checks")
            << field;
        if (!lambda.empty() && !init->getBeginLoc().isMacroID())
            diagnostic << FixItHint::CreateReplacement(
                SourceRange(afterName(*field, result), init->getEndLoc()),
                " = " + lambda);
    }

private:
    static SourceLocation afterName(const FieldDecl& field,
                                    const MatchFinder::MatchResult& result)
    {
        return Lexer::getLocForEndOfToken(field.getLocation(),
                                          0,
                                          *result.SourceManager,
                                          result.Context->getLangOpts());
    }

    static bool isNullDefault(const Expr& init)
    {
        const auto* stripped = init.IgnoreImplicit();

        if (isa<CXXNullPtrLiteralExpr>(stripped))
            return true;

        if (const auto* list = dyn_cast<InitListExpr>(stripped))
        {
            if (list->getNumInits() == 0)
                return true;
            return list->getNumInits() == 1
                   && isa<CXXNullPtrLiteralExpr>(list->getInit(0)->IgnoreImplicit());
        }

        if (const auto* construct = dyn_cast<CXXConstructExpr>(stripped))
        {
            if (construct->getNumArgs() == 0)
                return true;
            return construct->getNumArgs() >= 1
                   && isa<CXXNullPtrLiteralExpr>(
                       construct->getArg(0)->IgnoreImplicit());
        }

        return false;
    }

    static std::string noOpLambda(const CXXRecordDecl& record, ASTContext& context)
    {
        const auto* spec = dyn_cast<ClassTemplateSpecializationDecl>(&record);
        if (spec == nullptr || spec->getTemplateArgs().size() == 0)
            return {};

        const auto* proto =
            spec->getTemplateArgs()[0].getAsType()->getAs<FunctionProtoType>();
        if (proto == nullptr)
            return {};

        const auto& policy = context.getPrintingPolicy();
        std::string params;
        for (auto paramType: proto->getParamTypes())
        {
            if (!params.empty())
                params += ", ";
            params += paramType.getAsString(policy);
        }

        auto returnType = proto->getReturnType();
        if (returnType->isVoidType())
            return params.empty() ? "[] {}" : "[] (" + params + ") {}";

        auto printed = returnType.getAsString(policy);
        auto base = printed.substr(0, printed.find('<'));
        if (base.find(' ') != std::string::npos)
            return "[] (" + params + ") -> " + printed + " { return {}; }";

        auto capture = params.empty() ? "[]" : "[] (" + params + ")";
        return capture + " { return " + printed + " {}; }";
    }
};

// ---------------------------------------------------------------------------
// eacp-no-raw-new-delete: RAII everywhere (CLAUDE.md: "Always use the most
// modern C++ and RAII practices").
// ---------------------------------------------------------------------------
class NoRawNewDeleteCheck : public ClangTidyCheck
{
public:
    using ClangTidyCheck::ClangTidyCheck;

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override
    {
        return langOpts.CPlusPlus;
    }

    void registerMatchers(MatchFinder* finder) override
    {
        finder->addMatcher(
            cxxNewExpr(unless(isExpansionInSystemHeader())).bind("new"), this);
        finder->addMatcher(
            cxxDeleteExpr(unless(isExpansionInSystemHeader())).bind("delete"), this);
    }

    void check(const MatchFinder::MatchResult& result) override
    {
        if (const auto* newExpr = result.Nodes.getNodeAs<CXXNewExpr>("new"))
            diag(newExpr->getBeginLoc(),
                 "raw 'new'; prefer RAII (std::make_unique, containers, or "
                 "an owning wrapper)");

        if (const auto* deleteExpr = result.Nodes.getNodeAs<CXXDeleteExpr>("delete"))
            diag(deleteExpr->getBeginLoc(),
                 "raw 'delete'; prefer RAII ownership so cleanup is "
                 "automatic");
    }
};

// ---------------------------------------------------------------------------
// eacp-no-body-comments: comments inside function bodies (CLAUDE.md: "Don't
// use comments unless absolutely needed. Use named functions to make code
// self documenting"). Comments are collected during preprocessing and
// reported when they fall inside a matched body.
// ---------------------------------------------------------------------------
class NoBodyCommentsCheck
    : public ClangTidyCheck
    , public CommentHandler
{
public:
    using ClangTidyCheck::ClangTidyCheck;

    void registerPPCallbacks(const SourceManager&,
                             Preprocessor* preprocessor,
                             Preprocessor*) override
    {
        preprocessor->addCommentHandler(this);
    }

    bool HandleComment(Preprocessor& preprocessor, SourceRange range) override
    {
        auto text = Lexer::getSourceText(CharSourceRange::getCharRange(range),
                                         preprocessor.getSourceManager(),
                                         preprocessor.getLangOpts());
        if (!text.contains("NOLINT"))
            comments.push_back(range);
        return false;
    }

    void registerMatchers(MatchFinder* finder) override
    {
        finder->addMatcher(functionDecl(hasBody(compoundStmt().bind("body")),
                                        unless(isExpansionInSystemHeader()))
                               .bind("fn"),
                           this);
        finder->addMatcher(
            lambdaExpr(unless(isExpansionInSystemHeader())).bind("lambda"), this);
    }

    void check(const MatchFinder::MatchResult& result) override
    {
        const Stmt* body = result.Nodes.getNodeAs<CompoundStmt>("body");
        if (const auto* lambda = result.Nodes.getNodeAs<LambdaExpr>("lambda"))
            body = lambda->getBody();
        if (body == nullptr)
            return;

        const auto& sourceManager = *result.SourceManager;
        for (const auto& comment: comments)
        {
            if (!sourceManager.isPointWithin(
                    comment.getBegin(), body->getBeginLoc(), body->getEndLoc()))
                continue;
            if (!reported.insert(comment.getBegin().getRawEncoding()).second)
                continue;
            diag(comment.getBegin(),
                 "comment inside a function body; prefer a named function "
                 "that makes the code self-documenting (CLAUDE.md)");
        }
    }

private:
    std::vector<SourceRange> comments;
    std::set<unsigned> reported;
};

// ---------------------------------------------------------------------------

class EacpModule : public clang::tidy::ClangTidyModule
{
public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& factories) override
    {
        factories.registerCheck<UseAutoCheck>("eacp-use-auto");
        factories.registerCheck<NoAutoFunctionReturnCheck>(
            "eacp-no-auto-function-return");
        factories.registerCheck<StdFunctionMemberDefaultCheck>(
            "eacp-std-function-member-default");
        factories.registerCheck<NoRawNewDeleteCheck>("eacp-no-raw-new-delete");
        factories.registerCheck<NoBodyCommentsCheck>("eacp-no-body-comments");
    }
};

} // namespace eacp::tidy

namespace clang::tidy
{
static ClangTidyModuleRegistry::Add<eacp::tidy::EacpModule>
    eacpModuleRegistration("eacp-module", "Checks for the eacp CLAUDE.md rules.");
} // namespace clang::tidy
