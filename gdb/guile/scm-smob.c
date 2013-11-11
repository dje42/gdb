/* GDB/Scheme smobs (gsmob is pronounced "gee smob")

   Copyright (C) 2013 Free Software Foundation, Inc.

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

/* Smobs are Guile's "small object".
   They are used to export C structs to Scheme.

   Note: There's only room in the encoding space for 256, and while we won't
   come close to that, mixed with other libraries maybe someday we could.
   We don't worry about it now, except to be aware of the issue.
   We could allocate just a few smobs and use the unused smob flags field to
   specify the gdb smob kind, that is left for another day if it ever is
   needed.

   We want the objects we export to Scheme to be extensible by the user.
   A gsmob (gdb smob) adds a simple API on top of smobs to support this.
   This allows GDB objects to be easily extendable in a useful manner.
   To that end, all smobs in gdb have gdb_smob as the first member.

   On top of gsmobs there are "chained gsmobs".  They are used to assist with
   life-time tracking of GDB objects vs Scheme objects.  Gsmobs can "subclass"
   chained_gdb_smob, which contains a doubly-linked list to assist with
   life-time tracking.

   Gsmobs (and chained gsmobs) add an extra field that is unused by gdb: "aux".
   It is free to be used by the application as it chooses.

   In addition to the "aux" field, the other way we allow for extending smobs
   is by providing two hooks: *smob->scm, *scm->smob*.  They are either #f
   or a procedure of one argument.
   All Scheme objects coming out of GDB are passed through *smob->scm*
   (if a procedure).  It can either return the original object or return any
   object that contains the original object so that *scm->smob* can get it
   back.
   All Scheme objects passed to GDB are passed through *scm->smob*
   (if a procedure and if necessary).  Its job is to return back the object
   that was passed to *smob->scm*.
   Ironically, the exception is <gdb:exception> smobs, for simplicity in
   the code.  */

#include "defs.h"
#include "hashtab.h"
#include "gdb_assert.h"
#include "objfiles.h"
#include "guile-internal.h"

/* We need to call this.  Undo our hack to prevent others from calling it.  */
#undef scm_make_smob_type

static htab_t registered_gsmobs;

static const char scm_from_smob_name[] = "*smob->scm*";
static const char scm_to_smob_name[] = "*scm->smob*";

static SCM scm_from_smob_var;
static SCM scm_to_smob_var;

/* Hash function for registered_gsmobs hash table.  */

static hashval_t
hash_scm_t_bits (const void *item)
{
  uintptr_t v = (uintptr_t) item;

  return v;
}

/* Equality function for registered_gsmobs hash table.  */

static int
eq_scm_t_bits (const void *item_lhs, const void *item_rhs)
{
  return item_lhs == item_rhs;
}

/* Record GSMOB_CODE as being a gdb smob.
   GSMOB_CODE is the result of scm_make_smob_type.  */

static void
register_gsmob (scm_t_bits gsmob_code)
{
  void **slot;

  slot = htab_find_slot (registered_gsmobs, (void *) gsmob_code, INSERT);
  gdb_assert (*slot == NULL);
  *slot = (void *) gsmob_code;
}

/* Return non-zero if SCM is any registered gdb smob object.  */

static int
gdbscm_is_gsmob (SCM scm)
{
  void **slot;

  if (SCM_IMP (scm))
    return 0;
  slot = htab_find_slot (registered_gsmobs, (void *) SCM_TYP16 (scm),
			 NO_INSERT);
  return slot != NULL;
}

/* Call this to register a smob, instead of scm_make_smob_type.  */

scm_t_bits
gdbscm_make_smob_type (const char *name, size_t size)
{
  scm_t_bits result = scm_make_smob_type (name, size);

  register_gsmob (result);
  return result;
}

/* Initialize a gsmob.  */

void
gdbscm_init_gsmob (gdb_smob *base)
{
  base->aux = SCM_BOOL_F;
}

/* Initialize a chained_gdb_smob.
   This is the same as gdbscm_init_gsmob except that it also sets prev,next
   to NULL.  */

void
gdbscm_init_chained_gsmob (chained_gdb_smob *base)
{
  gdbscm_init_gsmob ((gdb_smob *) base);
  base->prev = NULL;
  base->next = NULL;
}

/* Call this from each smob's "mark" routine.
   In general, this should be called as:
   return gdbscm_mark_gsmob (base);  */

SCM
gdbscm_mark_gsmob (gdb_smob *base)
{
  /* Return the last one to mark as an optimization.
     The marking infrastructure will mark it for us.  */
  return base->aux;
}

/* Call this from each smob's "mark" routine.
   In general, this should be called as:
   return gdbscm_mark_chained_gsmob (base);  */

SCM
gdbscm_mark_chained_gsmob (chained_gdb_smob *base)
{
  /* Return the last one to mark as an optimization.
     The marking infrastructure will mark it for us.  */
  return base->aux;
}

/* Given an SCM that is a gdb smob, call out to Scheme to convert it
   to a possibly new SCM to return to the user.
   The result is the result of calling the conversion function.
   If *scm->smob* is #f then return SMOB unchanged.
   If a Scheme exception was thrown during conversion a <gdb:exception>
   object is returned.  */

SCM
gdbscm_scm_from_gsmob_safe (SCM smob)
{
  SCM proc, result;

  proc = scm_variable_ref (scm_from_smob_var);
  if (gdbscm_is_false (proc))
    return smob;
  /* We could check for whether PROC is a procedure here, but there's
     no real need.  Let the safe call catch this.  */
  result = gdbscm_safe_call_1 (proc, smob, NULL);

  return result;
}

/* Wrapper around gdbscm_scm_from_gsmob_safe that will throw an exception
   if there's a problem during the conversion.  */

SCM
gdbscm_scm_from_gsmob_unsafe (SCM smob)
{
  SCM result = gdbscm_scm_from_gsmob_safe (smob);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  return result;
}

/* Return SCM if it matches TAG, or try to convert it to one.
   If SCM can't be converted to the desired gsmob, and there was no error
   during conversion, return #f.

   TAG is the desired gsmob's tag.  If TAG is zero, then call *scm->smob* and
   replace the smob predicate check with a check for whether the returned
   object is any gsmob.

   This performs the reverse operation that gdbscm_scm_from_gsmob_{,un}safe
   perform.

   The conversion function must return a smob of the original type (prior to
   any *smob->scm* conversion) or #f if the object was not recognized.  Any
   other value is an error.
   If *smob->scm* is #f and SCM doesn't match TAG, return #f.

   If a Scheme exception was thrown during conversion a <gdb:exception>
   object is returned.  */

SCM
gdbscm_scm_to_gsmob_safe (SCM scm, scm_t_bits tag)
{
  SCM proc, result;

  if (tag != 0)
    {
      if (SCM_SMOB_PREDICATE (tag, scm))
	return scm;
    }
  else
    {
      if (gdbscm_is_gsmob (scm))
	return scm;
    }

  proc = scm_variable_ref (scm_to_smob_var);
  if (gdbscm_is_false (proc))
    return SCM_BOOL_F;

  /* We could check for whether PROC is a procedure here, but there's
     no real need.  Let the safe call catch this.  */
  result = gdbscm_safe_call_1 (proc, scm, NULL);

  if (gdbscm_is_false (result))
    return SCM_BOOL_F;
  if (gdbscm_is_exception (result))
    return result;
  if (tag != 0)
    {
      if (SCM_SMOB_PREDICATE (tag, result))
	return result;
    }
  else
    {
      if (gdbscm_is_gsmob (result))
	return result;
    }
  return gdbscm_make_out_of_range_error (NULL, 0, result,
		_("Result of *scm->smob* must be requested gsmob or #f"));
}

/* Wrapper around gdbscm_scm_to_smob_safe that will throw an exception
   if there's a problem during the conversion.  */

SCM
gdbscm_scm_to_gsmob_unsafe (SCM scm, scm_t_bits tag)
{
  SCM result = gdbscm_scm_to_gsmob_safe (scm, tag);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  return result;
}

/* gsmob accessors */

/* Return the gsmob in SELF, passing through *scm->smob* if necessary.
   Throws an exception if an error occurs during the conversion.  */

static SCM
gsscm_get_gsmob_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM gsmob = gdbscm_scm_to_gsmob_safe (self, 0);

  if (gdbscm_is_exception (gsmob))
    gdbscm_throw (gsmob);

  SCM_ASSERT_TYPE (gdbscm_is_gsmob (gsmob), self, arg_pos, func_name,
		   "any gdb smob");

  return gsmob;
}

/* (gsmob-kind gsmob) -> symbol

   Note: While one might want to name this gsmob-class-name, it is named
   "-kind" because smobs aren't real GOOPS classes.  */

static SCM
gdbscm_gsmob_kind (SCM self)
{
  SCM smob, result;
  scm_t_bits smobnum;
  const char *name;
  char *kind;

  smob = gsscm_get_gsmob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);

  smobnum = SCM_SMOBNUM (smob);
  name = SCM_SMOBNAME (smobnum);
  kind = xstrprintf ("<%s>", name);
  result = gdbscm_symbol_from_c_string (kind);
  xfree (kind);

  return result;
}

/* (gsmob-aux gsmob) -> object */

static SCM
gdbscm_gsmob_aux (SCM self)
{
  SCM smob;
  gdb_smob *base;

  smob = gsscm_get_gsmob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  base = (gdb_smob *) SCM_SMOB_DATA (self);

  return base->aux;
}

/* (set-gsmob-aux! gsmob object) -> unspecified */

static SCM
gdbscm_set_gsmob_aux_x (SCM self, SCM aux)
{
  SCM smob;
  gdb_smob *base;

  smob = gsscm_get_gsmob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  base = (gdb_smob *) SCM_SMOB_DATA (self);
  base->aux = aux;

  return SCM_UNSPECIFIED;
}

/* When underlying gdb data structures are deleted, we need to update any
   smobs with references to them.  There are several smobs that reference
   objfile-based data, so we provide helpers to manage this.  */

/* Add G_SMOB to the reference chain for OBJFILE specified by DATA_KEY.
   OBJFILE may be NULL, in which case just set prev,next to NULL.  */

void
gdbscm_add_objfile_ref (struct objfile *objfile,
			const struct objfile_data *data_key,
			chained_gdb_smob *g_smob)
{
  g_smob->prev = NULL;
  if (objfile != NULL)
    {
      g_smob->next = objfile_data (objfile, data_key);
      if (g_smob->next)
	g_smob->next->prev = g_smob;
      set_objfile_data (objfile, data_key, g_smob);
    }
  else
    g_smob->next = NULL;
}

/* Remove G_SMOB from the reference chain for OBJFILE specified
   by DATA_KEY.  OBJFILE may be NULL.  */

void
gdbscm_remove_objfile_ref (struct objfile *objfile,
			   const struct objfile_data *data_key,
			   chained_gdb_smob *g_smob)
{
  if (g_smob->prev)
    g_smob->prev->next = g_smob->next;
  else if (objfile != NULL)
    set_objfile_data (objfile, data_key, g_smob->next);
  if (g_smob->next)
    g_smob->next->prev = g_smob->prev;
}

/* Create a hash table for mapping a pointer to a gdb data structure to the
   gsmob that wraps it.  */

htab_t
gdbscm_create_gsmob_ptr_map (htab_hash hash_fn, htab_eq eq_fn)
{
  htab_t htab = htab_create_alloc (7, hash_fn, eq_fn,
				   NULL, xcalloc, xfree);

  return htab;
}

/* Return a pointer to the htab entry for the gsmob wrapping PTR.
   If INSERT is non-zero, create an entry if one doesn't exist.
   Otherwise NULL is returned if the entry is not found.  */

void **
gdbscm_find_gsmob_ptr_slot (htab_t htab, void *ptr, int insert)
{
  void **slot = htab_find_slot (htab, ptr, insert ? INSERT : NO_INSERT);

  return slot;
}

/* Remove PTR from HTAB.
   PTR is a pointer to a gsmob that wraps a pointer to a GDB datum.
   This is used, for example, when an object is freed.

   It is an error to call this if PTR is not in HTAB (only because it allows
   for some consistency checking).  */

void
gdbscm_clear_gsmob_ptr_slot (htab_t htab, void *ptr)
{
  void **slot = htab_find_slot (htab, ptr, NO_INSERT);

  gdb_assert (slot != NULL);
  htab_clear_slot (htab, slot);
}

/* Initialize the Scheme gsmobs code.  */

static const scheme_variable gsmob_variables[] =
{
  { scm_from_smob_name, SCM_BOOL_F,
    /* Doc strings don't work as well for variables, maybe some day.  */
    "\
Either #f or a procedure called when creating a GDB smob.\n\
The procedure takes one parameter, the smob, and typically returns\n\
a modified representation of the object." },

  { scm_to_smob_name, SCM_BOOL_F,
    /* Doc strings don't work as well for variables, maybe some day.  */
    "\
Either #f or a procedure called when passing an object to GDB.\n\
The procedure takes two parameters, the object and an object representing\n\
the desired smob's class.  It must return an object of the specified smob\n\
class.  The procedure is intended to undo the transformation that\n\
*smob->scm* does." },

  END_VARIABLES
};

static const scheme_function gsmob_functions[] =
{
  { "gsmob-kind", 1, 0, 0, gdbscm_gsmob_kind,
    "\
Return the kind of the smob, e.g., <gdb:breakpoint>, as a symbol." },

  { "gsmob-aux", 1, 0, 0, gdbscm_gsmob_aux,
    "\
Return the \"aux\" member of the object." },

  { "set-gsmob-aux!", 2, 0, 0, gdbscm_set_gsmob_aux_x,
    "\
Set the \"aux\" member of any GDB smob.\n\
The \"aux\" member is not used by GDB, the application is free to use it." },

  END_FUNCTIONS
};

void
gdbscm_initialize_smobs (void)
{
  registered_gsmobs = htab_create_alloc (10,
					 hash_scm_t_bits, eq_scm_t_bits,
					 NULL, xcalloc, xfree);

  gdbscm_define_variables (gsmob_variables, 0);
  gdbscm_define_functions (gsmob_functions, 1);

  scm_from_smob_var = scm_c_private_variable (gdbscm_module_name,
					      scm_from_smob_name);
  scm_to_smob_var = scm_c_private_variable (gdbscm_module_name,
					    scm_to_smob_name);
}
