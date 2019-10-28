#include "absl/strings/match.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/ShowOperation.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

vector<unique_ptr<Location>> LSPLoop::getReferencesToSymbol(LSPTypechecker &typechecker, core::SymbolRef symbol,
                                                            vector<unique_ptr<Location>> locations) const {
    if (symbol.exists()) {
        auto run2 = queryBySymbol(typechecker, symbol);
        locations = extractLocations(typechecker.state(), run2.responses, move(locations));
    }
    return locations;
}

unique_ptr<ResponseMessage> LSPLoop::handleTextDocumentReferences(LSPTypechecker &typechecker, const MessageId &id,
                                                                  const ReferenceParams &params) const {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentReferences);
    ShowOperation op(*config, "References", "Finding all references...");
    prodCategoryCounterInc("lsp.messages.processed", "textDocument.references");

    const core::GlobalState &gs = typechecker.state();
    auto result =
        queryByLoc(typechecker, params.textDocument->uri, *params.position, LSPMethod::TextDocumentReferences, false);
    if (result.error) {
        // An error happened while setting up the query.
        response->error = move(result.error);
    } else {
        // An explicit null indicates that we don't support this request (or that nothing was at the location).
        // Note: Need to correctly type variant here so it goes into right 'slot' of result variant.
        response->result = variant<JSONNullObject, vector<unique_ptr<Location>>>(JSONNullObject());
        auto &queryResponses = result.responses;
        if (!queryResponses.empty()) {
            const bool fileIsTyped =
                config->uri2FileRef(gs, params.textDocument->uri).data(gs).strictLevel >= core::StrictLevel::True;
            auto resp = move(queryResponses[0]);
            // N.B.: Ignores literals.
            // If file is untyped, only supports find reference requests from constants and class definitions.
            if (auto constResp = resp->isConstant()) {
                response->result = getReferencesToSymbol(typechecker, constResp->symbol);
            } else if (auto defResp = resp->isDefinition()) {
                if (fileIsTyped || defResp->symbol.data(gs)->isClassOrModule()) {
                    response->result = getReferencesToSymbol(typechecker, defResp->symbol);
                }
            } else if (fileIsTyped && resp->isIdent()) {
                auto identResp = resp->isIdent();
                auto loc = identResp->owner.data(gs)->loc();
                if (loc.exists()) {
                    auto run2 = typechecker.query(
                        core::lsp::Query::createVarQuery(identResp->owner, identResp->variable), {loc.file()});
                    response->result = extractLocations(gs, run2.responses);
                }
            } else if (fileIsTyped && resp->isSend()) {
                auto sendResp = resp->isSend();
                auto start = sendResp->dispatchResult.get();
                vector<unique_ptr<Location>> locations;
                while (start != nullptr) {
                    if (start->main.method.exists() && !start->main.receiver->isUntyped()) {
                        locations = getReferencesToSymbol(typechecker, start->main.method, move(locations));
                    }
                    start = start->secondary.get();
                }
                response->result = move(locations);
            }
        }
    }
    return response;
}

} // namespace sorbet::realmain::lsp
