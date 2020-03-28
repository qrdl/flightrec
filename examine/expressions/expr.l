D   [0-9]
O   [0-7]
L   [a-zA-Z_]
H   [a-fA-F0-9]
E   ([Ee][+-]?{D}+)
P   ([Pp][+-]?{D}+)
FS  (f|F|l|L)
IS  ((u|U)|(u|U)?(l|L|ll|LL)|(l|L|ll|LL)(u|U))
S   [ \t\v\n\f]

%{
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "flightrec.h"
#include "expression.h"
#include "expr_internal.h"
#include "y.tab.h"

void yyerror(struct ast_node **node, const char *s);

static char *ltrim(char *source);
static int find_struct_or_union(char *name);
static int find_var_or_type(char *name);
static char decode_char(char *source, int *consumed);
static char *decode_string(char *source);
%}

%%

"sizeof"    { return(SIZEOF); }
"void"      { yylval.ast = new_ast_node(NODE_TYPE_TYPE); return(VOID); }

"struct"{S}+{L}({L}|{D})*   { return find_struct_or_union(ltrim(yytext+sizeof("struct")-1)); }
"union"{S}+{L}({L}|{D})*    { return find_struct_or_union(ltrim(yytext+sizeof("union")-1)); }

"float" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(float); return(TYPE); }
"double" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); return(TYPE); }
"long double" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(long double); return(TYPE); }

"unsigned"{S}+"long"{S}+"long"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(unsigned long long); return(TYPE); }
"unsigned"{S}+"long"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(unsigned long); return(TYPE); }
"unsigned"{S}+"short"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(unsigned short); return(TYPE); }
"unsigned"{S}+"char" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = 1; return(TYPE); }
"unsigned"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(unsigned int); return(TYPE); }
"uint64_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(uint64_t); return(TYPE); }
"uint32_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(uint32_t); return(TYPE); }
"uint16_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(uint16_t); return(TYPE); }
"uint8_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = sizeof(uint8_t); return(TYPE); }

("signed"{S}+)?"long"{S}+"long"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(signed long long); return(TYPE); }
("signed"{S}+)?"long"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(signed long); return(TYPE); }
("signed"{S}+)?"int" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(signed int); return(TYPE); }
("signed"{S}+)?"short"({S}+"int")? { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(signed short); return(TYPE); }
("signed"{S}+)?"char" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = 1; return(TYPE); }
"signed" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(signed int); return(TYPE); }
"int64_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); return(TYPE); }
"int32_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int32_t); return(TYPE); }
"int16_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int16_t); return(TYPE); }
"int8_t" { yylval.ast = new_ast_node(NODE_TYPE_TYPE); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = 1; return(TYPE); }

0[xX]{H}+{IS}?              { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = strtol(yytext, NULL, 16); return(CONSTANT); }
0{O}*{IS}?                  { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = strtol(yytext, NULL, 8); return(CONSTANT); }
[1-9]{D}*{IS}?              { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = strtol(yytext, NULL, 10); return(CONSTANT); }
'\\{O}{O}?{O}?'             { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = decode_char(yytext+2, NULL); return(CONSTANT); }
'\\x{H}{H}?'                { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = decode_char(yytext+2, NULL); return(CONSTANT); }
'\\.'                       { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(char); yylval.ast->int_value = decode_char(yytext+2, NULL); return(CONSTANT); }
'.'                         { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = sizeof(int64_t); yylval.ast->int_value = yytext[1]; return(CONSTANT); }
{D}+{E}{FS}?                { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
{D}*"."{D}+{E}?{FS}?        { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
{D}+"."{D}*{E}?{FS}?        { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
0[xX]{H}+{P}{FS}?           { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
0[xX]{H}*"."{H}+{P}?{FS}?   { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
0[xX]{H}+"."{H}*{P}?{FS}?   { yylval.ast = new_ast_node(NODE_TYPE_FLOAT); yylval.ast->type_kind = TKIND_FLOAT; yylval.ast->size = sizeof(double); yylval.ast->float_value = strtod(yytext, NULL); return(CONSTANT); }
"NULL"                      { yylval.ast = new_ast_node(NODE_TYPE_INT); yylval.ast->type_kind = TKIND_UNSIGNED; yylval.ast->size = 1; yylval.ast->int_value = 0; return(CONSTANT); }

\"(\\.|[^\\"\n])*\"         { yylval.ast = new_ast_node(NODE_TYPE_STRING); yylval.ast->type_kind = TKIND_SIGNED; yylval.ast->size = 1; yylval.ast->indirect = 1; yylval.ast->str_value = decode_string(yytext); return(CONSTANT); }

{L}({L}|{D})*               { return find_var_or_type(yytext); }

">>"        { return(RIGHT_OP); }
"<<"        { return(LEFT_OP); }
"->"        { return(PTR_OP); }
"&&"        { return(AND_OP); }
"||"        { return(OR_OP); }
"<="        { return(LE_OP); }
">="        { return(GE_OP); }
"=="        { return(EQ_OP); }
"!="        { return(NE_OP); }
"("         { return('('); }
")"			{ return(')'); }
("["|"<:")  { return('['); }
("]"|":>")  { return(']'); }
"."         { return('.'); }
"&"         { return('&'); }
"!"         { return('!'); }
"~"         { return('~'); }
"-"         { return('-'); }
"+"         { return('+'); }
"*"         { return('*'); }
"/"         { return('/'); }
"%"         { return('%'); }
"<"         { return('<'); }
">"         { return('>'); }
"^"         { return('^'); }
"|"         { return('|'); }

{S}         { /* do nothing */ }
.			{ yyerror(NULL, "Unsupported character in expression"); }

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

// C grammar isn't context-free, therefore need to resolve types in lexer
int find_struct_or_union(char *name) {
    yylval.ast = new_ast_node(NODE_TYPE_TYPE);
    if (SUCCESS == get_struct_details(name, &yylval.ast->type_offset, &yylval.ast->type_kind, &yylval.ast->size)) {
        return(TYPE);
    } else {
        yyerror(NULL, "Invalid struct/union name");
        return 0;
    }
}

int find_var_or_type(char *name) {
    yylval.ast = new_ast_node(NODE_TYPE_TYPE);
    if (SUCCESS == get_type_details(name, &yylval.ast->type_offset, &yylval.ast->type_kind, &yylval.ast->size)) {
        return(TYPE);
    }
    yylval.ast->node_type = NODE_TYPE_VAR;
    yylval.ast->var_name = name;
    return(IDENTIFIER);
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
