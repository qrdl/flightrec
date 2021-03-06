%{
 /*************************************************************************
 *
 *  File:       tester.l
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Lexer for tester tool
 *
 *  Notes:
 *
 **************************************************************************
 *
 *  Copyright (C) 2017-2020 Ilya Caramishev (flightrec@qrdl.com)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 **************************************************************************/
%}
S   [ \t\v\n\f]
D	[0-9]
E	[Ee][+-]?{D}+

%{
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "tester.h"
#include "tester.tab.h"

void yyerror(const char *s);

static char *ltrim(char *source);
static char decode_char(char *source, int *consumed);
static char *decode_string(char *source);
static char *process_literal(char *source);

extern int lineno;
%}

%%

"start"     { return START; }
"stop"      { return STOP; }
"case"      { return CASE; }
"request"   { return REQUEST; }
"response"  { return RESPONSE; }
"expect"    { return EXPECT; }
"set"       { return SET; }
"LENGTH"    { return LEN; }

"=="        { return EQ; }
"!="        { return NE; }
"=~"        { return MATCH; }
"!~"        { return NMATCH; }
"="         { return '='; }
"{"         { return '{'; }
"}"         { return '}'; }
"["         { return '['; }
"]"         { return ']'; }
"/"         { return '/'; }
"("         { return '('; }
")"         { return ')'; }

{D}+                { yylval.value.literal = strdup(yytext); return INT;}
{D}+{E}?	        { yylval.value.literal = strdup(yytext); return FLOAT;}
{D}*"."{D}+({E})?	{ yylval.value.literal = strdup(yytext); return FLOAT;}
{D}+"."{D}*({E})?	{ yylval.value.literal = strdup(yytext); return FLOAT;}


\"(\\.|[^\\"\n])*\"     { yytext[strlen(yytext)-1] = '\0'; yylval.value.literal = process_literal(yytext+1); return STRING; }
\'(\\.|[^\\'\n])*\'     { yytext[strlen(yytext)-1] = '\0'; yylval.value.literal = process_literal(yytext+1); return STRING; }

"#".*                   { /* comment - skip till the end of line */ }
[^ 0-9\n\t\v\r\f/!=\[{()][^ \n\t\v\r\f/!=\[{()]*   { yylval.value.literal = process_literal(yytext); return STRING; }

"\n"                    { lineno++; }
{S}                     { /* do nothing */ }

%%

int yywrap(void) {
    return 1;
}

char *ltrim(char *source) {
    while (isspace(*source)) {
        source++;
    }
    return source;
}

// decode escaped character (escape isn't included)
char decode_char(char *source, int *consumed) {
    // add support for Unicode \u and \U escaped chars
    char res = 0;
    switch (*source) {
        case 'x':
            // hex
            for (char *cur = source+1; isxdigit(*cur); cur++) {
                if (consumed) {
                    (*consumed)++;
                }
                res <<= 4;
                if (*cur <= '9') {          // 0 - 9
                    res |= *cur - '0';
                } else if (*cur <= 'F') {   // A - F
                    res |= *cur - 'A' + 10;
                } else {                    // a -f
                    res |= *cur -'a' + 10;
                }
            }
            return res;
        case '0' ... '7':
            // octal
            for (char *cur = source; *cur >= '0' && *cur <= '7'; cur++) {
                if (consumed) {
                    (*consumed)++;
                }
                res <<= 3;
                res |= *cur - '0';
            }
            return res;
        case '?':
            return '\?';
        case 'a':
            return '\a';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'v':
            return '\v';
        default:
            return *source;
    }
}

char *decode_string(char *source) {
    char *res = malloc(strlen(source) + 1);
    char *target = res;
    int consumed = 0;    
    for (char *src = source; *src; src++) {
        if ('\\' == *src) {
            *target = decode_char(src + 1, &consumed);
            target += consumed + 1;
        } else {
            *target++ = *src;
        }
    }
    *target = '\0';
    return target;
}

/* look for variable references in a form of $(var_name), and replace with actual variable content */
char *process_literal(char *source) {
    char *cur;
    size_t len = strlen(source);
    char *result = malloc(len+1);
    size_t i = 0;
    for (cur = source; *cur; cur++) {
        if ('$' == cur[0] && '(' == cur[1]) {
            char *tail = strchr(cur, ')');
            if (!tail) {
                yyerror("Unterminated variable name");
            }
            size_t name_len = tail - cur + 1;
            *tail = '\0';
            const char *value = get_var(cur+2);
            if (!value) {
                yyerror("Unknown variable");
            }
            size_t val_len = strlen(value);
            if (val_len > name_len) {
                char *tmp;
                len += val_len - name_len;
                tmp = realloc(result, len + 1);
                if (tmp) {
                    result = tmp;
                }
            }
            strcpy(result+i, value);
            cur = tail;
            i += val_len;
        } else {
            result[i++] = *cur;
        }
    }
    result[i] = '\0';

    return result;
}
