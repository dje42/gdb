/* Scheme interface to symbols.

   Copyright (C) 2008-2013 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* See README file in this directory for implementation notes, coding
   conventions, et.al.  */

#include "defs.h"
#include "block.h"
#include "exceptions.h"
#include "frame.h"
#include "symtab.h"
#include "objfiles.h"
#include "value.h"
#include "guile-internal.h"

/* The <gdb:symbol> smob.  */

typedef struct {
  /* This always appears first.
     A symbol object is associated with an objfile, so use a chained_gdb_smob
     to keep track of all symbols associated with the objfile.  This lets us
     invalidate the underlying struct symbol when the objfile is deleted.  */
  chained_gdb_smob base;

  /* The GDB symbol structure this smob is wrapping.  */
  struct symbol *symbol;
} symbol_smob;

static const char symbol_smob_name[] = "gdb:symbol";

/* The tag Guile knows the symbol smob by.  */
static scm_t_bits symbol_smob_tag;

/* Keywords used in argument passing.  */
static SCM syscm_block_keyword;
static SCM syscm_domain_keyword;
static SCM syscm_frame_keyword;

static const struct objfile_data *syscm_objfile_data_key;

/* Administrivia for symbol smobs.  */

/* The smob "mark" function for <gdb:symbol>.  */

static SCM
syscm_mark_symbol_smob (SCM self)
{
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (self);

  /* Do this last.  */
  return gdbscm_mark_chained_gsmob (&s_smob->base);
}

/* The smob "free" function for <gdb:symbol>.  */

static size_t
syscm_free_symbol_smob (SCM self)
{
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (self);

  gdbscm_remove_objfile_ref ((s_smob->symbol != NULL
			      && SYMBOL_SYMTAB (s_smob->symbol) != NULL)
			     ? SYMBOL_SYMTAB (s_smob->symbol)->objfile
			     : NULL,
			     syscm_objfile_data_key, &s_smob->base);
  /* Not necessary, done to catch bugs.  */
  s_smob->symbol = NULL;

  return 0;
}

/* The smob "print" function for <gdb:symbol>.  */

static int
syscm_print_symbol_smob (SCM self, SCM port, scm_print_state *pstate)
{
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (self);

  if (pstate->writingp)
    gdbscm_printf (port, "#<%s ", symbol_smob_name);
  gdbscm_printf (port, "%s",
		 s_smob->symbol != NULL
		 ? SYMBOL_PRINT_NAME (s_smob->symbol)
		 : "<invalid>");
  if (pstate->writingp)
    scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* The smob "equalp" function for <gdb:symbol>.  */

static SCM
syscm_equal_p_symbol_smob (SCM s1, SCM s2)
{
  symbol_smob *s1_smob, *s2_smob;

  s1_smob = (symbol_smob *) SCM_SMOB_DATA (s1);
  s2_smob = (symbol_smob *) SCM_SMOB_DATA (s2);

  if (s1_smob->symbol == NULL || s2_smob->symbol == NULL)
    return SCM_BOOL_F;
  return scm_from_bool (s1_smob->symbol == s2_smob->symbol);
}

/* Low level routine to create a <gdb:symbol> object.  */

static SCM
syscm_make_symbol_smob (void)
{
  symbol_smob *s_smob = (symbol_smob *)
    scm_gc_malloc (sizeof (symbol_smob), symbol_smob_name);
  SCM s_scm;

  s_smob->symbol = NULL;
  s_scm = scm_new_smob (symbol_smob_tag, (scm_t_bits) s_smob);
  gdbscm_init_chained_gsmob (&s_smob->base);

  return s_scm;
}

/* Return non-zero if SCM is a symbol smob.  */

int
syscm_is_symbol (SCM scm)
{
  return SCM_SMOB_PREDICATE (symbol_smob_tag, scm);
}

/* (symbol? object) -> boolean */

static SCM
gdbscm_symbol_p (SCM scm)
{
  return scm_from_bool (syscm_is_symbol (scm));
}

/* Create a new <gdb:symbol> object that encapsulates SYMBOL.
   The new symbol is registered with the life-cycle chain of the
   associated objfile (if any).  */

SCM
syscm_gsmob_from_symbol (struct symbol *symbol)
{
  SCM s_scm = syscm_make_symbol_smob ();
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (s_scm);

  gdbscm_add_objfile_ref (SYMBOL_SYMTAB (symbol)
			  ? SYMBOL_SYMTAB (symbol)->objfile
			  : NULL,
			  syscm_objfile_data_key, &s_smob->base);
  s_smob->symbol = symbol;

  return s_scm;
}

/* Create a new <gdb:symbol> object that encapsulates SYMBOL.
   The object is passed through *smob->scm*.
   A Scheme exception is thrown if there is an error.  */

SCM
syscm_scm_from_symbol_unsafe (struct symbol *symbol)
{
  /* This doesn't use syscm_gsmob_from_symbol because we don't want to
     cause any side-effects until we know the conversion worked.  */
  SCM s_scm = syscm_make_symbol_smob ();
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (s_scm);
  SCM result;

  result = gdbscm_scm_from_gsmob_unsafe (s_scm);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  gdbscm_add_objfile_ref (SYMBOL_SYMTAB (symbol)
			  ? SYMBOL_SYMTAB (symbol)->objfile
			  : NULL,
			  syscm_objfile_data_key, &s_smob->base);
  s_smob->symbol = symbol;

  return result;
}

/* Returns the <gdb:symbol> object contained in SCM or #f if SCM is not a
   <gdb:symbol> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

SCM
syscm_scm_to_symbol_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, symbol_smob_tag);
}

/* Returns the <gdb:symbol> object in SELF.
   Throws an exception if SELF is not a <gdb:symbol> object
   (after passing it through *scm->smob*).  */

static SCM
syscm_get_symbol_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM s_scm = syscm_scm_to_symbol_gsmob (self);

  if (gdbscm_is_exception (s_scm))
    gdbscm_throw (s_scm);

  SCM_ASSERT_TYPE (syscm_is_symbol (s_scm), self, arg_pos, func_name,
		   symbol_smob_name);

  return s_scm;
}

/* Returns a pointer to the symbol smob of SELF.
   Throws an exception if SELF is not a <gdb:symbol> object
   (after passing it through *scm->smob*).  */

static symbol_smob *
syscm_get_symbol_smob_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM s_scm = syscm_get_symbol_arg_unsafe (self, arg_pos, func_name);
  symbol_smob *s_smob = (symbol_smob *) SCM_SMOB_DATA (s_scm);

  return s_smob;
}

/* Return non-zero if symbol S_SMOB is valid.  */

static int
syscm_is_valid (symbol_smob *s_smob)
{
  return s_smob->symbol != NULL;
}

/* Throw a Scheme error if SELF is not a valid symbol smob.
   Otherwise return a pointer to the symbol smob.  */

static symbol_smob *
syscm_get_valid_symbol_smob_arg_unsafe (SCM self, int arg_pos,
					const char *func_name)
{
  symbol_smob *s_smob =
    syscm_get_symbol_smob_arg_unsafe (self, arg_pos, func_name);

  if (!syscm_is_valid (s_smob))
    {
      gdbscm_invalid_object_error (func_name, arg_pos, self,
				   "invalid <gdb:symbol>");
    }

  return s_smob;
}

/* Throw a Scheme error if SELF is not a valid symbol smob.
   Otherwise return a pointer to the symbol struct.  */

struct symbol *
syscm_get_valid_symbol_arg_unsafe (SCM self, int arg_pos,
				   const char *func_name)
{
  symbol_smob *s_smob = syscm_get_valid_symbol_smob_arg_unsafe (self, arg_pos,
								func_name);

  return s_smob->symbol;
}

/* This function is called when an objfile is about to be freed.
   Invalidate the symbol as further actions on the symbol would result
   in bad data.  All access to s_smob->symbol should be gated by
   syscm_get_valid_symbol_smob_arg which will raise an exception on invalid
   symbols.  */

static void
syscm_del_objfile_symbols (struct objfile *objfile, void *datum)
{
  symbol_smob *s_smob = datum;

  while (s_smob != NULL)
    {
      symbol_smob *next = (symbol_smob *) s_smob->base.next;

      s_smob->symbol = NULL;
      s_smob->base.next = NULL;
      s_smob->base.prev = NULL;

      s_smob = next;
    }
}

/* Symbol methods.  */

/* (symbol-valid? <gdb:symbol>) -> boolean
   Returns #t if SELF still exists in GDB.  */

static SCM
gdbscm_symbol_valid_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);

  return scm_from_bool (syscm_is_valid (s_smob));
}

/* (symbol-type <gdb:symbol>) -> <gdb:type>
   Return the type of SELF, or #f if SELF has no type.  */

static SCM
gdbscm_symbol_type (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  if (SYMBOL_TYPE (symbol) == NULL)
    return SCM_BOOL_F;

  return tyscm_scm_from_type_unsafe (SYMBOL_TYPE (symbol));
}

/* (symbol-symtab <gdb:symbol>) -> <gdb:symtab>
   Return the symbol table of SELF.  */

static SCM
gdbscm_symbol_symtab (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return stscm_scm_from_symtab_unsafe (SYMBOL_SYMTAB (symbol));
}

/* (symbol-name <gdb:symbol>) -> string */

static SCM
gdbscm_symbol_name (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return gdbscm_scm_from_c_string (SYMBOL_NATURAL_NAME (symbol));
}

/* (symbol-linkage-name <gdb:symbol>) -> string */

static SCM
gdbscm_symbol_linkage_name (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return gdbscm_scm_from_c_string (SYMBOL_LINKAGE_NAME (symbol));
}

/* (symbol-print-name <gdb:symbol>) -> string */

static SCM
gdbscm_symbol_print_name (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return gdbscm_scm_from_c_string (SYMBOL_PRINT_NAME (symbol));
}

/* (symbol-addr-class <gdb:symbol>) -> integer */

static SCM
gdbscm_symbol_addr_class (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return scm_from_int (SYMBOL_CLASS (symbol));
}

/* (symbol-argument? <gdb:symbol>) -> boolean */

static SCM
gdbscm_symbol_argument_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return scm_from_bool (SYMBOL_IS_ARGUMENT (symbol));
}

/* (symbol-constant? <gdb:symbol>) -> boolean */

static SCM
gdbscm_symbol_constant_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;
  enum address_class class;

  class = SYMBOL_CLASS (symbol);

  return scm_from_bool (class == LOC_CONST || class == LOC_CONST_BYTES);
}

/* (symbol-function? <gdb:symbol>) -> boolean */

static SCM
gdbscm_symbol_function_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;
  enum address_class class;

  class = SYMBOL_CLASS (symbol);

  return scm_from_bool (class == LOC_BLOCK);
}

/* (symbol-variable? <gdb:symbol>) -> boolean */

static SCM
gdbscm_symbol_variable_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;
  enum address_class class;

  class = SYMBOL_CLASS (symbol);

  return scm_from_bool (!SYMBOL_IS_ARGUMENT (symbol)
			&& (class == LOC_LOCAL || class == LOC_REGISTER
			    || class == LOC_STATIC || class == LOC_COMPUTED
			    || class == LOC_OPTIMIZED_OUT));
}

/* (symbol-needs-frame? <gdb:symbol>) -> boolean
   Return #t if the symbol needs a frame for evaluation.  */

static SCM
gdbscm_symbol_needs_frame_p (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct symbol *symbol = s_smob->symbol;
  volatile struct gdb_exception except;
  int result = 0;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      result = symbol_read_needs_frame (symbol);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return scm_from_bool (result);
}

/* (symbol-line <gdb:symbol>) -> integer
   Return the line number at which the symbol was defined.  */

static SCM
gdbscm_symbol_line (SCM self)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symbol *symbol = s_smob->symbol;

  return scm_from_int (SYMBOL_LINE (symbol));
}

/* (symbol-value <gdb:symbol> [#:frame <gdb:frame>]) -> <gdb:value>
   Return the value of the symbol, or an error in various circumstances.  */

static SCM
gdbscm_symbol_value (SCM self, SCM rest)
{
  symbol_smob *s_smob =
    syscm_get_valid_symbol_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct symbol *symbol = s_smob->symbol;
  SCM keywords[] = { syscm_frame_keyword, SCM_BOOL_F };
  int frame_pos = -1;
  SCM frame_scm = SCM_BOOL_F;
  frame_smob *f_smob = NULL;
  struct frame_info *frame_info = NULL;
  struct value *value = NULL;
  volatile struct gdb_exception except;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG2, keywords, "#O",
			      rest, &frame_pos, &frame_scm);
  if (!gdbscm_is_false (frame_scm))
    f_smob = frscm_get_frame_smob_arg_unsafe (frame_scm, frame_pos, FUNC_NAME);

  if (SYMBOL_CLASS (symbol) == LOC_TYPEDEF)
    {
      gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self,
				 _("Cannot get the value of a typedef"));
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      if (f_smob != NULL)
	{
	  frame_info = frscm_frame_smob_to_frame (f_smob);
	  if (frame_info == NULL)
	    error (_("Invalid frame"));
	}
      
      if (symbol_read_needs_frame (symbol) && frame_info == NULL)
	error (_("Symbol requires a frame to compute its value"));

      value = read_var_value (symbol, frame_info);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return vlscm_scm_from_value_unsafe (value);
}

/* (lookup-symbol name [#:block <gdb:block>] [#:domain domain])
     -> (<gdb:symbol> field-of-this?)
   The result is #f if the symbol is not found.
   See comment in lookup_symbol_in_language for field-of-this?.  */

static SCM
gdbscm_lookup_symbol (SCM name_scm, SCM rest)
{
  char *name;
  SCM keywords[] = { syscm_block_keyword, syscm_domain_keyword, SCM_BOOL_F };
  const struct block *block = NULL;
  SCM block_scm = SCM_BOOL_F;
  int domain = VAR_DOMAIN;
  int block_arg_pos = -1, domain_arg_pos = -1;
  struct field_of_this_result is_a_field_of_this;
  struct symbol *symbol = NULL;
  volatile struct gdb_exception except;
  struct cleanup *cleanups;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, keywords, "s#Oi",
			      name_scm, &name, rest,
			      &block_arg_pos, &block_scm,
			      &domain_arg_pos, &domain);

  cleanups = make_cleanup (xfree, name);

  if (block_arg_pos >= 0)
    {
      SCM except_scm;

      block = bkscm_scm_to_block (block_scm, block_arg_pos, FUNC_NAME,
				  &except_scm);
      if (block == NULL)
	{
	  do_cleanups (cleanups);
	  gdbscm_throw (except_scm);
	}
    }
  else
    {
      struct frame_info *selected_frame;

      TRY_CATCH (except, RETURN_MASK_ALL)
	{
	  selected_frame = get_selected_frame (_("no frame selected"));
	  block = get_frame_block (selected_frame, NULL);
	}
      GDBSCM_HANDLE_GDB_EXCEPTION_WITH_CLEANUPS (except, cleanups);
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      symbol = lookup_symbol (name, block, domain, &is_a_field_of_this);
    }
  do_cleanups (cleanups);
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  if (symbol == NULL)
    return SCM_BOOL_F;

  return scm_list_2 (syscm_scm_from_symbol_unsafe (symbol),
		     scm_from_bool (is_a_field_of_this.type != NULL));
}

/* (lookup-global-symbol name [#:domain domain]) -> <gdb:symbol>
   The result is #f if the symbol is not found.  */

static SCM
gdbscm_lookup_global_symbol (SCM name_scm, SCM rest)
{
  char *name;
  SCM keywords[] = { syscm_domain_keyword, SCM_BOOL_F };
  int domain_arg_pos = -1;
  int domain = VAR_DOMAIN;
  struct symbol *symbol = NULL;
  volatile struct gdb_exception except;
  struct cleanup *cleanups;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, keywords, "s#i",
			      name_scm, &name, rest,
			      &domain_arg_pos, &domain);

  cleanups = make_cleanup (xfree, name);

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      symbol = lookup_symbol_global (name, NULL, domain);
    }
  do_cleanups (cleanups);
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  if (symbol == NULL)
    return SCM_BOOL_F;

  return syscm_scm_from_symbol_unsafe (symbol);
}

/* Initialize the Scheme symbol support.  */

/* Note: The SYMBOL_ prefix on the integer constants here is present for
   compatibility with the Python support.  */

static const scheme_integer_constant symbol_integer_constants[] =
{
  { "SYMBOL_LOC_UNDEF", LOC_UNDEF },
  { "SYMBOL_LOC_CONST", LOC_CONST },
  { "SYMBOL_LOC_STATIC", LOC_STATIC },
  { "SYMBOL_LOC_REGISTER", LOC_REGISTER },
  { "SYMBOL_LOC_ARG", LOC_ARG },
  { "SYMBOL_LOC_REF_ARG", LOC_REF_ARG },
  { "SYMBOL_LOC_LOCAL", LOC_LOCAL },
  { "SYMBOL_LOC_TYPEDEF", LOC_TYPEDEF },
  { "SYMBOL_LOC_LABEL", LOC_LABEL },
  { "SYMBOL_LOC_BLOCK", LOC_BLOCK },
  { "SYMBOL_LOC_CONST_BYTES", LOC_CONST_BYTES },
  { "SYMBOL_LOC_UNRESOLVED", LOC_UNRESOLVED },
  { "SYMBOL_LOC_OPTIMIZED_OUT", LOC_OPTIMIZED_OUT },
  { "SYMBOL_LOC_COMPUTED", LOC_COMPUTED },
  { "SYMBOL_LOC_REGPARM_ADDR", LOC_REGPARM_ADDR },

  { "SYMBOL_UNDEF_DOMAIN", UNDEF_DOMAIN },
  { "SYMBOL_VAR_DOMAIN", VAR_DOMAIN },
  { "SYMBOL_STRUCT_DOMAIN", STRUCT_DOMAIN },
  { "SYMBOL_LABEL_DOMAIN", LABEL_DOMAIN },
  { "SYMBOL_VARIABLES_DOMAIN", VARIABLES_DOMAIN },
  { "SYMBOL_FUNCTIONS_DOMAIN", FUNCTIONS_DOMAIN },
  { "SYMBOL_TYPES_DOMAIN", TYPES_DOMAIN },

  END_INTEGER_CONSTANTS
};

static const scheme_function symbol_functions[] =
{
  { "symbol?", 1, 0, 0, gdbscm_symbol_p,
    "\
Return #t if the object is a <gdb:symbol> object." },

  { "symbol-valid?", 1, 0, 0, gdbscm_symbol_valid_p,
    "\
Return #t if object is a valid <gdb:symbol> object.\n\
A valid symbol is a symbol that has not been freed.\n\
Symbols are freed when the objfile they come from is freed." },

  { "symbol-type", 1, 0, 0, gdbscm_symbol_type,
    "\
Return the type of symbol." },

  { "symbol-symtab", 1, 0, 0, gdbscm_symbol_symtab,
    "\
Return the symbol table (<gdb:symtab>) containing symbol." },

  { "symbol-name", 1, 0, 0, gdbscm_symbol_name,
    "\
Return the name of the symbol as a string." },

  { "symbol-linkage-name", 1, 0, 0, gdbscm_symbol_linkage_name,
    "\
Return the linkage name of the symbol as a string." },

  { "symbol-print-name", 1, 0, 0, gdbscm_symbol_print_name,
    "\
Return the print name of the symbol as a string.\n\
This is either name or linkage-name, depending on whether the user\n\
asked GDB to display demangled or mangled names." },

  { "symbol-addr-class", 1, 0, 0, gdbscm_symbol_addr_class,
    "\
Return the address class of the symbol." },

  { "symbol-argument?", 1, 0, 0, gdbscm_symbol_argument_p,
    "\
Return #t if the symbol is a function argument." },

  { "symbol-constant?", 1, 0, 0, gdbscm_symbol_constant_p,
    "\
Return #t if the symbol is a constant." },

  { "symbol-function?", 1, 0, 0, gdbscm_symbol_function_p,
    "\
Return #t if the symbol is a function." },

  { "symbol-variable?", 1, 0, 0, gdbscm_symbol_variable_p,
    "\
Return #t if the symbol is a variable." },

  { "symbol-needs-frame?", 1, 0, 0, gdbscm_symbol_needs_frame_p,
    "\
Return #t if the symbol needs a frame to compute its value." },

  { "symbol-line", 1, 0, 0, gdbscm_symbol_line,
    "\
Return the line number at which the symbol was defined." },

  { "symbol-value", 1, 0, 1, gdbscm_symbol_value,
    "\
Return the value of the symbol.\n\
\n\
  Arguments: <gdb:symbol> [#:frame frame]" },

  { "lookup-symbol", 1, 0, 1, gdbscm_lookup_symbol,
    "\
Return (<gdb:symbol> field-of-this?) if found, otherwise #f.\n\
\n\
  Arguments: name [#:block block] [#:domain domain]\n\
    name:   a string containing the name of the symbol to lookup\n\
    block:  a <gdb:block> object\n\
    domain: a SYMBOL_*_DOMAIN value" },

  { "lookup-global-symbol", 1, 0, 1, gdbscm_lookup_global_symbol,
    "\
Return <gdb:symbol> if found, otherwise #f.\n\
\n\
  Arguments: name [#:domain domain]\n\
    name:   a string containing the name of the symbol to lookup\n\
    domain: a SYMBOL_*_DOMAIN value" },

  END_FUNCTIONS
};

void
gdbscm_initialize_symbols (void)
{
  symbol_smob_tag =
    gdbscm_make_smob_type (symbol_smob_name, sizeof (symbol_smob));
  scm_set_smob_mark (symbol_smob_tag, syscm_mark_symbol_smob);
  scm_set_smob_free (symbol_smob_tag, syscm_free_symbol_smob);
  scm_set_smob_print (symbol_smob_tag, syscm_print_symbol_smob);
  scm_set_smob_equalp (symbol_smob_tag, syscm_equal_p_symbol_smob);

  gdbscm_define_integer_constants (symbol_integer_constants, 1);
  gdbscm_define_functions (symbol_functions, 1);

  syscm_block_keyword = scm_from_latin1_keyword ("block");
  syscm_domain_keyword = scm_from_latin1_keyword ("domain");
  syscm_frame_keyword = scm_from_latin1_keyword ("frame");

  /* Register an objfile "free" callback so we can properly
     invalidate symbols when an object file is about to be deleted.  */
  syscm_objfile_data_key =
    register_objfile_data_with_cleanup (NULL, syscm_del_objfile_symbols);
}
