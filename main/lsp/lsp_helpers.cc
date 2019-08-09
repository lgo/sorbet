#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "common/FileOps.h"
#include "lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

constexpr string_view sorbetScheme = "sorbet:";
constexpr string_view httpsScheme = "https";

string LSPLoop::remoteName2Local(string_view uri) const {
    const bool isSorbetURI = absl::StartsWith(uri, sorbetScheme);
    if (!absl::StartsWith(uri, rootUri) && !enableSorbetURIs && !isSorbetURI) {
        logger->error("Unrecognized URI received from client: {}", uri);
        return string(uri);
    }

    const string_view root = isSorbetURI ? sorbetScheme : rootUri;
    const char *start = uri.data() + root.length();
    if (*start == '/') {
        ++start;
    }

    string path = string(start, uri.end());
    // Note: May be `https://` or `https%3A//`. VS Code URLencodes the : in sorbet:https:// paths.
    const bool isHttps = isSorbetURI && absl::StartsWith(path, httpsScheme) && path.length() > httpsScheme.length() &&
                         (path[httpsScheme.length()] == ':' || path[httpsScheme.length()] == '%');
    if (isHttps) {
        // URL decode the :
        return absl::StrReplaceAll(path, {{"%3A", ":"}});
    } else if (rootPath.length() > 0) {
        return absl::StrCat(rootPath, "/", path);
    } else {
        // Special case: Folder is '' (current directory)
        return path;
    }
}

string LSPLoop::localName2Remote(string_view uri, bool useSorbetUri) const {
    ENFORCE(absl::StartsWith(uri, rootPath));
    string_view relativeUri = uri.substr(rootPath.length());
    if (relativeUri.at(0) == '/') {
        relativeUri = relativeUri.substr(1);
    }

    // Special case: Root uri is '' (happens in Monaco)
    if (rootUri.length() == 0) {
        return string(relativeUri);
    }

    if (useSorbetUri) {
        return absl::StrCat(sorbetScheme, relativeUri);
    }
    return absl::StrCat(rootUri, "/", relativeUri);
}

core::FileRef LSPLoop::uri2FileRef(string_view uri) const {
    if (!absl::StartsWith(uri, rootUri) && !absl::StartsWith(uri, sorbetScheme)) {
        return core::FileRef();
    }
    auto needle = remoteName2Local(uri);
    return initialGS->findFileByPath(needle);
}

string LSPLoop::fileRef2Uri(const core::GlobalState &gs, core::FileRef file) const {
    string uri;
    if (!file.exists()) {
        uri = "???";
    } else {
        auto &messageFile = file.data(gs);
        if (messageFile.isPayload()) {
            if (enableSorbetURIs) {
                uri = absl::StrCat(sorbetScheme, messageFile.path());
            } else {
                uri = string(messageFile.path());
            }
        } else {
            // Tell localName2Remote to use a sorbet: URI if the file is not present on the client AND the client
            // supports sorbet: URIs
            uri = localName2Remote(file.data(gs).path(),
                                   enableSorbetURIs && FileOps::isFileIgnored(rootPath, messageFile.path(),
                                                                              opts.lspDirsMissingFromClient, {}));
        }
    }
    return uri;
} // namespace sorbet::realmain::lsp

unique_ptr<Range> loc2Range(const core::GlobalState &gs, core::Loc loc) {
    if (!loc.exists()) {
        // this will happen if e.g. we disable the stdlib (e.g. to speed up testing in fuzzers).
        return nullptr;
    }
    auto pair = loc.position(gs);
    // All LSP numbers are zero-based, ours are 1-based.
    return make_unique<Range>(make_unique<Position>(pair.first.line - 1, pair.first.column - 1),
                              make_unique<Position>(pair.second.line - 1, pair.second.column - 1));
}

unique_ptr<core::Loc> range2Loc(const core::GlobalState &gs, const Range &range, core::FileRef file) {
    ENFORCE(range.start->line >= 0);
    ENFORCE(range.start->character >= 0);
    ENFORCE(range.end->line >= 0);
    ENFORCE(range.end->character >= 0);

    auto start = core::Loc::pos2Offset(file.data(gs),
                                       core::Loc::Detail{(u4)range.start->line + 1, (u4)range.start->character + 1});
    auto end =
        core::Loc::pos2Offset(file.data(gs), core::Loc::Detail{(u4)range.end->line + 1, (u4)range.end->character + 1});

    return make_unique<core::Loc>(file, start, end);
}

unique_ptr<Location> LSPLoop::loc2Location(const core::GlobalState &gs, core::Loc loc) const {
    auto range = loc2Range(gs, loc);
    if (range == nullptr) {
        return nullptr;
    }
    string uri = fileRef2Uri(gs, loc.file());
    if (loc.file().exists() && loc.file().data(gs).isPayload() && !enableSorbetURIs) {
        // This is hacky because VSCode appends #4,3 (or whatever the position is of the
        // error) to the uri before it shows it in the UI since this is the format that
        // VSCode uses to denote which location to jump to. However, if you append #L4
        // to the end of the uri, this will work on github (it will ignore the #4,3)
        //
        // As an example, in VSCode, on hover you might see
        //
        // string.rbi(18,7): Method `+` has specified type of argument `arg0` as `String`
        //
        // When you click on the link, in the browser it appears as
        // https://git.corp.stripe.com/stripe-internal/ruby-typer/tree/master/rbi/core/string.rbi#L18%2318,7
        // but shows you the same thing as
        // https://git.corp.stripe.com/stripe-internal/ruby-typer/tree/master/rbi/core/string.rbi#L18
        uri = fmt::format("{}#L{}", uri, loc.position(gs).first.line);
    }
    return make_unique<Location>(uri, std::move(range));
}

int cmpPositions(const Position &a, const Position &b) {
    const int line = a.line - b.line;
    if (line != 0) {
        return line;
    }
    return a.character - b.character;
}

int cmpLocations(const Location &a, const Location &b) {
    const int fileCmp = a.uri.compare(b.uri);
    if (fileCmp != 0) {
        return fileCmp;
    }
    const int startCmp = cmpPositions(*a.range->start, *b.range->start);
    if (startCmp != 0) {
        return startCmp;
    }
    return cmpPositions(*a.range->end, *b.range->end);
}

int cmpRanges(const Range &a, const Range &b) {
    const int cmpStart = cmpPositions(*a.start, *b.start);
    if (cmpStart != 0) {
        // One starts before the other.
        return cmpStart;
    }
    return cmpPositions(*a.end, *b.end);
}

vector<unique_ptr<Location>>
LSPLoop::extractLocations(const core::GlobalState &gs,
                          const vector<unique_ptr<core::lsp::QueryResponse>> &queryResponses,
                          vector<unique_ptr<Location>> locations) const {
    for (auto &q : queryResponses) {
        core::Loc loc = q->getLoc();
        if (loc.exists() && loc.file().exists()) {
            auto fileIsTyped = loc.file().data(gs).strictLevel >= core::StrictLevel::True;
            // If file is untyped, only support responses involving constants and definitions.
            if (fileIsTyped || q->isConstant() || q->isDefinition()) {
                addLocIfExists(gs, locations, loc);
            }
        }
    }
    // Dedupe locations
    fast_sort(locations, [](const unique_ptr<Location> &a, const unique_ptr<Location> &b) -> bool {
        return cmpLocations(*a, *b) < 0;
    });
    locations.resize(std::distance(
        locations.begin(), std::unique(locations.begin(), locations.end(),
                                       [](const unique_ptr<Location> &a, const unique_ptr<Location> &b) -> bool {
                                           return cmpLocations(*a, *b) == 0;
                                       })));
    return locations;
}

bool hideSymbol(const core::GlobalState &gs, core::SymbolRef sym) {
    if (!sym.exists() || sym == core::Symbols::root()) {
        return true;
    }
    auto data = sym.data(gs);
    if (data->isClass() && data->attachedClass(gs).exists()) {
        return true;
    }
    if (data->isClass() && data->superClass() == core::Symbols::StubModule()) {
        return true;
    }
    // this doesn't work. there are some <static-init> that still make it through. changing the check to just
    //
    //   data->name == core::Names::staticInit()
    //
    // works better (it prevents more <static-init> from getting through), but it allows the root <static-init> through.
    if (data->name.data(gs)->kind == core::NameKind::UNIQUE &&
        data->name.data(gs)->unique.original == core::Names::staticInit()) {
        return true;
    }
    if (data->name.data(gs)->kind == core::NameKind::UNIQUE &&
        data->name.data(gs)->unique.original == core::Names::blockTemp()) {
        return true;
    }
    return false;
}

bool hasSimilarName(const core::GlobalState &gs, core::NameRef name, string_view pattern) {
    string_view view = name.data(gs)->shortName(gs);
    auto fnd = view.find(pattern);
    return fnd != string_view::npos;
}

string methodDetail(const core::GlobalState &gs, core::SymbolRef method, core::TypePtr receiver, core::TypePtr retType,
                    const unique_ptr<core::TypeConstraint> &constraint) {
    ENFORCE(method.exists());
    // handle this case anyways so that we don't crash in prod when this method is mis-used
    if (!method.exists()) {
        return "";
    }

    if (!retType) {
        retType = getResultType(gs, method.data(gs)->resultType, method, receiver, constraint);
    }
    string methodReturnType =
        (retType == core::Types::void_()) ? "void" : absl::StrCat("returns(", retType->show(gs), ")");
    vector<string> typeAndArgNames;

    vector<string> flags;
    const core::SymbolData &sym = method.data(gs);
    string accessFlagString = "";
    if (sym->isMethod()) {
        if (sym->hasGeneratedSig()) {
            flags.emplace_back("generated");
        }
        if (sym->isAbstract()) {
            flags.emplace_back("abstract");
        }
        if (sym->isOverridable()) {
            flags.emplace_back("overridable");
        }
        if (sym->isOverride()) {
            flags.emplace_back("override");
        }
        if (sym->isImplementation()) {
            flags.emplace_back("implementation");
        }
        if (sym->isPrivate()) {
            accessFlagString = "private ";
        }
        if (sym->isProtected()) {
            accessFlagString = "protected ";
        }
        for (auto &argSym : method.data(gs)->arguments()) {
            // Don't display synthetic arguments (like blk).
            if (!argSym.isSyntheticBlockArgument()) {
                typeAndArgNames.emplace_back(
                    absl::StrCat(argSym.argumentName(gs), ": ",
                                 getResultType(gs, argSym.type, method, receiver, constraint)->show(gs)));
            }
        }
    }

    string flagString = "";
    if (!flags.empty()) {
        flagString = fmt::format("{}.", fmt::join(flags, "."));
    }
    string paramsString = "";
    if (!typeAndArgNames.empty()) {
        paramsString = fmt::format("params({}).", fmt::join(typeAndArgNames, ", "));
    }
    return fmt::format("{}sig {{{}{}{}}}", accessFlagString, flagString, paramsString, methodReturnType);
}

core::TypePtr getResultType(const core::GlobalState &gs, core::TypePtr type, core::SymbolRef inWhat,
                            core::TypePtr receiver, const unique_ptr<core::TypeConstraint> &constr) {
    core::Context ctx(gs, inWhat);
    auto resultType = type;
    if (auto *proxy = core::cast_type<core::ProxyType>(receiver.get())) {
        receiver = proxy->underlying();
    }
    if (auto *applied = core::cast_type<core::AppliedType>(receiver.get())) {
        /* instantiate generic classes */
        resultType = core::Types::resultTypeAsSeenFrom(ctx, resultType, inWhat.data(ctx)->enclosingClass(ctx),
                                                       applied->klass, applied->targs);
    }
    if (!resultType) {
        resultType = core::Types::untypedUntracked();
    }
    if (receiver) {
        resultType = core::Types::replaceSelfType(ctx, resultType, receiver); // instantiate self types
    }
    if (constr) {
        resultType = core::Types::instantiate(ctx, resultType, *constr); // instantiate generic methods
    }
    return resultType;
}

SymbolKind symbolRef2SymbolKind(const core::GlobalState &gs, core::SymbolRef symbol) {
    auto sym = symbol.data(gs);
    if (sym->isClass()) {
        if (sym->isClassModule()) {
            return SymbolKind::Module;
        }
        if (sym->isClassClass()) {
            return SymbolKind::Class;
        }
    } else if (sym->isMethod()) {
        if (sym->name == core::Names::initialize()) {
            return SymbolKind::Constructor;
        } else {
            return SymbolKind::Method;
        }
    } else if (sym->isField()) {
        return SymbolKind::Field;
    } else if (sym->isStaticField()) {
        return SymbolKind::Constant;
    } else if (sym->isTypeMember()) {
        return SymbolKind::TypeParameter;
    } else if (sym->isTypeArgument()) {
        return SymbolKind::TypeParameter;
    }
    return SymbolKind::Unknown;
}

} // namespace sorbet::realmain::lsp
