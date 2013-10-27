/*
 * Copyright © 2013 Ran Benita <ran234@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>

#include "xkbcommon/xkbcommon-compose.h"
#include "utils.h"
#include "scanner-utils.h"
#include "compose.h"
#include "parser.h"

#define MAX_LHS_LEN 10

/*
 * Grammer adapted from libX11/modules/im/ximcp/imLcPrs.c.
 * See also the XCompose(5) manpage.
 *
 * TODO: Handle include statements.
 *
 * We don't support the MODIFIER rules, which are commented out.
 *
 * FILE          ::= { [PRODUCTION] [COMMENT] "\n" | INCLUDE }
 * INCLUDE       ::= "include" '"' INCLUDE_STRING '"'
 * PRODUCTION    ::= LHS ":" RHS [ COMMENT ]
 * COMMENT       ::= "#" {<any character except null or newline>}
 * LHS           ::= EVENT { EVENT }
 * EVENT         ::= "<" keysym ">"
 * # EVENT         ::= [MODIFIER_LIST] "<" keysym ">"
 * # MODIFIER_LIST ::= ("!" {MODIFIER} ) | "None"
 * # MODIFIER      ::= ["~"] modifier_name
 * RHS           ::= ( STRING | keysym | STRING keysym )
 * STRING        ::= '"' { CHAR } '"'
 * CHAR          ::= GRAPHIC_CHAR | ESCAPED_CHAR
 * GRAPHIC_CHAR  ::= locale (codeset) dependent code
 * ESCAPED_CHAR  ::= ('\\' | '\"' | OCTAL | HEX )
 * OCTAL         ::= '\' OCTAL_CHAR [OCTAL_CHAR [OCTAL_CHAR]]
 * OCTAL_CHAR    ::= (0|1|2|3|4|5|6|7)
 * HEX           ::= '\' (x|X) HEX_CHAR [HEX_CHAR]]
 * HEX_CHAR      ::= (0|1|2|3|4|5|6|7|8|9|A|B|C|D|E|F|a|b|c|d|e|f)
 *
 * INCLUDE_STRING is a filesystem path, with the following %-expansions:
 *     %% - '%'.
 *     %H - The user's home directory (the $HOME environment variable).
 *     %L - The name of the locale specific Compose file (e.g.,
 *          "/usr/share/X11/locale/<localename>/Compose").
 *     %S - The name of the system directory for Compose files (e.g.,
 *          "/usr/share/X11/locale").
 */

enum rules_token {
    TOK_END_OF_FILE = 0,
    TOK_END_OF_LINE,
    TOK_INCLUDE,
    TOK_INCLUDE_STRING,
    TOK_LHS_KEYSYM,
    TOK_COLON,
    TOK_STRING,
    TOK_RHS_KEYSYM,
    TOK_ERROR
};

static void
scanner_log(enum xkb_log_level level, struct scanner *s, const char *msg)
{
    xkb_log(s->ctx, level, 0, "%s:%d:%d: %s\n", s->file_name,
            s->token_line, s->token_column, msg);
}

static int
scanner_error(struct scanner *s, const char *msg)
{
    scanner_log(XKB_LOG_LEVEL_ERROR, s, msg);
    return TOK_ERROR;
}

static void
scanner_warn(struct scanner *s, const char *msg)
{
    scanner_log(XKB_LOG_LEVEL_WARNING, s, msg);
}

/* Values returned with some tokens, like yylval. */
union lvalue {
    char *string;
    xkb_keysym_t keysym;
};

static enum rules_token
lex(struct scanner *s, union lvalue *val)
{
skip_more_whitespace_and_comments:
    /* Skip spaces. */
    while (is_space(peek(s)))
        if (next(s) == '\n')
            return TOK_END_OF_LINE;

    /* Skip comments. */
    if (chr(s, '#')) {
        while (!eof(s) && !eol(s)) next(s);
        goto skip_more_whitespace_and_comments;
    }

    /* See if we're done. */
    if (eof(s)) return TOK_END_OF_FILE;

    /* New token. */
    s->token_line = s->line;
    s->token_column = s->column;
    s->buf_pos = 0;

    /* LHS Keysym. */
    if (chr(s, '<')) {
        while (peek(s) != '>' && !eol(s))
            buf_append(s, next(s));
        if (!buf_append(s, '\0') || !chr(s, '>'))
            return scanner_error(s, "unterminated keysym literal");
        val->keysym = xkb_keysym_from_name(s->buf, XKB_KEYSYM_NO_FLAGS);
        if (val->keysym == XKB_KEY_NoSymbol)
            return scanner_error(s, "unrecognized keysym");
        return TOK_LHS_KEYSYM;
    }

    /* Colon. */
    if (chr(s, ':'))
        return TOK_COLON;

    /* String literal. */
    if (chr(s, '\"')) {
        while (!eof(s) && !eol(s) && peek(s) != '\"') {
            if (chr(s, '\\')) {
                uint8_t o;
                if (chr(s, '\\')) {
                    buf_append(s, '\\');
                }
                else if (chr(s, '"')) {
                    buf_append(s, '"');
                }
                else if (chr(s, 'x') || chr(s, 'X')) {
                    if (hex(s, &o))
                        buf_append(s, (char) o);
                    else
                        scanner_warn(s, "illegal hexadecimal escape sequence in string literal");
                }
                else if (oct(s, &o)) {
                    buf_append(s, (char) o);
                }
                else {
                    scanner_warn(s, "unknown escape sequence in string literal");
                    /* Ignore. */
                }
            } else {
                buf_append(s, next(s));
            }
        }
        if (!buf_append(s, '\0') || !chr(s, '\"'))
            return scanner_error(s, "unterminated string literal");
        val->string = s->buf;
        return TOK_STRING;
    }

    /* RHS keysym or include. */
    if (is_alpha(peek(s)) || peek(s) == '_') {
        s->buf_pos = 0;
        while (is_alnum(peek(s)) || peek(s) == '_')
            buf_append(s, next(s));
        if (!buf_append(s, '\0'))
            return scanner_error(s, "identifier too long");

        if (streq(s->buf, "include"))
            return TOK_INCLUDE;

        val->keysym = xkb_keysym_from_name(s->buf, XKB_KEYSYM_NO_FLAGS);
        if (val->keysym == XKB_KEY_NoSymbol)
            return scanner_error(s, "unrecognized keysym");
        return TOK_RHS_KEYSYM;
    }

    return scanner_error(s, "unrecognized token");
}

static enum rules_token
lex_include_string(struct scanner *s, union lvalue *val)
{
    while (is_space(peek(s)))
        if (next(s) == '\n')
            return TOK_END_OF_LINE;

    s->token_line = s->line;
    s->token_column = s->column;
    s->buf_pos = 0;

    if (!chr(s, '\"'))
        return scanner_error(s, "include statement must be followed by a path");

    while (!eof(s) && !eol(s) && peek(s) != '\"') {
        if (chr(s, '%')) {
            if (chr(s, '%')) {
                buf_append(s, '%');
            }
            else if (chr(s, 'H')) {
                /* TODO */
            }
            else if (chr(s, 'L')) {
                /* TODO */
            }
            else if (chr(s, 'S')) {
                /* TODO */
            }
            else {
                return scanner_error(s, "unknown % format in include statement");
            }
        } else {
            buf_append(s, next(s));
        }
    }
    if (!buf_append(s, '\0') || !chr(s, '\"'))
        return scanner_error(s, "unterminated include statement");
    val->string = s->buf;
    return TOK_INCLUDE_STRING;
}

struct production {
    xkb_keysym_t lhs[MAX_LHS_LEN];
    unsigned int len;
    xkb_keysym_t keysym;
    char string[256];
    bool has_keysym;
    bool has_string;
};

static uint32_t
add_node(struct xkb_compose *compose, xkb_keysym_t keysym)
{
    struct node new = {
        .keysym = keysym,
        .next = 0,
        .successor = 0,
        .utf8 = 0,
        .ks = XKB_KEY_NoSymbol,
    };
    darray_append(compose->tree, new);
    return darray_size(compose->tree) - 1;
}

static void
add_production(struct xkb_compose *compose, struct scanner *scanner,
               struct production *production)
{
    int lhs_pos;
    uint32_t curr;
    struct node *node;

    curr = 0;
    node = &darray_item(compose->tree, curr);

    for (lhs_pos = 0; lhs_pos < production->len; lhs_pos++) {
        while (production->lhs[lhs_pos] != node->keysym) {
            if (node->next == 0) {
                uint32_t next = add_node(compose, production->lhs[lhs_pos]);
                node = &darray_item(compose->tree, curr);
                node->next = next;
            }

            curr = node->next;
            node = &darray_item(compose->tree, curr);
        }

        if (lhs_pos + 1 == production->len)
            break;

        if (node->successor == 0) {
            if (node->utf8 != 0) {
                scanner_warn(scanner,
                             "a sequence already exists which is a prefix of this sequence; overriding");
                node->utf8 = 0;
            }

            {
                uint32_t successor = add_node(compose, production->lhs[lhs_pos + 1]);
                node = &darray_item(compose->tree, curr);
                node->successor = successor;
            }
        }

        curr = node->successor;
        node = &darray_item(compose->tree, curr);
    }

    if (node->successor != 0) {
        scanner_warn(scanner,
                     "the compose sequence is a prefix of another; skipping line");
        return;
    }

    if (node->utf8 != 0) {
        scanner_warn(scanner,
                     "the compose sequence already exists; skipping line");
        return;
    }

    if (production->has_string) {
        node->utf8 = darray_size(compose->utf8);
        darray_append_items(compose->utf8, production->string,
                            strlen(production->string) + 1);
    }
    if (production->has_keysym) {
        node->ks = production->keysym;
    }
}

static bool
parse(struct xkb_compose *compose, struct scanner *scanner)
{
    int ret;
    enum rules_token tok;
    union lvalue val;
    struct production production;

initial:
    production.len = 0;
    production.has_keysym = false;
    production.has_string = false;

    /* fallthrough */

initial_eol:
    switch (tok = lex(scanner, &val)) {
    case TOK_END_OF_LINE:
        goto initial_eol;
    case TOK_END_OF_FILE:
        goto finished;
    case TOK_INCLUDE:
        goto include;
    case TOK_LHS_KEYSYM:
        production.lhs[production.len++] = val.keysym;
        goto lhs;
    default:
        goto unexpected;
    }

include:
    switch (tok = lex_include_string(scanner, &val)) {
    case TOK_INCLUDE_STRING:
        /* TODO */
        goto initial;
    default:
        goto unexpected;
    }

lhs:
    switch (tok = lex(scanner, &val)) {
    case TOK_LHS_KEYSYM:
        if (production.len + 1 > MAX_LHS_LEN) {
            scanner_warn(scanner,
                         "too many keysyms on left-hand side; skipping line");
            goto skip;
        }
        production.lhs[production.len++] = val.keysym;
        goto lhs;
    case TOK_COLON:
        if (production.len <= 0) {
            scanner_warn(scanner,
                         "expected at least one keysym on left-hand side; skipping line");
            goto skip;
        }
        goto rhs;
    default:
        goto unexpected;
    }

rhs:
    switch (tok = lex(scanner, &val)) {
    case TOK_STRING:
        if (production.has_string) {
            scanner_warn(scanner,
                         "right-hand side can have at most one string; skipping line");
            goto skip;
        }
        if (*val.string == '\0') {
            scanner_warn(scanner,
                         "right-hand side string must not be empty; skipping line");
            goto skip;
        }
        ret = snprintf(production.string, sizeof(production.string),
                       "%s", val.string);
        if (ret < 0 || ret >= sizeof(production.string)) {
            scanner_warn(scanner,
                         "right-hand side string is too long; skipping line");
            goto skip;
        }
        production.has_string = true;
        goto rhs;
    case TOK_RHS_KEYSYM:
        if (production.has_keysym) {
            scanner_warn(scanner,
                         "right-hand side can have at most one keysym; skipping line");
            goto skip;
        }
        production.keysym = val.keysym;
        production.has_keysym = true;
    case TOK_END_OF_LINE:
        if (!production.has_string && !production.has_keysym) {
            scanner_warn(scanner,
                         "right-hand side must have at least one of string or keysym; skipping line");
            goto skip;
        }
        add_production(compose, scanner, &production);
        goto initial;
    default:
        goto unexpected;
    }

unexpected:
    if (tok == TOK_ERROR)
        scanner_error(scanner, "failed to parse file");
    else
        scanner_error(scanner, "unexpected token");
    return false;

skip:
    while (tok != TOK_END_OF_LINE && tok != TOK_END_OF_FILE) {
        if (tok == TOK_STRING)
            free(val.string);
        tok = lex(scanner, &val);
    }
    goto initial;

finished:
    return true;
}

bool
parse_string(struct xkb_compose *compose, const char *string, size_t len,
             const char *file_name)
{
    struct scanner scanner;
    scanner_init(&scanner, compose->ctx, string, len, file_name);
    return parse(compose, &scanner);
}

bool
parse_file(struct xkb_compose *compose, FILE *file, const char *file_name)
{
    bool ok;
    const char *string;
    size_t size;

    ok = map_file(file, &string, &size);
    if (!ok) {
        fprintf(stderr, "Couldn't read Compose file %s: %s\n",
                file_name, strerror(errno));
        return false;
    }

    ok = parse_string(compose, string, size, file_name);
    unmap_file(string, size);
    return ok;
}
