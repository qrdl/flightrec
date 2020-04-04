#ifndef _TESTER_H
#define _TESTER_H

#include <jsonapi.h>

#define SUCCESS 0
#define FAILURE 1

#define DATATYPE_STRING 1
#define DATATYPE_INT    2
#define DATATYPE_FLOAT  3
#define DATATYPE_BOOL   4

#define OP_JSON     1
#define OP_CONST    2

struct value {
    char datatype;  // one of DATATYPE_XXX contants
    char op_type;   // one of OP_XXX contants
    union {
        void *json;
        char *literal;
    };
};

int start(char *cmd_line, char **error);
int stop(char **error);

int request(const char *message, char **error);
int response(char **error);

void set_var(const char *name, const char *value);
const char *get_var(const char *name);

int match(const char *string, const char *pattern);

extern JSON_OBJ *json;

#endif
