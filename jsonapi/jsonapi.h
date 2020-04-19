/**************************************************************************
 *
 *  File:       jsonapi.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      JSON API
 *
 *  Notes:      The reason for this API is to stay independent from JSON
 *              library implementations.
 *              Now it uses json-c (https://github.com/json-c/json-c)
 *              but can be easily changed to other library, if needed
 *
 **************************************************************************
 *
 *  Copyright (C) 2017-2020 Ilya Caramishev (ilya@qrdl.com)
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
#ifndef _JSONAPI_H
#define _JSONAPI_H
/* stddef.h is required as json.h doesn't include it and uses size_t */
#include <stddef.h>
#include <json-c/json.h>

#define JSON_ERR_MIN        0
#define JSON_OK             0
#define JSON_ERR_INVALID    1
#define JSON_ERR_NOTFOUND   2
#define JSON_ERR_MISMATCH   3
#define JSON_ERR_MAX        3

/* ({ ... }) is GCC's statement expressions: http://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html */

#define JSON_OBJ                        json_object
#define JSON_PARSE(S)                   ({ \
                                            json_err = JSON_OK; \
                                            json_object *ret = json_tokener_parse(S); \
                                            if (!ret) \
                                                json_err = JSON_ERR_INVALID; \
                                            ret; \
                                        })
#define JSON_PRINT(O)                   json_object_to_json_string(O)
#define JSON_GET_ARRAY_SIZE(O)          json_object_array_length(O)
#define JSON_GET_OBJECT_TYPE(O)         json_object_get_type(O)
#define JSON_NEW_OBJ()                  json_object_new_object()
#define JSON_COPY_OBJ(O)                ({ \
                                            json_object *ret = NULL; \
                                            json_object_deep_copy(O, &ret, NULL); \
                                            ret; \
                                        })
#define JSON_RELEASE(O)                 json_object_put(O)


/***** Get object value *****/
#define JSON_GET_STRING_VALUE(O)        ({ \
                                            const char *ret = NULL; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (json_type_string != json_object_get_type(O)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_string(O); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_INT32_VALUE(O)         ({ \
                                            int32_t ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (json_type_int != json_object_get_type(O)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_int(O); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_INT64_VALUE(O)         ({ \
                                            int64_t ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (json_type_int != json_object_get_type(O)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_int64(O); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_DBL_VALUE(O)           ({ \
                                            double ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (json_type_double != json_object_get_type(O)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_double(O); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_BOOL_VALUE(O)          ({ \
                                            int ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (json_type_boolean != json_object_get_type(O)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_boolean(O); \
                                            } \
                                            ret; \
                                        })

/***** Get object field value by key *****/
#define JSON_GET_OBJ(O,K)               ({ \
                                            json_object *ret = NULL; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &ret)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_ARRAY(O,K)             ({ \
                                            json_object *ret = NULL; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &ret)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_array != json_object_get_type(ret)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_STRING_FIELD(O,K)      ({ \
                                            json_object *tmp = NULL; \
                                            const char *ret = NULL; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &tmp)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_string != json_object_get_type(tmp)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_string(tmp); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_INT32_FIELD(O,K)       ({ \
                                            json_object *tmp = NULL; \
                                            int32_t ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &tmp)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_int != json_object_get_type(tmp)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_int(tmp); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_INT64_FIELD(O,K)       ({ \
                                            json_object *tmp = NULL; \
                                            int64_t ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &tmp)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_int != json_object_get_type(tmp)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_int64(tmp); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_DBL_FIELD(O,K)         ({ \
                                            json_object *tmp = NULL; \
                                            double ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &tmp)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_double != json_object_get_type(tmp)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_double(tmp); \
                                            } \
                                            ret; \
                                        })
#define JSON_GET_BOOL_FIELD(O,K)        ({ \
                                            json_object *tmp = NULL; \
                                            int ret = 0; \
                                            if (O) { \
                                                json_err = JSON_OK; \
                                                if (!json_object_object_get_ex(O, K, &tmp)) \
                                                    json_err = JSON_ERR_NOTFOUND; \
                                                else if (json_type_boolean != json_object_get_type(tmp)) \
                                                    json_err = JSON_ERR_MISMATCH; \
                                                else \
                                                    ret = json_object_get_boolean(tmp); \
                                            } \
                                            ret; \
                                        })

/***** Get array item value by index *****/
#define JSON_GET_ITEM(O,I)              ({ \
                                            json_err = JSON_OK; \
                                            json_object *ret = json_object_array_get_idx(O, I); \
                                            if (!ret) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            ret; \
                                        })
#define JSON_GET_STRING_ITEM(O,I)       ({ \
                                            const char *ret = NULL; \
                                            json_err = JSON_OK; \
                                            json_object *tmp = json_object_array_get_idx(O, I); \
                                            if (!tmp) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            else if (json_type_string != json_object_get_type(tmp)) \
                                                json_err = JSON_ERR_MISMATCH; \
                                            else \
                                                ret = json_object_get_string(tmp); \
                                            ret; \
                                        })
#define JSON_GET_INT32_ITEM(O,I)        ({ \
                                            int32_t ret = 0; \
                                            json_err = JSON_OK; \
                                            json_object *tmp = json_object_array_get_idx(O, I); \
                                            if (!tmp) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            else if (json_type_int != json_object_get_type(tmp)) \
                                                json_err = JSON_ERR_MISMATCH; \
                                            else \
                                                ret = json_object_get_int(tmp); \
                                            ret; \
                                        })                                    
#define JSON_GET_INT64_ITEM(O,I)        ({ \
                                            int64_t ret = 0; \
                                            json_err = JSON_OK; \
                                            json_object *tmp = json_object_array_get_idx(O, I); \
                                            if (!tmp) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            else if (json_type_int != json_object_get_type(tmp)) \
                                                json_err = JSON_ERR_MISMATCH; \
                                            else \
                                                ret = json_object_get_int64(tmp); \
                                            ret; \
                                        })                                    
#define JSON_GET_DBL_ITEM(O,I)          ({ \
                                            double ret = 0; \
                                            json_err = JSON_OK; \
                                            json_object *tmp = json_object_array_get_idx(O, I); \
                                            if (!tmp) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            else if (json_type_double != json_object_get_type(tmp)) \
                                                json_err = JSON_ERR_MISMATCH; \
                                            else \
                                                ret = json_object_get_double(tmp); \
                                            ret; \
                                        })
#define JSON_GET_BOOL_ITEM(O,I)         ({ \
                                            int ret = 0; \
                                            json_err = JSON_OK; \
                                            json_object *tmp = json_object_array_get_idx(O, I); \
                                            if (!tmp) \
                                                json_err = JSON_ERR_NOTFOUND; \
                                            else if (json_type_boolean != json_object_get_type(tmp)) \
                                                json_err = JSON_ERR_MISMATCH; \
                                            else \
                                                ret = json_object_get_boolean(tmp); \
                                            ret; \
                                        })                                

/***** Add object field *****/
#define JSON_NEW_INT32_FIELD(O,K,F)     ({ \
                                            json_object *ret = json_object_new_int(F); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_INT64_FIELD(O,K,F)     ({ \
                                            json_object *ret = json_object_new_int64(F); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_DBL_FIELD(O,K,F)       ({ \
                                            json_object *ret = json_object_new_double(F); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_STRING_FIELD(O,K,F)    ({ \
                                            json_object *ret = json_object_new_string(F); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_TRUE_FIELD(O,K)        ({ \
                                            json_object *ret = json_object_new_boolean(1); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_FALSE_FIELD(O,K)       ({ \
                                            json_object *ret = json_object_new_boolean(0); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_OBJ_FIELD(O,K)         ({ \
                                            json_object *ret = json_object_new_object(); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_NEW_ARRAY_FIELD(O,K)       ({ \
                                            json_object *ret = json_object_new_array(); \
                                            json_object_object_add(O, K, ret); \
                                            ret; \
                                        })
#define JSON_ADD_OBJ_FIELD(O,K,F)       json_object_object_add(O, K, F)

/***** Add array items *****/
#define JSON_NEW_ARRAY()                json_object_new_array()
#define JSON_ADD_INT32_ITEM(O,F)        json_object_array_add(O, json_object_new_int(F))
#define JSON_ADD_INT64_ITEM(O,F)        json_object_array_add(O, json_object_new_int64(F))
#define JSON_ADD_DBL_ITEM(O,F)          json_object_array_add(O, json_object_new_double(F))
#define JSON_ADD_STRING_ITEM(O,F)       json_object_array_add(O, json_object_new_string(F))
#define JSON_ADD_TRUE_ITEM(O)           json_object_array_add(O, json_object_new_boolean(1))
#define JSON_ADD_FALSE_ITEM(O)          json_object_array_add(O, json_object_new_boolean(0))
#define JSON_ADD_OBJ_ITEM(O,F)          json_object_array_add(O, F)
#define JSON_ADD_NEW_ITEM(O)            ({ \
                                            json_object *ret = json_object_new_object(); \
                                            json_object_array_add(O, ret); \
                                            ret; \
                                        })

const char *json_strerror(int err);

extern int json_err;

#endif

