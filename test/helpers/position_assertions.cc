#include "gtest/gtest.h"
// ^ Include first because it violates linting rules.

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "common/FileOps.h"
#include "test/helpers/lsp.h"
#include "test/helpers/position_assertions.h"
#include <regex>

using namespace std;

namespace sorbet::test {
// Matches '    #    ^^^^^ label: dafhdsjfkhdsljkfh*&#&*%'
// and '    # label: foobar'.
const regex rangeAssertionRegex("(#[ ]*)(\\^*)[ ]*([a-zA-Z-]+): (.*)$");

const regex whitespaceRegex("^[ ]*$");

// Maps assertion comment names to their constructors.
const UnorderedMap<
    string, function<shared_ptr<RangeAssertion>(string_view, unique_ptr<Range> &, int, string_view, string_view)>>
    assertionConstructors = {
        {"error", ErrorAssertion::make},
        {"error-with-dupes", ErrorAssertion::make},
        {"usage", UsageAssertion::make},
        {"def", DefAssertion::make},
        {"type", TypeAssertion::make},
        {"type-def", TypeDefAssertion::make},
        {"disable-fast-path", BooleanPropertyAssertion::make},
        {"disable-stress-incremental", BooleanPropertyAssertion::make},
        {"exhaustive-apply-code-action", BooleanPropertyAssertion::make},
        {"assert-fast-path", FastPathAssertion::make},
        {"assert-slow-path", BooleanPropertyAssertion::make},
        {"hover", HoverAssertion::make},
        {"completion", CompletionAssertion::make},
        {"apply-completion", ApplyCompletionAssertion::make},
        {"apply-code-action", ApplyCodeActionAssertion::make},
        {"no-stdlib", BooleanPropertyAssertion::make},
};

// Ignore any comments that have these labels (e.g. `# typed: true`).
const UnorderedSet<string> ignoredAssertionLabels = {"typed", "TODO", "linearization", "commented-out-error"};

constexpr string_view NOTHING_LABEL = "(nothing)"sv;
constexpr string_view NULL_LABEL = "null"sv;

/** Returns true if `b` is a subset of `a`. Only works on single-line ranges. Assumes ranges are well-formed (start <=
 * end) */
bool rangeIsEqual(const Range &a, const Range &b) {
    return (a.start->line == b.start->line && a.end->line == b.end->line && a.start->character == b.start->character &&
            a.end->character == b.end->character);
}

string prettyPrintRangeComment(string_view sourceLine, const Range &range, string_view comment) {
    int numLeadingSpaces = range.start->character;
    if (numLeadingSpaces < 0) {
        ADD_FAILURE() << fmt::format("Invalid range: {} < 0", range.start->character);
        return "";
    }
    string sourceLineNumber = fmt::format("{}", range.start->line + 1);
    EXPECT_EQ(range.start->line, range.end->line) << "Multi-line ranges are not supported at this time.";
    if (range.start->line != range.end->line) {
        return string(comment);
    }

    int numCarets = range.end->character - range.start->character;
    if (numCarets == RangeAssertion::END_OF_LINE_POS) {
        // Caret the entire line.
        numCarets = sourceLine.length();
    }

    return fmt::format("{}: {}\n {}{} {}", sourceLineNumber, sourceLine,
                       string(numLeadingSpaces + sourceLineNumber.length() + 1, ' '), string(numCarets, '^'), comment);
}

string_view getLine(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents, string_view uriPrefix,
                    const Location &loc) {
    auto filename = uriToFilePath(uriPrefix, loc.uri);
    auto foundFile = sourceFileContents.find(filename);
    EXPECT_NE(sourceFileContents.end(), foundFile) << fmt::format("Unable to find file `{}`", filename);
    auto &file = foundFile->second;
    return file->getLine(loc.range->start->line + 1);
}

void assertLocationsMatch(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents, string_view uriPrefix,
                          string_view symbol, const vector<shared_ptr<RangeAssertion>> &allLocs, int line,
                          int character, string_view locSourceLine, string_view locFilename,
                          vector<unique_ptr<Location>> &locations) {
    fast_sort(locations,
              [&](const unique_ptr<Location> &a, const unique_ptr<Location> &b) -> bool { return a->cmp(*b) < 0; });

    auto expectedLocationsIt = allLocs.begin();
    auto actualLocationsIt = locations.begin();
    while (expectedLocationsIt != allLocs.end() && actualLocationsIt != locations.end()) {
        auto expectedLocation = (*expectedLocationsIt)->getLocation(uriPrefix);
        auto &actualLocation = *actualLocationsIt;

        // If true, the expectedLocation is the entire actualLocation
        if (actualLocation->uri == expectedLocation->uri &&
            rangeIsEqual(*actualLocation->range, *expectedLocation->range)) {
            // Assertion passes. Consume both.
            actualLocationsIt++;
            expectedLocationsIt++;
        } else {
            const int cmp = expectedLocation->cmp(*actualLocation);
            if (cmp < 0) {
                // Expected location is *before* actual location.
                auto expectedFilePath = uriToFilePath(uriPrefix, expectedLocation->uri);
                ADD_FAILURE_AT(expectedFilePath.c_str(), expectedLocation->range->start->line + 1)
                    << fmt::format("Sorbet did not report a reference to symbol `{}`.\nGiven symbol at:\n{}\nSorbet "
                                   "did not report reference at:\n{}",
                                   symbol,
                                   prettyPrintRangeComment(
                                       locSourceLine, *RangeAssertion::makeRange(line, character, character + 1), ""),
                                   prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *expectedLocation),
                                                           *expectedLocation->range, ""));
                expectedLocationsIt++;
            } else if (cmp > 0) {
                // Expected location is *after* actual location
                auto actualFilePath = uriToFilePath(uriPrefix, actualLocation->uri);
                ADD_FAILURE_AT(actualFilePath.c_str(), actualLocation->range->start->line + 1)
                    << fmt::format("Sorbet reported unexpected reference to symbol `{}`.\nGiven symbol "
                                   "at:\n{}\nSorbet reported an unexpected (additional?) reference at:\n{}",
                                   symbol,
                                   prettyPrintRangeComment(
                                       locSourceLine, *RangeAssertion::makeRange(line, character, character + 1), ""),
                                   prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *actualLocation),
                                                           *actualLocation->range, ""));
                actualLocationsIt++;
            } else {
                // Should never happen.
                ADD_FAILURE()
                    << "Error in test runner: identical locations weren't reported as subsets of one another.";
                expectedLocationsIt++;
                actualLocationsIt++;
            }
        }
    }

    while (expectedLocationsIt != allLocs.end()) {
        auto expectedLocation = (*expectedLocationsIt)->getLocation(uriPrefix);
        auto expectedFilePath = uriToFilePath(uriPrefix, expectedLocation->uri);
        ADD_FAILURE_AT(expectedFilePath.c_str(), expectedLocation->range->start->line + 1) << fmt::format(
            "Sorbet did not report a reference to symbol `{}`.\nGiven symbol at:\n{}\nSorbet "
            "did not report reference at:\n{}",
            symbol,
            prettyPrintRangeComment(locSourceLine, *RangeAssertion::makeRange(line, character, character + 1), ""),
            prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *expectedLocation), *expectedLocation->range,
                                    ""));
        expectedLocationsIt++;
    }

    while (actualLocationsIt != locations.end()) {
        auto &actualLocation = *actualLocationsIt;
        auto actualFilePath = uriToFilePath(uriPrefix, actualLocation->uri);
        ADD_FAILURE_AT(actualFilePath.c_str(), actualLocation->range->start->line + 1) << fmt::format(
            "Sorbet reported unexpected reference to symbol `{}`.\nGiven symbol "
            "at:\n{}\nSorbet reported an unexpected (additional?) reference at:\n{}",
            symbol,
            prettyPrintRangeComment(locSourceLine, *RangeAssertion::makeRange(line, character, character + 1), ""),
            prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *actualLocation), *actualLocation->range,
                                    ""));
        actualLocationsIt++;
    }
}

RangeAssertion::RangeAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine)
    : filename(filename), range(move(range)), assertionLine(assertionLine) {}

int RangeAssertion::matches(string_view otherFilename, const Range &otherRange) {
    const int filenamecmp = filename.compare(otherFilename);
    if (filenamecmp != 0) {
        return filenamecmp;
    }
    if (range->end->character == RangeAssertion::END_OF_LINE_POS) {
        // This assertion matches the whole line.
        // (Will match diagnostics that span multiple lines for parity with existing test logic.)
        const int targetLine = range->start->line;
        const int cmp = targetLine - otherRange.start->line;
        if (cmp >= 0 && targetLine <= otherRange.end->line) {
            return 0;
        } else {
            return cmp;
        }
    }
    return range->cmp(otherRange);
}

int RangeAssertion::cmp(const RangeAssertion &b) const {
    const int filenameCmp = filename.compare(b.filename);
    if (filenameCmp != 0) {
        return filenameCmp;
    }
    const int rangeCmp = range->cmp(*b.range);
    if (rangeCmp != 0) {
        return rangeCmp;
    }
    return toString().compare(b.toString());
}

ErrorAssertion::ErrorAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view message,
                               bool matchesDuplicateErrors)
    : RangeAssertion(filename, range, assertionLine), message(message), matchesDuplicateErrors(matchesDuplicateErrors) {
}

shared_ptr<ErrorAssertion> ErrorAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                string_view assertionContents, string_view assertionType) {
    return make_shared<ErrorAssertion>(filename, range, assertionLine, assertionContents,
                                       assertionType == "error-with-dupes");
}

string ErrorAssertion::toString() const {
    return fmt::format("{}: {}", (matchesDuplicateErrors ? "error-with-dupes" : "error"), message);
}

bool ErrorAssertion::check(const Diagnostic &diagnostic, string_view sourceLine, string_view errorPrefix) {
    // The error message must contain `message`.
    if (diagnostic.message.find(message) == string::npos) {
        ADD_FAILURE_AT(filename.c_str(), range->start->line + 1) << fmt::format(
            "{}Expected error of form:\n{}\nFound error:\n{}", errorPrefix,
            prettyPrintRangeComment(sourceLine, *range, toString()),
            prettyPrintRangeComment(sourceLine, *diagnostic.range, fmt::format("error: {}", diagnostic.message)));
        return false;
    }
    return true;
}

unique_ptr<Range> RangeAssertion::makeRange(int sourceLine, int startChar, int endChar) {
    return make_unique<Range>(make_unique<Position>(sourceLine, startChar), make_unique<Position>(sourceLine, endChar));
}

vector<shared_ptr<ErrorAssertion>>
RangeAssertion::getErrorAssertions(const vector<shared_ptr<RangeAssertion>> &assertions) {
    vector<shared_ptr<ErrorAssertion>> rv;
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<ErrorAssertion>(assertion)) {
            rv.push_back(assertionOfType);
        }
    }
    return rv;
}

vector<shared_ptr<RangeAssertion>> parseAssertionsForFile(const shared_ptr<core::File> &file) {
    vector<shared_ptr<RangeAssertion>> assertions;

    int nextChar = 0;
    // 'line' is from line linenum
    int lineNum = 0;
    // The last non-comment-assertion line that we've encountered.
    // When we encounter a comment assertion, it will refer to this
    // line.
    int lastSourceLineNum = 0;

    auto source = file->source();
    auto filename = string(file->path().begin(), file->path().end());
    auto &lineBreaks = file->lineBreaks();

    for (auto lineBreak : lineBreaks) {
        // Ignore first line break entry.
        if (lineBreak == -1) {
            continue;
        }
        string_view lineView = source.substr(nextChar, lineBreak - nextChar);
        string line = string(lineView.begin(), lineView.end());
        nextChar = lineBreak + 1;

        // Groups: Line up until first caret, carets, assertion type, assertion contents.
        smatch matches;
        if (regex_search(line, matches, rangeAssertionRegex)) {
            int numCarets = matches[2].str().size();
            auto textBeforeComment = matches.prefix().str();
            bool lineHasCode = !regex_match(textBeforeComment, whitespaceRegex);
            if (numCarets != 0) {
                // Position assertion assertions.
                if (lineNum == 0) {
                    ADD_FAILURE_AT(filename.c_str(), lineNum + 1) << fmt::format(
                        "Invalid assertion comment found on line 1, before any code:\n{}\nAssertion comments that "
                        "point to "
                        "specific character ranges with carets (^) should come after the code they point to.",
                        line);
                    // Ignore erroneous comment.
                    continue;
                }
            }

            if (numCarets == 0 && lineHasCode) {
                // Line-based assertion comment is on a line w/ code, meaning
                // the assertion is for that line.
                lastSourceLineNum = lineNum;
            }

            unique_ptr<Range> range;
            if (numCarets > 0) {
                int caretBeginPos = textBeforeComment.size() + matches[1].str().size();
                int caretEndPos = caretBeginPos + numCarets;
                range = RangeAssertion::makeRange(lastSourceLineNum, caretBeginPos, caretEndPos);
            } else {
                range = RangeAssertion::makeRange(lastSourceLineNum);
            }

            if (numCarets != 0 && lineHasCode) {
                // Character-based assertion comment is on line w/ code, so
                // next line could point to code on this line.
                lastSourceLineNum = lineNum;
            }

            string assertionType = matches[3].str();
            string assertionContents = matches[4].str();

            const auto &findConstructor = assertionConstructors.find(assertionType);
            if (findConstructor != assertionConstructors.end()) {
                assertions.push_back(
                    findConstructor->second(filename, range, lineNum, assertionContents, assertionType));
            } else if (ignoredAssertionLabels.find(assertionType) == ignoredAssertionLabels.end()) {
                ADD_FAILURE_AT(filename.c_str(), lineNum + 1)
                    << fmt::format("Found unrecognized assertion of type `{}`. Expected one of {{{}}}.\nIf this is a "
                                   "regular comment that just happens to be formatted like an assertion comment, you "
                                   "can add the label to `ignoredAssertionLabels`.",
                                   assertionType,
                                   fmt::map_join(assertionConstructors.begin(), assertionConstructors.end(), ", ",
                                                 [](const auto &entry) -> string { return entry.first; }));
            }
        } else {
            lastSourceLineNum = lineNum;
        }
        lineNum += 1;
    }
    return assertions;
}

vector<shared_ptr<RangeAssertion>>
RangeAssertion::parseAssertions(const UnorderedMap<string, shared_ptr<core::File>> filesAndContents) {
    vector<shared_ptr<RangeAssertion>> assertions;
    for (auto &fileAndContents : filesAndContents) {
        auto fileAssertions = parseAssertionsForFile(fileAndContents.second);
        assertions.insert(assertions.end(), make_move_iterator(fileAssertions.begin()),
                          make_move_iterator(fileAssertions.end()));
    }

    // Sort assertions in (filename, range, message) order
    fast_sort(assertions, [](const shared_ptr<RangeAssertion> &a, const shared_ptr<RangeAssertion> &b) -> bool {
        return a->cmp(*b) < 0;
    });

    return assertions;
}

unique_ptr<Location> RangeAssertion::getLocation(string_view uriPrefix) {
    auto uri = filePathToUri(uriPrefix, filename);
    return make_unique<Location>(uri, range->copy());
}

unique_ptr<DocumentHighlight> RangeAssertion::getDocumentHighlight() {
    auto newRange = make_unique<Range>(make_unique<Position>(range->start->line, range->start->character),
                                       make_unique<Position>(range->end->line, range->end->character));
    return make_unique<DocumentHighlight>(move(newRange));
}

pair<string_view, int> getSymbolAndVersion(string_view assertionContents) {
    int version = 1;
    vector<string_view> split = absl::StrSplit(assertionContents, ' ');
    EXPECT_GE(split.size(), 0);
    EXPECT_LT(split.size(), 3) << fmt::format(
        "Invalid usage and def assertion; multiple words found:\n{}\nUsage and def assertions should be "
        "of the form:\n# [^*] [usage | def | type | type-def]: symbolname [version?]",
        assertionContents);
    if (split.size() == 2) {
        string_view versionString = split[1];
        version = stoi(string(versionString));
    }
    return pair<string_view, int>(split[0], version);
}

DefAssertion::DefAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view symbol,
                           int version)
    : RangeAssertion(filename, range, assertionLine), symbol(symbol), version(version) {}

shared_ptr<DefAssertion> DefAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                            string_view assertionContents, string_view assertionType) {
    auto symbolAndVersion = getSymbolAndVersion(assertionContents);
    return make_shared<DefAssertion>(filename, range, assertionLine, symbolAndVersion.first, symbolAndVersion.second);
}

vector<unique_ptr<Location>> &extractLocations(ResponseMessage &respMsg) {
    static vector<unique_ptr<Location>> empty;
    auto &result = *(respMsg.result);
    auto &locationsOrNull = get<variant<JSONNullObject, vector<unique_ptr<Location>>>>(result);
    if (auto isNull = get_if<JSONNullObject>(&locationsOrNull)) {
        return empty;
    }
    return get<vector<unique_ptr<Location>>>(locationsOrNull);
}

vector<unique_ptr<DocumentHighlight>> &extractDocumentHighlights(ResponseMessage &respMsg) {
    static vector<unique_ptr<DocumentHighlight>> empty;
    auto &result = *(respMsg.result);
    auto &highlightsOrNull = get<variant<JSONNullObject, vector<unique_ptr<DocumentHighlight>>>>(result);
    if (auto isNull = get_if<JSONNullObject>(&highlightsOrNull)) {
        return empty;
    }
    return get<vector<unique_ptr<DocumentHighlight>>>(highlightsOrNull);
}

void DefAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents, LSPWrapper &lspWrapper,
                         int &nextId, string_view uriPrefix, const Location &queryLoc) {
    const int line = queryLoc.range->start->line;
    // Can only query with one character, so just use the first one.
    const int character = queryLoc.range->start->character;
    auto locSourceLine = getLine(sourceFileContents, uriPrefix, queryLoc);
    auto defSourceLine = getLine(sourceFileContents, uriPrefix, *getLocation(uriPrefix));
    string locFilename = uriToFilePath(uriPrefix, queryLoc.uri);
    string defUri = filePathToUri(uriPrefix, filename);

    const int id = nextId++;
    auto responses = lspWrapper.getLSPResponsesFor(makeDefinitionRequest(id, queryLoc.uri, line, character));
    ASSERT_EQ(1, responses.size()) << "Unexpected number of responses to a `textDocument/definition` request.";
    ASSERT_NO_FATAL_FAILURE(assertResponseMessage(id, *responses.at(0)));

    auto &respMsg = responses.at(0)->asResponse();
    ASSERT_TRUE(respMsg.result.has_value());
    ASSERT_FALSE(respMsg.error.has_value());
    auto &locations = extractLocations(respMsg);

    if (this->symbol == NOTHING_LABEL) {
        // Special case: Nothing should be defined here.
        for (auto &location : locations) {
            ADD_FAILURE_AT(filename.c_str(), line + 1) << fmt::format(
                "Sorbet returned a definition for a location that we expected no definition for. For "
                "location:\n{}\nFound definition:\n{}",
                prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *location), *location->range, ""));
        }
        return;
    }

    if (locations.empty()) {
        ADD_FAILURE_AT(locFilename.c_str(), line + 1) << fmt::format(
            "Sorbet did not find a definition for location that references symbol `{}`.\nExpected definition "
            "of:\n{}\nTo be:\n{}",
            symbol, prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
            prettyPrintRangeComment(defSourceLine, *range, ""));
        return;
    } else if (locations.size() > 1) {
        ADD_FAILURE_AT(locFilename.c_str(), line + 1) << fmt::format(
            "Sorbet unexpectedly returned multiple locations for definition of symbol `{}`.\nFor "
            "location:\n{}\nSorbet returned the following definition locations:\n{}",
            symbol, prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
            fmt::map_join(locations, "\n", [&sourceFileContents, &uriPrefix](const auto &arg) -> string {
                return prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *arg), *arg->range, "");
            }));
        return;
    }

    auto &location = locations.at(0);
    // Note: Sorbet will point to the *statement* that defines the symbol, not just the symbol.
    // For example, it'll point to "class Foo" instead of just "Foo", or `5` in `a = 5` instead of `a`.
    // Thus, we just check that it returns the same line.
    if (location->uri != defUri || location->range->start->line != range->start->line) {
        string foundLocationString = "null";
        if (location != nullptr) {
            foundLocationString =
                prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *location), *location->range, "");
        }

        ADD_FAILURE_AT(filename.c_str(), line + 1)
            << fmt::format("Sorbet did not return the expected definition for location. Expected "
                           "definition of:\n{}\nTo be:\n{}\nBut was:\n{}",
                           prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
                           prettyPrintRangeComment(defSourceLine, *range, ""), foundLocationString);
    }
}

void UsageAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                           LSPWrapper &lspWrapper, int &nextId, string_view uriPrefix, string_view symbol,
                           const Location &queryLoc, const vector<shared_ptr<RangeAssertion>> &allLocs) {
    const int line = queryLoc.range->start->line;
    // Can only query with one character, so just use the first one.
    const int character = queryLoc.range->start->character;
    auto locSourceLine = getLine(sourceFileContents, uriPrefix, queryLoc);
    string locFilename = uriToFilePath(uriPrefix, queryLoc.uri);

    auto referenceParams =
        make_unique<ReferenceParams>(make_unique<TextDocumentIdentifier>(queryLoc.uri),
                                     // TODO: Try with this false, too.
                                     make_unique<Position>(line, character), make_unique<ReferenceContext>(true));
    int id = nextId++;
    auto responses = lspWrapper.getLSPResponsesFor(make_unique<LSPMessage>(
        make_unique<RequestMessage>("2.0", id, LSPMethod::TextDocumentReferences, move(referenceParams))));
    if (responses.size() != 1) {
        EXPECT_EQ(1, responses.size()) << "Unexpected number of responses to a `textDocument/references` request.";
        return;
    }

    ASSERT_NO_FATAL_FAILURE(assertResponseMessage(id, *responses.at(0)));
    auto &respMsg = responses.at(0)->asResponse();
    ASSERT_TRUE(respMsg.result.has_value());
    ASSERT_FALSE(respMsg.error.has_value());
    auto &locations = extractLocations(respMsg);
    if (symbol == NOTHING_LABEL) {
        // Special case: This location should not report usages of anything.
        for (auto &foundLocation : locations) {
            auto actualFilePath = uriToFilePath(uriPrefix, foundLocation->uri);
            ADD_FAILURE_AT(actualFilePath.c_str(), foundLocation->range->start->line + 1) << fmt::format(
                "Sorbet returned references for a location that should not report references.\nGiven location "
                "at:\n{}\nSorbet reported an unexpected reference at:\n{}",
                prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *foundLocation), *foundLocation->range,
                                        ""));
        }
        return;
    }

    assertLocationsMatch(sourceFileContents, uriPrefix, symbol, allLocs, line, character, locSourceLine, locFilename,
                         locations);
}

void HighlightAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                               LSPWrapper &lspWrapper, int &nextId, string_view uriPrefix, string_view symbol,
                               const Location &queryLoc, const vector<shared_ptr<RangeAssertion>> &allLocs) {
    const int line = queryLoc.range->start->line;
    // Can only query with one character, so just use the first one.
    const int character = queryLoc.range->start->character;
    auto locSourceLine = getLine(sourceFileContents, uriPrefix, queryLoc);
    string locFilename = uriToFilePath(uriPrefix, queryLoc.uri);

    int id = nextId++;
    auto request = make_unique<LSPMessage>(make_unique<RequestMessage>(
        "2.0", id, LSPMethod::TextDocumentDocumentHighlight,
        make_unique<TextDocumentPositionParams>(make_unique<TextDocumentIdentifier>(string(queryLoc.uri)),
                                                make_unique<Position>(line, character))));

    auto responses = lspWrapper.getLSPResponsesFor(move(request));
    if (responses.size() != 1) {
        EXPECT_EQ(1, responses.size())
            << "Unexpected number of responses to a `textDocument/documentHighlight` request.";
        return;
    }

    ASSERT_NO_FATAL_FAILURE(assertResponseMessage(id, *responses.at(0)));
    auto &respMsg = responses.at(0)->asResponse();
    ASSERT_TRUE(respMsg.result.has_value());
    ASSERT_FALSE(respMsg.error.has_value());
    auto &highlights = extractDocumentHighlights(respMsg);

    // Convert highlights to locations, used by common test functions across assertions involving multiple files.
    vector<unique_ptr<Location>> locations;
    for (auto const &highlight : highlights) {
        auto location = make_unique<Location>(queryLoc.uri, move(highlight->range));
        locations.push_back(move(location));
    }

    if (symbol == NOTHING_LABEL) {
        // Special case: This location should not report usages of anything.
        for (auto &foundLocation : locations) {
            auto actualFilePath = uriToFilePath(uriPrefix, foundLocation->uri);
            ADD_FAILURE_AT(actualFilePath.c_str(), foundLocation->range->start->line + 1) << fmt::format(
                "Sorbet returned references for a highlight that should not report references.\nGiven location "
                "at:\n{}\nSorbet reported an unexpected reference at:\n{}",
                prettyPrintRangeComment(locSourceLine, *RangeAssertion::makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *foundLocation), *foundLocation->range,
                                        ""));
        }
        return;
    }

    assertLocationsMatch(sourceFileContents, uriPrefix, symbol, allLocs, line, character, locSourceLine, locFilename,
                         locations);
}

string DefAssertion::toString() const {
    return fmt::format("def: {}", symbol);
}

UsageAssertion::UsageAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view symbol,
                               int version)
    : RangeAssertion(filename, range, assertionLine), symbol(symbol), version(version) {}

shared_ptr<UsageAssertion> UsageAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                string_view assertionContents, string_view assertionType) {
    auto symbolAndVersion = getSymbolAndVersion(assertionContents);
    return make_shared<UsageAssertion>(filename, range, assertionLine, symbolAndVersion.first, symbolAndVersion.second);
}

string UsageAssertion::toString() const {
    return fmt::format("usage: {}", symbol);
}

TypeDefAssertion::TypeDefAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                   string_view symbol)
    : RangeAssertion(filename, range, assertionLine), symbol(symbol) {}

shared_ptr<TypeDefAssertion> TypeDefAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                    string_view assertionContents, string_view assertionType) {
    auto [symbol, _version] = getSymbolAndVersion(assertionContents);
    return make_shared<TypeDefAssertion>(filename, range, assertionLine, symbol);
}

void TypeDefAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                             LSPWrapper &lspWrapper, int &nextId, string_view uriPrefix, string_view symbol,
                             const Location &queryLoc, const std::vector<std::shared_ptr<RangeAssertion>> &typeDefs) {
    const int line = queryLoc.range->start->line;
    // Can only query with one character, so just use the first one.
    const int character = queryLoc.range->start->character;
    auto locSourceLine = getLine(sourceFileContents, uriPrefix, queryLoc);
    string locFilename = uriToFilePath(uriPrefix, queryLoc.uri);

    const int id = nextId++;
    auto request = make_unique<LSPMessage>(make_unique<RequestMessage>(
        "2.0", id, LSPMethod::TextDocumentTypeDefinition,
        make_unique<TextDocumentPositionParams>(make_unique<TextDocumentIdentifier>(string(queryLoc.uri)),
                                                make_unique<Position>(line, character))));
    auto responses = lspWrapper.getLSPResponsesFor(move(request));
    ASSERT_EQ(1, responses.size());

    ASSERT_NO_FATAL_FAILURE(assertResponseMessage(id, *responses.at(0)));
    auto &respMsg = responses.at(0)->asResponse();
    ASSERT_TRUE(respMsg.result.has_value());
    ASSERT_FALSE(respMsg.error.has_value());

    auto &locations = extractLocations(respMsg);

    if (symbol == NOTHING_LABEL) {
        // can't add type-def for NOTHING_LABEL
        for (auto &location : locations) {
            auto filePath = uriToFilePath(uriPrefix, location->uri);
            ADD_FAILURE_AT(filePath.c_str(), location->range->start->line + 1) << fmt::format(
                "Sorbet returned references for a location that should not report references.\nGiven go to type def "
                "here:\n{}\nSorbet reported an unexpected definition at:\n{}",
                prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(sourceFileContents, uriPrefix, *location), *location->range, ""));
        }
        return;
    }

    if (typeDefs.empty()) {
        ADD_FAILURE_AT(locFilename.c_str(), line + 1) << fmt::format(
            "There are no 'type-def: {0}' assertions for this 'type: {0}' assertion:\n{1}\n"
            "To assert that there are no results, use the {2} label",
            symbol, prettyPrintRangeComment(locSourceLine, *makeRange(line, character, character + 1), ""),
            NOTHING_LABEL);
        return;
    }

    assertLocationsMatch(sourceFileContents, uriPrefix, symbol, typeDefs, line, character, locSourceLine, locFilename,
                         locations);
}

string TypeDefAssertion::toString() const {
    return fmt::format("type-def: {}", symbol);
}

TypeAssertion::TypeAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view symbol)
    : RangeAssertion(filename, range, assertionLine), symbol(symbol) {}

shared_ptr<TypeAssertion> TypeAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                              string_view assertionContents, string_view assertionType) {
    auto [symbol, _version] = getSymbolAndVersion(assertionContents);
    return make_shared<TypeAssertion>(filename, range, assertionLine, symbol);
}

string TypeAssertion::toString() const {
    return fmt::format("type: {}", symbol);
}

void reportMissingError(const string &filename, const ErrorAssertion &assertion, string_view sourceLine,
                        string_view errorPrefix, bool missingDuplicate = false) {
    auto coreMessage = missingDuplicate ? "Error was not duplicated" : "Did not find expected error";
    auto messagePostfix = missingDuplicate ? "\nYou can fix this error by changing the assertion to `error:`." : "";
    ADD_FAILURE_AT(filename.c_str(), assertion.range->start->line + 1)
        << fmt::format("{}{}:\n{}{}", errorPrefix, coreMessage,
                       prettyPrintRangeComment(sourceLine, *assertion.range, assertion.toString()), messagePostfix);
}

void reportUnexpectedError(const string &filename, const Diagnostic &diagnostic, string_view sourceLine,
                           string_view errorPrefix) {
    ADD_FAILURE_AT(filename.c_str(), diagnostic.range->start->line + 1) << fmt::format(
        "{}Found unexpected error:\n{}\nNote: If there is already an assertion for this error, then this is a "
        "duplicate error. Change the assertion to `# error-with-dupes: <error message>` if the duplicate is expected.",
        errorPrefix,
        prettyPrintRangeComment(sourceLine, *diagnostic.range, fmt::format("error: {}", diagnostic.message)));
}

string getSourceLine(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents, const string &filename,
                     int line) {
    auto it = sourceFileContents.find(filename);
    if (it == sourceFileContents.end()) {
        ADD_FAILURE() << fmt::format("Unable to find referenced source file `{}`", filename);
        return "";
    }

    auto &file = it->second;
    if (line >= file->lineCount()) {
        ADD_FAILURE_AT(filename.c_str(), line + 1) << "Invalid line number for range.";
        return "";
    } else {
        // Note: line is a 0-indexed line number, but file uses 1-indexed line numbers.
        auto lineView = file->getLine(line + 1);
        return string(lineView.begin(), lineView.end());
    }
}

bool isDuplicateDiagnostic(string_view filename, ErrorAssertion *assertion, const Diagnostic &d) {
    return assertion && assertion->matchesDuplicateErrors && assertion->matches(filename, *d.range) == 0 &&
           d.message.find(assertion->message) != string::npos;
}

bool ErrorAssertion::checkAll(const UnorderedMap<string, shared_ptr<core::File>> &files,
                              vector<shared_ptr<ErrorAssertion>> errorAssertions,
                              map<string, vector<unique_ptr<Diagnostic>>> &filenamesAndDiagnostics,
                              string errorPrefix) {
    // Sort input error assertions so they are in (filename, line, column) order.
    fast_sort(errorAssertions, [](const shared_ptr<ErrorAssertion> &a, const shared_ptr<ErrorAssertion> &b) -> bool {
        return a->cmp(*b) < 0;
    });

    auto assertionsIt = errorAssertions.begin();

    bool success = true;

    // Due to map's default sort order, this loop iterates over diagnostics in filename order.
    for (auto &filenameAndDiagnostics : filenamesAndDiagnostics) {
        auto &filename = filenameAndDiagnostics.first;
        auto &diagnostics = filenameAndDiagnostics.second;

        // Sort diagnostics within file in range, message order.
        // This explicit sort, combined w/ the map's implicit sort order, ensures that this loop iterates over
        // diagnostics in (filename, range, message) order -- matching the sort order of errorAssertions.
        fast_sort(diagnostics, [](const unique_ptr<Diagnostic> &a, const unique_ptr<Diagnostic> &b) -> bool {
            const int rangeCmp = a->range->cmp(*b->range);
            if (rangeCmp != 0) {
                return rangeCmp < 0;
            }
            return a->message.compare(b->message) < 0;
        });

        auto diagnosticsIt = diagnostics.begin();
        ErrorAssertion *lastAssertion = nullptr;
        bool lastAssertionMatchedDuplicate = false;

        while (diagnosticsIt != diagnostics.end() && assertionsIt != errorAssertions.end()) {
            // See if the ranges match.
            auto &diagnostic = *diagnosticsIt;
            auto &assertion = *assertionsIt;

            if (isDuplicateDiagnostic(filename, lastAssertion, *diagnostic)) {
                diagnosticsIt++;
                lastAssertionMatchedDuplicate = true;
                continue;
            } else {
                if (lastAssertion && lastAssertion->matchesDuplicateErrors && !lastAssertionMatchedDuplicate) {
                    reportMissingError(lastAssertion->filename, *lastAssertion,
                                       getSourceLine(files, lastAssertion->filename, lastAssertion->range->start->line),
                                       errorPrefix, true);
                    success = false;
                }
                lastAssertionMatchedDuplicate = false;
                lastAssertion = nullptr;
            }

            const int cmp = assertion->matches(filename, *diagnostic->range);
            if (cmp > 0) {
                // Diagnostic comes *before* this assertion, so we don't
                // have an assertion that matches the diagnostic.
                reportUnexpectedError(filename, *diagnostic,
                                      getSourceLine(files, filename, diagnostic->range->start->line), errorPrefix);
                // We've 'consumed' the diagnostic -- nothing matches it.
                diagnosticsIt++;
                success = false;
            } else if (cmp < 0) {
                // Diagnostic comes *after* this assertion
                // We don't have a diagnostic that matches the assertion.
                reportMissingError(assertion->filename, *assertion,
                                   getSourceLine(files, assertion->filename, assertion->range->start->line),
                                   errorPrefix);
                // We've 'consumed' this error assertion -- nothing matches it.
                assertionsIt++;
                success = false;
            } else {
                // Ranges match, so check the assertion.
                success = assertion->check(*diagnostic,
                                           getSourceLine(files, assertion->filename, assertion->range->start->line),
                                           errorPrefix) &&
                          success;
                // We've 'consumed' the diagnostic and assertion.
                // Save assertion in case it matches multiple diagnostics.
                lastAssertion = assertion.get();
                diagnosticsIt++;
                assertionsIt++;
            }
        }

        while (diagnosticsIt != diagnostics.end()) {
            // We had more diagnostics than error assertions.
            auto &diagnostic = *diagnosticsIt;
            if (isDuplicateDiagnostic(filename, lastAssertion, *diagnostic)) {
                lastAssertionMatchedDuplicate = true;
            } else {
                reportUnexpectedError(filename, *diagnostic,
                                      getSourceLine(files, filename, diagnostic->range->start->line), errorPrefix);
                success = false;

                if (lastAssertion && lastAssertion->matchesDuplicateErrors && !lastAssertionMatchedDuplicate) {
                    reportMissingError(lastAssertion->filename, *lastAssertion,
                                       getSourceLine(files, lastAssertion->filename, lastAssertion->range->start->line),
                                       errorPrefix, true);
                }
                lastAssertion = nullptr;
                lastAssertionMatchedDuplicate = false;
            }
            diagnosticsIt++;
        }
    }

    while (assertionsIt != errorAssertions.end()) {
        // Had more error assertions than diagnostics
        reportMissingError((*assertionsIt)->filename, **assertionsIt,
                           getSourceLine(files, (*assertionsIt)->filename, (*assertionsIt)->range->start->line),
                           errorPrefix);
        success = false;
        assertionsIt++;
    }
    return success;
}

shared_ptr<BooleanPropertyAssertion> BooleanPropertyAssertion::make(string_view filename, unique_ptr<Range> &range,
                                                                    int assertionLine, string_view assertionContents,
                                                                    string_view assertionType) {
    return make_shared<BooleanPropertyAssertion>(filename, range, assertionLine, assertionContents == "true",
                                                 assertionType);
}

optional<bool> BooleanPropertyAssertion::getValue(string_view type,
                                                  const vector<shared_ptr<RangeAssertion>> &assertions) {
    EXPECT_NE(assertionConstructors.find(string(type)), assertionConstructors.end())
        << "Unrecognized boolean property assertion: " << type;
    for (auto &assertion : assertions) {
        if (auto boolAssertion = dynamic_pointer_cast<BooleanPropertyAssertion>(assertion)) {
            if (boolAssertion->assertionType == type) {
                return boolAssertion->value;
            }
        }
    }
    return nullopt;
}

BooleanPropertyAssertion::BooleanPropertyAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                   bool value, string_view assertionType)
    : RangeAssertion(filename, range, assertionLine), assertionType(string(assertionType)), value(value){};

string BooleanPropertyAssertion::toString() const {
    return fmt::format("{}: {}", assertionType, value);
}

shared_ptr<FastPathAssertion> FastPathAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                      string_view assertionContents, string_view assertionType) {
    optional<vector<string>> expectedFiles;
    if (!assertionContents.empty()) {
        expectedFiles = absl::StrSplit(assertionContents, ',');
        fast_sort(*expectedFiles);
    }
    return make_shared<FastPathAssertion>(filename, range, assertionLine, std::move(expectedFiles));
}

optional<shared_ptr<FastPathAssertion>> FastPathAssertion::get(const vector<shared_ptr<RangeAssertion>> &assertions) {
    for (auto &assertion : assertions) {
        if (auto fastPathAssertion = dynamic_pointer_cast<FastPathAssertion>(assertion)) {
            return fastPathAssertion;
        }
    }
    return nullopt;
}

FastPathAssertion::FastPathAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                     optional<vector<string>> expectedFiles)
    : RangeAssertion(filename, range, assertionLine), expectedFiles(move(expectedFiles)) {}

void FastPathAssertion::check(SorbetTypecheckRunInfo &info, string_view folder, int updateVersion,
                              string_view errorPrefix) {
    string updateFile = fmt::format("{}.{}.rbupdate", filename.substr(0, -3), updateVersion);
    if (!info.tookFastPath) {
        ADD_FAILURE_AT(updateFile.c_str(), assertionLine)
            << errorPrefix << "Expected file update to take fast path, but it took the slow path.";
    }
    if (expectedFiles.has_value()) {
        vector<string> expectedFilePaths;
        for (auto &f : *expectedFiles) {
            expectedFilePaths.push_back(absl::StrCat(folder, f));
        }
        fast_sort(info.filesTypechecked);
        vector<string> unTypecheckedFiles;
        set_difference(expectedFilePaths.begin(), expectedFilePaths.end(), info.filesTypechecked.begin(),
                       info.filesTypechecked.end(), back_inserter(unTypecheckedFiles));
        for (auto &f : unTypecheckedFiles) {
            ADD_FAILURE_AT(updateFile.c_str(), assertionLine)
                << errorPrefix << fmt::format("Expected file update to cause {} to also be typechecked.", f);
        }
    }
}

string FastPathAssertion::toString() const {
    return fmt::format("FastPathAssertion: {}", expectedFiles ? fmt::format("{}", fmt::join(*expectedFiles, ",")) : "");
}

shared_ptr<HoverAssertion> HoverAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                string_view assertionContents, string_view assertionType) {
    return make_shared<HoverAssertion>(filename, range, assertionLine, assertionContents);
}
HoverAssertion::HoverAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view message)
    : RangeAssertion(filename, range, assertionLine), message(string(message)) {}

void HoverAssertion::checkAll(const vector<shared_ptr<RangeAssertion>> &assertions,
                              const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                              LSPWrapper &wrapper, int &nextId, string_view uriPrefix, string errorPrefix) {
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<HoverAssertion>(assertion)) {
            assertionOfType->check(sourceFileContents, wrapper, nextId, uriPrefix, errorPrefix);
        }
    }
}

// Retrieve contents of a Hover response as a string.
string_view hoverToString(variant<JSONNullObject, unique_ptr<Hover>> &hoverResult) {
    if (auto nullResp = get_if<JSONNullObject>(&hoverResult)) {
        return NULL_LABEL;
    } else {
        auto &hover = get<unique_ptr<Hover>>(hoverResult);
        string_view value = hover->contents->value;
        if (value.empty()) {
            return NOTHING_LABEL;
        }
        return value;
    }
}

// Returns `true` if `line` matches a full line of text in `text`.
bool containsLine(string_view text, string_view line) {
    for (int pos = text.find(line); pos != string::npos; pos = text.find(line, pos + 1)) {
        const bool startsOnNewLine = pos == 0 || text.at(pos - 1) == '\n';
        const bool endsLine = pos + line.size() == text.size() || text.at(pos + line.size()) == '\n';
        if (startsOnNewLine && endsLine) {
            return true;
        }
    }
    return false;
}

void HoverAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents, LSPWrapper &wrapper,
                           int &nextId, string_view uriPrefix, string errorPrefix) {
    auto uri = filePathToUri(uriPrefix, filename);
    auto pos = make_unique<TextDocumentPositionParams>(make_unique<TextDocumentIdentifier>(uri), range->start->copy());
    auto id = nextId++;
    auto msg = make_unique<LSPMessage>(make_unique<RequestMessage>("2.0", id, LSPMethod::TextDocumentHover, move(pos)));
    auto responses = wrapper.getLSPResponsesFor(move(msg));
    ASSERT_EQ(responses.size(), 1);
    auto &responseMsg = responses.at(0);
    ASSERT_TRUE(responseMsg->isResponse());
    auto &response = responseMsg->asResponse();
    ASSERT_TRUE(response.result.has_value());
    auto &hoverResponse = get<variant<JSONNullObject, unique_ptr<Hover>>>(*response.result);
    auto hoverContents = hoverToString(hoverResponse);

    // Match a full line. Makes it possible to disambiguate `String` and `T.nilable(String)`.
    if (!containsLine(hoverContents, this->message)) {
        auto sourceLine = getSourceLine(sourceFileContents, filename, range->start->line);
        ADD_FAILURE_AT(filename.c_str(), range->start->line + 1)
            << fmt::format("{}Expected hover contents:\n{}\nFound hover contents:\n{}", errorPrefix,
                           prettyPrintRangeComment(sourceLine, *range, toString()),
                           prettyPrintRangeComment(sourceLine, *range, fmt::format("hover: {}", hoverContents)));
    }
}

string HoverAssertion::toString() const {
    return fmt::format("hover: {}", message);
}

shared_ptr<CompletionAssertion> CompletionAssertion::make(string_view filename, unique_ptr<Range> &range,
                                                          int assertionLine, string_view assertionContents,
                                                          string_view assertionType) {
    return make_shared<CompletionAssertion>(filename, range, assertionLine, assertionContents);
}
CompletionAssertion::CompletionAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                         string_view message)
    : RangeAssertion(filename, range, assertionLine), message(string(message)) {}

void CompletionAssertion::checkAll(const vector<shared_ptr<RangeAssertion>> &assertions,
                                   const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                                   LSPWrapper &wrapper, int &nextId, string_view uriPrefix, string errorPrefix) {
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<CompletionAssertion>(assertion)) {
            assertionOfType->check(sourceFileContents, wrapper, nextId, uriPrefix, errorPrefix);
        }
    }
}

void CompletionAssertion::check(const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                                LSPWrapper &wrapper, int &nextId, string_view uriPrefix, string errorPrefix) {
    auto completionList = doTextDocumentCompletion(wrapper, *this->range, nextId, this->filename, uriPrefix);
    ASSERT_NE(completionList, nullptr) << "doTextDocumentCompletion failed; see error above.";

    // TODO(jez) Add ability to expect CompletionItemKind of each item
    string actualMessage =
        completionList->items.size() == 0
            ? "(nothing)"
            : fmt::format("{}", fmt::map_join(completionList->items.begin(), completionList->items.end(), ", ",
                                              [](const auto &item) -> string { return item->label; }));

    auto partial = absl::EndsWith(this->message, ", ...");
    if (partial) {
        auto prefix = this->message.substr(0, this->message.size() - 5);
        if (!absl::StartsWith(actualMessage, prefix)) {
            auto sourceLine = getSourceLine(sourceFileContents, filename, range->start->line);
            ADD_FAILURE_AT(filename.c_str(), range->start->line + 1) << fmt::format(
                "{}Expected partial completion contents:\n{}\nFound incompatible completion contents:\n{}", errorPrefix,
                prettyPrintRangeComment(sourceLine, *range, toString()),
                prettyPrintRangeComment(sourceLine, *range, fmt::format("completion: {}", actualMessage)));
        }
    } else {
        if (this->message != actualMessage) {
            auto sourceLine = getSourceLine(sourceFileContents, filename, range->start->line);
            ADD_FAILURE_AT(filename.c_str(), range->start->line + 1) << fmt::format(
                "{}Expected completion contents:\n{}\nFound completion contents:\n{}", errorPrefix,
                prettyPrintRangeComment(sourceLine, *range, toString()),
                prettyPrintRangeComment(sourceLine, *range, fmt::format("completion: {}", actualMessage)));
        }
    }
}

string CompletionAssertion::toString() const {
    return fmt::format("completion: {}", message);
}

shared_ptr<ApplyCompletionAssertion> ApplyCompletionAssertion::make(string_view filename, unique_ptr<Range> &range,
                                                                    int assertionLine, string_view assertionContents,
                                                                    string_view assertionType) {
    static const regex versionIndexRegex(R"(^\[(\w+)\]\s+item:\s+(\d+)$)");

    smatch matches;
    string assertionContentsString = string(assertionContents);
    if (regex_search(assertionContentsString, matches, versionIndexRegex)) {
        auto version = matches[1].str();
        auto index = stoi(matches[2].str());
        return make_shared<ApplyCompletionAssertion>(filename, range, assertionLine, version, index);
    }

    ADD_FAILURE_AT(string(filename).c_str(), assertionLine + 1)
        << fmt::format("Improperly formatted apply-completion assertion. Expected '[<version>] <index>'. Found '{}'",
                       assertionContents);

    return nullptr;
}

ApplyCompletionAssertion::ApplyCompletionAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                   string_view version, int index)
    : RangeAssertion(filename, range, assertionLine), version(string(version)), index(index) {}

void ApplyCompletionAssertion::checkAll(const vector<shared_ptr<RangeAssertion>> &assertions,
                                        const UnorderedMap<string, shared_ptr<core::File>> &sourceFileContents,
                                        LSPWrapper &wrapper, int &nextId, string_view uriPrefix, string errorPrefix) {
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<ApplyCompletionAssertion>(assertion)) {
            assertionOfType->check(sourceFileContents, wrapper, nextId, uriPrefix, errorPrefix);
        }
    }
}

void ApplyCompletionAssertion::check(const UnorderedMap<std::string, std::shared_ptr<core::File>> &sourceFileContents,
                                     LSPWrapper &wrapper, int &nextId, std::string_view uriPrefix,
                                     std::string errorPrefix) {
    auto completionList = doTextDocumentCompletion(wrapper, *this->range, nextId, this->filename, uriPrefix);
    ASSERT_NE(completionList, nullptr) << "doTextDocumentCompletion failed; see error above.";

    auto &items = completionList->items;
    ASSERT_LE(0, this->index);
    ASSERT_LT(this->index, items.size());

    auto &completionItem = items[this->index];

    auto it = sourceFileContents.find(this->filename);
    ASSERT_NE(it, sourceFileContents.end()) << fmt::format("Unable to find source file `{}`", this->filename);
    auto &file = it->second;

    auto expectedUpdatedFilePath =
        fmt::format("{}.{}.rbedited", this->filename.substr(0, this->filename.size() - strlen(".rb")), this->version);

    string expectedEditedFileContents;
    try {
        expectedEditedFileContents = FileOps::read(expectedUpdatedFilePath);
    } catch (FileNotFoundException e) {
        ADD_FAILURE_AT(filename.c_str(), this->assertionLine + 1) << fmt::format(
            "Missing {} which should contain test file after applying code actions.", expectedUpdatedFilePath);
        return;
    }

    ASSERT_NE(completionItem->textEdit, nullopt);
    auto &textEdit = completionItem->textEdit.value();

    // TODO(jez) Share this code with CodeAction (String -> TextEdit -> ())
    auto beginLine = static_cast<u4>(textEdit->range->start->line + 1);
    auto beginCol = static_cast<u4>(textEdit->range->start->character + 1);
    auto beginOffset = core::Loc::pos2Offset(*file, {beginLine, beginCol}).value();

    auto endLine = static_cast<u4>(textEdit->range->end->line + 1);
    auto endCol = static_cast<u4>(textEdit->range->end->character + 1);
    auto endOffset = core::Loc::pos2Offset(*file, {endLine, endCol}).value();

    string actualEditedFileContents = string(file->source());
    actualEditedFileContents.replace(beginOffset, endOffset - beginOffset, textEdit->newText);

    EXPECT_EQ(expectedEditedFileContents, actualEditedFileContents) << fmt::format(
        "The expected (rbedited) file contents for this completion did not match the actual file contents post-edit",
        expectedUpdatedFilePath);
}

string ApplyCompletionAssertion::toString() const {
    return fmt::format("apply-completion: [{}] item: {}", version, index);
}

shared_ptr<ApplyCodeActionAssertion> ApplyCodeActionAssertion::make(string_view filename, unique_ptr<Range> &range,
                                                                    int assertionLine, string_view assertionContents,
                                                                    string_view assertionType) {
    static const regex titleVersionRegex(R"(^\[(\w+)\]\s+(.*?)$)");

    smatch matches;
    string assertionContentsString = string(assertionContents);
    if (regex_search(assertionContentsString, matches, titleVersionRegex)) {
        string version = matches[1].str();
        string title = matches[2].str();
        return make_shared<ApplyCodeActionAssertion>(filename, range, assertionLine, title, version);
    }

    ADD_FAILURE_AT(string(filename).c_str(), assertionLine + 1)
        << fmt::format("Found improperly formatted apply-code-action assertion. Expected apply-code-action "
                       "[version] code-action-title.");
    return nullptr;
}
ApplyCodeActionAssertion::ApplyCodeActionAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                   string_view title, string_view version)
    : RangeAssertion(filename, range, assertionLine), title(string(title)), version(string(version)) {}

string ApplyCodeActionAssertion::toString() const {
    return fmt::format("apply-code-action: [{}] {}", version, title);
}

void ApplyCodeActionAssertion::check(const UnorderedMap<std::string, std::shared_ptr<core::File>> &sourceFileContents,
                                     const CodeAction &codeAction, std::string_view rootUri) {
    for (auto &c : *codeAction.edit.value()->documentChanges) {
        auto filename = uriToFilePath(rootUri, c->textDocument->uri);
        auto it = sourceFileContents.find(filename);
        ASSERT_NE(it, sourceFileContents.end()) << fmt::format("Unable to find referenced source file `{}`", filename);
        auto &file = it->second;

        auto expectedUpdatedFilePath =
            fmt::format("{}.{}.rbedited", filename.substr(0, filename.size() - strlen(".rb")), version);
        string expectedEditedFileContents;
        try {
            expectedEditedFileContents = FileOps::read(expectedUpdatedFilePath);
        } catch (FileNotFoundException e) {
            ADD_FAILURE_AT(filename.c_str(), assertionLine + 1) << fmt::format(
                "Missing {} which should contain test file after applying code actions.", expectedUpdatedFilePath);
            return;
        }

        string actualEditedFileContents = string(file->source());

        // First, sort the edits by increasing starting location and verify that none overlap.
        fast_sort(c->edits, [](const auto &l, const auto &r) -> bool { return l->range->cmp(*r->range) < 0; });
        for (u4 i = 1; i < c->edits.size(); i++) {
            ASSERT_LT(c->edits[i - 1]->range->end->cmp(*c->edits[i]->range->start), 0)
                << fmt::format("Received quick fix edit\n{}\nthat overlaps edit\n{}\nThe test runner does not support "
                               "overlapping autocomplete edits, and it's likely that this is a bug.",
                               c->edits[i - 1]->toJSON(), c->edits[i]->toJSON());
        }

        // Now, apply the edits in the reverse order so that the indices don't change.
        reverse(c->edits.begin(), c->edits.end());
        for (auto &e : c->edits) {
            core::Loc::Detail reqPos;
            reqPos.line = e->range->start->line + 1;
            reqPos.column = e->range->start->character + 1;
            auto startOffset = core::Loc::pos2Offset(*file, reqPos).value();

            reqPos.line = e->range->end->line + 1;
            reqPos.column = e->range->end->character + 1;
            auto endOffset = core::Loc::pos2Offset(*file, reqPos).value();

            actualEditedFileContents.replace(startOffset, endOffset - startOffset, e->newText);
        }
        EXPECT_EQ(actualEditedFileContents, expectedEditedFileContents) << fmt::format(
            "Invalid quick fix result. Expected edited result ({}) to be:\n{}\n...but actually resulted in:\n{}",
            expectedUpdatedFilePath, expectedEditedFileContents, actualEditedFileContents);
    }
}

} // namespace sorbet::test
