/* vim: set ts=2 sw=2 tw=99 et:
 *
 * Copyright (C) 2012 David Anderson
 *
 * This file is part of SourcePawn.
 *
 * SourcePawn is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 * 
 * SourcePawn is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * SourcePawn. If not, see http://www.gnu.org/licenses/.
 */
#include <string.h>
#include "string-pool.h"
#include "parser.h"

using namespace ke;

Parser::Parser(CompileContext &cc, TranslationUnit *tu)
: cc_(cc),
  pool_(cc_.pool()),
  tu_(tu),
  scanner_(cc, tu),
  allowDeclarations_(true),
  allowSingleLineFunctions_(true)
{
  atom_Float_ = cc_.add("Float");
  atom_String_ = cc_.add("String");
  atom_underbar_ = cc_.add("_");
}

bool
Parser::peek(TokenKind kind)
{
  return scanner_.peek() == kind;
}

bool
Parser::match(TokenKind kind)
{
  if (scanner_.next() == kind)
    return true;
  scanner_.undo();
  return false;
}

bool
Parser::expect(TokenKind kind)
{
  TokenKind got = scanner_.next();
  if (got == kind)
    return true;
  cc_.reportError(scanner_.begin(), Message_WrongToken, TokenNames[kind], TokenNames[got]);
  return false;
}

Atom *
Parser::maybeName()
{
  if (!match(TOK_NAME))
    return nullptr;
  return scanner_.current_name();
}

Atom *
Parser::expectName()
{
  if (!expect(TOK_NAME))
    return nullptr;
  return scanner_.current_name();
}

// Requires ; or \n.
bool
Parser::requireTerminator()
{
  if (scanner_.requireSemicolons())
    return expect(TOK_SEMICOLON);
  if (match(TOK_SEMICOLON))
    return true;
  if (scanner_.peekTokenSameLine() == TOK_EOL)
    return true;
  cc_.reportError(scanner_.begin(), Message_ExpectedNewlineOrSemi);
  return false;
}

// Requires \n on the same line.
bool
Parser::requireNewline()
{
  if (scanner_.peekTokenSameLine() == TOK_EOL)
    return true;
  cc_.reportError(scanner_.begin(), Message_ExpectedNewline);
  return false;
}

// Requires ; or \n on the same line.
bool
Parser::requireNewlineOrSemi()
{
  if (scanner_.peekTokenSameLine() == TOK_SEMICOLON)
    scanner_.next();
  if (scanner_.peekTokenSameLine() == TOK_EOL)
    return true;
  cc_.reportError(scanner_.begin(), Message_ExpectedNewline);
  return false;
}

void
Parser::parse_new_typename(TypeSpecifier *spec, const Token *tok)
{
  if (IsNewTypeToken(tok->kind)) {
    spec->setBuiltinType(tok->kind);
    return;
  }

  if (tok->kind == TOK_LABEL) {
    NameProxy *proxy = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());
    spec->setNamedType(TOK_LABEL, proxy);
    cc_.reportError(scanner_.begin(), Message_NewDeclsRequired);
    return;
  }

  if (tok->kind != TOK_NAME) {
    cc_.reportError(scanner_.begin(), Message_ExpectedTypeExpr);
    return;
  }

  NameProxy *proxy = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());
  spec->setNamedType(TOK_NAME, proxy);

  if (proxy->name() == atom_Float_)
    cc_.reportError(scanner_.begin(), Message_TypeIsDeprecated, "Float", "float");
  else if (proxy->name() == atom_String_)
    cc_.reportError(scanner_.begin(), Message_TypeIsDeprecated, "String", "char");
  else if (proxy->name() == atom_underbar_)
    cc_.reportError(scanner_.begin(), Message_TypeIsDeprecated, "_", "int");
}

void
Parser::parse_function_type(TypeSpecifier *spec, uint32_t flags)
{
  TypeSpecifier returnType;
  parse_new_type_expr(&returnType, 0);

  // :TODO: only allow new-style arguments.
  ParameterList *params = arguments();
  if (!params)
    return;

  FunctionSignature *signature = new (pool_) FunctionSignature(returnType, params);
  spec->setFunctionType(signature);
}

void
Parser::parse_new_type_expr(TypeSpecifier *spec, uint32_t flags)
{
  if (match(TOK_CONST)) {
    if (spec->isConst())
      cc_.reportError(scanner_.begin(), Message_ConstSpecifiedTwice);
    spec->setConst(scanner_.begin());
  }

  bool lparen = match(TOK_LPAREN);
  bool function = lparen
                  ? expect(TOK_FUNCTION)
                  : match(TOK_FUNCTION);
  if (function)
    parse_function_type(spec, flags);
  else
    parse_new_typename(spec, scanner_.nextToken());

  if (lparen)
    match(TOK_RPAREN);

  // If we didn't already fill out array dimensions, do so now.
  if (!spec->isArray() && match(TOK_LBRACKET)) {
    SourceLocation begin = scanner_.begin();
    uint32_t rank = 0;
    do {
      rank++;
      if (!match(TOK_RBRACKET))
        cc_.reportError(scanner_.begin(), Message_FixedArrayInPrefix);
    } while (match(TOK_LBRACKET));
    spec->setRank(begin, rank);
  }

  if (flags & DeclFlags::Argument) {
    if (match(TOK_AMPERSAND)) {
      if (!spec->isArray())
        spec->setByRef(scanner_.begin());
      else
        cc_.reportError(scanner_.begin(), Message_TypeCannotBeReference, "array");
    }
  }
}

bool
Parser::parse_new_decl(Declaration *decl, uint32_t flags)
{
  parse_new_type_expr(&decl->spec, flags);

  if (flags & DeclFlags::NamedMask) {
    bool named = false;
    if (flags & DeclFlags::MaybeNamed) {
      named = match(TOK_NAME);
    } else {
      if (!expect(TOK_NAME))
        return false;
      named = true;
    }

    if (named) {
      decl->name = scanner_.current();
      if (match(TOK_LBRACKET))
        parse_old_array_dims(decl, flags);
    }
  }

  return true;
}

void
Parser::parse_old_array_dims(Declaration *decl, uint32_t flags)
{
  TypeSpecifier *spec = &decl->spec;

  SourceLocation loc = scanner_.begin();
  if (spec->isByRef())
    cc_.reportError(loc, Message_TypeCannotBeReference, "array");

  uint32_t rank = 0;
  ExpressionList *dims = nullptr;
  do {
    rank++;

    // Check if the size is unspecified.
    if (match(TOK_RBRACKET)) {
      if (dims)
        dims->append(nullptr);
      continue;
    }

    // Allocate a list to store dimension sizes.
    if (!dims) {
      dims = new (pool_) ExpressionList();
      for (uint32_t i = 0; i < rank; i++)
        dims->append(nullptr);
    }

    Expression *expr = expression();
    if (!expr)
      break;
    dims->append(expr);

    if (!expect(TOK_RBRACKET))
      break;
  } while (match(TOK_LBRACKET));

  if (spec->isArray()) {
    cc_.reportError(loc, Message_DoubleArrayDims);
    return;
  }

  if (dims)
    spec->setDimensionSizes(loc, dims);
  else
    spec->setRank(loc, rank);

  spec->setHasPostDims();
}

bool
Parser::parse_old_decl(Declaration *decl, uint32_t flags)
{
  TypeSpecifier *spec = &decl->spec;

  if (match(TOK_CONST)) {
    if (spec->isConst())
      cc_.reportError(scanner_.begin(), Message_ConstSpecifiedTwice);
    spec->setConst(scanner_.begin());
  }

  if (flags & DeclFlags::Argument) {
    if (match(TOK_AMPERSAND))
      spec->setByRef(scanner_.begin());
  }

  if (match(TOK_LABEL)) {
    NameProxy *proxy = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());
    spec->setNamedType(TOK_LABEL, proxy);
  } else {
    spec->setBuiltinType(TOK_IMPLICIT_INT);
  }

  // Look for varargs and end early.
  if ((flags & DeclFlags::Argument) && match(TOK_ELLIPSES)) {
    spec->setVariadic(scanner_.begin());
    return true;
  }

  if (flags & DeclFlags::NamedMask) {
    // If this is label-less, check for something like "new int x".
    if (!peek(TOK_NAME)) {
      TokenKind kind = scanner_.next();
      if (IsNewTypeToken(kind))
        cc_.reportError(scanner_.begin(), Message_NewStyleBadKeyword);
      else
        scanner_.undo();
    }
    if (!expect(TOK_NAME))
      return false;

    decl->name = scanner_.current();

    if (match(TOK_LBRACKET))
      parse_old_array_dims(decl, flags);
  }

  // :TODO: require newdecls
  return true;
}

bool
Parser::reparse_decl(Declaration *decl, uint32_t flags)
{
  if (!decl->spec.isNewDecl()) {
    decl->spec.resetWithAttrs(TypeSpecifier::Const);
    return parse_old_decl(decl, flags);
  }

  // Newer decls are more complex to reparse.
  if (!expect(TOK_NAME))
    return false;
  decl->name = scanner_.current();

  if (decl->spec.hasPostDims()) {
    // We have something like:
    //   int x[], y...
    //
    // Reset the fact that we saw an array.
    decl->spec.resetArray();
    if (match(TOK_LBRACKET))
      parse_old_array_dims(decl, flags);
  } else {
    // Note: in spcomp2, we just have to make sure we're not doubling up on
    // dimension lists. In spcomp1, we had to reset the dimension sizes as
    // well because parsing initializers would change them.
    if (match(TOK_LBRACKET)) {
      if (decl->spec.isArray())
        cc_.reportError(scanner_.begin(), Message_DoubleArrayDims);
    }

    assert(!decl->spec.dims());
  }

  return true;
}

// The infamous parse_decl() from spcomp1.
bool
Parser::parse_decl(Declaration *decl, uint32_t flags)
{
  // Match early varargs as old decls.
  if ((flags & DeclFlags::Argument) && peek(TOK_ELLIPSES))
    return parse_old_decl(decl, flags);

  // Match const first - it's a common prefix for old and new decls.
  if (match(TOK_CONST))
    decl->spec.setConst(scanner_.begin());

  // Sometimes we know ahead of time whether the declaration will be old, for
  // example, if preceded by "new" or "decl".
  if (flags & DeclFlags::Old)
    return parse_old_decl(decl, flags);

  // If parsing an argument, there are two simple checks for whether this is a
  // new or old-style declaration.
  if ((flags & DeclFlags::Argument) && (peek(TOK_AMPERSAND) || peek(TOK_LBRACE)))
    return parse_old_decl(decl, flags);

  // Another dead giveaway is a label. Operators would work here too, but they
  // are not supported in spcomp2.
  if (peek(TOK_LABEL))
    return parse_old_decl(decl, flags);

  // Otherwise, eat a symbol and try to see what's afterit.
  if (match(TOK_NAME)) {
    if (peek(TOK_NAME) || peek(TOK_AMPERSAND)) {
      // This is a new-style declaration. Given the name back to the lexer.
      scanner_.undo();
      return parse_new_decl(decl, flags);
    }

    // Make sure to save the name token locally first.
    Token name = *scanner_.current();
    if ((flags & DeclFlags::NamedMask) && match(TOK_LBRACKET)) {
      // Oh no - we have to parse array dims before we ca n tell what kind of
      // declarator this is. It could be either:
      //   "x[] y" (new-style), or
      //   "y[],"  (old-style)
      parse_old_array_dims(decl, flags);

      if (peek(TOK_NAME) || peek(TOK_AMPERSAND)) {
        // This must be a newdecl, "x[] y" or "x[] &y", the latter of which
        // is illegal, but we flow it through the right path anyway.
        decl->spec.unsetHasPostDims();
        scanner_.pushBack(name);
        return parse_new_decl(decl, flags);
      }

      // We got something like "x[]". Just finish off the declaration.
      decl->name = name;
      decl->spec.setBuiltinType(TOK_INT);
      return true;
    }

    // Give the symbol back to the lexer; this is an old decl.
    scanner_.undo();
    return parse_old_decl(decl, flags);
  }

  // All else has failed. Probably got a type keyword. Try new-style.
  return parse_new_decl(decl, flags);
}

Expression *
Parser::primitive()
{
  switch (scanner_.next()) {
    case TOK_FLOAT_LITERAL:
    {
      Token *tok = scanner_.current();
      return new (pool_) FloatLiteral(tok->start, tok->doubleValue());
    }

    case TOK_HEX_LITERAL:
    {
      Token *tok = scanner_.current();
      return new (pool_) IntegerLiteral(tok->start, tok->intValue());
    }

    case TOK_INTEGER_LITERAL:
    {
      Token *tok = scanner_.current();
      return new (pool_) IntegerLiteral(tok->start, tok->intValue());
    }

    case TOK_TRUE:
    case TOK_FALSE:
      return new (pool_) BooleanLiteral(scanner_.begin(), scanner_.current()->kind);

    case TOK_STRING_LITERAL:
    {
      Atom *literal = scanner_.current_name();
      return new (pool_) StringLiteral(scanner_.begin(), literal);
    }

    case TOK_CHAR_LITERAL:
    {
      Token *tok = scanner_.current();
      return new (pool_) CharLiteral(scanner_.begin(), tok->charValue());
    }

    case TOK_THIS:
      return new (pool_) ThisExpression(scanner_.begin());

    case TOK_LBRACE:
      return parseCompoundLiteral();

    default:
    {
      TokenKind kind = scanner_.current()->kind;
      if (kind != TOK_ERROR)
        cc_.reportError(scanner_.begin(), Message_ExpectedExpression, TokenNames[kind]);
      return nullptr;
    }
  }
}

Expression *
Parser::parseStructInitializer(const SourceLocation &pos)
{
  NameAndValueList *pairs = new (pool_) NameAndValueList();

  while (!match(TOK_RBRACE)) {
    if (!expect(TOK_NAME))
      return nullptr;
    Token name = *scanner_.current();

    if (!match(TOK_ASSIGN))
      return nullptr;

    Expression *expr = expression();
    if (!expr)
      return nullptr;

    // Eat an optional comma.
    match(TOK_COMMA);

    pairs->append(new (pool_) NameAndValue(name, expr));
  }

  return new (pool_) StructInitializer(pos, pairs);
}

Expression *
Parser::parseCompoundLiteral()
{
  SourceLocation pos = scanner_.begin();

  // If the syntax is <literal> "=" we treat this as a struct initializer.
  if (match(TOK_NAME)) {
    bool assigns = peek(TOK_ASSIGN);

    // Push the name back.
    scanner_.undo();

    if (assigns)
      return parseStructInitializer(pos);
  }

  // Otherwise, we need to build a list.
  ExpressionList *list = new (pool_) ExpressionList();
  while (!peek(TOK_RBRACE)) {
    Expression *item = expression();
    if (!item)
      return nullptr;
    if (!match(TOK_COMMA))
      break;
  }
  expect(TOK_RBRACE);

  return new (pool_) ArrayLiteral(pos, TOK_LBRACE, list);
}

Expression *
Parser::prefix()
{
  switch (scanner_.next()) {
    case TOK_LPAREN:
    {
      Expression *expr = expression();
      if (!expr)
        return nullptr;
      if (!expect(TOK_RPAREN))
        return nullptr;
      return expr;
    }

    case TOK_NAME:
      return new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());

    default:
    {
      if (IsNewTypeToken(scanner_.current()->kind)) {
        // Treat the type as a name, even though it's a keyword.
        Atom *atom = cc_.add(scanner_.literal());
        return new (pool_) NameProxy(scanner_.begin(), atom);
      }

      scanner_.undo();
      return primitive();
    }
  }
}

Expression *
Parser::call(Expression *callee)
{
  ExpressionList *arguments = new (pool_) ExpressionList();

  SourceLocation pos = scanner_.begin();
  expect(TOK_LPAREN);
  
  if (!match(TOK_RPAREN)) {
    while (true) {
      Expression *expr = expression();
      if (!expr)
        return nullptr;

      if (!arguments->append(expr))
        return nullptr;

      if (!match(TOK_COMMA))
        break;
    }

    if (!expect(TOK_RPAREN))
      return nullptr;
  }

  return new (pool_) CallExpression(pos, callee, arguments);
}

Expression *
Parser::index(Expression *left)
{
  expect(TOK_LBRACKET);
  
  SourceLocation pos = scanner_.begin();
  Expression *expr = expression();
  if (!expr)
    return nullptr;

  if (!expect(TOK_RBRACKET))
    return nullptr;

  return new (pool_) IndexExpression(pos, left, expr);
}

Expression *
Parser::primary()
{
  Expression *expr = prefix();
  if (!expr)
    return nullptr;

  for (;;) {
    switch (scanner_.peek()) {
      case TOK_LPAREN:
        if ((expr = call(expr)) == nullptr)
          return nullptr;
        break;

      case TOK_LBRACKET:
        if ((expr = index(expr)) == nullptr)
          return nullptr;
        break;

      default:
        return expr;
    }
  }
}

Expression *
Parser::unary()
{
  TokenKind token = scanner_.peek();

  SourceLocation pos = scanner_.begin();
  switch (token) {
    case TOK_INCREMENT:
    case TOK_DECREMENT:
    {
      scanner_.next();
      Expression *expr = unary();
      if (!expr)
        return nullptr;
      return new (pool_) IncDecExpression(pos, token, expr, false);
    }

    case TOK_MINUS:
    case TOK_NOT:
    case TOK_TILDE:
    {
      scanner_.next();
      Expression *expr = unary();
      if (!expr)
        return nullptr;
      if (token == TOK_MINUS)
        token = TOK_NEGATE;
      return new (pool_) UnaryExpression(pos, token, expr);
    }

    case TOK_SIZEOF:
    {
      scanner_.next();
      if (!expect(TOK_LPAREN))
        return nullptr;
      Expression *expr = unary();
      if (!expr)
        return nullptr;
      if (!expect(TOK_RPAREN))
        return nullptr;
      return new (pool_) UnaryExpression(pos, token, expr);
    }

    case TOK_LABEL:
    {
      scanner_.next();
      Atom *tag = scanner_.current_name();

      NameProxy *proxy = new (pool_) NameProxy(scanner_.begin(), tag);

      Expression *expr = unary();
      if (!expr)
        return nullptr;

      return new (pool_) UnaryExpression(pos, TOK_LABEL, expr, proxy);
    }

    default:
      break;
  }

  Expression *expr = primary();

  token = scanner_.peek();
  if (token == TOK_INCREMENT || token == TOK_DECREMENT) {
    scanner_.next();
    return new (pool_) IncDecExpression(pos, token, expr, true);
  }

  return expr;
}

Expression *
Parser::multiplication()
{
  Expression *left = unary();
  do {
    if (!match(TOK_SLASH) && !match(TOK_STAR) && !match(TOK_PERCENT))
      break;
    SourceLocation pos = scanner_.begin();
    TokenKind kind = scanner_.current()->kind;
    Expression *right = unary();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, kind, left, right);
  } while (left);
  return left;
}

Expression *
Parser::addition()
{
  Expression *left = multiplication();
  while (left) {
    if (!match(TOK_PLUS) && !match(TOK_MINUS))
      break;
    SourceLocation pos = scanner_.begin();
    TokenKind kind = scanner_.current()->kind;
    Expression *right = multiplication();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, kind, left, right);
  }
  return left;
}

Expression *
Parser::shift()
{
  Expression *left = addition();
  while (left) {
    if (!match(TOK_SHL) && !match(TOK_SHR) && !match(TOK_USHR))
      break;
    SourceLocation pos = scanner_.begin();
    TokenKind kind = scanner_.current()->kind;
    Expression *right = addition();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, kind, left, right);
  }
  return left;
}

Expression *
Parser::bitand_()
{
  Expression *left = shift();
  while (left) {
    if (!match(TOK_BITAND))
      break;
    SourceLocation pos = scanner_.begin();
    Expression *right = shift();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, TOK_BITAND, left, right);
  }
  return left;
}

Expression *
Parser::bitxor()
{
  Expression *left = bitand_();
  while (left) {
    if (!match(TOK_BITXOR))
      break;
    SourceLocation pos = scanner_.begin();
    Expression *right = shift();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, TOK_BITXOR, left, right);
  }
  return left;
}

Expression *
Parser::bitor_()
{
  Expression *left = bitxor();
  while (left) {
    if (!match(TOK_BITOR))
      break;
    SourceLocation pos = scanner_.begin();
    Expression *right = bitxor();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, TOK_BITOR, left, right);
  }
  return left;
}

Expression *
Parser::relational()
{
  Expression *left = bitor_();
  size_t count = 0;
  while (left) {
    TokenKind kind = scanner_.peek();
    if (kind < TOK_LT || kind > TOK_GE)
      break;
    scanner_.next();
    SourceLocation pos = scanner_.begin();
    Expression *right = shift();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, kind, left, right);
    if (++count > 1) {
      cc_.reportError(pos, Message_NoChainedRelationalOps);
      return nullptr;
    }
  }
  return left;
}

Expression *
Parser::equals()
{
  Expression *left = relational();
  while (left) {
    if (!match(TOK_EQUALS) && !match(TOK_NOTEQUALS))
      break;
    TokenKind kind = scanner_.current()->kind;
    SourceLocation pos = scanner_.begin();
    Expression *right = relational();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, kind, left, right);
  }
  return left;
}

Expression *
Parser::and_()
{
  Expression *left = equals();
  while (left) {
    if (!match(TOK_AND))
      break;
    SourceLocation pos = scanner_.begin();
    Expression *right = equals();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, TOK_AND, left, right);
  }
  return left;
}

Expression *
Parser::or_()
{
  Expression *left = and_();
  while (left) {
    if (!match(TOK_OR))
      break;
    SourceLocation pos = scanner_.begin();
    Expression *right = and_();
    if (!right)
      return nullptr;
    left = new (pool_) BinaryExpression(pos, TOK_OR, left, right);
  }
  return left;
}

Expression *
Parser::ternary()
{
  Expression *cond = or_();
  if (!cond)
    return nullptr;
  
  if (!match(TOK_QMARK))
    return cond;

  Expression *left;
  SourceLocation pos = scanner_.begin();
  AutoAllowTags<false> disableTags(scanner_);
  {
    if ((left = expression()) == nullptr)
      return nullptr;
  }

  if (!expect(TOK_COLON))
    return nullptr;

  Expression *right = expression();
  if (!right)
    return nullptr;

  return new (pool_) TernaryExpression(pos, cond, left, right);
}

Expression *
Parser::assignment()
{
  Expression *left = ternary();
  while (left) {
    TokenKind token = scanner_.peek();
    if (token != TOK_ASSIGN && (token < TOK_ASSIGN_ADD || token > TOK_ASSIGN_SHL))
      break;
    scanner_.next();
    SourceLocation pos = scanner_.begin();
    Expression *expr = assignment();
    if (!expr)
      return nullptr;

    left = new (pool_) Assignment(pos, token, left, expr);
  }
  return left;
}

Expression *
Parser::expression()
{
  return assignment();
}

Statement *
Parser::while_()
{
  // while ::= "while" "(" expr ")" statement
  SourceLocation pos = scanner_.begin();

  if (!expect(TOK_LPAREN))
    return nullptr;

  Expression *condition = expression();
  if (!condition)
    return nullptr;

  if (!expect(TOK_RPAREN))
    return nullptr;

  Statement *body = statementOrBlock();
  if (!body)
    return nullptr;

  requireNewline();

  return new (pool_) WhileStatement(pos, TOK_WHILE, condition, body);
}

Statement *
Parser::do_()
{
  // do ::= "do" block "while" "(" expr ")"
  SourceLocation pos = scanner_.begin();
  scanner_.next();

  Statement *body = block();
  if (!body)
    return nullptr;

  if (!expect(TOK_WHILE))
    return nullptr;

  if (!expect(TOK_LPAREN))
    return nullptr;
  Expression *condition = expression();
  if (!condition)
    return nullptr;
  if (!expect(TOK_RPAREN))
    return nullptr;

  requireTerminator();

  return new (pool_) WhileStatement(pos, TOK_DO, condition, body);
}

bool
Parser::matchMethodBind()
{
  if (!match(TOK_LPAREN))
    return nullptr;

  if (match(TOK_RPAREN)) {
    if (match(TOK_ASSIGN))
      return true;
    scanner_.undo();
  }
  scanner_.undo();
  return nullptr;
}

LayoutEntry *
Parser::parseAccessor()
{
  TypeSpecifier spec;
  parse_new_type_expr(&spec, 0);

  if (!expect(TOK_NAME))
    return nullptr;
  NameToken name = *scanner_.current();

  if (!expect(TOK_LBRACE))
    return nullptr;

  FunctionOrAlias getter, setter, dummy;
  while (!match(TOK_RBRACE)) {
    expect(TOK_PUBLIC);

    bool native = match(TOK_NATIVE);

    Atom *name = expectName();
    if (!name)
      return nullptr;

    FunctionOrAlias *out = &dummy;
    if (strcmp(name->chars(), "get") == 0)
      out = &getter;
    else if (strcmp(name->chars(), "set") == 0)
      out = &setter;
    else
      cc_.reportError(scanner_.begin(), Message_InvalidAccessorName);

    if (!out->isEmpty())
      cc_.reportError(scanner_.begin(), Message_AccessorRedeclared, name->chars());

    if (matchMethodBind()) {
      if (!expect(TOK_NAME))
        return nullptr;

      NameProxy *alias = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());
      requireNewlineOrSemi();
      *out = FunctionOrAlias(alias);
    } else {
      ParameterList *params = arguments();
      if (!params)
        return nullptr;

      MethodBody *body = nullptr;
      if (!native)
        body = methodBody();

      requireNewlineOrSemi();

      FunctionNode *node = new (pool_) FunctionNode(
        (native ? TOK_NATIVE : TOK_NONE),
        body,
        FunctionSignature(spec, params)
      );
      *out = FunctionOrAlias(node);
    }
  }

  return new (pool_) LayoutEntry(name, spec, getter, setter);
}

LayoutEntry *
Parser::parseMethod()
{
  bool native = match(TOK_NATIVE);
  bool destructor = match(TOK_TILDE);

  Declaration decl;
  if (destructor) {
    decl.spec.setBuiltinType(TOK_VOID);

    if (!expect(TOK_NAME))
      return nullptr;
    decl.name = *scanner_.current();
  } else {
    if (!parse_decl(&decl, DeclFlags::MaybeFunction))
      return nullptr;
  }

  if (matchMethodBind()) {
    if (!expect(TOK_NAME))
      return nullptr;

    // Build an aliased definition (like "public X() = Y".
    NameProxy *alias = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());
    requireNewlineOrSemi();
    return new (pool_) LayoutEntry(decl.name, FunctionOrAlias(alias));
  }

  ParameterList *params = arguments();
  if (!params)
    return nullptr;

  // Grab the body, or if none is required, require a terminator.
  MethodBody *body = nullptr;
  if (native)
    requireNewlineOrSemi();
  else
    body = methodBody();

  FunctionNode *node = new (pool_) FunctionNode(
    (native ? TOK_NATIVE : TOK_NONE),
    body,
    FunctionSignature(decl.spec, params)
  );

  return new (pool_) LayoutEntry(decl.name, FunctionOrAlias(node));
}

Statement *
Parser::methodmap(TokenKind kind)
{
  SourceLocation begin = scanner_.begin();

  if (!expect(TOK_NAME))
    return nullptr;
  NameToken name = *scanner_.current();

  bool nullable = match(TOK_NULLABLE);

  NameProxy *extends = nullptr;
  if (match(TOK_LT) && expect(TOK_NAME))
    extends = new (pool_) NameProxy(scanner_.begin(), scanner_.current_name());

  if (!expect(TOK_LBRACE))
    return nullptr;

  LayoutList *list = new (pool_) LayoutList();
  while (!match(TOK_RBRACE)) {
    LayoutEntry *entry = nullptr;
    if (match(TOK_PUBLIC))
      entry = parseMethod();
    else if (match(TOK_PROPERTY))
      entry = parseAccessor();
    else
      cc_.reportError(scanner_.begin(), Message_ExpectedLayoutMember);
    if (!entry)
      return nullptr;

    list->append(entry);
  }

  LayoutStatement *layout = new (pool_) LayoutStatement(
    begin,
    TOK_METHODMAP,
    name,
    extends,
    list
  );

  if (nullable)
    layout->setNullable();

  requireNewlineOrSemi();
  return layout;
}

Statement *
Parser::switch_()
{
  // switch ::= "switch" "(" expr ")" "{" case* defaultcase? "}"
  // case ::= "case" casevals ":" statement
  // defaultcase ::= "default" ":" statement
  //
  SourceLocation pos = scanner_.begin();

  if (!expect(TOK_LPAREN))
    return nullptr;
  Expression *expr = expression();
  if (!expr)
    return nullptr;
  if (!expect(TOK_RPAREN))
    return nullptr;

  if (!expect(TOK_LBRACE))
    return nullptr;

  SourceLocation defaultPos;
  PoolList<Case *> *cases = new (pool_) PoolList<Case *>();

  Statement *defaultCase = nullptr;

  while (!peek(TOK_RBRACE)) {
    Expression *expr = nullptr;
    ExpressionList *others = nullptr;
    if (match(TOK_DEFAULT)) {
      if (defaultCase) {
        cc_.reportError(scanner_.begin(), Message_OneDefaultPerSwitch);
        return nullptr;
      }

      defaultPos = scanner_.begin();
    } else {
      if (defaultCase) {
        cc_.reportError(defaultPos, Message_DefaultMustBeLastCase);
        return nullptr;
      }

      if (!expect(TOK_CASE))
        return nullptr;

      // A limitation in the grammar is that |case <NAME>:| will be
      // detected as a label.
      AutoAllowTags<false> disableTags(scanner_);

      if ((expr = expression()) == nullptr)
        return nullptr;

      if (peek(TOK_COMMA)) {
        others = new (pool_) ExpressionList();
        while (match(TOK_COMMA)) {
          Expression *other = expression();
          if (!other)
            return nullptr;
          if (!others->append(other))
            return nullptr;
        }
      }
    }

    if (!expect(TOK_COLON))
      return nullptr;

    Statement *stmt = statementOrBlock();
    if (!stmt)
      return nullptr;

    requireNewline();

    if (!peek(TOK_CASE) && !peek(TOK_DEFAULT) && !peek(TOK_RBRACE)) {
      cc_.reportError(scanner_.begin(), Message_SingleStatementPerCase);
      return nullptr;
    }

    if (expr) {
      Case *caze = new (pool_) Case(expr, others, stmt);
      if (!cases->append(caze))
        return nullptr;
    } else {
      defaultCase = stmt;
    }
  }

  if (!expect(TOK_RBRACE))
    return nullptr;

  requireNewline();

  return new (pool_) SwitchStatement(pos, expr, cases, defaultCase);
}

Statement *
Parser::for_()
{
  // for ::= "for" "(" forinit? ";" forcond? ";" forstep ")" statement
  // forint ::= "new" vardecl |
  //      exprstmt
  // forcond ::= expr
  // forstep ::= exprstmt
  //
  SourceLocation pos = scanner_.begin();
  if (!expect(TOK_LPAREN))
    return nullptr;

  Statement *decl = nullptr;
  if (!match(TOK_SEMICOLON)) {
    bool is_decl = false;
    if (match(TOK_NEW))
      is_decl = true;
    else if (IsNewTypeToken(scanner_.peek()))
      is_decl = true;

    if (is_decl) {
      if ((decl = localVariableDeclaration(TOK_NEW, DeclFlags::Inline)) == nullptr)
        return nullptr;
    } else {
      if ((decl = expressionStatement()) == nullptr)
        return nullptr;
    }
    if (!expect(TOK_SEMICOLON))
      return nullptr;
  }

  Expression *condition = nullptr;
  if (!match(TOK_SEMICOLON)) {
    if ((condition = expression()) == nullptr)
      return nullptr;
    if (!expect(TOK_SEMICOLON))
      return nullptr;
  }

  Statement *update = nullptr;
  if (!match(TOK_RPAREN)) {
    if ((update = expressionStatement()) == nullptr)
      return nullptr;
    if (!expect(TOK_RPAREN))
      return nullptr;
  }

  Statement *body = statementOrBlock();
  if (!body)
    return nullptr;

  requireNewline();

  return new (pool_) ForStatement(pos, decl, condition, update, body);
}

ExpressionList *
Parser::dimensions()
{
  // dimensions ::= ("[" expr? "]")*

  ExpressionList *postDimensions = new (pool_) ExpressionList();
  while (match(TOK_LBRACKET)) {
    Expression *dim = nullptr;
    if (!match(TOK_RBRACKET)) {
      if ((dim = expression()) == nullptr)
        return nullptr;
      if (!expect(TOK_RBRACKET))
        return nullptr;
    }

    if (!postDimensions->append(dim))
      return nullptr;
  }
  return postDimensions;
}

Statement *
Parser::variable(TokenKind tok, Declaration *decl, uint32_t attrs)
{
  Expression *init = nullptr;
  if (match(TOK_ASSIGN))
    init = expression();

  VariableDeclaration *first = new (pool_) VariableDeclaration(decl->name, decl->spec, init);
  VariableDeclaration *prev = first;
  while (match(TOK_COMMA)) {
    // Parse the next declaration re-using any sticky information from the
    // first decl.
    if (!reparse_decl(decl, DeclFlags::Variable))
      break;

    Expression *init = nullptr;
    if (match(TOK_ASSIGN))
      init = expression();

    VariableDeclaration *var = new (pool_) VariableDeclaration(decl->name, decl->spec, init);
    prev->setNext(var);
    prev = var;
  }

  if (!(attrs & DeclFlags::Inline))
    requireTerminator();

  return first;
}

// Wrapper around variable() for locals.
Statement *
Parser::localVariableDeclaration(TokenKind kind, uint32_t flags)
{
  Declaration decl;

  if (!allowDeclarations_)
    cc_.reportError(scanner_.begin(), Message_VariableMustBeInBlock);

  flags |= DeclFlags::Variable;
  if (!parse_decl(&decl, flags))
    return nullptr;

  return variable(kind, &decl, flags);
}

Statement *
Parser::return_()
{
  // return ::= "return" term |
  //      "return" "expr"
  SourceLocation pos = scanner_.begin();

  Expression *expr = nullptr;
  TokenKind next = scanner_.peekTokenSameLine();
  if (next != TOK_EOL && next != TOK_EOF && next != TOK_SEMICOLON) {
    if ((expr = expression()) == nullptr)
      return nullptr;

    // We only care about non-void returns when determining whether a
    // tagless function is non-void.
    encounteredReturn_ = true;
  }

  requireTerminator();
  return new (pool_) ReturnStatement(pos, expr);
}

Statement *
Parser::expressionStatement()
{
  // exprstmt ::= expr
  Expression *left = assignment();
  if (!left)
    return nullptr;

  return new (pool_) ExpressionStatement(left);
}

// Parses statements, expecting the "{" to have already been parsed.
StatementList *
Parser::statements()
{
  StatementList *list = new (pool_) StatementList();
  while (!match(TOK_RBRACE)) {
    // Call statement() directly, so we don't set allowDeclaratiosn to false.
    Statement *stmt = statement();
    if (!stmt)
      return nullptr;
    list->append(stmt);
  }
  return list;
}

Statement *
Parser::block()
{
  // block ::= "{" statement* "}"
  if (!expect(TOK_LBRACE))
    return nullptr;

  SourceLocation pos = scanner_.begin();

  SaveAndSet<bool> save(&allowDeclarations_, true);
  StatementList *list = statements();
  if (!list)
    return nullptr;

  return new (pool_) BlockStatement(pos, list, TOK_LBRACE);
}

Statement *
Parser::if_()
{
  // if ::= "if" "(" expr ")" statement elseif* else?
  // elseif ::= "elseif" "(" expr ")" statement
  // else ::= "else" statement
  SourceLocation pos = scanner_.begin();
  if (!expect(TOK_LPAREN))
    return nullptr;

  Expression *cond = expression();
  if (!cond)
    return nullptr;

  if (!expect(TOK_RPAREN))
    return nullptr;

  Statement *ifTrue = statementOrBlock();
  if (!ifTrue)
    return nullptr;

  IfStatement *outer = new (pool_) IfStatement(pos, cond, ifTrue);

  IfStatement *last = outer;
  while (match(TOK_ELSE)) {
    if (!match(TOK_IF)) {
      Statement *ifFalse = statementOrBlock();
      if (!ifFalse)
        return nullptr;

      last->setIfFalse(ifFalse);
      break;
    }

    SourceLocation pos = scanner_.begin();
    if (!expect(TOK_LPAREN))
      return nullptr;

    Expression *otherCond = expression();
    if (!otherCond)
      return nullptr;

    if (!expect(TOK_RPAREN))
      return nullptr;

    Statement *otherIfTrue = statementOrBlock();
    if (!otherIfTrue)
      return nullptr;

    IfStatement *inner = new (pool_) IfStatement(pos, otherCond, otherIfTrue);
    last->setIfFalse(inner);
    last = inner;
  }

  requireNewline();

  return outer;
}

Statement *
Parser::statement()
{
  // statement ::= stmt term
  // stmt ::= do | for | if | while | struct | enum |
  //      localvars | return | switch | break | continue

  Statement *stmt = nullptr;

  // Shortcut out early for block, since it wants to expect(TOK_LBRACE).
  if (peek(TOK_LBRACE))
    return block();

  TokenKind kind = scanner_.next();

  // We don't have enough lookahead to differentiate some declarations from
  // expressions, so we cheat a bit here and just do some pattern matching:
  //
  //   "name[]" probably starts a declaration, as does "name name".
  if (kind == TOK_NAME) {
    bool is_decl = false;

    if (match(TOK_LBRACKET)) {
      if (peek(TOK_RBRACKET))
        is_decl = true;
      scanner_.undo();
    } else if (peek(TOK_NAME)) {
      is_decl = true;
    }

    if (is_decl) {
      scanner_.undo();
      return localVariableDeclaration(TOK_NEW);
    }
  }

  // Other declarations don't need any special sniffing.
  if (IsNewTypeToken(kind) ||
      (kind == TOK_DECL ||
       kind == TOK_STATIC ||
       kind == TOK_NEW))
  {
    if (IsNewTypeToken(kind)) {
      scanner_.undo();
      kind = TOK_NEW;
    }

    return localVariableDeclaration(kind);
  }

  // Statements which must be followed by a semicolon will break out of the
  // switch. If they may end in BlockStatements, they'll immediately return.
  switch (kind) {
    case TOK_FOR:
      return for_();

    case TOK_WHILE:
      return while_();

    case TOK_BREAK:
      stmt = new (pool_) BreakStatement(scanner_.begin());
      break;

    case TOK_CONTINUE:
      stmt = new (pool_) ContinueStatement(scanner_.begin());
      break;

    case TOK_DO:
      return do_();

    case TOK_RETURN:
      return return_();

    case TOK_ENUM:
      return enum_();

    case TOK_SWITCH:
      return switch_();

    case TOK_IF:
      return if_();

    default:
      break;
  }

  if (!stmt) {
    scanner_.undo();
    if ((stmt = expressionStatement()) == nullptr)
      return nullptr;
  }

  requireTerminator();
  return stmt;
}

Statement *
Parser::statementOrBlock()
{
  SaveAndSet<bool> save(&allowDeclarations_, false);
  return statement();
}

Statement *
Parser::enum_()
{
  // enum ::= "enum" name? { enum_members? }
  // enum_members ::= enum_member ","? |
  //          enum_member "," enum_members
  // enum_member ::= ident ("=" constexpr)?
  SourceLocation pos = scanner_.begin();

  Atom *name = nullptr;
  if (match(TOK_NAME) || match(TOK_LABEL))
    name = scanner_.current_name();

  EnumStatement::EntryList *entries = new (pool_) EnumStatement::EntryList();

  if (!expect(TOK_LBRACE))
    return nullptr;

  do {
    if (scanner_.peek() == TOK_RBRACE)
      break;

    Atom *name = expectName();
    if (!name)
      return nullptr;
    NameProxy *proxy = new (pool_) NameProxy(scanner_.begin(), name);

    Expression *expr = nullptr;
    if (match(TOK_ASSIGN)) {
      if ((expr = expression()) == nullptr)
        return nullptr;
    }

    if (!entries->append(EnumStatement::Entry(proxy, expr)))
      return nullptr;
  } while (match(TOK_COMMA));
  if (!expect(TOK_RBRACE))
    return nullptr;

  requireTerminator();

  return new (pool_) EnumStatement(pos, name, entries);
}

ParameterList *
Parser::arguments()
{
  ParameterList *params = new (pool_) ParameterList;

  if (!expect(TOK_LPAREN))
    return nullptr;

  if (match(TOK_RPAREN))
    return params;

  bool variadic = false;
  for (;;) {
    Declaration decl;
    if (!parse_decl(&decl, DeclFlags::Argument))
      break;

    Expression *init = nullptr;
    if (match(TOK_ASSIGN))
      init = expression();

    if (decl.spec.isVariadic()) {
      if (variadic)
        cc_.reportError(decl.spec.variadicLoc(), Message_MultipleVarargs);
      variadic = true;
    }

    VariableDeclaration *node = new (pool_) VariableDeclaration(
      decl.name,
      decl.spec,
      init
    );
    params->append(node);
    if (!match(TOK_COMMA))
      break;
  }

  expect(TOK_RPAREN);
  return params;
}

MethodBody *
Parser::methodBody()
{
  SaveAndSet<bool> saveReturnState(&encounteredReturn_, false);
  SaveAndSet<bool> saveDeclState(&allowDeclarations_, true);

  SourceLocation pos;
  StatementList *list;
  if (match(TOK_LBRACE)) {
    pos = scanner_.begin();
    if ((list = statements()) == nullptr)
      return nullptr;
  } else {
    Statement *stmt = statement();
    if (!stmt)
      return nullptr;
    list = new (pool_) StatementList();
    list->append(stmt);
  }

  requireNewline();

  return new (pool_) MethodBody(pos, list, encounteredReturn_);
}

Statement *
Parser::function(TokenKind kind, const Declaration &decl, void *, uint32_t attrs)
{
  ParameterList *params = arguments();
  if (!params)
    return nullptr;

  MethodBody *body = nullptr;
  if (kind != TOK_FORWARD && kind != TOK_NATIVE) {
    if ((body = methodBody()) == nullptr)
      return nullptr;
  }

  if (body)
    requireNewline();
  else
    requireTerminator();

  FunctionSignature signature(decl.spec, params);
  return new (pool_) FunctionStatement(decl.name, kind, body, signature);
}

Statement *
Parser::global(TokenKind kind)
{
  Declaration decl;

  if (kind == TOK_NATIVE || kind == TOK_FORWARD) {
    if (!parse_decl(&decl, DeclFlags::MaybeFunction))
      return nullptr;
    return function(kind, decl, nullptr, DeclAttrs::None);
  }

  uint32_t attrs = DeclAttrs::None;
  if (kind == TOK_PUBLIC)
    attrs |= DeclAttrs::Public;
  if (kind == TOK_STOCK)
    attrs |= DeclAttrs::Stock;
  if (kind == TOK_STATIC)
    attrs |= DeclAttrs::Static;

  if ((attrs & DeclAttrs::Static) && match(TOK_STOCK))
    attrs |= DeclAttrs::Stock;

  uint32_t flags = DeclFlags::MaybeFunction | DeclFlags::Variable;
  if (kind == TOK_NEW)
    flags |= DeclFlags::Old;

  if (!parse_decl(&decl, flags))
    return nullptr;

  if (kind == TOK_NEW || decl.spec.hasPostDims() || !peek(TOK_LPAREN)) {
    if (kind == TOK_NEW && decl.spec.isNewDecl())
      cc_.reportError(decl.name.start, Message_NewStyleBadKeyword);
    return variable(TOK_NEW, &decl, attrs);
  }
  return function(TOK_FUNCTION, decl, nullptr, attrs);
}

Statement *
Parser::struct_(TokenKind kind)
{
  SourceLocation loc = scanner_.begin();

  if (!expect(TOK_NAME))
    return nullptr;
  NameToken name = *scanner_.current();

  if (!expect(TOK_LBRACE))
    return nullptr;

  uint32_t flags = DeclFlags::Field;
  if (kind == TOK_UNION)
    flags |= DeclFlags::MaybeNamed;

  LayoutList *list = new (pool_) LayoutList();
  while (!match(TOK_RBRACE)) {
    Declaration decl;

    // Structs need a |public| keyword right now.
    if (kind == TOK_STRUCT)
      expect(TOK_PUBLIC);

    if (!parse_new_decl(&decl, flags))
      return nullptr;

    LayoutEntry *entry = new (pool_) LayoutEntry(decl.name, decl.spec);
    list->append(entry);

    requireNewlineOrSemi();
  }

  requireNewlineOrSemi();
  return new (pool_) LayoutStatement(loc, kind, name, nullptr, list);
}

Statement *
Parser::typedef_()
{
  SourceLocation begin = scanner_.begin();

  Atom *name = expectName();
  if (!name)
    return nullptr;

  expect(TOK_ASSIGN);

  TypeSpecifier spec;
  parse_new_type_expr(&spec, 0);

  requireNewlineOrSemi();
  return new (pool_) TypedefStatement(begin, name, spec);
}

ParseTree *
Parser::parse()
{
  StatementList *list = new (pool_) StatementList();

  for (;;) {
    Statement *statement = nullptr;

    TokenKind kind = scanner_.next();
    switch (kind) {
      case TOK_ERROR:
        return nullptr;

      case TOK_EOF:
        break;

      case TOK_NAME:
      case TOK_CHAR:
      case TOK_INT:
      case TOK_VOID:
      case TOK_OBJECT:
      case TOK_FLOAT:
      case TOK_LABEL:
        scanner_.undo();
        // Fallthrough.
      case TOK_NEW:
      case TOK_STATIC:
      case TOK_PUBLIC:
      case TOK_STOCK:
      case TOK_NATIVE:
      case TOK_FORWARD:
      {
        statement = global(kind);
        break;
      }

      case TOK_METHODMAP:
        statement = methodmap(TOK_METHODMAP);
        break;

      case TOK_ENUM:
        statement = enum_();
        break;

      case TOK_STRUCT:
      case TOK_UNION:
        statement = struct_(kind);
        break;

      case TOK_TYPEDEF:
        statement = typedef_();
        break;

      case TOK_FUNCTAG:
        cc_.reportError(scanner_.begin(), Message_FunctagsNotSupported);
        scanner_.eatRestOfLine();
        break;

      default:
        cc_.reportError(scanner_.begin(), Message_ExpectedGlobal);
        goto err_out;
    }

    if (!statement) {
      if (scanner_.current()->kind == TOK_EOF)
        break;
    } else {
      list->append(statement);
    }
  }

 err_out:
  return new (pool_) ParseTree(list);
}

class AstPrinter : public AstVisitor
{
  FILE *fp_;
  size_t level_;

  private:
  void prefix() {
    for (size_t i = 0; i < level_; i++)
      fprintf(fp_, "  ");
  }
  void indent() {
    level_++;
  }
  void unindent() {
    level_--;
  }

  public:
  AstPrinter(FILE *fp)
    : fp_(fp),
    level_(0)
  {
  }

  void dump(const TypeSpecifier &spec, Atom *name) {
    if (spec.isConst())
      fprintf(fp_, "const ");
    if (spec.resolver() == TOK_NAME) {
      fprintf(fp_, "%s", spec.proxy()->name()->chars());
    } else if (spec.resolver() == TOK_LABEL) {
      fprintf(fp_, "%s:", spec.proxy()->name()->chars());
    } else if (IsNewTypeToken(spec.resolver())) {
      fprintf(fp_, "%s", TokenNames[spec.resolver()]);
    } else if (spec.resolver() == TOK_IMPLICIT_INT) {
      fprintf(fp_, "implicit-int");
    } else if (spec.resolver() == TOK_FUNCTION) {
      fprintf(fp_, "function ");
      dump(spec.signature());
    }

    if (spec.resolver() != TOK_LABEL && !spec.dims() && spec.rank()) {
      for (uint32_t i = 0; i < spec.rank(); i++)
        fprintf(fp_, "[]");
    }

    if (name)
      fprintf(fp_, " %s", name->chars());

    if (spec.resolver() == TOK_LABEL || spec.dims()) {
      for (uint32_t i = 0; i < spec.rank(); i++)
        fprintf(fp_, "[]");
    }
  }

  void dump(FunctionSignature *sig) {
    dump(*sig->returnType(), nullptr);
    if (!sig->parameters()->length()) {
      fprintf(fp_, " ()\n");
      return;
    }
    fprintf(fp_, " (\n");
    indent();
    for (size_t i = 0; i < sig->parameters()->length(); i++) {
      prefix();
      VariableDeclaration *param = sig->parameters()->at(i);
      dump(*param->spec(), param->name());
      fprintf(fp_, "\n");
    }
    unindent();
    prefix();
    fprintf(fp_, ")");
  }
  void dump(LayoutEntry *entry, const FunctionOrAlias &method, const char *prefix) {
    if (prefix)
      fprintf(fp_, "%s method ", prefix);
    else
      fprintf(fp_, "method ");
    if (method.isAlias()) {
      fprintf(fp_, "%s = %s", entry->name()->chars(), method.alias()->name()->chars());
    } else {
      FunctionNode *node = method.fun();
      fprintf(fp_, "%s ", entry->name()->chars());
      dump(node->signature());
    }
  }
  void visitNameProxy(NameProxy *name) {
    prefix();
    fprintf(fp_, "[ NameProxy (%s)\n", name->name()->chars());
  }
  void visitCallExpression(CallExpression *node) {
    prefix();
    fprintf(fp_, "[ CallExpression\n");
    indent();
    node->callee()->accept(this);
    for (size_t i = 0; i < node->arguments()->length(); i++)
      node->arguments()->at(i)->accept(this);
    unindent();
  }
  void visitFunctionStatement(FunctionStatement *node) {
    prefix();
    fprintf(fp_, "[ FunctionStatement (%s)\n", node->name()->chars());
    indent();
    {
      prefix();
      dump(node->signature());
      fprintf(fp_, "\n");
      if (node->body())
        node->body()->accept(this);
    }
    unindent();
  }
  void visitExpressionStatement(ExpressionStatement *node) {
    prefix();
    fprintf(fp_, "[ ExpressionStatement\n");
    indent();
    node->expression()->accept(this);
    unindent();
  }
  void visitAssignment(Assignment *node) {
    prefix();
    fprintf(fp_, "[ Assignment\n");
    indent();
    node->lvalue()->accept(this);
    node->expression()->accept(this);
    unindent();
  }
  void visitTernaryExpression(TernaryExpression *node) {
    prefix();
    fprintf(fp_, "[ TernaryExpression\n");
    indent();
    node->condition()->accept(this);
    node->left()->accept(this);
    node->right()->accept(this);
    unindent();
  }
  void visitBinaryExpression(BinaryExpression *node) {
    prefix();
    fprintf(fp_, "[ BinaryExpression (%s)\n", TokenNames[node->token()]);
    indent();
    node->left()->accept(this);
    node->right()->accept(this);
    unindent();
  }
  void visitUnaryExpression(UnaryExpression *node) {
    prefix();
    fprintf(fp_, "[ UnaryExpression (%s)\n", TokenNames[node->token()]);
    indent();
    node->expression()->accept(this);
    unindent();
  }
  void visitReturnStatement(ReturnStatement *node) {
    prefix();
    fprintf(fp_, "[ ReturnStatement\n");
    indent();
    if (node->expression())
      node->expression()->accept(this);
    unindent();
  }
  void visitForStatement(ForStatement *node) {
    prefix();
    fprintf(fp_, "[ ForStatement\n");
    indent();
    if (node->initialization())
      node->initialization()->accept(this);
    if (node->condition())
      node->condition()->accept(this);
    if (node->update())
      node->update()->accept(this);
    node->body()->accept(this);
    unindent();
  }
  void visitBlockStatement(BlockStatement *node) {
    prefix();
    fprintf(fp_, "[ BlockStatement\n");
    indent();
    for (size_t i = 0; i < node->statements()->length(); i++)
      node->statements()->at(i)->accept(this);
    unindent();
  }
  void visitVariableDeclaration(VariableDeclaration *node) {
    prefix();
    fprintf(fp_, "[ VariableDeclaration (%s)\n", node->name()->chars());
    indent();
    if (node->initialization())
      node->initialization()->accept(this);
    unindent();
    if (node->next())
      node->next()->accept(this);
  }
  void visitCharLiteral(CharLiteral *node) {
    prefix();
    fprintf(fp_, "[ CharLiteral (%c)\n", (char)node->value());
  }
  void visitIntegerLiteral(IntegerLiteral *node) {
    prefix();
    fprintf(fp_, "[ IntegerLiteral (%" KE_FMT_I64 ")\n", node->value());
  }
  void visitBooleanLiteral(BooleanLiteral *node) {
    prefix();
    fprintf(fp_, "[ BooleanLiteral (%s)\n",
        (node->token() == TOK_TRUE) ? "true" : "false");
  }
  void visitFloatLiteral(FloatLiteral *node) {
    prefix();
    fprintf(fp_, "[ FloatLiteral (%f)\n", node->value());
  }
  void visitIfStatement(IfStatement *node) {
    prefix();
    fprintf(fp_, "[ IfStatement\n");
    indent();
    node->ifTrue()->accept(this);
    if (node->ifFalse())
      node->ifFalse()->accept(this);
    unindent();
  }
  void visitIndexExpression(IndexExpression *node) {
    prefix();
    fprintf(fp_, "[ IndexExpression\n");
    indent();
    node->left()->accept(this);
    node->right()->accept(this);
    unindent();
  }
  void visitEnumStatement(EnumStatement *node) {
    prefix();
    fprintf(fp_, "[ EnumStatement (%s)\n", node->name() ? node->name()->chars() : "<anonymous>");
    indent();
    for (size_t i = 0; i < node->entries()->length(); i++) {
      prefix();
      fprintf(fp_, "%s =\n", node->entries()->at(i).proxy->name()->chars());
      if (node->entries()->at(i).expr) {
        indent();
        node->entries()->at(i).expr->accept(this);
        unindent();
      }
    }
    unindent();
  }
  void visitWhileStatement(WhileStatement *node) {
    prefix();
    fprintf(fp_, "[ WhileStatement (%s)\n",
        (node->token() == TOK_DO) ? "do" : "while");
    indent();
    if (node->token() == TOK_DO) {
      node->condition()->accept(this);
      node->body()->accept(this);
    } else {
      node->condition()->accept(this);
      node->body()->accept(this);
    }
    unindent();
  }
  void visitBreakStatement(BreakStatement *node) {
    prefix();
    fprintf(fp_, "[ BreakStatement\n");
  }
  void visitContinueStatement(ContinueStatement *node) {
    prefix();
    fprintf(fp_, "[ ContinueStatement\n");
  }
  void visitStringLiteral(StringLiteral *node) {
    prefix();
    fprintf(fp_, "[ StringLiteral\n");
  }
  void visitIncDecExpression(IncDecExpression *node) {
    prefix();
    fprintf(fp_, "[ IncDecExpression (postfix=%d)\n", node->postfix());
    indent();
    node->expression()->accept(this);
    unindent();
  }
  void visitThisExpression(ThisExpression *node) {
    prefix();
    fprintf(fp_, "[ ThisExpression\n");
  }
  void visitSwitchStatement(SwitchStatement *node) {
    prefix();
    fprintf(fp_, "[ SwitchStatement\n");
    indent();
    node->expression()->accept(this);
    for (size_t i = 0; i < node->cases()->length(); i++) {
      Case *c = node->cases()->at(i);
      c->expression()->accept(this);
      if (c->others()) {
        for (size_t j = 0; j < c->others()->length(); j++)
          c->others()->at(j)->accept(this);
      }
      indent();
      c->statement()->accept(this);
      unindent();
    }
    if (node->defaultCase())
      node->defaultCase()->accept(this);
    unindent();
  }
  void visitArrayLiteral(ArrayLiteral *node) {
    prefix();
    fprintf(fp_, "[ ArrayLiteral\n");
    indent();
    for (size_t i = 0; i < node->expressions()->length(); i++) {
      Expression *expr = node->expressions()->at(i);
      indent();
      expr->accept(this);
      unindent();
    }
  }
  void visitStructInitializer(StructInitializer *node) {
    prefix();
    fprintf(fp_, "[ StructInitializer\n");
    indent();
    for (size_t i = 0; i < node->pairs()->length(); i++) {
      NameAndValue *pair = node->pairs()->at(i);
      prefix();
      fprintf(fp_, "%s = \n", pair->name()->chars());
      indent();
      pair->expr()->accept(this);
      unindent();
    }
    unindent();
  }
  void visitTypedefStatement(TypedefStatement *node) {
    prefix();
    fprintf(fp_, "[ TypedefStatement\n");
    indent();
    prefix();
    dump(*node->spec(), node->name());
    fprintf(fp_, "\n");
    unindent();
  }
  void visitLayoutStatement(LayoutStatement *node) {
    prefix();
    fprintf(fp_, "[ LayoutStatement %s %s\n",
      TokenNames[node->spec()],
      node->name()->chars()
    );
    indent();
    for (size_t i = 0; i < node->body()->length(); i++) {
      LayoutEntry *entry = node->body()->at(i);
      prefix();
      switch (entry->type()) {
        case LayoutEntry::Field:
          fprintf(fp_, "field ");
          dump(*entry->spec(), entry->name());
          break;
        case LayoutEntry::Method:
          dump(entry, entry->method(), nullptr);
          break;
        case LayoutEntry::Accessor:
        {
          if (!entry->getter().isEmpty())
            dump(entry, entry->getter(), "getter");
          if (!entry->setter().isEmpty())
            dump(entry, entry->setter(), "setter");
          break;
        }
        default:
          assert(false);
      }
      fprintf(fp_, "\n");
    }
    unindent();
  }
};

void
ParseTree::dump(FILE *fp)
{
  AstPrinter printer(fp);

  for (size_t i = 0; i < statements_->length(); i++)
    statements_->at(i)->accept(&printer);
}