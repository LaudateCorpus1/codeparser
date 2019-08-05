
#include "CharacterDecoder.h"

#include "LongNameMap.h"
#include "Utils.h"

#include <cctype>

CharacterDecoder::CharacterDecoder() : cur(0), characterQueue(), Issues(), totalTimeMicros() {}

void CharacterDecoder::init() {
    cur = WLCharacter(0);
    characterQueue.clear();
    Issues.clear();
    totalTimeMicros = std::chrono::microseconds::zero();
}

void CharacterDecoder::deinit() {
    characterQueue.clear();
    Issues.clear();
}

//
// Returns a useful character
//
// Keeps track of character counts
//
WLCharacter CharacterDecoder::nextWLCharacter(NextCharacterPolicy policy) {
    TimeScoper Scoper(totalTimeMicros);
    
    //
    // handle the queue before anything else
    //
    // We know only single-SourceCharacter WLCharacters are queued
    //
    if (!characterQueue.empty()) {
        
        auto p = characterQueue[0];
        
        // erase first
        characterQueue.erase(characterQueue.begin());
        
        auto curSource = p.first;
        auto location = p.second;
        
        TheSourceManager->setSourceLocation(location);
        
        TheSourceManager->setWLCharacterStart();
        
        TheSourceManager->setWLCharacterEnd();
        
        cur = WLCharacter(curSource.to_point());
        
        return cur;
    }
    
    auto curSource = TheByteDecoder->nextSourceCharacter();
    
    if (curSource == SOURCECHARACTER_ENDOFFILE) {
        
        TheSourceManager->setWLCharacterStart();
        TheSourceManager->setWLCharacterEnd();
        
        cur = WLCharacter(curSource.to_point());
        
        return cur;
    }
    
    if (curSource != SourceCharacter('\\') ||
        ((policy & DISABLE_ESCAPES) == DISABLE_ESCAPES)) {
        
        TheSourceManager->setWLCharacterStart();
        TheSourceManager->setWLCharacterEnd();
        
        cur = WLCharacter(curSource.to_point());
        
        return cur;
    }
    
    //
    // Handle \
    //
    // handle escapes like line continuation and special characters
    //
    
    TheSourceManager->setWLCharacterStart();
    auto CharacterStart = TheSourceManager->getWLCharacterStart();
    curSource = TheByteDecoder->nextSourceCharacter();
    
    switch (curSource.to_point()) {
        case '\r': case '\n': {
            
            //
            // Line continuation
            //
            
            bool wasCR = (curSource.to_point() == '\r');
            
            auto c = nextWLCharacter();
            
            if ((policy & LC_UNDERSTANDS_CRLF) == LC_UNDERSTANDS_CRLF) {
                if (wasCR && (c == WLCharacter('\n'))) {
                    nextWLCharacter();
                }
            }
            
            //
            // Process the white space and glue together pieces
            //
            if ((policy & PRESERVE_WS_AFTER_LC) != PRESERVE_WS_AFTER_LC) {
                while (cur.isSpace()) {
                    
                    //
                    // No need to check isAbort() inside decoder loops
                    //
                    
                    nextWLCharacter();
                }
            }
            
            if ((policy & CURRENTLY_INSIDE_TOKEN) != CURRENTLY_INSIDE_TOKEN) {
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_STRAYLINECONTINUATION, std::string("Stray line continuation.\nConsider removing."), SYNTAXISSUESEVERITY_FORMATTING, Source(CharacterStart, CharacterStart));
                
                Issues.push_back(Issue);
            }
        }
            break;
        case '[':
            cur = handleLongName(curSource, CharacterStart, policy, false);
            break;
        case ':':
            cur = handle4Hex(curSource, CharacterStart, policy);
            break;
        case '.':
            cur = handle2Hex(curSource, CharacterStart, policy);
            break;
        case '|':
            cur = handle6Hex(curSource, CharacterStart, policy);
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            cur = handleOctal(curSource, CharacterStart, policy);
            break;
        //
        // Simple escaped characters
        // \b \f \n \r \t
        //
        case 'b':
            cur = WLCharacter('\b', ESCAPE_SINGLE);
            break;
        case 'f':
            cur = WLCharacter('\f', ESCAPE_SINGLE);
            break;
        case 'n':
            cur = WLCharacter('\n', ESCAPE_SINGLE);
            break;
        case 'r':
            cur = WLCharacter('\r', ESCAPE_SINGLE);
            break;
        case 't':
            cur = WLCharacter('\t', ESCAPE_SINGLE);
            break;
        //
        // \\ \" \< \>
        //
        // String meta characters
        // What are \< and \> ?
        // https://mathematica.stackexchange.com/questions/105018/what-are-and-delimiters-in-box-expressions
        // https://stackoverflow.com/q/6065887
        //
        case '"':
            cur = WLCharacter('"', ESCAPE_SINGLE);
            break;
        case '\\':
            
            //
            // if inside a string, then test whether this \ is the result of the "feature" of
            // converting "\[Alpa]" into "\\[Alpa]" and never giving any further warnings
            // when dealing with "\\[Alpa]"
            //
            
            if (policy == INSIDE_STRING) {
                
                auto oldSourceLoc = TheSourceManager->getSourceLocation();
                
                auto test = TheByteDecoder->nextSourceCharacter();

                if (test == SourceCharacter('[')) {

                    auto tmpPolicy = policy;

                    tmpPolicy |= DISABLE_CHARACTERDECODINGISSUES;
                    
                    handleLongName(test, CharacterStart+1, tmpPolicy, true);

                } else {
                    
                    //
                    // Must append in TheByteDecoder
                    // Cannot append in TheCharacterDecoder, because only single-SourceCharacter WLCharacters are expected in
                    // TheCharacterDecoder queue
                    //
                    
                    //
                    // test for '\n' here because we want to make sure to have the correct SourceLocation
                    // A newline means advancing to next line and resetting Column to 0
                    // Note: '\n' is not the only single-byte character that can start a newline
                    // FIXME: If test == SourceCharacter('\r'), then there could also be a newline, either
                    // as \r\n or as a stray \r that is not paired with a \n
                    //
                    if (test == SourceCharacter('\n')) {
                        
                        //
                        // CharacterStart is the first \, so then queuedCharacterStart is
                        // start of the next line
                        //
                        auto queuedCharacterStart = SourceLocation(CharacterStart.Line+1, 0);
                        
                        TheByteDecoder->append('\n', queuedCharacterStart);
                        
                    } else {
                        
                        auto testStr = test.string();
                        
                        //
                        // CharacterStart is the first \, so then queuedCharacterStart is the first
                        // character to be queued
                        //
                        auto queuedCharacterStart = CharacterStart+2;
                        
                        for (auto b : testStr) {
                            TheByteDecoder->append(b, queuedCharacterStart);
                        }
                    }
                    
                    //
                    // and make sure to reset SourceLoc
                    //
                    TheSourceManager->setSourceLocation(oldSourceLoc);
                }
            }
            
            cur = WLCharacter('\\', ESCAPE_SINGLE);
            break;
        case '<':
            cur = WLCharacter(CODEPOINT_STRINGMETA_OPEN, ESCAPE_SINGLE);
            break;
        case '>':
            cur = WLCharacter(CODEPOINT_STRINGMETA_CLOSE, ESCAPE_SINGLE);
            break;
        //
        // Linear syntax characters
        // \! \% \& \( \) \* \+ \/ \@ \^ \_ \` \<space>
        //
        case '!':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_BANG, ESCAPE_SINGLE);
            break;
        case '%':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_PERCENT, ESCAPE_SINGLE);
            break;
        case '&':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_AMP, ESCAPE_SINGLE);
            break;
        case '(':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_OPENPAREN, ESCAPE_SINGLE);
            break;
        case ')':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_CLOSEPAREN, ESCAPE_SINGLE);
            break;
        case '*':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_STAR, ESCAPE_SINGLE);
            break;
        case '+':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_PLUS, ESCAPE_SINGLE);
            break;
        case '/':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_SLASH, ESCAPE_SINGLE);
            break;
        case '@':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_AT, ESCAPE_SINGLE);
            break;
        case '^':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_CARET, ESCAPE_SINGLE);
            break;
        case '_':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_UNDER, ESCAPE_SINGLE);
            break;
        case '`':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_BACKTICK, ESCAPE_SINGLE);
            break;
        case ' ':
            cur = WLCharacter(CODEPOINT_LINEARSYNTAX_SPACE, ESCAPE_SINGLE);
            break;
        case EOF: {
            auto Loc = TheSourceManager->getSourceLocation();
            
            auto Issue = SyntaxIssue(SYNTAXISSUETAG_SYNTAXERROR, std::string("Incomplete character ``\\``"), SYNTAXISSUESEVERITY_FATAL, Source(CharacterStart, Loc));
            
            Issues.push_back(Issue);
            
            cur = WLCharacter('\\');
            
            break;
        }
        default: {
            
            //
            // Anything else
            //

            auto Loc = TheSourceManager->getSourceLocation();

            //
            // Make the warnings a little more relevant
            //
            
            if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
                
                if (curSource.isUpper() && curSource.isHex()) {
                    
                    auto curSourceStr = curSource.string();
                    
                    auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character ``\\") + curSourceStr + "``.\nDid you mean ``" + curSourceStr + "`` or ``\\[" + curSourceStr + "XXXX]`` or ``\\:" + curSourceStr + "XXX``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                    
                    Issues.push_back(Issue);
                    
                } else if (curSource.isUpper()) {
                    
                    auto curSourceStr = curSource.string();
                    
                    auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character ``\\") + curSourceStr + "``.\nDid you mean ``" + curSourceStr + "`` or ``\\[" + curSourceStr + "XXXX]``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                    
                    Issues.push_back(Issue);
                    
                } else if (curSource.isHex()) {
                    
                    auto curSourceStr = curSource.string();
                    
                    auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character ``\\") + curSourceStr + "``.\nDid you mean ``" + curSourceStr + "`` or ``\\:" + curSourceStr + "xxx``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                    
                    Issues.push_back(Issue);
                    
                } else {
                    
                    auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                    
                    curSourceStr = Utils::makeGraphical(curSourceStr);
                    
                    auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character ``\\") + curSourceStr + "``.\nDid you mean ``" + curSourceStr + "`` or ``\\\\" + curSourceStr + "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                    
                    Issues.push_back(Issue);
                }
            }
            
            //
            // Keep these treated as 2 characters. This is how bad escapes are handled in WL strings.
            //
            
            cur = WLCharacter('\\');
            
            TheSourceManager->setSourceLocation(CharacterStart);
            TheSourceManager->setWLCharacterStart();
            TheSourceManager->setWLCharacterEnd();
            
            append(curSource, Loc);
            
            break;
        }
    }

    TheSourceManager->setWLCharacterEnd();
    
    return cur;
}

//
// Append character c
//
void CharacterDecoder::append(SourceCharacter c, SourceLocation location) {
    characterQueue.push_back(std::make_pair(c, location));
}

WLCharacter CharacterDecoder::currentWLCharacter() const {
   return cur;
}

//
// return the next WL character
//
// CharacterStart: location of \
//
WLCharacter CharacterDecoder::handleLongName(SourceCharacter curSourceIn, SourceLocation CharacterStart, NextCharacterPolicy policy, bool unlikelyEscapeChecking) {
    
    auto curSource = curSourceIn;
    
    assert(curSource == SourceCharacter('['));
    
    //
    // Do not write leading \[ or trailing ] to LongName
    //
    std::ostringstream LongName;
    
    curSource = TheByteDecoder->nextSourceCharacter();
    
    auto wellFormed = false;

    auto atleast1DigitOrAlpha = false;

    //
    // Read at least 1 alnum before entering loop
    //
    if (curSource.isDigitOrAlpha()) {
        
        atleast1DigitOrAlpha = true;

        LongName.put(curSource.to_char());
        
        curSource = TheByteDecoder->nextSourceCharacter();

        while (true) {
        
            //
            // No need to check isAbort() inside decoder loops
            //
            
            if (curSource.isDigitOrAlpha()) {
                
                LongName.put(curSource.to_char());
                
                curSource = TheByteDecoder->nextSourceCharacter();
                
            } else if (curSource == SourceCharacter(']')) {
                
                wellFormed = true;
                
                break;
                
            } else {
                
                //
                // Unrecognized
                //
                // Something like \[A!] which is not a long name
                //
                
                break;
            }
        }
    }
    
    auto LongNameStr = LongName.str();
    
    if (!wellFormed) {

        //
        // Not well-formed
        //

        auto Loc = TheSourceManager->getSourceLocation();

        if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
            
            if (atleast1DigitOrAlpha) {
                
                //
                // Something like \[A!]
                // Something like \[CenterDot\]
                //
                // Make the warning message a little more relevant
                //
                
                auto curSourceStr = curSource.string();
                
                auto suggestion = longNameSuggestion(LongNameStr);
                
                if (!suggestion.empty()) {
                    suggestion = std::string("\nDid you mean ``\\[") + suggestion + "]``?";
                }
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\[") + LongNameStr +
                                         curSourceStr + "``." + suggestion, SYNTAXISSUESEVERITY_ERROR,
                                         Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
                
            } else {
                
                //
                // Malformed some other way
                //
                // Something like \[!
                // Something like \[*
                //
                
                auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                
                curSourceStr = Utils::makeGraphical(curSourceStr);
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\[") + LongNameStr +
                                         curSourceStr + "``.\nDid you mean ``[" + LongNameStr + curSourceStr + "`` or ``\\\\[" + LongNameStr + curSourceStr +
                                         "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
            }
        }
        
        TheSourceManager->setSourceLocation(CharacterStart);
        TheSourceManager->setWLCharacterStart();
        TheSourceManager->setWLCharacterEnd();
        
        TheByteDecoder->append('[', CharacterStart+1);
        for (size_t i = 0; i < LongNameStr.size(); i++) {
            TheByteDecoder->append(LongNameStr[i], CharacterStart+2+i);
        }
        for (auto b : curSource.string()) {
            TheByteDecoder->append(b, Loc);
        }

        return WLCharacter('\\');
    }

    //
    // Well-formed
    //

    //
    // if unlikelyEscapeChecking, then make sure to append all of the Source characters again
    //
    
    auto it = LongNameToCodePointMap.find(LongNameStr);
    if (it == LongNameToCodePointMap.end() ||
        unlikelyEscapeChecking) {
        
        //
        // Unrecognized name
        //
        
        auto Loc = TheSourceManager->getSourceLocation();
        
        if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
            
            auto suggestion = longNameSuggestion(LongNameStr);
            
            if (!suggestion.empty()) {
                suggestion = std::string("\nDid you mean ``\\[") + suggestion + "]``?";
            }
            
            auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\[") + LongNameStr +
                                     "]``." + suggestion, SYNTAXISSUESEVERITY_ERROR,
                                     Source(CharacterStart, Loc));
            
            Issues.push_back(Issue);
        }
        
        if (unlikelyEscapeChecking) {
            
            auto Issue = SyntaxIssue(SYNTAXISSUETAG_ESCAPESEQUENCE, std::string("Unlikely escape sequence: ``\\\\[") + LongNameStr +
                                     "]``.\nDid you mean ``\\[" + LongNameStr + "]``?", SYNTAXISSUESEVERITY_REMARK,
                                     Source(CharacterStart-1, Loc));
            
            Issues.push_back(Issue);
        }
        
        TheSourceManager->setSourceLocation(CharacterStart);
        TheSourceManager->setWLCharacterStart();
        TheSourceManager->setWLCharacterEnd();
        
        TheByteDecoder->append('[', CharacterStart+1);
        for (size_t i = 0; i < LongNameStr.size(); i++) {
            TheByteDecoder->append(LongNameStr[i], CharacterStart+2+i);
        }
        TheByteDecoder->append(']', Loc);
        
        return WLCharacter('\\');
    }

    //
    // Success!
    //

    auto Loc = TheSourceManager->getSourceLocation();
    
    auto point = it->second;
    
    if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
        
        //
        // The well-formed, recognized name could still be unsupported or undocumented
        //
        if (Utils::isUnsupportedLongName(LongNameStr)) {
            
            auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNSUPPORTEDCHARACTER, std::string("Unsupported character: ``\\[") + LongNameStr +
                                     "]``.", SYNTAXISSUESEVERITY_ERROR,
                                     Source(CharacterStart, Loc));
            
            Issues.push_back(Issue);
            
        } else if (Utils::isUndocumentedLongName(LongNameStr)) {
            
            auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNDOCUMENTEDCHARACTER, std::string("Undocumented character: ``\\[") + LongNameStr +
                                     "]``.", SYNTAXISSUESEVERITY_REMARK,
                                     Source(CharacterStart, Loc));
            
            Issues.push_back(Issue);
        }
    }
    
    return WLCharacter(point, ESCAPE_LONGNAME);
}

//
// return the next WL character
//
// CharacterStart: location of \
//
WLCharacter CharacterDecoder::handle4Hex(SourceCharacter curSourceIn, SourceLocation CharacterStart, NextCharacterPolicy policy) {
    
    auto curSource = curSourceIn;
    
    assert(curSource == SourceCharacter(':'));
    
    std::ostringstream Hex;
    
    
    for (auto i = 0; i < 4; i++) {
        
        curSource = TheByteDecoder->nextSourceCharacter();
        
        if (curSource.isHex()) {
            
            Hex.put(curSource.to_char());
            
        } else {
            
            //
            // Not well-formed
            //
            // Something like \:z
            //

            auto HexStr = Hex.str();
        
            auto Loc = TheSourceManager->getSourceLocation();
            
            if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
                
                auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                
                curSourceStr = Utils::makeGraphical(curSourceStr);
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\:") + HexStr + curSourceStr +
                                         "``.\nDid you mean ``:" + HexStr + curSourceStr + "`` or ``\\\\:" + HexStr + curSourceStr + "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
            }
            
            TheSourceManager->setSourceLocation(CharacterStart);
            TheSourceManager->setWLCharacterStart();
            TheSourceManager->setWLCharacterEnd();
            
            TheByteDecoder->append(':', CharacterStart+1);
            for (size_t i = 0; i < HexStr.size(); i++) {
                TheByteDecoder->append(HexStr[i], CharacterStart+2+i);
            }
            for (auto b : curSource.string()) {
                TheByteDecoder->append(b, Loc);
            }
            
            return WLCharacter('\\');
        }
    }

    auto HexStr = Hex.str();
    
    auto point = Utils::parseInteger(HexStr, 16);
    
    return WLCharacter(point, ESCAPE_4HEX);
}

//
// return the next WL character
//
// CharacterStart: location of \
//
WLCharacter CharacterDecoder::handle2Hex(SourceCharacter curSourceIn, SourceLocation CharacterStart, NextCharacterPolicy policy) {
    
    auto curSource = curSourceIn;
    
    assert(curSource == SourceCharacter('.'));
    
    std::ostringstream Hex;
    
    for (auto i = 0; i < 2; i++) {
        
        curSource = TheByteDecoder->nextSourceCharacter();
        
        if (curSource.isHex()) {
            
            Hex.put(curSource.to_char());
            
        } else {
            
            //
            // Not well-formed
            //
            // Something like \.z
            //

            auto HexStr = Hex.str();
        
            auto Loc = TheSourceManager->getSourceLocation();
            
            if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
                
                auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                
                curSourceStr = Utils::makeGraphical(curSourceStr);
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, "Unrecognized character: ``\\." + HexStr +
                                         curSourceStr + "``.\nDid you mean ``." + HexStr + curSourceStr + "`` or ``\\\\." + HexStr + curSourceStr + "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
            }
            
            TheSourceManager->setSourceLocation(CharacterStart);
            TheSourceManager->setWLCharacterStart();
            TheSourceManager->setWLCharacterEnd();
            
            TheByteDecoder->append('.', CharacterStart);
            for (size_t i = 0; i < HexStr.size(); i++) {
                TheByteDecoder->append(HexStr[i], CharacterStart+2+i);
            }
            for (auto b : curSource.string()) {
                TheByteDecoder->append(b, Loc);
            }

            return WLCharacter('\\');
        }
    }
    
    auto HexStr = Hex.str();

    auto point = Utils::parseInteger(HexStr, 16);
    
    return WLCharacter(point, ESCAPE_2HEX);
}

//
// return the next WL character
//
// CharacterStart: location of \
//
WLCharacter CharacterDecoder::handleOctal(SourceCharacter curSourceIn, SourceLocation CharacterStart, NextCharacterPolicy policy) {
    
    auto curSource = curSourceIn;
    
    assert(curSource.isOctal());
    
    std::ostringstream Octal;
    
    Octal.put(curSource.to_char());
    
    for (auto i = 0; i < 3-1; i++) {
        
        curSource = TheByteDecoder->nextSourceCharacter();
        
        if (curSource.isOctal()) {
            
            Octal.put(curSource.to_char());
            
        } else {
            
            //
            // Not well-formed
            //
            // Something like \z
            //

            auto OctalStr = Octal.str();
        
            auto Loc = TheSourceManager->getSourceLocation();
            
            if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
                
                auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                
                curSourceStr = Utils::makeGraphical(curSourceStr);
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\") + OctalStr +
                                         curSourceStr + "``.\nDid you mean ``" + OctalStr + curSourceStr + "`` or ``\\\\" + OctalStr + curSourceStr + "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
            }
            
            TheSourceManager->setSourceLocation(CharacterStart);
            TheSourceManager->setWLCharacterStart();
            TheSourceManager->setWLCharacterEnd();
            
            for (size_t i = 0; i < OctalStr.size(); i++) {
                TheByteDecoder->append(OctalStr[i], CharacterStart+1+i);
            }
            for (auto b : curSource.string()) {
                TheByteDecoder->append(b, Loc);
            }
            
            return WLCharacter('\\');
        }
    }
    
    auto OctalStr = Octal.str();

    auto point = Utils::parseInteger(OctalStr, 8);
    
    return WLCharacter(point, ESCAPE_OCTAL);
}

//
// return the next WL character
//
// CharacterStart: location of \
//
WLCharacter CharacterDecoder::handle6Hex(SourceCharacter curSourceIn, SourceLocation CharacterStart, NextCharacterPolicy policy) {
    
    auto curSource = curSourceIn;
    
    assert(curSource == SourceCharacter('|'));
    
    std::ostringstream Hex;
    
    for (auto i = 0; i < 6; i++) {
        
        curSource = TheByteDecoder->nextSourceCharacter();
        
        if (curSource.isHex()) {
            
            Hex.put(curSource.to_char());
            
        } else {
            
            //
            // Not well-formed
            //
            // Something like \|z
            //

            auto HexStr = Hex.str();
        
            auto Loc = TheSourceManager->getSourceLocation();
            
            if ((policy & DISABLE_CHARACTERDECODINGISSUES) != DISABLE_CHARACTERDECODINGISSUES) {
                
                auto curSourceStr = (curSource == SOURCECHARACTER_ENDOFFILE) ? std::string("") : curSource.string();
                
                curSourceStr = Utils::makeGraphical(curSourceStr);
                
                auto Issue = SyntaxIssue(SYNTAXISSUETAG_UNRECOGNIZEDCHARACTER, std::string("Unrecognized character: ``\\|") + HexStr +
                                         curSourceStr + "``.\nDid you mean ``|" + HexStr + curSourceStr + "`` or ``\\\\|" + HexStr + curSourceStr + "``?", SYNTAXISSUESEVERITY_ERROR, Source(CharacterStart, Loc));
                
                Issues.push_back(Issue);
            }
            
            TheSourceManager->setSourceLocation(CharacterStart);
            TheSourceManager->setWLCharacterStart();
            TheSourceManager->setWLCharacterEnd();
            
            TheByteDecoder->append('|', CharacterStart+1);
            for (size_t i = 0; i < HexStr.size(); i++) {
                TheByteDecoder->append(HexStr[i], CharacterStart+2+i);
            }
            for (auto b : curSource.string()) {
                TheByteDecoder->append(b, Loc);
            }
            
            return WLCharacter('\\');
        }
    }
    
    auto HexStr = Hex.str();

    auto point = Utils::parseInteger(HexStr, 16);
    
    return WLCharacter(point, ESCAPE_6HEX);
}

std::vector<SyntaxIssue> CharacterDecoder::getIssues() const {
    return Issues;
}

std::vector<Metadata> CharacterDecoder::getMetadatas() const {
    
    std::vector<Metadata> Metadatas;
    
    auto totalTimeMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalTimeMicros);
    
    Metadatas.push_back(Metadata("CharacterDecoderTotalTimeMillis", std::to_string(totalTimeMillis.count())));
    
    return Metadatas;
}

CharacterDecoder *TheCharacterDecoder = nullptr;


//
// Respect the actual escape style 
//
std::ostream& operator<<(std::ostream& stream, WLCharacter c) {
    
    auto i = c.to_point();
    
    auto escape = c.escape();
    
    switch (escape) {
        case ESCAPE_NONE: {
            stream << SourceCharacter(i);
            return stream;
        }
        case ESCAPE_SINGLE: {
            switch (i) {
                case '\b':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('b');
                    return stream;
                case '\f':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('f');
                    return stream;
                case '\n':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('n');
                    return stream;
                case '\r':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('r');
                    return stream;
                case '\t':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('t');
                    return stream;
                case '\\':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('\\');
                    return stream;
                case '"':
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('"');
                    return stream;
                case CODEPOINT_STRINGMETA_OPEN:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('<');
                    return stream;
                case CODEPOINT_STRINGMETA_CLOSE:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('>');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_BANG:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('!');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_PERCENT:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('%');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_AMP:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('&');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_OPENPAREN:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('(');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_CLOSEPAREN:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter(')');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_STAR:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('*');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_PLUS:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('+');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_SLASH:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('/');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_AT:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('@');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_CARET:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('^');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_UNDER:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('_');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_BACKTICK:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter('`');
                    return stream;
                case CODEPOINT_LINEARSYNTAX_SPACE:
                    stream << SourceCharacter('\\');
                    stream << SourceCharacter(' ');
                    return stream;
                default:
                    assert(false);
            }
        }
        case ESCAPE_LONGNAME: {
            
            auto LongName = CodePointToLongNameMap[i];
            
            stream << SourceCharacter('\\');
            stream << SourceCharacter('[');
            for (size_t idx = 0; idx < LongName.size(); idx++) {
                stream << SourceCharacter(LongName[idx]);
            }
            stream << SourceCharacter(']');
            
            return stream;
        }
        case ESCAPE_OCTAL: {
            
            auto ii = i;
            auto o0 = ii % 8;
            ii = ii / 8;
            auto o1 = ii % 8;
            ii = ii / 8;
            auto o2 = ii % 8;
            
            stream << SourceCharacter('\\');
            stream << SourceCharacter(Utils::fromDigit(o2));
            stream << SourceCharacter(Utils::fromDigit(o1));
            stream << SourceCharacter(Utils::fromDigit(o0));
            return stream;
        }
        case ESCAPE_2HEX: {
            
            auto ii = i;
            auto x0 = ii % 16;
            ii = ii / 16;
            auto x1 = ii % 16;
            
            stream << SourceCharacter('\\');
            stream << SourceCharacter('.');
            stream << SourceCharacter(Utils::fromDigit(x1));
            stream << SourceCharacter(Utils::fromDigit(x0));
            return stream;
        }
        case ESCAPE_4HEX: {
            
            auto ii = i;
            auto x0 = ii % 16;
            ii = ii / 16;
            auto x1 = ii % 16;
            ii = ii / 16;
            auto x2 = ii % 16;
            ii = ii / 16;
            auto x3 = ii % 16;
            
            stream << SourceCharacter('\\');
            stream << SourceCharacter(':');
            stream << SourceCharacter(Utils::fromDigit(x3));
            stream << SourceCharacter(Utils::fromDigit(x2));
            stream << SourceCharacter(Utils::fromDigit(x1));
            stream << SourceCharacter(Utils::fromDigit(x0));
            return stream;
        }
        case ESCAPE_6HEX: {
            
            auto ii = i;
            auto x0 = ii % 16;
            ii = ii / 16;
            auto x1 = ii % 16;
            ii = ii / 16;
            auto x2 = ii % 16;
            ii = ii / 16;
            auto x3 = ii % 16;
            ii = ii / 16;
            auto x4 = ii % 16;
            ii = ii / 16;
            auto x5 = ii % 16;
            
            stream << SourceCharacter('\\');
            stream << SourceCharacter('|');
            stream << SourceCharacter(Utils::fromDigit(x5));
            stream << SourceCharacter(Utils::fromDigit(x4));
            stream << SourceCharacter(Utils::fromDigit(x3));
            stream << SourceCharacter(Utils::fromDigit(x2));
            stream << SourceCharacter(Utils::fromDigit(x1));
            stream << SourceCharacter(Utils::fromDigit(x0));
            return stream;
        }
        default: {
            assert(false);
            return stream;
        }
    }
}

bool WLCharacter::operator==(const WLCharacter &o) const {
    return value_ == o.value_ && escape_ == o.escape_;
}

bool WLCharacter::operator!=(const WLCharacter &o) const {
    return value_ != o.value_ || escape_ != o.escape_;
}

//
// for std::map
//
bool WLCharacter::operator<(const WLCharacter &o) const {
    return value_ < o.value_;
}

std::string WLCharacter::string() const {
    
    std::ostringstream String;
    
    String << *this;
    
    return String.str();
}

bool WLCharacter::isEscaped() const {
    return escape_ != ESCAPE_NONE;
}

bool WLCharacter::isDollar() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return value_ == '$';
}

bool WLCharacter::isLetterlike() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    
    //
    // Most of ASCII control characters are letterlike.
    //
    // Except for BEL and DEL. Note that '\x07' and '\x7f' are missing.
    //
    // The control characters could be escaped, so do not check isEscaped() here
    //
    
    return std::isalpha(value_) || value_ == '$' || value_ == 0x00 || value_ == 0x01 || value_ == 0x02 ||
    value_ == 0x03 || value_ == 0x04 || value_ == 0x05 || value_ == 0x06 || value_ == 0x08 || value_ == 0x0e ||
    value_ == 0x0f || value_ == 0x10 || value_ == 0x11 || value_ == 0x12 || value_ == 0x13 || value_ == 0x14 ||
    value_ == 0x15 || value_ == 0x16 || value_ == 0x17 || value_ == 0x18 || value_ == 0x19 || value_ == 0x1a ||
    value_ == 0x1b || value_ == 0x1c || value_ == 0x1d || value_ == 0x1e ||value_ == 0x1f;
}

bool WLCharacter::isStrangeLetterlike() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    
    //
    // Dump out if not a letterlike character
    //
    if (!isLetterlike()) {
        return false;
    }
    
    //
    // Using control character as letterlike is strange
    //
    if (isControl()) {
        return true;
    }
    
    return false;
}

bool WLCharacter::isSpace() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return value_ == ' ' || value_ == '\t' || value_ == '\v' || value_ == '\f';
}

bool WLCharacter::isNewline() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return value_ == '\n' || value_ == '\r';
}

bool WLCharacter::isEndOfFile() const {
    return value_ == EOF;
}

bool WLCharacter::isDigit() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return std::isdigit(value_);
}

bool WLCharacter::isAlpha() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return std::isalpha(value_);
}

bool WLCharacter::isPunctuation() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return std::ispunct(value_);
}

bool WLCharacter::isUninterpretable() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    if (isEscaped()) {
        return false;
    }
    return value_ == 0x07 || value_ == 0x7f;
}

bool WLCharacter::isLinearSyntax() const {
    switch (value_) {
        case CODEPOINT_LINEARSYNTAX_CLOSEPAREN:
        case CODEPOINT_LINEARSYNTAX_BANG:
        case CODEPOINT_LINEARSYNTAX_AT:
        case CODEPOINT_LINEARSYNTAX_PERCENT:
        case CODEPOINT_LINEARSYNTAX_CARET:
        case CODEPOINT_LINEARSYNTAX_AMP:
        case CODEPOINT_LINEARSYNTAX_STAR:
        case CODEPOINT_LINEARSYNTAX_OPENPAREN:
        case CODEPOINT_LINEARSYNTAX_UNDER:
        case CODEPOINT_LINEARSYNTAX_PLUS:
        case CODEPOINT_LINEARSYNTAX_SLASH:
        case CODEPOINT_LINEARSYNTAX_BACKTICK:
        case CODEPOINT_LINEARSYNTAX_SPACE:
            return true;
        default:
            return false;
    }
}

bool WLCharacter::isControl() const {
    if (!(0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    return std::iscntrl(value_);
}

//
// isLetterlikeCharacter is special because it is defined in terms of other categories
//
// basically, if it's not anything else, then it's letterlike
//
bool WLCharacter::isLetterlikeCharacter() const {
    //
    // Reject if ASCII, should use isLetterlike()
    //
    if ((0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    
    if (isPunctuationCharacter()) {
        return false;
    }
    
    if (isEndOfFile()) {
        return false;
    }
    
    if (isLinearSyntax()) {
        return false;
    }

    if (isSpaceCharacter()) {
        return false;
    }

    if (isNewlineCharacter()) {
        return false;
    }

    if (isUninterpretableCharacter()) {
        return false;
    }
    
    if (escape_ == ESCAPE_LONGNAME && Utils::isUnsupportedLongName(CodePointToLongNameMap[value_])) {
        return false;
    }

    return true;
}

bool WLCharacter::isStrangeLetterlikeCharacter() const {
    
    //
    // Dump out if not a letterlike character
    //
    if (!isLetterlikeCharacter()) {
        return false;
    }
    
    //
    // Assume that if some high character like 0xf456 is directly encoded with no escaping, then it is purposeful
    //
    if (escape_ == ESCAPE_NONE) {
        return false;
    }

    //
    //
    //
    if (escape_ == ESCAPE_LONGNAME) {
        return Utils::isStrangeLetterlikeLongName(CodePointToLongNameMap[value_]);
    }
    
    //
    // Using control character as letterlike is strange
    //
    if (isControlCharacter()) {
        return true;
    }
    
    //
    // Assume that using other escapes is strange
    //
    
    return true;
}

bool WLCharacter::isControlCharacter() const {
    //
    // Reject if ASCII, should use isControl()
    //
    if ((0 <= value_ && value_ <= 0x7f)) {
        return false;
    }
    
    //
    // C1 block of Unicode
    //
    if ((0x80 <= value_ && value_ <= 0x9f)) {
        return true;
    }
    
    return false;
}

//
// TODO: implement a suggestion mechanism for long name typos
//
// example:
// input: Alpa
// return Alpha
//
// Return empty string if no suggestion.
//
std::string CharacterDecoder::longNameSuggestion(std::string input) {
    return "";
}




