/* -*- C++ -*- */
/* Copyright (C) 2002 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef _SP_H_
#define _SP_H_

// Return codes from sp_create_* and sp_drop_*:
#define SP_OK                 0
#define SP_KEY_NOT_FOUND     -1
#define SP_OPEN_TABLE_FAILED -2
#define SP_WRITE_ROW_FAILED  -3
#define SP_DELETE_ROW_FAILED -4
#define SP_GET_FIELD_FAILED  -5
#define SP_PARSE_ERROR       -6

sp_head *
sp_find_procedure(THD *thd, LEX_STRING *name);

int
sp_create_procedure(THD *thd, char *name, uint namelen, char *def, uint deflen);

int
sp_drop_procedure(THD *thd, char *name, uint namelen);


sp_head *
sp_find_function(THD *thd, LEX_STRING *name);

int
sp_create_function(THD *thd, char *name, uint namelen, char *def, uint deflen);

int
sp_drop_function(THD *thd, char *name, uint namelen);

// QQ Temporary until the function call detection in sql_lex has been reworked.
bool
sp_function_exists(THD *thd, LEX_STRING *name);


// QQ More temporary stuff until the real cache is implemented. This is
// needed since we have to read the functions before we do anything else.
void
sp_add_fun_to_lex(LEX *lex, LEX_STRING fun);
void
sp_merge_funs(LEX *dst, LEX *src);
int
sp_cache_functions(THD *thd, LEX *lex);
void
sp_clear_function_cache(THD *thd);

#endif /* _SP_H_ */
