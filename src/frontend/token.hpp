#pragma once

#include "common/source.hpp"
#include <string>

namespace transom {

enum class TokenKind {
    // Literals
    Int,
    Float,
    String,
    StringPart,   // chunk of an interpolated string before {
    InterpStart,  // { inside an interpolated string
    InterpEnd,    // } closing an interpolated expression
    StringEnd,    // final chunk of an interpolated string
    Ident,

    // Keywords
    KwImport,
    KwUse,
    KwStyle,
    KwPlugin,
    KwExport,
    KwApp,
    KwComponent,
    KwStruct,
    KwEnum,
    KwMatch,
    KwPersist,
    KwState,
    KwDerived,
    KwView,
    KwSlot,
    KwEvery,
    KwFn,
    KwLet,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwBreak,
    KwContinue,
    KwConst,
    KwReturn,
    KwTrue,
    KwFalse,
    KwNull,
    KwAnd,
    KwOr,
    KwNot,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Bang,
    AmpAmp,
    PipePipe,
    Arrow, // ->
    FatArrow, // =>
    PipeGreater, // |>
    QuestionQuestion, // ??

    // Delimiters
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Dot,
    Semicolon,
    Newline,

    Eof,
    Error
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string lexeme;
    Span span;
};

const char* tokenKindName(TokenKind kind);

} // namespace transom
