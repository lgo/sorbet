#include "core/lsp/QueryResponse.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {
std::unique_ptr<DocumentSymbol> symbolRef2DocumentSymbol(const core::GlobalState &gs, core::SymbolRef symRef,
                                                         core::FileRef filter);

void symbolRef2DocumentSymbolWalkMembers(const core::GlobalState &gs, core::SymbolRef sym, core::FileRef filter,
                                         vector<unique_ptr<DocumentSymbol>> &out) {
    for (auto mem : sym.data(gs)->membersStableOrderSlow(gs)) {
        if (mem.first != core::Names::attached() && mem.first != core::Names::singleton()) {
            bool foundThisFile = false;
            for (auto loc : mem.second.data(gs)->locs()) {
                foundThisFile = foundThisFile || loc.file() == filter;
            }
            if (!foundThisFile) {
                continue;
            }
            auto inner = symbolRef2DocumentSymbol(gs, mem.second, filter);
            if (inner) {
                out.push_back(move(inner));
            }
        }
    }
}

std::unique_ptr<DocumentSymbol> symbolRef2DocumentSymbol(const core::GlobalState &gs, core::SymbolRef symRef,
                                                         core::FileRef filter) {
    if (!symRef.exists()) {
        return nullptr;
    }
    auto sym = symRef.data(gs);
    if (!sym->loc().file().exists() || hideSymbol(gs, symRef)) {
        return nullptr;
    }
    auto kind = symbolRef2SymbolKind(gs, symRef);
    // TODO: this range should cover body. Currently it doesn't.
    auto range = loc2Range(gs, sym->loc());
    auto selectionRange = loc2Range(gs, sym->loc());
    if (range == nullptr || selectionRange == nullptr) {
        return nullptr;
    }

    string prefix = "";
    if (sym->owner.exists() && sym->owner.data(gs)->isClass() && sym->owner.data(gs)->attachedClass(gs).exists()) {
        prefix = "self.";
    }
    auto result = make_unique<DocumentSymbol>(prefix + sym->name.show(gs), kind, move(range), move(selectionRange));
    if (sym->isMethod()) {
        result->detail = methodDetail(gs, symRef, nullptr, nullptr, nullptr);
    } else {
        // Currently released version of VSCode has a bug that requires this non-optional field to be present
        result->detail = "";
    }

    vector<unique_ptr<DocumentSymbol>> children;
    symbolRef2DocumentSymbolWalkMembers(gs, symRef, filter, children);
    if (sym->isClass()) {
        auto singleton = sym->lookupSingletonClass(gs);
        if (singleton.exists()) {
            symbolRef2DocumentSymbolWalkMembers(gs, singleton, filter, children);
        }
    }
    result->children = move(children);
    return result;
}

LSPResult LSPLoop::handleTextDocumentDocumentSymbol(unique_ptr<core::GlobalState> gs, const MessageId &id,
                                                    const DocumentSymbolParams &params) const {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentDocumentSymbol);
    if (!opts.lspDocumentSymbolEnabled) {
        response->error =
            make_unique<ResponseError>((int)LSPErrorCodes::InvalidRequest,
                                       "The `Document Symbol` LSP feature is experimental and disabled by default.");
        return LSPResult::make(move(gs), move(response));
    }

    prodCategoryCounterInc("lsp.messages.processed", "textDocument.documentSymbol");
    vector<unique_ptr<DocumentSymbol>> result;
    string_view uri = params.textDocument->uri;
    auto fref = uri2FileRef(uri);
    for (u4 idx = 1; idx < gs->symbolsUsed(); idx++) {
        core::SymbolRef ref(gs.get(), idx);
        if (!hideSymbol(*gs, ref) &&
            // a bit counter-intuitive, but this actually should be `!= fref`, as it prevents duplicates.
            (ref.data(*gs)->owner.data(*gs)->loc().file() != fref || ref.data(*gs)->owner == core::Symbols::root())) {
            for (auto definitionLocation : ref.data(*gs)->locs()) {
                if (definitionLocation.file() == fref) {
                    auto data = symbolRef2DocumentSymbol(*gs, ref, fref);
                    if (data) {
                        result.push_back(move(data));
                        break;
                    }
                }
            }
        }
    }
    response->result = move(result);
    return LSPResult::make(move(gs), move(response));
}

} // namespace sorbet::realmain::lsp
