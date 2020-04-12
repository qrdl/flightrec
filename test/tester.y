%{
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include <jsonapi.h>

#include "tester.h"

#define STATUS_OK      0
#define STATUS_FAILED  1
#define STATUS_INVALID 2

#define EPSILON 0.000001

/* yyerror() is standard function that may be called from inside YACC-genrated code, while INVALID() is called by rules */
void yyerror(const char *s);
#define INVALID(...) do { \
    fprintf(stderr, "%s:%d\t", filename, lineno); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    status = STATUS_INVALID; \
    printf("INVALID\n"); \
    YYABORT; \
} while (0)

#define FAILED(...) do { \
    fprintf(stderr, "%s:%d\t", filename, lineno); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    status = STATUS_FAILED; \
    printf("FAILED\n"); \
    YYABORT; \
} while (0)


FILE *yyin;
int yylex(void);

char *filename;
int lineno;

static int cases;
static int status = STATUS_OK;

#define GET_DOUBLE(A) ({ \
            double ret = 0; \
            switch ((A).datatype) {\
                case DATATYPE_STRING: \
                    ret = atof(JSON_GET_STRING_VALUE((A).json)); \
                    break; \
                case DATATYPE_INT: \
                    ret = (double)JSON_GET_INT32_VALUE((A).json); \
                    break; \
                case DATATYPE_FLOAT: \
                    ret = JSON_GET_DBL_VALUE((A).json); \
                    break; \
            } \
            ret; \
})

#define GET_DOUBLE_OPERANDS(A,B) \
            double operand1, operand2; \
            if (OP_CONST == (A).op_type) { \
                operand1 = atof((A).literal); \
            } else { \
                operand1 = GET_DOUBLE(A); \
            } \
            if (OP_CONST == (B).op_type) { \
                operand2 = atof((B).literal); \
            } else { \
                operand2 = GET_DOUBLE(B); \
            }

#define GET_BOOL_OPERANDS(A,B) \
            char operand1, operand2; \
            if (OP_CONST == (A).op_type) { \
                if (!strcmp((A).literal, "true") || 0 != atof((A).literal)) \
                    operand1 = 1; \
                else \
                    operand1 = 0; \
            } else { \
                operand1 = JSON_GET_BOOL_VALUE((A).json); \
            } \
            if (OP_CONST == (B).op_type) { \
                if (!strcmp((B).literal, "true") || 0 != atof((B).literal)) \
                    operand2 = 1; \
                else \
                    operand2 = 0; \
            } else { \
                operand2 = JSON_GET_BOOL_VALUE((B).json); \
            }


#define GET_STRING_OPERANDS(A,B) \
            const char *operand1, *operand2; \
            if (OP_CONST == (A).op_type) { \
                operand1 = (A).literal; \
            } else { \
                operand1 = JSON_GET_STRING_VALUE((A).json); \
            } \
            if (OP_CONST == (B).op_type) { \
                operand2 = (B).literal; \
            } else { \
                operand2 = JSON_GET_STRING_VALUE((B).json); \
            }

%}

%union {
    struct value value;
}

%define parse.lac full
%define parse.error verbose

%token START STOP CASE REQUEST RESPONSE EXPECT SET EQ NE MATCH NMATCH
%token <value> INT FLOAT STRING

%type <value> operand json_path

%start file

%%

file
    : file_item
    | file file_item
    ;

file_item
    : start
    | case
    | stop
    | set
;

case
    : case_begin '{' case_body '}' {
        if (STATUS_OK != status) {
            YYABORT;
        }
        printf("OK\n");
        cases++;
    }
    ;

/* have to use special rule for case keywork in order to report case start BEFORE the actual execution */
case_begin
    : CASE STRING {
        printf("\t%s ... ", $2.literal);
        fflush(stdout);
        free($2.literal);
    }
    ;

case_body
    : statement
    | case_body statement
    ;

statement
    : request
    | response
    | set
    ;

request
    : REQUEST STRING {
        /* do a request, if fails, report error */
        char *error;
        if (SUCCESS != request($2.literal, &error)) {
            INVALID("Cannot send request: %s", error);
        }
        free($2.literal);
    }
    ;

response
    : response_begin '{' response_lines '}'
    | response_begin response_line
    | response_begin '{' '}'
    ;

/* have to use special rule for response keywork in order to actually read the response BEFORE the checking its content */
response_begin
    : RESPONSE {
        /* read a response, blocking (use timeout), if fails, report error */
        char *error;
        if (SUCCESS != response(&error)) {
            INVALID("Cannot get response: %s", error);
        }
    }
    ;

response_lines
    : response_line
    | response_lines response_line
    ;

response_line
    : expect
    | set
    ;

expect
    : EXPECT operand EQ operand {
        if (DATATYPE_BOOL == $2.datatype || DATATYPE_BOOL == $4.datatype) {
            GET_BOOL_OPERANDS($2, $4);
            if (operand1 != operand2) {
                FAILED("Values %s and %s aren't equal", operand1 ? "true" : "false", operand2 ? "true" : "false");
            }
        } else if (     DATATYPE_FLOAT == $2.datatype || DATATYPE_FLOAT == $4.datatype ||
                        DATATYPE_INT   == $2.datatype || DATATYPE_INT   == $4.datatype) {
            GET_DOUBLE_OPERANDS($2, $4);
            if (fabs(operand1 - operand2) >= EPSILON) {
                FAILED("Values %lf and %lf aren't equal", operand1, operand2);
            }
        } else {
            GET_STRING_OPERANDS($2, $4);
            if (strcmp(operand1, operand2)) {
                FAILED("Values \"%s\" and \"%s\" aren't equal", operand1, operand2);
            }
        }
        if (OP_CONST == $2.op_type) {
            free($2.literal);
        }
        if (OP_CONST == $4.op_type) {
            free($4.literal);
        }
    }
    | EXPECT operand NE operand {
        if (DATATYPE_BOOL == $2.datatype || DATATYPE_BOOL == $4.datatype) {
            GET_BOOL_OPERANDS($2, $4);
            if (operand1 == operand2) {
                FAILED("Values %s and %s are equal", operand1 ? "true" : "false", operand2 ? "true" : "false");
            }
        } else if (     DATATYPE_FLOAT == $2.datatype || DATATYPE_FLOAT == $4.datatype ||
                        DATATYPE_INT   == $2.datatype || DATATYPE_INT   == $4.datatype) {
            GET_DOUBLE_OPERANDS($2, $4);
            if (fabs(operand1 - operand2) < EPSILON) {
                FAILED("Values %lf and %lf are equal", operand1, operand2);
            }
        } else {
            GET_STRING_OPERANDS($2, $4);
            if (!strcmp(operand1, operand2)) {
                FAILED("Values \"%s\" and \"%s\" are equal", operand1, operand2);
            }
        }
        if (OP_CONST == $2.op_type) {
            free($2.literal);
        }
        if (OP_CONST == $4.op_type) {
            free($4.literal);
        }
    }
    | EXPECT operand MATCH operand {
        if (    DATATYPE_FLOAT == $2.datatype || DATATYPE_FLOAT == $4.datatype ||
                DATATYPE_INT   == $2.datatype || DATATYPE_INT   == $4.datatype) {
            INVALID("Unsupported comparison operator for numeric operands");
        } else if (DATATYPE_BOOL == $2.datatype || DATATYPE_BOOL == $4.datatype) {
            INVALID("Unsupported comparison operator for boolean operands");
        } else {
            GET_STRING_OPERANDS($2, $4);
            if (!match(operand1, operand2)) {
                FAILED("Value \"%s\" doesn't match pattern \"%s\"", operand1, operand2);
            }
        }
        if (OP_CONST == $2.op_type) {
            free($2.literal);
        }
        if (OP_CONST == $4.op_type) {
            free($4.literal);
        }
    }
    | EXPECT operand NMATCH operand {
        if (    DATATYPE_FLOAT == $2.datatype || DATATYPE_FLOAT == $4.datatype ||
                DATATYPE_INT   == $2.datatype || DATATYPE_INT   == $4.datatype) {
            INVALID("Unsupported comparison operator for numeric operands");
        } else if (DATATYPE_BOOL == $2.datatype || DATATYPE_BOOL == $4.datatype) {
            INVALID("Unsupported comparison operator for boolean operands");
        } else {
            GET_STRING_OPERANDS($2, $4);
            if (match(operand1, operand2)) {
                FAILED("Value \"%s\" does match pattern \"%s\"", operand1, operand2);
            }
        }
        if (OP_CONST == $2.op_type) {
            free($2.literal);
        }
        if (OP_CONST == $4.op_type) {
            free($4.literal);
        }
    }
    ;

operand
    : json_path {
        $$ = $1;
        $$.op_type = OP_JSON;
        switch (JSON_GET_OBJECT_TYPE($1.json)) {
            case json_type_double:
                $$.datatype = DATATYPE_FLOAT;
                break;
            case json_type_int:
                $$.datatype = DATATYPE_INT;
                break;
            case json_type_boolean:
                $$.datatype = DATATYPE_BOOL;
                break;
            case json_type_string:
                $$.datatype = DATATYPE_STRING;
                break;
            default:
                INVALID("Unsupported JSON data type as operand");
                YYABORT;
        }
    }
    | STRING {
        $$ = $1;
        $$.op_type = OP_CONST;
        $$.datatype = DATATYPE_STRING;
    }
    | INT {
        $$ = $1;
        $$.op_type = OP_CONST;
        $$.datatype = DATATYPE_INT;
    }
    | FLOAT {
        $$ = $1;
        $$.op_type = OP_CONST;
        $$.datatype = DATATYPE_FLOAT;
    }
    ;

json_path
    : '/' STRING {
        $$.json = JSON_GET_OBJ(json, $2.literal);
        if (JSON_OK != json_err) {
            INVALID("JSON path '%s' not found", $2.literal);
        }
        free($2.literal);
    }
    | json_path '/' STRING {
        $$.json = JSON_GET_OBJ($1.json, $3.literal);
        if (JSON_OK != json_err) {
            INVALID("JSON path '%s' not found", $3.literal);
        }
        free($3.literal);
    }
    | '/' STRING '[' INT ']' {
        $$.json = JSON_GET_ARRAY(json, $2.literal);
        if (JSON_ERR_MISMATCH == json_err) {
            INVALID("JSON path '%s' isn't subscriptable", $2.literal);
        } else if (JSON_OK != json_err) {
            INVALID("JSON path '%s' not found", $2.literal);
        }
        int index = atoi($4.literal);
        $$.json = JSON_GET_ITEM($$.json, index);
        if (JSON_OK != json_err) {
            INVALID("JSON path '%s[%d]' not found", $2.literal, index);
        }
        free($2.literal);
        free($4.literal);
    }
    | json_path '/' STRING '[' INT ']' {
        $$.json = JSON_GET_ARRAY($1.json, $3.literal);
        if (JSON_ERR_MISMATCH == json_err) {
            INVALID("JSON path '%s' isn't subscriptable", $3.literal);
        } else if (JSON_OK != json_err) {
            INVALID("JSON path '%s' not found", $3.literal);
        }
        int index = atoi($5.literal);
        $$.json = JSON_GET_ITEM($$.json, index);
        if (JSON_OK != json_err) {
            INVALID("JSON path '%s[%d]' not found", $3.literal, index);
        }
        free($3.literal);
        free($5.literal);
    }
    ;

set
    : SET STRING '=' operand {
        if (OP_CONST == $4.op_type) {
            set_var($2.literal, $4.literal);    // regardless of datatype 'literal' contains string value
        } else {
            switch ($4.datatype) {
                char val[32];
                case DATATYPE_STRING:
                    set_var($2.literal, JSON_GET_STRING_VALUE($4.json));
                    break;
                case DATATYPE_INT:
                    snprintf(val, sizeof(val), "%d", JSON_GET_INT32_VALUE($4.json));
                    set_var($2.literal, val);
                    break;
                case DATATYPE_FLOAT:
                    snprintf(val, sizeof(val), "%lf", JSON_GET_DBL_VALUE($4.json));
                    set_var($2.literal, val);
                    break;
                case DATATYPE_BOOL:
                    if (JSON_GET_DBL_VALUE($4.json)) {
                        set_var($2.literal, "true");
                    } else {
                        set_var($2.literal, "false");
                    }
                    break;
            }
        }
    }
    ;

start
    : START STRING {
        /* start child process, if failed, report error. If child already started, report error */
        char *error;
        if (SUCCESS != start($2.literal, &error)) {
            INVALID("Cannot start process '%s': %s", $2.literal, error);
        }
        free($2.literal);
    }
    ;

stop
    : STOP {
        /* kill child process, if child not started, report error */
        char *error;
        if (SUCCESS != stop(&error)) {
            INVALID("Cannot stop process: %s", error);
        }
    }
    ;

%%

void yyerror(char const *s) {
    fprintf(stderr, "%s:%d\tParse error: %s\n", filename, lineno, s);
    status = STATUS_INVALID;
}

int main(int argc, char *argv[]) {
#if YYDEBUG
    yydebug = 1;
#endif
    if (argc < 2) {
        printf("Usage: %s [-v var_name=var_value ...] <test_script> [<test_script> ...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int i;
    for (i = 1; i < argc; i=i+2) {
        if (strcmp(argv[i], "-v")) {
            break;
        }
        char *var_name = argv[i+1];
        char *cur = strchr(var_name, '=');
        if (!cur) {
            printf("Usage: %s [-v var_name=var_value ...] <test_script> [<test_script> ...]\n", argv[0]);
            return EXIT_FAILURE;
        }
        *cur = '\0';    // terminate var name
        cur++;
        if ('"' == cur[0] && '"' == cur[strlen(cur)-1]) {
            /* unquote quoted value */
            cur++;
            cur[strlen(cur)-1] = '\0';
        }
        set_var(var_name, cur);
    }
    int files = 0;
    for (; i < argc; i++) {
        yyin = fopen(argv[i], "r");
        if (!yyin) {
            printf("Cannot open file '%s': %s", argv[i], strerror(errno));
            return EXIT_FAILURE;
        }
        printf("Processing file '%s' ...\n", argv[i]);
        lineno = 1;
        filename = argv[i];
        yyparse();
        fclose(yyin);
        if (status != STATUS_OK) {
            break;
        }
        files++;
    }
    printf("Fully processed: %d file(s), %d case(s)\n", files, cases);
    if (STATUS_FAILED == status) {
        printf("FAILED\n");
        return EXIT_FAILURE;
    } else if (STATUS_INVALID == status) {
        printf("INVALID\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
