// slang-language-server-completion.cpp

#include "slang-language-server-completion.h"

#include "../core/slang-char-util.h"
#include "../core/slang-file-system.h"
#include "slang-ast-all.h"
#include "slang-ast-print.h"
#include "slang-check-impl.h"
#include "slang-language-server-ast-lookup.h"
#include "slang-language-server.h"
#include "slang-syntax.h"

#include <chrono>

namespace Slang
{

static const char* kDeclKeywords[] = {
    "throws",     "static",         "const",     "in",       "out",     "inout",
    "ref",        "__subscript",    "__init",    "property", "get",     "set",
    "class",      "struct",         "interface", "public",   "private", "internal",
    "protected",  "typedef",        "typealias", "uniform",  "export",  "groupshared",
    "extension",  "associatedtype", "namespace", "This",     "using",   "__generic",
    "__exported", "import",         "enum",      "cbuffer",  "tbuffer", "func",
    "functype",   "typename",       "each",      "expand",   "where",   "override"};
static const char* kStmtKeywords[] = {
    "if",
    "else",
    "switch",
    "case",
    "default",
    "return",
    "try",
    "throw",
    "throws",
    "catch",
    "while",
    "for",
    "do",
    "static",
    "const",
    "in",
    "out",
    "inout",
    "ref",
    "__subscript",
    "__init",
    "property",
    "get",
    "set",
    "class",
    "struct",
    "interface",
    "public",
    "private",
    "internal",
    "protected",
    "typedef",
    "typealias",
    "uniform",
    "export",
    "groupshared",
    "extension",
    "associatedtype",
    "this",
    "namespace",
    "This",
    "using",
    "__generic",
    "__exported",
    "import",
    "enum",
    "break",
    "continue",
    "discard",
    "defer",
    "cbuffer",
    "tbuffer",
    "func",
    "is",
    "as",
    "nullptr",
    "none",
    "true",
    "false",
    "functype",
    "sizeof",
    "alignof",
    "__target_switch",
    "__intrinsic_asm",
    "each",
    "expand"};

static const char* hlslSemanticNames[] = {
    "register",
    "packoffset",
    "read",
    "write",
    "SV_BaseInstanceID",
    "SV_BaryCentrics",
    "SV_ClipDistance",
    "SV_CullDistance",
    "SV_Coverage",
    "SV_Depth",
    "SV_DepthGreaterEqual",
    "SV_DepthLessEqual",
    "SV_DispatchThreadID",
    "SV_DomainLocation",
    "SV_GroupID",
    "SV_GroupIndex",
    "SV_GroupThreadID",
    "SV_GSInstanceID",
    "SV_InnerCoverage",
    "SV_InsideTessFactor",
    "SV_InstanceID",
    "SV_IsFrontFace",
    "SV_OutputControlPointID",
    "SV_Position",
    "SV_PointSize",
    "SV_PointCoord",
    "SV_PrimitiveID",
    "SV_DrawIndex",
    "SV_DeviceIndex",
    "SV_RenderTargetArrayIndex",
    "SV_SampleIndex",
    "SV_StencilRef",
    "SV_Target",
    "SV_TessFactor",
    "SV_VertexID",
    "SV_ViewID",
    "SV_ViewportArrayIndex",
    "SV_VulkanVertexID",
    "SV_VulkanInstanceID",
    "SV_ShadingRate",
    "SV_StartVertexLocation",
    "SV_StartInstanceLocation",
};

bool isDeclKeyword(const UnownedStringSlice& slice)
{
    for (auto keyword : kDeclKeywords)
    {
        if (slice == keyword)
            return true;
    }
    return false;
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteHLSLSemantic()
{
    if (version->linkage->contentAssistInfo.completionSuggestions.scopeKind !=
        CompletionSuggestions::ScopeKind::HLSLSemantics)
    {
        return SLANG_FAIL;
    }
    List<LanguageServerProtocol::CompletionItem> items;
    for (auto name : hlslSemanticNames)
    {
        LanguageServerProtocol::CompletionItem item;
        item.label = name;
        item.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
        items.add(item);
    }
    return CompletionResult(_Move(items));
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteAttributes()
{
    if (version->linkage->contentAssistInfo.completionSuggestions.scopeKind !=
        CompletionSuggestions::ScopeKind::Attribute)
    {
        return SLANG_FAIL;
    }
    return collectAttributes();
}

CompletionResult CompletionContext::gatherFileAndModuleCompletionItems(
    const String& prefixPath,
    bool translateModuleName,
    bool isImportString,
    Index lineIndex,
    Index fileNameEnd,
    Index sectionStart,
    Index sectionEnd,
    char closingChar)
{
    auto realPrefix = prefixPath.getUnownedSlice();
    while (realPrefix.startsWith(".."))
    {
        realPrefix = realPrefix.tail(2);
        if (realPrefix.startsWith("/") || realPrefix.startsWith("\\"))
        {
            realPrefix = realPrefix.tail(1);
        }
    }

    struct FileEnumerationContext
    {
        List<LanguageServerProtocol::TextEditCompletionItem> items;
        HashSet<String> itemSet;
        CompletionContext* completionContext;
        String path;
        String workspaceRoot;
        bool translateModuleName;
        bool isImportString;
    } context;
    context.completionContext = this;
    context.translateModuleName = translateModuleName;
    context.isImportString = isImportString;
    if (version->workspace->rootDirectories.getCount())
        context.workspaceRoot = version->workspace->rootDirectories[0];
    if (context.workspaceRoot.getLength() &&
        context.workspaceRoot[context.workspaceRoot.getLength() - 1] !=
            Path::kOSCanonicalPathDelimiter)
    {
        context.workspaceRoot = context.workspaceRoot + String(Path::kOSCanonicalPathDelimiter);
    }

    auto addCandidate = [&](const String& path)
    {
        context.path = path;
        Path::getCanonical(context.path, context.path);
        if (path.getUnownedSlice().endsWithCaseInsensitive(realPrefix))
        {
            OSFileSystem::getExtSingleton()->enumeratePathContents(
                path.getBuffer(),
                [](SlangPathType pathType, const char* name, void* userData)
                {
                    FileEnumerationContext* context = (FileEnumerationContext*)userData;
                    LanguageServerProtocol::TextEditCompletionItem item;
                    if (pathType == SLANG_PATH_TYPE_DIRECTORY)
                    {
                        item.label = name;
                        item.kind = LanguageServerProtocol::kCompletionItemKindFolder;
                        if (item.label.indexOf('.') != -1)
                            return;
                    }
                    else
                    {
                        auto nameSlice = UnownedStringSlice(name);
                        if (context->isImportString || context->translateModuleName)
                        {
                            if (!nameSlice.endsWithCaseInsensitive(".slang"))
                                return;
                        }
                        StringBuilder nameSB;
                        auto fileName = UnownedStringSlice(name);
                        if (context->translateModuleName || context->isImportString)
                            fileName = fileName.head(nameSlice.getLength() - 6);
                        for (auto ch : fileName)
                        {
                            if (context->translateModuleName)
                            {
                                switch (ch)
                                {
                                case '-':
                                    nameSB.appendChar('_');
                                    break;
                                case '.':
                                    // Ignore any file items that contains a "."
                                    return;
                                default:
                                    nameSB.appendChar(ch);
                                    break;
                                }
                            }
                            else
                            {
                                nameSB.appendChar(ch);
                            }
                        }
                        item.label = nameSB.produceString();
                        item.kind = LanguageServerProtocol::kCompletionItemKindFile;
                    }
                    if (item.label.getLength())
                    {
                        auto key = String(item.kind) + item.label;
                        if (context->itemSet.add(key))
                        {
                            item.detail = Path::combine(context->path, String(name));
                            Path::getCanonical(item.detail, item.detail);

                            if (item.detail.getUnownedSlice().startsWithCaseInsensitive(
                                    context->workspaceRoot.getUnownedSlice()))
                            {
                                item.detail = item.detail.getUnownedSlice().tail(
                                    context->workspaceRoot.getLength());
                            }
                            context->items.add(item);
                        }
                    }
                },
                &context);
        }
    };

    // A big workspace may take a long time to enumerate, thus we limit the amount
    // of time allowed to scan the file directory.

    auto startTime = std::chrono::high_resolution_clock::now();
    bool isIncomplete = false;

    for (auto& searchPath : this->version->workspace->additionalSearchPaths)
    {
        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::high_resolution_clock::now() - startTime)
                               .count();
        if (elapsedTime > 200)
        {
            isIncomplete = true;
            break;
        }
        addCandidate(searchPath);
    }
    if (this->version->workspace->searchInWorkspace)
    {
        for (auto& searchPath : this->version->workspace->workspaceSearchPaths)
        {
            auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::high_resolution_clock::now() - startTime)
                                   .count();
            if (elapsedTime > 200)
            {
                isIncomplete = true;
                break;
            }
            addCandidate(searchPath);
        }
    }
    for (auto& item : context.items)
    {
        item.textEdit.range.start.line = (int)lineIndex;
        item.textEdit.range.end.line = (int)lineIndex;
        if (!translateModuleName && item.kind == LanguageServerProtocol::kCompletionItemKindFile)
        {
            item.textEdit.range.start.character = (int)sectionStart;
            item.textEdit.range.end.character = (int)fileNameEnd;
            item.textEdit.newText = item.label;
            if (closingChar)
                item.textEdit.newText.appendChar(closingChar);
        }
        else
        {
            item.textEdit.newText = item.label;
            item.textEdit.range.start.character = (int)sectionStart;
            item.textEdit.range.end.character = (int)sectionEnd;
        }
    }

    if (!isIncomplete)
    {
        bool useCommitChars =
            translateModuleName && (commitCharacterBehavior != CommitCharacterBehavior::Disabled);
        if (useCommitChars)
        {
            if (translateModuleName)
            {
                for (auto& item : context.items)
                {
                    for (auto ch : getCommitChars())
                    {
                        item.commitCharacters.add(ch);
                    }
                }
            }
        }
    }
    return CompletionResult(_Move(context.items));
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteImport()
{
    const char* prefixes[] = {"import ", "__include ", "implementing "};
    UnownedStringSlice lineContent;
    Index pos = -1;
    for (auto prefix : prefixes)
    {
        auto importStr = UnownedStringSlice(prefix);
        lineContent = doc->getLine(line);
        pos = lineContent.indexOf(importStr);
        if (pos == -1)
            continue;
        auto lineBeforeImportKeyword = lineContent.head(pos).trim();
        if (lineBeforeImportKeyword.getLength() != 0 && lineBeforeImportKeyword != "__exported")
            continue;
        pos += importStr.getLength();
        goto validLine;
    }
    return SLANG_FAIL;
validLine:;
    while (pos < lineContent.getLength() && pos < col - 1 &&
           CharUtil::isWhitespace(lineContent[pos]))
        pos++;
    if (pos < lineContent.getLength() && lineContent[pos] == '"')
    {
        return tryCompleteRawFileName(lineContent, pos, true);
    }

    StringBuilder prefixSB;
    Index lastPos = col - 2;
    if (lastPos < 0)
        return SLANG_FAIL;
    while (lastPos >= pos && lineContent[lastPos] != '.')
    {
        if (lineContent[lastPos] == ';')
            return SLANG_FAIL;
        lastPos--;
    }
    UnownedStringSlice prefixSlice;
    if (lastPos > pos)
        prefixSlice = lineContent.subString(pos, lastPos - pos);
    Index sectionEnd = col - 1;
    while (sectionEnd < lineContent.getLength() &&
           (lineContent[sectionEnd] != '.' && lineContent[sectionEnd] != ';'))
        sectionEnd++;
    Index fileNameEnd = sectionEnd;
    while (fileNameEnd < lineContent.getLength() && lineContent[fileNameEnd] != ';')
        fileNameEnd++;
    for (auto ch : prefixSlice)
    {
        if (ch == '.')
            prefixSB.appendChar(Path::kOSCanonicalPathDelimiter);
        else if (ch == '_')
            prefixSB.appendChar('-');
        else
            prefixSB.appendChar(ch);
    }
    auto prefix = prefixSB.produceString();
    return gatherFileAndModuleCompletionItems(
        prefix,
        true,
        false,
        line - 1,
        fileNameEnd,
        lastPos + 1,
        sectionEnd,
        0);
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteRawFileName(
    UnownedStringSlice lineContent,
    Index pos,
    bool isImportString)
{
    while (pos < lineContent.getLength() && (lineContent[pos] != '\"' && lineContent[pos] != '<'))
        pos++;
    char closingChar = '"';
    if (pos < lineContent.getLength() && lineContent[pos] == '<')
        closingChar = '>';
    pos++;
    StringBuilder prefixSB;
    Index lastPos = col - 2;
    if (lastPos < 0)
        return SLANG_FAIL;
    while (lastPos >= pos && (lineContent[lastPos] != '/' && lineContent[lastPos] != '\\'))
    {
        if (lineContent[lastPos] == '\"' || lineContent[lastPos] == '>')
            return SLANG_FAIL;
        lastPos--;
    }
    Index sectionEnd = col - 1;
    if (sectionEnd < 0)
        return SLANG_FAIL;
    while (sectionEnd < lineContent.getLength() &&
           (lineContent[sectionEnd] != '\"' && lineContent[sectionEnd] != '>' &&
            lineContent[sectionEnd] != '/' && lineContent[sectionEnd] != '\\'))
    {
        sectionEnd++;
    }
    Index fileNameEnd = sectionEnd;
    while (fileNameEnd < lineContent.getLength() && lineContent[fileNameEnd] != ';')
        fileNameEnd++;
    UnownedStringSlice prefixSlice;
    if (lastPos > pos)
        prefixSlice = lineContent.subString(pos, lastPos - pos);
    for (auto ch : prefixSlice)
    {
        if (ch == '/' || ch == '\\')
            prefixSB.appendChar(Path::kOSCanonicalPathDelimiter);
        else
            prefixSB.appendChar(ch);
    }
    auto prefix = prefixSB.produceString();
    return gatherFileAndModuleCompletionItems(
        prefix,
        false,
        isImportString,
        line - 1,
        fileNameEnd,
        lastPos + 1,
        sectionEnd,
        closingChar);
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteInclude()
{
    auto lineContent = doc->getLine(line);
    if (!lineContent.startsWith("#"))
        return SLANG_FAIL;

    static auto includeStr = UnownedStringSlice("include ");
    Index pos = lineContent.indexOf(includeStr);
    if (pos == -1)
        return SLANG_FAIL;
    for (Index i = 1; i < pos; i++)
    {
        if (!CharUtil::isWhitespace(lineContent[i]))
            return SLANG_FAIL;
    }
    pos += includeStr.getLength();
    return tryCompleteRawFileName(lineContent, pos, false);
}

LanguageServerResult<CompletionResult> CompletionContext::tryCompleteMemberAndSymbol()
{
    return collectMembersAndSymbols();
}

String CompletionContext::formatDeclForCompletion(
    DeclRef<Decl> declRef,
    ASTBuilder* astBuilder,
    CompletionSuggestions::FormatMode formatMode,
    int& outNameStart)
{
    outNameStart = 0;
    switch (formatMode)
    {
    case CompletionSuggestions::FormatMode::Name:
        return getText(declRef.getDecl()->getName());
    default:
        break;
    }

    ASTPrinter printer(astBuilder, ASTPrinter::OptionFlag::ParamNames);
    if (auto genDecl = as<GenericDecl>(declRef))
        declRef = astBuilder->getMemberDeclRef(genDecl, genDecl.getDecl()->inner);
    auto callableDecl = as<CallableDecl>(declRef);
    if (!callableDecl)
        return String();
    if (formatMode == CompletionSuggestions::FormatMode::FullSignature)
    {
        printer.addType(callableDecl.getDecl()->returnType.type);
        printer.getStringBuilder() << " ";
    }
    outNameStart = (int)printer.getStringBuilder().getLength();
    printer.getStringBuilder() << getText(declRef.getDecl()->getName());
    auto outerGeneric = as<GenericDecl>(declRef.getParent());
    if (outerGeneric)
    {
        printer.addGenericParams(outerGeneric);
    }
    printer.addDeclParams(declRef);
    if (callableDecl.getDecl()->errorType.type != astBuilder->getBottomType() &&
        callableDecl.getDecl()->errorType.type != astBuilder->getErrorType())
    {
        printer.getStringBuilder() << " throws ";
        printer.addType(callableDecl.getDecl()->errorType);
    }
    if (outerGeneric)
    {
        for (auto constraint :
             outerGeneric.getDecl()->getMembersOfType<GenericTypeConstraintDecl>())
        {
            printer.getStringBuilder() << "\n";
            bool indentUsingTab = indent.startsWith("\t");
            if (indentUsingTab)
                printer.getStringBuilder() << "\t";
            else
                printer.getStringBuilder() << "    ";
            printer.getStringBuilder() << "where ";
            printer.addType(constraint->sub.type);
            if (constraint->isEqualityConstraint)
                printer.getStringBuilder() << " == ";
            else
                printer.getStringBuilder() << " : ";
            printer.addType(constraint->sup.type);
        }
    }
    return printer.getString();
}

// Returns true if `exprNode` is the same as `targetExpr`, or if the original expr node
// of `exprNode` before any checking/transformation is the same as `targetExpr`.
bool matchExpr(Expr* exprNode, SyntaxNode* targetExpr)
{
    if (!exprNode)
        return false;
    if (exprNode == targetExpr)
        return true;
    if (auto invokeExpr = as<AppExprBase>(exprNode))
        return matchExpr(invokeExpr->originalFunctionExpr, targetExpr);
    if (auto overloadedExpr = as<OverloadedExpr>(exprNode))
        return matchExpr(overloadedExpr->originalExpr, targetExpr);
    if (auto partiallyAppliedExpr = as<PartiallyAppliedGenericExpr>(exprNode))
        return matchExpr(partiallyAppliedExpr->originalExpr, targetExpr);
    if (auto extractExistentialExpr = as<ExtractExistentialValueExpr>(exprNode))
        return matchExpr(extractExistentialExpr->originalExpr, targetExpr);
    if (auto declRefExpr = as<DeclRefExpr>(exprNode))
        return matchExpr(declRefExpr->originalExpr, targetExpr);
    return false;
}

// Infer the accepted types at the completion position based on the AST nodes.
//
List<Type*> CompletionContext::getExpectedTypesAtCompletion(const List<ASTLookupResult>& astNodes)
{
    List<Type*> expectedType;
    if (astNodes.getCount() == 0)
        return expectedType;
    auto& path = astNodes.getFirst().path;
    if (path.getCount() < 2)
        return expectedType;
    auto completionExprNode = path.getLast();
    auto parentNode = path[path.getCount() - 2];
    auto collectArgumentType = [&](AppExprBase* appExpr, Index argIndex)
    {
        if (!appExpr)
            return;
        auto functionExpr = appExpr->functionExpr;
        if (!functionExpr)
            return;
        if (as<InvokeExpr>(appExpr))
        {
            // If we are in an invoke expr, we will use the parameter type of the
            // callee as the expected type.
            auto processDeclRefCallee = [&](DeclRef<Decl> calleeDeclRef)
            {
                auto decl = calleeDeclRef.getDecl();
                auto callableDecl = as<CallableDecl>(decl);
                if (!callableDecl)
                    return;
                Index paramIndex = 0;
                for (auto paramDeclRef :
                     getMembersOfType<ParamDecl>(version->linkage->getASTBuilder(), callableDecl))
                {
                    if (paramIndex == argIndex)
                    {
                        expectedType.add(getType(version->linkage->getASTBuilder(), paramDeclRef));
                        return;
                    }
                    paramIndex++;
                }
            };
            if (auto declRefExpr = as<DeclRefExpr>(functionExpr))
                processDeclRefCallee(declRefExpr->declRef);
            else if (auto overloadedExpr = as<OverloadedExpr>(functionExpr))
            {
                for (auto& lookupResult : overloadedExpr->lookupResult2)
                    processDeclRefCallee(lookupResult.declRef);
            }
        }
        else if (as<GenericAppExpr>(appExpr))
        {
            auto declRefExpr = as<DeclRefExpr>(functionExpr);
            if (!declRefExpr)
                return;
            auto genericDecl = as<GenericDecl>(declRefExpr->declRef.getDecl());
            if (!genericDecl)
                return;

            for (auto member : genericDecl->getMembers())
            {
                if (auto valParamDecl = as<GenericValueParamDecl>(member))
                {
                    if (valParamDecl->parameterIndex == argIndex)
                    {
                        expectedType.add(valParamDecl->type.type);
                        return;
                    }
                }
            }
        }
    };

    if (auto implicitCastExpr = as<ImplicitCastExpr>(parentNode))
    {
        // If the completion request is in (SomeType)(!completionRequest), then we should prefer any
        // candidates that has `SomeType`.
        if (implicitCastExpr->arguments.getCount() == 1 &&
            matchExpr(implicitCastExpr->arguments[0], completionExprNode))
        {
            if (as<DeclRefType>(implicitCastExpr->type.type))
                expectedType.add(implicitCastExpr->type.type);
        }
        return expectedType;
    }
    if (auto invokeExpr = as<AppExprBase>(parentNode))
    {
        // If parent node is an invoke expr, check if we are in an argument position.
        for (Index i = 0; i < invokeExpr->arguments.getCount(); i++)
        {
            if (matchExpr(invokeExpr->arguments[i], completionExprNode))
            {
                // If we are in an argument position, we will use the expected type of the
                // argument.
                collectArgumentType(invokeExpr, i);
                break;
            }
        }
        return expectedType;
    }
    if (auto varDecl = as<VarDeclBase>(parentNode))
    {
        if (!varDecl)
            return expectedType;
        if (!matchExpr(varDecl->initExpr, completionExprNode))
            return expectedType;
        if (as<DeclRefType>(varDecl->type.type))
        {
            expectedType.add(varDecl->type.type);
        }
        return expectedType;
    }
    return expectedType;
}

Index CompletionContext::determineCompletionItemSortOrder(
    Decl* item,
    const List<Type*>& expectedTypes)
{
    if (expectedTypes.getCount() == 0)
        return -1;

    // Test if `itemType` matches `expectedType`, and return the relevance of the match.
    // -1 means no match, a positive number means a match.
    // The smaller the number, the more relevant the match is, and the item will be listed
    // earlier in the completion list.
    auto matchType = [&](Type* itemType, DeclRefType* expectedType) -> Index
    {
        if (itemType == expectedType)
            return 1; // Exact match

        auto declRef = isDeclRefTypeOf<Decl>(itemType);
        if (!declRef)
            return -1; // No match

        if (declRef.getDecl() == expectedType->getDeclRef().getDecl())
            return 2; // Match by decl

        // We may also want to extend the matching logic to include subtyping or other
        // coercion relationships. But for now, we will just check for simple matches
        // to avoid performance problems.
        //
        return -1;
    };

    Index result = -1;

    // If we have any expected types, we will sort the completion candiate items by their relevance
    // to the expected types.
    // If the item has expected type, we will assign a sort order to make it appear at the top
    // of the completion list.
    for (auto et : expectedTypes)
    {
        Index currentSortOrder = -1;
        auto etDeclRefType = as<DeclRefType>(et);
        if (!etDeclRefType)
            continue;
        if (item == etDeclRefType->getDeclRef().getDecl())
        {
            if (as<EnumDecl>(item))
                currentSortOrder = 0;
            else if (!as<InterfaceDecl>(item))
                currentSortOrder = 1;
        }
        else if (auto varItem = as<VarDeclBase>(item))
        {
            currentSortOrder = matchType(varItem->type.type, etDeclRefType);
        }
        else if (auto callableItem = as<CallableDecl>(item))
        {
            // If the item is a callable decl, we will check if the return type matches the expected
            // type.
            currentSortOrder = matchType(callableItem->returnType.type, etDeclRefType);
        }
        if (result == -1 || (currentSortOrder != -1 && currentSortOrder < result))
        {
            // If we have a better match, we will update the result.
            result = currentSortOrder;
        }
    }
    // Always list decls within the same module first.
    // Note if result == 0, it means the item is representing the expected enum type itself,
    // so we always want to list it first by not increasing `result`.
    if (result > 0 && getModule(item) != parsedModule)
        result++;
    // List core module decls last.
    if (result > 0 && isFromCoreModule(item))
        result++;
    return result;
}

CompletionResult CompletionContext::collectMembersAndSymbols()
{
    List<LanguageServerProtocol::CompletionItem> result;
    auto linkage = version->linkage;
    if (linkage->contentAssistInfo.completionSuggestions.scopeKind ==
        CompletionSuggestions::ScopeKind::Swizzle)
    {
        createSwizzleCandidates(
            result,
            linkage->contentAssistInfo.completionSuggestions.swizzleBaseType,
            linkage->contentAssistInfo.completionSuggestions.elementCount);
    }
    else if (
        linkage->contentAssistInfo.completionSuggestions.scopeKind ==
        CompletionSuggestions::ScopeKind::Capabilities)
    {
        return createCapabilityCandidates();
    }
    bool useCommitChars = true;
    bool addKeywords = false;
    switch (linkage->contentAssistInfo.completionSuggestions.scopeKind)
    {
    case CompletionSuggestions::ScopeKind::Member:
    case CompletionSuggestions::ScopeKind::Swizzle:
        useCommitChars =
            (commitCharacterBehavior == CommitCharacterBehavior::MembersOnly ||
             commitCharacterBehavior == CommitCharacterBehavior::All);
        break;
    case CompletionSuggestions::ScopeKind::Expr:
    case CompletionSuggestions::ScopeKind::Decl:
    case CompletionSuggestions::ScopeKind::Stmt:
        useCommitChars = (commitCharacterBehavior == CommitCharacterBehavior::All);
        addKeywords = true;
        break;
    default:
        return result;
    }

    // If we are completing an override function signature, don't add keywords to the result.
    switch (linkage->contentAssistInfo.completionSuggestions.formatMode)
    {
    case CompletionSuggestions::FormatMode::FullSignature:
    case CompletionSuggestions::FormatMode::FuncSignatureWithoutReturnType:
        addKeywords = false;
        break;
    }
    auto lookupResults = findASTNodesAt(
        doc,
        version->linkage->getSourceManager(),
        parsedModule->getModuleDecl(),
        ASTLookupType::CompletionRequest,
        canonicalPath,
        line,
        col);
    auto expectedTypes = getExpectedTypesAtCompletion(lookupResults);
    HashSet<String> deduplicateSet;
    for (Index i = 0;
         i < linkage->contentAssistInfo.completionSuggestions.candidateItems.getCount();
         i++)
    {
        auto& suggestedItem = linkage->contentAssistInfo.completionSuggestions.candidateItems[i];
        auto member = suggestedItem.declRef.getDecl();
        if (auto genericDecl = as<GenericDecl>(member))
            member = genericDecl->inner;
        if (!member)
            continue;
        if (!member->getName())
            continue;
        LanguageServerProtocol::CompletionItem item;
        int nameStart = 0;
        item.label = formatDeclForCompletion(
            suggestedItem.declRef,
            linkage->m_astBuilder,
            linkage->contentAssistInfo.completionSuggestions.formatMode,
            nameStart);
        if (item.label.getLength() == 0)
            continue;
        if (linkage->contentAssistInfo.completionSuggestions.formatMode ==
            CompletionSuggestions::FormatMode::FullSignature)
        {
            // If the completion item is a `static` function, but there is no `static` keyword
            // on the current incomplete decl, then we will add `static` keyword to the completion
            // result.
            if (suggestedItem.declRef.getDecl() &&
                suggestedItem.declRef.getDecl()->findModifier<HLSLStaticModifier>() &&
                linkage->contentAssistInfo.completionSuggestions.currentPartialDecl &&
                !linkage->contentAssistInfo.completionSuggestions.currentPartialDecl
                     ->findModifier<HLSLStaticModifier>())
            {
                item.label = "static " + item.label;
                nameStart += 7;
                // Add an item for 'static' keyword.
                LanguageServerProtocol::CompletionItem staticItem;
                staticItem.label = "static";
                staticItem.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
                staticItem.data = "-1"; // Use -1 to indicate this is a keyword.
                result.add(staticItem);
            }
        }
        item.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
        if (as<TypeConstraintDecl>(member))
        {
            continue;
        }
        if (as<ConstructorDecl>(member))
        {
            continue;
        }
        if (as<SubscriptDecl>(member))
        {
            continue;
        }
        if (item.label.getLength() == 0)
            continue;
        if (!_isIdentifierChar(item.label[0]))
            continue;
        if (item.label.startsWith("$"))
            continue;
        if (!deduplicateSet.add(item.label))
            continue;

        if (as<StructDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindStruct;
        }
        else if (as<ClassDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindClass;
        }
        else if (as<InterfaceDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindInterface;
        }
        else if (as<SimpleTypeDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindClass;
        }
        else if (as<PropertyDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindProperty;
        }
        else if (as<EnumDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindEnum;
        }
        else if (as<VarDeclBase>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindVariable;
        }
        else if (as<EnumCaseDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindEnumMember;
        }
        else if (as<CallableDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindMethod;
        }
        else if (as<AssocTypeDecl>(member))
        {
            item.kind = LanguageServerProtocol::kCompletionItemKindClass;
        }
        item.data = String(i);

        Index sortOrder = determineCompletionItemSortOrder(member, expectedTypes);
        if (sortOrder != -1)
        {
            item.sortText =
                (StringBuilder() << sortOrder << ":" << getText(member->getName())).produceString();
        }
        result.add(item);
        if (nameStart > 1)
        {
            // If the completion item is for a full function signature, add the return type part too
            // as a separate item.
            item.label = item.label.getUnownedSlice().head(nameStart - 1);
            item.kind = LanguageServerProtocol::kCompletionItemKindStruct;
            item.sortText =
                (StringBuilder()
                 << linkage->contentAssistInfo.completionSuggestions.candidateItems.getCount()
                 << ":" << item.label)
                    .produceString();
            result.add(item);
        }
    }
    if (addKeywords)
    {
        if (linkage->contentAssistInfo.completionSuggestions.scopeKind ==
            CompletionSuggestions::ScopeKind::Decl)
        {
            for (auto keyword : kDeclKeywords)
            {
                if (!deduplicateSet.add(keyword))
                    continue;
                LanguageServerProtocol::CompletionItem item;
                item.label = keyword;
                item.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
                item.data = "-1";
                result.add(item);
            }
        }
        else
        {
            for (auto keyword : kStmtKeywords)
            {
                if (!deduplicateSet.add(keyword))
                    continue;
                LanguageServerProtocol::CompletionItem item;
                item.label = keyword;
                item.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
                item.data = "-1";
                result.add(item);
            }
        }

        for (auto& def : linkage->contentAssistInfo.preprocessorInfo.macroDefinitions)
        {
            if (!def.name)
                continue;
            auto& text = def.name->text;
            if (!deduplicateSet.add(text))
                continue;
            LanguageServerProtocol::CompletionItem item;
            item.label = text;
            item.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
            item.data = "-1";
            result.add(item);
        }
    }
    if (useCommitChars)
    {
        for (auto& item : result)
        {
            for (auto ch : getCommitChars())
                item.commitCharacters.add(ch);
        }
    }
    return result;
}

CompletionResult CompletionContext::createCapabilityCandidates()
{
    List<LanguageServerProtocol::CompletionItem> result;
    List<UnownedStringSlice> names;
    getCapabilityNames(names);
    for (auto name : names.getArrayView(1, names.getCount() - 1))
    {
        if (name.startsWith("_"))
            continue;
        LanguageServerProtocol::CompletionItem item;
        item.data = 0;
        item.kind = LanguageServerProtocol::kCompletionItemKindEnumMember;
        item.label = name;
        result.add(item);
    }
    return result;
}

void CompletionContext::createSwizzleCandidates(
    List<LanguageServerProtocol::CompletionItem>& result,
    Type* type,
    IntegerLiteralValue elementCount[2])
{
    // Hard code members for vector and matrix types.
    if (auto vectorType = as<VectorExpressionType>(type))
    {
        const char* memberNames[4] = {"x", "y", "z", "w"};
        Type* elementType = nullptr;
        elementType = vectorType->getElementType();
        String typeStr;
        if (elementType)
            typeStr = elementType->toString();
        auto count = Math::Min((int)elementCount[0], 4);
        for (int i = 0; i < count; i++)
        {
            LanguageServerProtocol::CompletionItem item;
            item.data = 0;
            item.detail = typeStr;
            item.kind = LanguageServerProtocol::kCompletionItemKindVariable;
            item.label = memberNames[i];
            result.add(item);
        }
    }
    else if (auto scalarType = as<BasicExpressionType>(type))
    {
        String typeStr;
        typeStr = scalarType->toString();
        LanguageServerProtocol::CompletionItem item;
        item.data = 0;
        item.detail = typeStr;
        item.kind = LanguageServerProtocol::kCompletionItemKindVariable;
        item.label = "x";
        result.add(item);
    }
    else if (auto matrixType = as<MatrixExpressionType>(type))
    {
        Type* elementType = nullptr;
        elementType = matrixType->getElementType();
        String typeStr;
        if (elementType)
        {
            typeStr = elementType->toString();
        }
        int rowCount = Math::Min((int)elementCount[0], 4);
        int colCount = Math::Min((int)elementCount[1], 4);
        StringBuilder nameSB;
        for (int i = 0; i < rowCount; i++)
        {
            for (int j = 0; j < colCount; j++)
            {
                LanguageServerProtocol::CompletionItem item;
                item.data = 0;
                item.detail = typeStr;
                item.kind = LanguageServerProtocol::kCompletionItemKindVariable;
                nameSB.clear();
                nameSB << "_m" << i << j;
                item.label = nameSB.toString();
                result.add(item);
                nameSB.clear();
                nameSB << "_" << i + 1 << j + 1;
                item.label = nameSB.toString();
                result.add(item);
            }
        }
    }
    else if (auto tupleType = as<TupleType>(type))
    {
        auto count = Math::Min((int)elementCount[0], 4);
        for (int i = 0; i < count; i++)
        {
            LanguageServerProtocol::CompletionItem item;
            item.data = 0;
            if (tupleType->getMember(i))
                item.detail = tupleType->getMember(i)->toString();
            item.kind = LanguageServerProtocol::kCompletionItemKindVariable;
            item.label = String("_") + String(i);
            result.add(item);
        }
    }
}

LanguageServerProtocol::CompletionItem CompletionContext::generateGUIDCompletionItem()
{
    StringBuilder sb;
    sb << "COM(\"";
    auto docHash = doc->getURI().getHashCode() ^ doc->getText().getHashCode();
    int sectionLengths[] = {8, 4, 4, 4, 12};
    srand((unsigned int)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    auto hashStr = String(docHash, 16);
    sectionLengths[0] -= (int)hashStr.getLength();
    sb << hashStr;
    for (Index j = 0; j < SLANG_COUNT_OF(sectionLengths); j++)
    {
        auto len = sectionLengths[j];
        if (j != 0)
            sb << "-";
        for (int i = 0; i < len; i++)
        {
            auto digit = rand() % 16;
            if (digit < 10)
                sb << digit;
            else
                sb.appendChar((char)('A' + digit - 10));
        }
    }
    sb << "\")";
    LanguageServerProtocol::CompletionItem resultItem;
    resultItem.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
    resultItem.label = sb.produceString();
    return resultItem;
}

CompletionResult CompletionContext::collectAttributes()
{
    List<LanguageServerProtocol::CompletionItem> result;
    for (auto& item : version->linkage->contentAssistInfo.completionSuggestions.candidateItems)
    {
        if (auto attrDecl = as<AttributeDecl>(item.declRef.getDecl()))
        {
            if (attrDecl->getName())
            {
                LanguageServerProtocol::CompletionItem resultItem;
                resultItem.kind = LanguageServerProtocol::kCompletionItemKindKeyword;
                resultItem.label = attrDecl->getName()->text;
                result.add(resultItem);
            }
        }
        else if (auto decl = as<AggTypeDecl>(item.declRef.getDecl()))
        {
            if (decl->getName())
            {
                LanguageServerProtocol::CompletionItem resultItem;
                resultItem.kind = LanguageServerProtocol::kCompletionItemKindStruct;
                resultItem.label = decl->getName()->text;
                if (resultItem.label.endsWith("Attribute"))
                    resultItem.label.reduceLength(resultItem.label.getLength() - 9);
                result.add(resultItem);
            }
        }
    }

    // Add a suggestion for `[COM]` attribute with generated GUID.
    auto guidItem = generateGUIDCompletionItem();
    result.add(guidItem);
    return result;
}

} // namespace Slang
