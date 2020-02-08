/**************************************************************************
 *
 *  File:       test.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Managed strings library test and samples
 *
 *  Notes:
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
#include <stdio.h>
#include "stingray.h"

int main(void) {
    sr_string str = sr_new("", 128);
    sr_string str1 = sr_new("foo", 128);
    STRCPY(str, str1);
    printf("%s (%d)\n", CSTR(str), STRLEN(str));
    STRNCAT(str, "bar", 2);
    printf("%s (%d)\n", CSTR(str), STRLEN(str));
    STRNCAT(str, str1, 10);
    printf("%s (%d)\n", CSTR(str), STRLEN(str));
    CONCAT(str, "text", 5, (char)'-', 100, str1, CSTR(str)[0], 3.141593);
    printf("%s (%d)\n", CSTR(str), STRLEN(str));

    printf("%s\n", STRSTR(str, "text"));
    printf("%s\n", STRSTR(str, str1));
    printf("%s\n", STRSTR("affoooo", str1));

    printf("%s vs %s = %d\n", CSTR(str1), "foo", STRCMP(str1, "foo"));
    printf("%s vs %s = %d\n", CSTR(str1), "z", STRCMP(str1, "z"));
    printf("%s vs %s = %d\n", CSTR(str), CSTR(str1), STRCMP(str, str1));

    printf("%s\n", STRCHR(str, 't'));
    printf("%s\n", STRCHR(str1, 'o'));
    printf("%s\n", STRRCHR("baar", 'a'));

    STRFREE(str);
    STRFREE(str1);

    return 0;
}

