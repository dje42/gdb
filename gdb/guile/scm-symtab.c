/* Scheme interface to symbol tables.

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
#include "symtab.h"
#include "source.h"
#include "objfiles.h"
#include "block.h"
#include "guile-internal.h"

/* A <gdb:symtab> smob.  */

typedef struct {
  /* This always appears first.
     A symtab object is associated with an objfile, so use a chained_gdb_smob
     to keep track of all symtabs associated with the objfile.  This lets us
     invalidate the underlying struct symtab when the objfile is deleted.  */
  chained_gdb_smob base;

  /* The GDB symbol table structure.
     If this is NULL the symtab is invalid.  This can happen when the
     underlying objfile is freed.  */
  struct symtab *symtab;
} symtab_smob;

/* A <gdb:sal> smob.
   A smob describing a gdb symtab-and-line object.
   A sal is associated with an objfile.  All access must be gated by checking
   the validity of symtab_scm.  */

typedef struct {
  /* This always appears first.  */
  gdb_smob base;

  /* The <gdb:symtab> object of the symtab.
     We store this instead of a pointer to the symtab_smob because it's not
     clear GC will know the symtab_smob is referenced by us otherwise.  */
  SCM symtab_scm;

  /* The result of passing symtab_scm through *smob->scm*.
     This is what we hand back to the user.
     To simplify the code, this is computed lazily
     (stscm_scm_from_sal_unsafe only has to worry about one source of
     exceptions).  */
  SCM converted_symtab_scm;

  /* The GDB symbol table and line structure.
     This object is ephemeral in GDB, so keep our own copy.
     The symtab pointer in this struct is not usable: If the symtab is deleted
     this pointer will not be updated.  Use symtab_scm instead.  */
  struct symtab_and_line sal;
} sal_smob;

static const char symtab_smob_name[] = "gdb:symtab";
/* TODO: "symtab-and-line" is pretty long, and "sal" is short and unique
   enough.  Keep sal, or use long name?  */
static const char sal_smob_name[] = "gdb:sal";

/* The tags Guile knows the symbol table smobs by.  */
static scm_t_bits symtab_smob_tag;
static scm_t_bits sal_smob_tag;

static const struct objfile_data *stscm_objfile_data_key;

/* Administrivia for symtab smobs.  */

/* The smob "mark" function for <gdb:symtab>.  */

static SCM
stscm_mark_symtab_smob (SCM self)
{
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (self);

  /* Do this last.  */
  return gdbscm_mark_chained_gsmob (&st_smob->base);
}

/* The smob "free" function for <gdb:symtab>.  */

static size_t
stscm_free_symtab_smob (SCM self)
{
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (self);

  gdbscm_remove_objfile_ref (st_smob->symtab
			     ? st_smob->symtab->objfile
			     : NULL,
			     stscm_objfile_data_key,
			     &st_smob->base);
  /* Not necessary, done to catch bugs.  */
  st_smob->symtab = NULL;

  return 0;
}

/* The smob "print" function for <gdb:symtab>.  */

static int
stscm_print_symtab_smob (SCM self, SCM port, scm_print_state *pstate)
{
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (self);

  gdbscm_printf (port, "#<%s ", symtab_smob_name);
  gdbscm_printf (port, "%s",
		 st_smob->symtab != NULL
		 ? symtab_to_filename_for_display (st_smob->symtab)
		 : "<invalid>");
  scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* Low level routine to create a <gdb:symtab> object.  */

static SCM
stscm_make_symtab_smob (void)
{
  symtab_smob *st_smob = (symtab_smob *)
    scm_gc_malloc (sizeof (symtab_smob), symtab_smob_name);
  SCM st_scm;

  st_smob->symtab = NULL;
  st_scm = scm_new_smob (symtab_smob_tag, (scm_t_bits) st_smob);
  gdbscm_init_chained_gsmob (&st_smob->base);

  return st_scm;
}

/* Return non-zero if SCM is a symbol table smob.  */

static int
stscm_is_symtab (SCM scm)
{
  return SCM_SMOB_PREDICATE (symtab_smob_tag, scm);
}

/* (symtab? object) -> boolean */

static SCM
gdbscm_symtab_p (SCM scm)
{
  return scm_from_bool (stscm_is_symtab (scm));
}

/* Create a new <gdb:symtab> object that encapsulates SYMTAB.  */

SCM
stscm_gsmob_from_symtab (struct symtab *symtab)
{
  SCM st_scm = stscm_make_symtab_smob ();
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (st_scm);

  gdbscm_add_objfile_ref (symtab->objfile, stscm_objfile_data_key,
			  &st_smob->base);
  st_smob->symtab = symtab;

  return st_scm;
}

/* Create a new <gdb:symtab> object that encapsulates SYMTAB.
   The object is passed through *smob->scm*.
   A Scheme exception is thrown if there is an error.  */

SCM
stscm_scm_from_symtab_unsafe (struct symtab *symtab)
{
  /* This doesn't use stscm_gsmob_from_symtab because we don't want to
     cause any side-effects until we know the conversion worked.  */
  SCM st_scm = stscm_make_symtab_smob ();
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (st_scm);
  SCM result;

 /* Set this before calling out to Scheme to perform any conversion so
     that the conversion routine can see the symtab.  */
  st_smob->symtab = symtab;

  result = gdbscm_scm_from_gsmob_unsafe (st_scm);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  gdbscm_add_objfile_ref (symtab->objfile, stscm_objfile_data_key,
			  &st_smob->base);
 
  return result;
}

/* Returns the <gdb:symtab> object in SCM or #f if SCM is not a
   <gdb:symtab> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

static SCM
stscm_scm_to_symtab_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, symtab_smob_tag);
}

/* Returns the <gdb:symtab> object in SELF.
   Throws an exception if SELF is not a <gdb:symtab> object
   (after passing it through *scm->smob*).  */

static SCM
stscm_get_symtab_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM st_scm = stscm_scm_to_symtab_gsmob (self);

  if (gdbscm_is_exception (st_scm))
    gdbscm_throw (st_scm);

  SCM_ASSERT_TYPE (stscm_is_symtab (st_scm), self, arg_pos, func_name,
		   symtab_smob_name);

  return st_scm;
}

/* Returns a pointer to the symtab smob of SELF.
   Throws an exception if SELF is not a <gdb:symtab> object
   (after passing it through *scm->smob*).  */

static symtab_smob *
stscm_get_symtab_smob_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM st_scm = stscm_get_symtab_arg_unsafe (self, arg_pos, func_name);
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (st_scm);

  return st_smob;
}

/* Return non-zero if symtab ST_SMOB is valid.  */

static int
stscm_is_valid (symtab_smob *st_smob)
{
  return st_smob->symtab != NULL;
}

/* Throw a Scheme error if SELF is not a valid symtab smob.
   Otherwise return a pointer to the symtab_smob object.  */

static symtab_smob *
stscm_get_valid_symtab_smob_arg_unsafe (SCM self, int arg_pos,
					const char *func_name)
{
  symtab_smob *st_smob =
    stscm_get_symtab_smob_arg_unsafe (self, arg_pos, func_name);

  if (!stscm_is_valid (st_smob))
    {
      gdbscm_invalid_object_error (func_name, arg_pos, self,
				   "invalid <gdb:symtab>");
    }

  return st_smob;
}

/* This function is called when an objfile is about to be freed.
   Invalidate the symbol table as further actions on the symbol table
   would result in bad data.  All access to st_smob->symtab should be
   gated by stscm_get_valid_symtab_smob_arg_unsafe which will raise an
   exception on invalid symbol tables.  */

static void
stscm_del_objfile_symtab (struct objfile *objfile, void *datum)
{
  symtab_smob *st_smob = datum;

  while (st_smob)
    {
      symtab_smob *next = (symtab_smob *) st_smob->base.next;

      st_smob->symtab = NULL;
      st_smob->base.next = NULL;
      st_smob->base.prev = NULL;

      st_smob = next;
    }
}

/* Symbol table methods.  */

/* (symtab-valid? <gdb:symtab>) -> boolean
   Returns #t if SELF still exists in GDB.  */

static SCM
gdbscm_symtab_valid_p (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);

  return scm_from_bool (stscm_is_valid (st_smob));
}

/* (symtab-filename <gdb:symtab>) -> string */

static SCM
gdbscm_symtab_filename (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_valid_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct symtab *symtab = st_smob->symtab;

  return gdbscm_scm_from_c_string (symtab_to_filename_for_display (symtab));
}

/* (symtab-fullname <gdb:symtab>) -> string */

static SCM
gdbscm_symtab_fullname (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_valid_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct symtab *symtab = st_smob->symtab;

  return gdbscm_scm_from_c_string (symtab_to_fullname (symtab));
}

/* (symtab-objfile <gdb:symtab>) -> <gdb:objfile> */

static SCM
gdbscm_symtab_objfile (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_valid_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symtab *symtab = st_smob->symtab;

  return ofscm_scm_from_objfile_unsafe (symtab->objfile);
}

/* (symtab-global-block <gdb:symtab>) -> <gdb:block>
   Return the GLOBAL_BLOCK of the underlying symtab.  */

static SCM
gdbscm_symtab_global_block (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_valid_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symtab *symtab = st_smob->symtab;
  const struct blockvector *blockvector;
  const struct block *block;

  blockvector = BLOCKVECTOR (symtab);
  block = BLOCKVECTOR_BLOCK (blockvector, GLOBAL_BLOCK);

  return bkscm_scm_from_block_unsafe (block, symtab->objfile);
}

/* (symtab-static-block <gdb:symtab>) -> <gdb:block>
   Return the STATIC_BLOCK of the underlying symtab.  */

static SCM
gdbscm_symtab_static_block (SCM self)
{
  symtab_smob *st_smob =
    stscm_get_valid_symtab_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  const struct symtab *symtab = st_smob->symtab;
  const struct blockvector *blockvector;
  const struct block *block;

  blockvector = BLOCKVECTOR (symtab);
  block = BLOCKVECTOR_BLOCK (blockvector, STATIC_BLOCK);

  return bkscm_scm_from_block_unsafe (block, symtab->objfile);
}

/* Administrivia for sal (symtab-and-line) smobs.  */

/* The smob "mark" function for <gdb:sal>.  */

static SCM
stscm_mark_sal_smob (SCM self)
{
  sal_smob *s_smob = (sal_smob *) SCM_SMOB_DATA (self);

  scm_gc_mark (s_smob->symtab_scm);
  scm_gc_mark (s_smob->converted_symtab_scm);
  /* Do this last.  */
  return gdbscm_mark_gsmob (&s_smob->base);
}

/* The smob "free" function for <gdb:sal>.  */

static size_t
stscm_free_sal_smob (SCM self)
{
  sal_smob *s_smob = (sal_smob *) SCM_SMOB_DATA (self);

  /* Not necessary, done to catch bugs.  */
  s_smob->symtab_scm = SCM_BOOL_F;
  s_smob->converted_symtab_scm = SCM_BOOL_F;

  return 0;
}

/* The smob "print" function for <gdb:sal>.  */

static int
stscm_print_sal_smob (SCM self, SCM port, scm_print_state *pstate)
{
  sal_smob *s_smob = (sal_smob *) SCM_SMOB_DATA (self);
  symtab_smob *st_smob = (symtab_smob *) SCM_SMOB_DATA (s_smob->symtab_scm);

  gdbscm_printf (port, "#<%s ", symtab_smob_name);
  scm_write (s_smob->symtab_scm, port);
  if (s_smob->sal.line != 0)
    gdbscm_printf (port, " line %d", s_smob->sal.line);
  scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* Low level routine to create a <gdb:sal> object.  */

static SCM
stscm_make_sal_smob (void)
{
  sal_smob *s_smob =
    (sal_smob *) scm_gc_malloc (sizeof (sal_smob), sal_smob_name);
  SCM s_scm;

  s_smob->symtab_scm = SCM_BOOL_F;
  s_smob->converted_symtab_scm = SCM_BOOL_F;
  memset (&s_smob->sal, 0, sizeof (s_smob->sal));
  s_scm = scm_new_smob (sal_smob_tag, (scm_t_bits) s_smob);
  gdbscm_init_gsmob (&s_smob->base);

  return s_scm;
}

/* Return non-zero if SCM is a <gdb:sal> object.  */

static int
stscm_is_sal (SCM scm)
{
  return SCM_SMOB_PREDICATE (sal_smob_tag, scm);
}

/* (sal? object) -> boolean */

static SCM
gdbscm_sal_p (SCM scm)
{
  return scm_from_bool (stscm_is_sal (scm));
}

/* Create a new <gdb:sal> object that encapsulates SAL.  */

SCM
stscm_gsmob_from_sal (struct symtab_and_line sal)
{
  SCM s_scm;
  sal_smob *s_smob;

  s_scm = stscm_make_sal_smob ();
  s_smob = (sal_smob *) SCM_SMOB_DATA (s_scm);
  if (sal.symtab != NULL)
    s_smob->symtab_scm = stscm_gsmob_from_symtab (sal.symtab);
  /* s_smob->converted_symtab_scm is computed lazily.  */
  s_smob->sal = sal;

  return s_scm;
}

/* Create a new <gdb:sal> object that encapsulates SAL.
   The object is passed through *smob->scm*.
   A Scheme exception is thrown if there is an error.  */

SCM
stscm_scm_from_sal_unsafe (struct symtab_and_line sal)
{
  SCM s_scm = stscm_gsmob_from_sal (sal);
  SCM result = gdbscm_scm_from_gsmob_safe (s_scm);

  /* We have to be careful here.
     If there's an error during the *smob->scm* conversion for the sal,
     we need to unlink the symtab from its list.  */

  if (gdbscm_is_exception (result))
    {
      sal_smob *s_smob = (sal_smob *) SCM_SMOB_DATA (s_scm);

      /* Unlink the symtab from its list.  */
      if (!gdbscm_is_false (s_smob->symtab_scm))
	stscm_free_symtab_smob (s_smob->symtab_scm);

      gdbscm_throw (result);
    }

  return result;
}

/* Returns the <gdb:sal> object in SCM or #f if SCM is not a <gdb:sal> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

static SCM
stscm_scm_to_sal_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, sal_smob_tag);
}

/* Returns the <gdb:sal> object in SELF.
   Throws an exception if SELF is not a <gdb:sal> object
   (after passing it through *scm->smob*).  */

static SCM
stscm_get_sal_arg (SCM self, int arg_pos, const char *func_name)
{
  SCM s_scm = stscm_scm_to_sal_gsmob (self);

  if (gdbscm_is_exception (s_scm))
    gdbscm_throw (s_scm);

  SCM_ASSERT_TYPE (stscm_is_sal (s_scm), self, arg_pos, func_name,
		   sal_smob_name);

  return s_scm;
}

/* Returns a pointer to the sal smob of SELF.
   Throws an exception if SELF is not a <gdb:sal> object
   (after passing it through *scm->smob*).  */

static sal_smob *
stscm_get_sal_smob_arg (SCM self, int arg_pos, const char *func_name)
{
  SCM s_scm = stscm_get_sal_arg (self, arg_pos, func_name);
  sal_smob *s_smob = (sal_smob *) SCM_SMOB_DATA (s_scm);

  return s_smob;
}

/* Return non-zero if the symtab in S_SMOB is valid.  */

static int
stscm_sal_is_valid (sal_smob *s_smob)
{
  symtab_smob *st_smob;

  /* If there's no symtab that's ok, the sal is still valid.  */
  if (gdbscm_is_false (s_smob->symtab_scm))
    return 1;

  st_smob = (symtab_smob *) SCM_SMOB_DATA (s_smob->symtab_scm);

  return st_smob->symtab != NULL;
}

/* Throw a Scheme error if SELF is not a valid sal smob.
   Otherwise return a pointer to the sal_smob object.  */

static sal_smob *
stscm_get_valid_sal_smob_arg (SCM self, int arg_pos, const char *func_name)
{
  sal_smob *s_smob = stscm_get_sal_smob_arg (self, arg_pos, func_name);

  if (!stscm_sal_is_valid (s_smob))
    {
      gdbscm_invalid_object_error (func_name, arg_pos, self,
				   "invalid <gdb:sal>");
    }

  return s_smob;
}

/* sal methods */

/* (sal-valid? <gdb:sal>) -> boolean
   Returns #t if the symtab for SELF still exists in GDB.  */

static SCM
gdbscm_sal_valid_p (SCM self)
{
  sal_smob *s_smob = stscm_get_sal_smob_arg (self, SCM_ARG1, FUNC_NAME);

  return scm_from_bool (stscm_sal_is_valid (s_smob));
}

/* (sal-pc <gdb:sal>) -> address */

static SCM
gdbscm_sal_pc (SCM self)
{
  sal_smob *s_smob = stscm_get_valid_sal_smob_arg (self, SCM_ARG1, FUNC_NAME);
  const struct symtab_and_line *sal = &s_smob->sal;

  return gdbscm_scm_from_ulongest (sal->pc);
}

/* (sal-last <gdb:sal>) -> address
   Returns #f if no ending address is recorded.  */

static SCM
gdbscm_sal_last (SCM self)
{
  sal_smob *s_smob = stscm_get_valid_sal_smob_arg (self, SCM_ARG1, FUNC_NAME);
  const struct symtab_and_line *sal = &s_smob->sal;

  if (sal->end > 0)
    return gdbscm_scm_from_ulongest (sal->end - 1);
  return SCM_BOOL_F;
}

/* (sal-line <gdb:sal>) -> integer
   Returns #f if no line number is recorded.  */

static SCM
gdbscm_sal_line (SCM self)
{
  sal_smob *s_smob = stscm_get_valid_sal_smob_arg (self, SCM_ARG1, FUNC_NAME);
  const struct symtab_and_line *sal = &s_smob->sal;

  if (sal->line > 0)
    return scm_from_int (sal->line);
  return SCM_BOOL_F;
}

/* (sal-symtab <gdb:sal>) -> <gdb:symtab>
   Returns #f if no symtab is recorded.  */

static SCM
gdbscm_sal_symtab (SCM self)
{
  sal_smob *s_smob = stscm_get_valid_sal_smob_arg (self, SCM_ARG1, FUNC_NAME);
  const struct symtab_and_line *sal = &s_smob->sal;

  return s_smob->symtab_scm;
}

/* (find-pc-line address) -> <gdb:sal> */

static SCM
gdbscm_find_pc_line (SCM pc_scm)
{
  ULONGEST pc_ull;
  SCM result = SCM_BOOL_F; /* -Wall */
  struct symtab_and_line sal;
  volatile struct gdb_exception except;

  init_sal (&sal); /* -Wall */

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, NULL, "U", pc_scm, &pc_ull);

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      CORE_ADDR pc = (CORE_ADDR) pc_ull;

      sal = find_pc_line (pc, 0);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  result = stscm_scm_from_sal_unsafe (sal);

  return result;
}

/* Initialize the Scheme symbol support.  */

static const scheme_function symtab_functions[] =
{
  { "symtab?", 1, 0, 0, gdbscm_symtab_p,
    "\
Return #t if the object is a <gdb:symtab> object." },

  { "symtab-valid?", 1, 0, 0, gdbscm_symtab_valid_p,
    "\
Return #t if the symtab still exists in GDB.\n\
Symtabs are deleted when the corresponding objfile is freed." },

  { "symtab-filename", 1, 0, 0, gdbscm_symtab_filename,
    "\
Return the symtab's source file name." },

  { "symtab-fullname", 1, 0, 0, gdbscm_symtab_fullname,
    "\
Return the symtab's full source file name." },

  { "symtab-objfile", 1, 0, 0, gdbscm_symtab_objfile,
    "\
Return the symtab's objfile." },

  { "symtab-global-block", 1, 0, 0, gdbscm_symtab_global_block,
    "\
Return the symtab's global block." },

  { "symtab-static-block", 1, 0, 0, gdbscm_symtab_static_block,
    "\
Return the symtab's static block." },

  { "sal?", 1, 0, 0, gdbscm_sal_p,
    "\
Return #t if the object is a <gdb:sal> (symtab-and-line) object." },

  { "sal-valid?", 1, 0, 0, gdbscm_sal_valid_p,
    "\
Return #t if the symtab for the sal still exists in GDB.\n\
Symtabs are deleted when the corresponding objfile is freed." },

  { "sal-pc", 1, 0, 0, gdbscm_sal_pc,
    "\
Return the sal's address." },

  { "sal-last", 1, 0, 0, gdbscm_sal_last,
    "\
Return the last address specified by the sal, or #f if there is none." },

  { "sal-line", 1, 0, 0, gdbscm_sal_line,
    "\
Return the sal's line number, or #f if there is none." },

  { "sal-symtab", 1, 0, 0, gdbscm_sal_symtab,
    "\
Return the sal's symtab." },

  { "find-pc-line", 1, 0, 0, gdbscm_find_pc_line,
    "\
Return the sal corresponding to the address, or #f if there isn't one.\n\
\n\
  Arguments: address" },

  END_FUNCTIONS
};

void
gdbscm_initialize_symtabs (void)
{
  symtab_smob_tag =
    gdbscm_make_smob_type (symtab_smob_name, sizeof (symtab_smob));
  scm_set_smob_mark (symtab_smob_tag, stscm_mark_symtab_smob);
  scm_set_smob_free (symtab_smob_tag, stscm_free_symtab_smob);
  scm_set_smob_print (symtab_smob_tag, stscm_print_symtab_smob);

  sal_smob_tag = gdbscm_make_smob_type (sal_smob_name, sizeof (sal_smob));
  scm_set_smob_mark (sal_smob_tag, stscm_mark_sal_smob);
  scm_set_smob_free (sal_smob_tag, stscm_free_sal_smob);
  scm_set_smob_print (sal_smob_tag, stscm_print_sal_smob);

  gdbscm_define_functions (symtab_functions, 1);

  /* Register an objfile "free" callback so we can properly
     invalidate symbol tables, and symbol table and line data
     structures when an object file that is about to be deleted.  */
  stscm_objfile_data_key =
    register_objfile_data_with_cleanup (NULL, stscm_del_objfile_symtab);
}
