
#include "absl/strings/match.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

vector<unique_ptr<DocumentHighlight>>
LSPLoop::getHighlightsToSymbolInFile(LSPTypechecker &typechecker, string_view const uri, core::SymbolRef symbol,
                                     vector<unique_ptr<DocumentHighlight>> highlights) const {
    if (symbol.exists()) {
        auto run2 = queryBySymbol(typechecker, symbol, uri);
        auto locations = extractLocations(typechecker.state(), run2.responses);
        for (auto const &location : locations) {
            auto highlight = make_unique<DocumentHighlight>(move(location->range));
            highlights.push_back(move(highlight));
        }
    }
    return highlights;
}

vector<unique_ptr<DocumentHighlight>> locationsToDocumentHighlights(vector<unique_ptr<Location>> const locations) {
    vector<unique_ptr<DocumentHighlight>> highlights;
    for (auto const &location : locations) {
        auto highlight = make_unique<DocumentHighlight>(move(location->range));
        highlights.push_back(move(highlight));
    }
    return highlights;
}

unique_ptr<ResponseMessage>
LSPLoop::handleTextDocumentDocumentHighlight(LSPTypechecker &typechecker, const MessageId &id,
                                             const TextDocumentPositionParams &params) const {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentDocumentHighlight);
    if (!config->opts.lspDocumentHighlightEnabled) {
        response->error = make_unique<ResponseError>(
            (int)LSPErrorCodes::InvalidRequest, "The `Highlight` LSP feature is experimental and disabled by default.");
        return response;
    }

    prodCategoryCounterInc("lsp.messages.processed", "textDocument.documentHighlight");
    const core::GlobalState &gs = typechecker.state();
    auto uri = params.textDocument->uri;
    auto result = queryByLoc(typechecker, params.textDocument->uri, *params.position,
                             LSPMethod::TextDocumentDocumentHighlight, false);
    if (result.error) {
        // An error happened while setting up the query.
        response->error = move(result.error);
    } else {
        // An explicit null indicates that we don't support this request (or that nothing was at the location).
        // Note: Need to correctly type variant here so it goes into right 'slot' of result variant.
        response->result = variant<JSONNullObject, vector<unique_ptr<DocumentHighlight>>>(JSONNullObject());
        auto &queryResponses = result.responses;
        if (!queryResponses.empty()) {
            auto file = config->uri2FileRef(gs, uri);
            if (!file.exists()) {
                return response;
            }
            const bool fileIsTyped = file.data(gs).strictLevel >= core::StrictLevel::True;
            auto resp = move(queryResponses[0]);
            // N.B.: Ignores literals.
            // If file is untyped, only supports find reference requests from constants and class definitions.
            if (auto constResp = resp->isConstant()) {
                response->result = getHighlightsToSymbolInFile(typechecker, uri, constResp->symbol);
            } else if (auto defResp = resp->isDefinition()) {
                if (fileIsTyped || defResp->symbol.data(gs)->isClassOrModule()) {
                    response->result = getHighlightsToSymbolInFile(typechecker, uri, defResp->symbol);
                }
            } else if (fileIsTyped && resp->isIdent()) {
                auto identResp = resp->isIdent();
                auto loc = identResp->owner.data(gs)->loc();
                if (loc.exists()) {
                    auto run2 = typechecker.query(
                        core::lsp::Query::createVarQuery(identResp->owner, identResp->variable), {loc.file()});
                    auto locations = extractLocations(gs, run2.responses);
                    response->result = locationsToDocumentHighlights(move(locations));
                }
            } else if (fileIsTyped && resp->isSend()) {
                auto sendResp = resp->isSend();
                auto start = sendResp->dispatchResult.get();
                vector<unique_ptr<DocumentHighlight>> highlights;
                while (start != nullptr) {
                    if (start->main.method.exists() && !start->main.receiver->isUntyped()) {
                        highlights =
                            getHighlightsToSymbolInFile(typechecker, uri, start->main.method, move(highlights));
                    }
                    start = start->secondary.get();
                }
                response->result = move(highlights);
            }
        }
    }
    return response;
}

} // namespace sorbet::realmain::lsp
