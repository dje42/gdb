/* Scheme interface to types.

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
#include "arch-utils.h"
#include "value.h"
#include "exceptions.h"
#include "gdbtypes.h"
#include "objfiles.h"
#include "language.h"
#include "vec.h"
#include "bcache.h"
#include "dwarf2loc.h"
#include "typeprint.h"
#include "guile-internal.h"

/* The <gdb:type> smob.
   The type is chained with all types associated with its objfile, if any.
   This lets us copy the underlying struct type when the objfile is
   deleted.  */

typedef struct _type_smob
{
  /* This always appears first.  */
  chained_gdb_smob base;

  /* The GDB type structure this smob is wrapping.  */
  struct type *type;
} type_smob;

/* A field smob.  */

typedef struct
{
  gdb_smob base;

  /* Backlink to the containing <gdb:type> object.  */
  SCM type_scm;

  /* The field number in TYPE_SCM.  */
  int field_num;

  /* The result of passing type_scm through *smob->scm*.
     This is what we hand back to the user.
     To simplify the code, this is computed lazily
     (tyscm_scm_from_field_unsafe only has to worry about one source of
     exceptions).  */
  SCM converted_type_scm;
} field_smob;

static const char type_smob_name[] = "gdb:type";
static const char field_smob_name[] = "gdb:field";

static const char not_composite_error[] =
  "Type is not a structure, union, or enum type";

/* The tag Guile knows the type smob by.  */
static scm_t_bits type_smob_tag;

/* The tag Guile knows the field smob by.  */
static scm_t_bits field_smob_tag;

/* The "next" procedure for field iterators.  */
static SCM tyscm_next_field_x_scm;

/* Keywords used in argument passing.  */
static SCM tyscm_block_keyword;

static const struct objfile_data *tyscm_objfile_data_key;

static struct type *tyscm_get_composite (struct type *type);

static SCM gdbscm_type_equal_p (SCM type1_scm, SCM type2_scm);

/* Return the type field of T_SMOB.
   This exists so that we don't have to export the struct's contents.  */

struct type *
tyscm_type_smob_type (type_smob *t_smob)
{
  return t_smob->type;
}

/* Return the name of TYPE in expanded form.
   Space for the result is malloc'd, caller must free.
   If there's an error computing the name, the result is NULL and the
   exception is stored in *EXCP.  */

static char *
tyscm_type_name (struct type *type, SCM *excp)
{
  char *name = NULL;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      struct cleanup *old_chain;
      struct ui_file *stb;

      stb = mem_fileopen ();
      old_chain = make_cleanup_ui_file_delete (stb);

      LA_PRINT_TYPE (type, "", stb, -1, 0, &type_print_raw_options);

      name = ui_file_xstrdup (stb, NULL);
      do_cleanups (old_chain);
    }
  if (except.reason < 0)
    {
      *excp = gdbscm_scm_from_gdb_exception (except);
      return NULL;
    }

  return name;
}

/* Administrivia for type smobs.  */

/* The smob "mark" function for <gdb:type>.  */

static SCM
tyscm_mark_type_smob (SCM self)
{
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (self);

  /* Do this last.  */
  return gdbscm_mark_chained_gsmob (&t_smob->base);
}

/* The smob "free" function for <gdb:type>.  */

static size_t
tyscm_free_type_smob (SCM self)
{
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (self);

  gdbscm_remove_objfile_ref (t_smob->type != NULL
			     ? TYPE_OBJFILE (t_smob->type)
			     : NULL,
			     tyscm_objfile_data_key,
			     &t_smob->base);
  /* Not necessary, done to catch bugs.  */
  t_smob->type = NULL;

  return 0;
}

/* The smob "print" function for <gdb:type>.  */

static int
tyscm_print_type_smob (SCM self, SCM port, scm_print_state *pstate)
{
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (self);
  SCM exception;
  char *name = tyscm_type_name (t_smob->type, &exception);

  if (name == NULL)
    gdbscm_throw (exception);

  /* pstate->writingp = zero if invoked by display/~A, and nonzero if
     invoked by write/~S.  What to do here may need to evolve.
     IWBN if we could pass an argument to format that would we could use
     instead of writingp.  */
  if (pstate->writingp)
    gdbscm_printf (port, "#<%s ", type_smob_name);

  scm_puts (name, port);

  if (pstate->writingp)
    scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* The smob "type" function for <gdb:type>.  */

static SCM
tyscm_equal_p_type_smob (SCM t1, SCM t2)
{
  return gdbscm_type_equal_p (t1, t2);
}

/* Low level routine to create a <gdb:type> object.  */

static SCM
tyscm_make_type_smob (void)
{
  type_smob *t_smob = (type_smob *)
    scm_gc_malloc (sizeof (type_smob), type_smob_name);
  SCM t_scm;

  /* This must be filled in by the caller.  */
  t_smob->type = NULL;

  t_scm = scm_new_smob (type_smob_tag, (scm_t_bits) t_smob);
  gdbscm_init_chained_gsmob (&t_smob->base);

  return t_scm;
}

/* Return non-zero if SCM is a <gdb:type> object.  */

int
tyscm_is_type (SCM self)
{
  return SCM_SMOB_PREDICATE (type_smob_tag, self);
}

/* (type? object) -> boolean */

static SCM
gdbscm_type_p (SCM self)
{
  return scm_from_bool (tyscm_is_type (self));
}

/* Create a new <gdb:type> object that encapsulates TYPE.  */

SCM
tyscm_gsmob_from_type (struct type *type)
{
  SCM t_scm = tyscm_make_type_smob ();
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (t_scm);

  t_smob->type = type;
  gdbscm_add_objfile_ref (type != NULL ? TYPE_OBJFILE (type) : NULL,
			  tyscm_objfile_data_key, &t_smob->base);

  return t_scm;
}

/* Create a new <gdb:type> object that encapsulates TYPE.
   The object is passed through *smob->scm*.
   A Scheme exception is thrown if there is an error.  */

SCM
tyscm_scm_from_type_unsafe (struct type *type)
{
  /* This doesn't use tyscm_gsmob_from_type because we don't want to
     cause any side-effects until we know the conversion worked.  */
  SCM t_scm = tyscm_make_type_smob ();
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (t_scm);
  SCM result;

  /* Set this before calling out to Scheme to perform any conversion so
     that the conversion routine can see the type.  */
  t_smob->type = type;

  result = gdbscm_scm_from_gsmob_unsafe (t_scm);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  gdbscm_add_objfile_ref (type != NULL ? TYPE_OBJFILE (type) : NULL,
			  tyscm_objfile_data_key, &t_smob->base);

  return result;
}

/* Returns the <gdb:type> object in SCM or #f if SCM is not a
   <gdb:type> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

static SCM
tyscm_scm_to_type_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, type_smob_tag);
}

/* Returns the <gdb:type> object in SELF.
   Throws an exception if SELF is not a <gdb:type> object
   (after passing it through *scm->smob*).  */

static SCM
tyscm_get_type_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM t_scm = tyscm_scm_to_type_gsmob (self);

  if (gdbscm_is_exception (t_scm))
    gdbscm_throw (t_scm);

  SCM_ASSERT_TYPE (tyscm_is_type (t_scm), self, arg_pos, func_name,
		   type_smob_name);

  return t_scm;
}

/* Returns a pointer to the type smob of SELF.
   Throws an exception if SELF is not a <gdb:type> object
   (after passing it through *scm->smob*).  */

type_smob *
tyscm_get_type_smob_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM t_scm = tyscm_get_type_arg_unsafe (self, arg_pos, func_name);
  type_smob *t_smob = (type_smob *) SCM_SMOB_DATA (t_scm);

  return t_smob;
}

/* Called when OBJFILE is about to be deleted.
   Make a copy of all types associated with OBJFILE.  */

static void
save_objfile_types (struct objfile *objfile, void *datum)
{
  type_smob *t_smob = datum;
  htab_t copied_types;

  if (!gdb_scheme_initialized)
    return;

  copied_types = create_copied_types_hash (objfile);

  while (t_smob != NULL)
    {
      type_smob *next = (type_smob *) t_smob->base.next;

      htab_empty (copied_types);

      t_smob->type = copy_type_recursive (objfile, t_smob->type, copied_types);

      t_smob->base.next = NULL;
      t_smob->base.prev = NULL;

      t_smob = next;
    }

  htab_delete (copied_types);
}

/* Administrivia for field smobs.  */

/* The smob "mark" function for <gdb:field>.  */

static SCM
tyscm_mark_field_smob (SCM self)
{
  field_smob *f_smob = (field_smob *) SCM_SMOB_DATA (self);

  scm_gc_mark (f_smob->type_scm);
  /* Do this last.  */
  return gdbscm_mark_gsmob (&f_smob->base);
}

/* The smob "print" function for <gdb:field>.  */

static int
tyscm_print_field_smob (SCM self, SCM port, scm_print_state *pstate)
{
  field_smob *f_smob = (field_smob *) SCM_SMOB_DATA (self);

  gdbscm_printf (port, "#<%s ", field_smob_name);
  scm_write (f_smob->type_scm, port);
  gdbscm_printf (port, " %d", f_smob->field_num);
  scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* Low level routine to create a <gdb:field> object for field FIELD_NUM
   of type TYPE_SCM.  */

static SCM
tyscm_make_field_smob (SCM type_scm, int field_num)
{
  field_smob *f_smob = (field_smob *)
    scm_gc_malloc (sizeof (field_smob), field_smob_name);
  SCM result;

  f_smob->type_scm = type_scm;
  f_smob->field_num = field_num;
  result = scm_new_smob (field_smob_tag, (scm_t_bits) f_smob);
  gdbscm_init_gsmob (&f_smob->base);

  return result;
}

/* Return non-zero if SCM is a <gdb:field> object.  */

static int
tyscm_is_field (SCM self)
{
  return SCM_SMOB_PREDICATE (field_smob_tag, self);
}

/* (field? object) -> boolean */

static SCM
gdbscm_field_p (SCM self)
{
  return scm_from_bool (tyscm_is_field (self));
}

/* Create a new <gdb:field> object that encapsulates field FIELD_NUM
   in type TYPE_SCM.  */

SCM
tyscm_gsmob_from_field (SCM type_scm, int field_num)
{
  return tyscm_make_field_smob (type_scm, field_num);
}

/* Create a new <gdb:field> object that encapsulates TYPE_SCM/FIELD_NUM.
   The object is passed through *smob->scm*.
   A Scheme exception is thrown if there is an error.  */

SCM
tyscm_scm_from_field_unsafe (SCM type_scm, int field_num)
{
  SCM f_scm = tyscm_gsmob_from_field (type_scm, field_num);

  return gdbscm_scm_from_gsmob_unsafe (f_scm);
}

/* Returns the <gdb:field> object in SCM or #f if SCM is not a
   <gdb:field> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

static SCM
tyscm_scm_to_field_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, field_smob_tag);
}

/* Returns the <gdb:field> object in SELF.
   Throws an exception if SELF is not a <gdb:field> object
   (after passing it through *scm->smob*).  */

static SCM
tyscm_get_field_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM f_scm = tyscm_scm_to_field_gsmob (self);

  if (gdbscm_is_exception (f_scm))
    gdbscm_throw (f_scm);

  SCM_ASSERT_TYPE (tyscm_is_field (f_scm), self, arg_pos, func_name,
		   field_smob_name);

  return f_scm;
}

/* Returns a pointer to the field smob of SELF.
   Throws an exception if SELF is not a <gdb:field> object
   (after passing it through *scm->smob*).  */

static field_smob *
tyscm_get_field_smob_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM f_scm = tyscm_get_field_arg_unsafe (self, arg_pos, func_name);
  field_smob *f_smob = (field_smob *) SCM_SMOB_DATA (f_scm);

  return f_smob;
}

/* Returns a pointer to the type struct in F_SMOB
   (the type the field is in).  */

static struct type *
tyscm_field_smob_containing_type (field_smob *f_smob)
{
  type_smob *t_smob;

  gdb_assert (tyscm_is_type (f_smob->type_scm));
  t_smob = (type_smob *) SCM_SMOB_DATA (f_smob->type_scm);

  return t_smob->type;
}

/* Returns a pointer to the field struct of F_SMOB.  */

static struct field *
tyscm_field_smob_to_field (field_smob *f_smob)
{
  struct type *type = tyscm_field_smob_containing_type (f_smob);

  /* This should be non-NULL by construction.  */
  gdb_assert (TYPE_FIELDS (type) != NULL);

  return &TYPE_FIELD (type, f_smob->field_num);
}

/* Type smob accessors.  */

/* (type-code <gdb:type>) -> integer
   Return the code for this type.  */

static SCM
gdbscm_type_code (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;

  return scm_from_int (TYPE_CODE (type));
}

/* (type-fields <gdb:type>) -> list
   Return a list of all fields.  Each element is a <gdb:field> object.
   This also supports arrays, we return a field list of one element,
   the range type.  */

static SCM
gdbscm_type_fields (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  struct type *containing_type;
  SCM containing_type_scm, result;
  int i;

  containing_type = tyscm_get_composite (type);
  if (containing_type == NULL)
    gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self, not_composite_error);

  /* If SELF is a typedef or reference, we want the underlying type,
     which is what tyscm_get_composite returns.  */
  if (containing_type == type)
    containing_type_scm = self;
  else
    containing_type_scm = tyscm_scm_from_type_unsafe (containing_type);

  result = SCM_EOL;
  for (i = 0; i < TYPE_NFIELDS (containing_type); ++i)
    result = scm_cons (tyscm_make_field_smob (containing_type_scm, i), result);

  return scm_reverse_x (result, SCM_EOL);
}

/* (type-tag <gdb:type>) -> string
   Return the type's tag, or #f.  */

static SCM
gdbscm_type_tag (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;

  if (!TYPE_TAG_NAME (type))
    return SCM_BOOL_F;
  return gdbscm_scm_from_c_string (TYPE_TAG_NAME (type));
}

/* (type-sizeof <gdb:type>) -> integer
   Return the size of the type represented by SELF, in bytes.  */

static SCM
gdbscm_type_sizeof (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      check_typedef (type);
    }
  /* Ignore exceptions.  */

  return scm_from_long (TYPE_LENGTH (type));
}

/* (type-strip-typedefs <gdb:type>) -> <gdb:type>
   Return the type, stripped of typedefs. */

static SCM
gdbscm_type_strip_typedefs (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = check_typedef (type);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* Strip typedefs and pointers/reference from a type.  Then check that
   it is a struct, union, or enum type.  If not, return NULL.  */

static struct type *
tyscm_get_composite (struct type *type)
{
  volatile struct gdb_exception except;

  for (;;)
    {
      TRY_CATCH (except, RETURN_MASK_ALL)
	{
	  type = check_typedef (type);
	}
      GDBSCM_HANDLE_GDB_EXCEPTION (except);

      if (TYPE_CODE (type) != TYPE_CODE_PTR
	  && TYPE_CODE (type) != TYPE_CODE_REF)
	break;
      type = TYPE_TARGET_TYPE (type);
    }

  /* If this is not a struct, union, or enum type, raise TypeError
     exception.  */
  if (TYPE_CODE (type) != TYPE_CODE_STRUCT
      && TYPE_CODE (type) != TYPE_CODE_UNION
      && TYPE_CODE (type) != TYPE_CODE_ENUM)
    return NULL;

  return type;
}

/* Helper for tyscm_array and tyscm_vector.  */

static SCM
tyscm_array_1 (SCM self, SCM n1_scm, SCM n2_scm, int is_vector,
	       const char *func_name)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, func_name);
  struct type *type = t_smob->type;
  long n1, n2 = 0;
  struct type *array = NULL;
  volatile struct gdb_exception except;

  gdbscm_parse_function_args (func_name, SCM_ARG2, NULL, "l|l",
			      n1_scm, &n1, n2_scm, &n2);

  if (SCM_UNBNDP (n2_scm))
    {
      n2 = n1;
      n1 = 0;
    }

  if (n2 < n1)
    {
      gdbscm_out_of_range_error (func_name, SCM_ARG3,
				 scm_cons (scm_from_long (n1),
					   scm_from_long (n2)),
				 _("Array length must not be negative"));
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      array = lookup_array_range_type (type, n1, n2);
      if (is_vector)
	make_vector_type (array);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (array);
}

/* (type-array <gdb:type> [low-bound] high-bound) -> <gdb:type>
   The array has indices [low-bound,high-bound].
   If low-bound is not provided zero is used.
   Return an array type.

   IWBN if the one argument version specified a size, not the high bound.
   It's too easy to pass one argument thinking it is the size of the array.
   The current semantics are for compatibility with the Python version.
   Later we can add #:size.  */

static SCM
gdbscm_type_array (SCM self, SCM n1, SCM n2)
{
  return tyscm_array_1 (self, n1, n2, 0, FUNC_NAME);
}

/* (type-vector <gdb:type> [low-bound] high-bound) -> <gdb:type>
   The array has indices [low-bound,high-bound].
   If low-bound is not provided zero is used.
   Return a vector type.

   IWBN if the one argument version specified a size, not the high bound.
   It's too easy to pass one argument thinking it is the size of the array.
   The current semantics are for compatibility with the Python version.
   Later we can add #:size.  */

static SCM
gdbscm_type_vector (SCM self, SCM n1, SCM n2)
{
  return tyscm_array_1 (self, n1, n2, 1, FUNC_NAME);
}

/* (type-pointer <gdb:type>) -> <gdb:type>
   Return a <gdb:type> object which represents a pointer to SELF.  */

static SCM
gdbscm_type_pointer (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = lookup_pointer_type (type);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* (type-range <gdb:type>) -> (low high)
   Return the range of a type represented by SELF.  The return type is
   a list.  The first element is the low bound, and the second element
   is the high bound.  */

static SCM
gdbscm_type_range (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  SCM low_scm, high_scm;
  /* Initialize these to appease GCC warnings.  */
  LONGEST low = 0, high = 0;

  SCM_ASSERT_TYPE (TYPE_CODE (type) == TYPE_CODE_ARRAY
		   || TYPE_CODE (type) == TYPE_CODE_STRING
		   || TYPE_CODE (type) == TYPE_CODE_RANGE,
		   self, SCM_ARG1, FUNC_NAME, "ranged type");

  switch (TYPE_CODE (type))
    {
    case TYPE_CODE_ARRAY:
    case TYPE_CODE_STRING:
      low = TYPE_LOW_BOUND (TYPE_INDEX_TYPE (type));
      high = TYPE_HIGH_BOUND (TYPE_INDEX_TYPE (type));
      break;
    case TYPE_CODE_RANGE:
      low = TYPE_LOW_BOUND (type);
      high = TYPE_HIGH_BOUND (type);
      break;
    }

  low_scm = gdbscm_scm_from_longest (low);
  high_scm = gdbscm_scm_from_longest (high);

  return scm_list_2 (low_scm, high_scm);
}

/* (type-reference <gdb:type>) -> <gdb:type>
   Return a <gdb:type> object which represents a reference to SELF.  */

static SCM
gdbscm_type_reference (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = lookup_reference_type (type);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* (type-target <gdb:type>) -> <gdb:type>
   Return a <gdb:type> object which represents the target type of SELF.  */

static SCM
gdbscm_type_target (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;

  SCM_ASSERT (TYPE_TARGET_TYPE (type), self, SCM_ARG1, FUNC_NAME);

  return tyscm_scm_from_type_unsafe (TYPE_TARGET_TYPE (type));
}

/* (type-const <gdb:type>) -> <gdb:type>
   Return a const-qualified type variant.  */

static SCM
gdbscm_type_const (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = make_cv_type (1, 0, type, NULL);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* (type-volatile <gdb:type>) -> <gdb:type>
   Return a volatile-qualified type variant.  */

static SCM
gdbscm_type_volatile (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = make_cv_type (0, 1, type, NULL);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* (type-unqualified <gdb:type>) -> <gdb:type>
   Return an unqualified type variant.  */

static SCM
gdbscm_type_unqualified (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      type = make_cv_type (0, 0, type, NULL);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return tyscm_scm_from_type_unsafe (type);
}

/* (type-string <gdb:type>) -> string
   Return the name of type.
   TODO: template support elided for now.  */

static SCM
gdbscm_type_string (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  char *thetype;
  SCM exception, result;

  thetype = tyscm_type_name (type, &exception);

  if (thetype == NULL)
    gdbscm_throw (exception);

  result = gdbscm_scm_from_c_string (thetype);
  xfree (thetype);

  return result;
}

/* Field related accessors of types.  */

/* (type-length <gdb:type>) -> integer
   Return number of fields.  */

static SCM
gdbscm_type_length (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;

  type = tyscm_get_composite (type);
  if (type == NULL)
    gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self, not_composite_error);

  return scm_from_long (TYPE_NFIELDS (type));
}

/* (type-field <gdb:type> string) -> <gdb:field>
   Return the <gdb:field> object for the field named by the argument.  */

static SCM
gdbscm_type_field (SCM self, SCM field_scm)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  char *field;
  int i;
  struct cleanup *cleanups;

  SCM_ASSERT_TYPE (scm_is_string (field_scm), field_scm, SCM_ARG2, FUNC_NAME,
		   "string");

  /* We want just fields of this type, not of base types, so instead of
     using lookup_struct_elt_type, portions of that function are
     copied here.  */

  type = tyscm_get_composite (type);
  if (type == NULL)
    gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self, not_composite_error);

  field = gdbscm_scm_to_c_string (field_scm);
  cleanups = make_cleanup (xfree, field);

  for (i = 0; i < TYPE_NFIELDS (type); i++)
    {
      const char *t_field_name = TYPE_FIELD_NAME (type, i);

      if (t_field_name && (strcmp_iw (t_field_name, field) == 0))
	{
	    do_cleanups (cleanups);
	    return tyscm_make_field_smob (self, i);
	}
    }

  do_cleanups (cleanups);

  gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, field_scm,
			     _("Unknown field"));
}

/* (type-has-field? <gdb:type> string) -> boolean
   Return boolean indicating if type SELF has FIELD_SCM (a string).  */

static SCM
gdbscm_type_has_field_p (SCM self, SCM field_scm)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  char *field;
  int i;
  struct cleanup *cleanups;

  SCM_ASSERT_TYPE (scm_is_string (field_scm), field_scm, SCM_ARG2, FUNC_NAME,
		   "string");

  /* We want just fields of this type, not of base types, so instead of
     using lookup_struct_elt_type, portions of that function are
     copied here.  */

  type = tyscm_get_composite (type);
  if (type == NULL)
    gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self, not_composite_error);

  field = gdbscm_scm_to_c_string (field_scm);
  cleanups = make_cleanup (xfree, field);

  for (i = 0; i < TYPE_NFIELDS (type); i++)
    {
      const char *t_field_name = TYPE_FIELD_NAME (type, i);

      if (t_field_name && (strcmp_iw (t_field_name, field) == 0))
	{
	    do_cleanups (cleanups);
	    return SCM_BOOL_T;
	}
    }

  do_cleanups (cleanups);

  return SCM_BOOL_F;
}

/* (make-field-iterator <gdb:type>) -> <gdb:iterator>
   Make a field iterator object.  */

static SCM
gdbscm_make_field_iterator (SCM self)
{
  type_smob *t_smob =
    tyscm_get_type_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct type *type = t_smob->type;
  struct type *containing_type;
  SCM containing_type_scm;

  containing_type = tyscm_get_composite (type);
  if (containing_type == NULL)
    gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, self, not_composite_error);

  /* If SELF is a typedef or reference, we want the underlying type,
     which is what tyscm_get_composite returns.  */
  if (containing_type == type)
    containing_type_scm = self;
  else
    containing_type_scm = tyscm_scm_from_type_unsafe (containing_type);

  return gdbscm_make_iterator (containing_type_scm, scm_from_int (0),
			       tyscm_next_field_x_scm);
}

/* (type-next-field! <gdb:iterator>) -> <gdb:field>
   Return the next field in the iteration through the list of fields of the
   type, or #f.
   SELF is a <gdb:iterator> object created by gdbscm_make_field_iterator.
   This is the next! <gdb:iterator> function, not exported to the user.  */

static SCM
gdbscm_type_next_field_x (SCM self)
{
  iterator_smob *i_smob;
  type_smob *t_smob;
  struct type *type;
  SCM it_scm, result, progress, object;
  int field, rc;

  it_scm = itscm_scm_to_iterator_gsmob (self);
  if (gdbscm_is_exception (it_scm))
    gdbscm_throw (it_scm);
  SCM_ASSERT_TYPE (itscm_is_iterator (it_scm), self, SCM_ARG1, FUNC_NAME,
		   itscm_iterator_smob_name ());
  i_smob = (iterator_smob *) SCM_SMOB_DATA (it_scm);
  object = itscm_iterator_smob_object (i_smob);
  progress = itscm_iterator_smob_progress (i_smob);

  /* TODO: pass object through *scm->smob*.  */
  SCM_ASSERT_TYPE (tyscm_is_type (object), object,
		   SCM_ARG1, FUNC_NAME, type_smob_name);
  t_smob = (type_smob *) SCM_SMOB_DATA (object);
  type = t_smob->type;

  SCM_ASSERT_TYPE (scm_is_signed_integer (progress,
					  0, TYPE_NFIELDS (type)),
		   progress, SCM_ARG1, FUNC_NAME, "integer");
  field = scm_to_int (progress);

  if (field < TYPE_NFIELDS (type))
    {
      result = tyscm_make_field_smob (object, field);
      itscm_set_iterator_smob_progress_x (i_smob, scm_from_int (field + 1));
      return result;
    }

  return SCM_BOOL_F;
}

/* Field smob accessors.  */

/* (field-name <gdb:field>) -> string
   Return the name of this field or #f if there isn't one.  */

static SCM
gdbscm_field_name (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);

  if (FIELD_NAME (*field))
    return gdbscm_scm_from_c_string (FIELD_NAME (*field));
  return SCM_BOOL_F;
}

/* (field-type <gdb:field>) -> <gdb:type>
   Return the <gdb:type> object of the field or #f if there isn't one.  */

static SCM
gdbscm_field_type (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);

  /* A field can have a NULL type in some situations.  */
  if (FIELD_TYPE (*field))
    return tyscm_scm_from_type_unsafe (FIELD_TYPE (*field));
  return SCM_BOOL_F;
}

/* (field-enumval <gdb:field>) -> integer
   For enum values, return its value as an integer.  */

static SCM
gdbscm_field_enumval (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);
  struct type *type = tyscm_field_smob_containing_type (f_smob);

  SCM_ASSERT_TYPE (TYPE_CODE (type) == TYPE_CODE_ENUM,
		   self, SCM_ARG1, FUNC_NAME, "enum type");

  return scm_from_long (FIELD_ENUMVAL (*field));
}

/* (field-bitpos <gdb:field>) -> integer
   For bitfields, return its offset in bits.  */

static SCM
gdbscm_field_bitpos (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);
  struct type *type = tyscm_field_smob_containing_type (f_smob);

  SCM_ASSERT_TYPE (TYPE_CODE (type) != TYPE_CODE_ENUM,
		   self, SCM_ARG1, FUNC_NAME, "non-enum type");

  return scm_from_long (FIELD_BITPOS (*field));
}

/* (field-bitsize <gdb:field>) -> integer
   Return the size of the field in bits.  */

static SCM
gdbscm_field_bitsize (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);

  return scm_from_long (FIELD_BITPOS (*field));
}

/* (field-artificial? <gdb:field>) -> boolean
   Return #t if field is artificial.  */

static SCM
gdbscm_field_artificial_p (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);

  return scm_from_bool (FIELD_ARTIFICIAL (*field));
}

/* (field-baseclass? <gdb:field>) -> boolean
   Return #t if field is a baseclass.  */

static SCM
gdbscm_field_baseclass_p (SCM self)
{
  field_smob *f_smob =
    tyscm_get_field_smob_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  struct field *field = tyscm_field_smob_to_field (f_smob);
  struct type *type = tyscm_field_smob_containing_type (f_smob);

  if (TYPE_CODE (type) == TYPE_CODE_CLASS)
    return scm_from_bool (f_smob->field_num < TYPE_N_BASECLASSES (type));
  return SCM_BOOL_F;
}

/* equal? for types.  */

static SCM
gdbscm_type_equal_p (SCM type1_scm, SCM type2_scm)
{
  type_smob *type1_smob, *type2_smob;
  struct type *type1, *type2;
  int result = 0;
  volatile struct gdb_exception except;

  SCM_ASSERT_TYPE (tyscm_is_type (type1_scm), type1_scm, SCM_ARG1, FUNC_NAME,
		   type_smob_name);
  SCM_ASSERT_TYPE (tyscm_is_type (type2_scm), type2_scm, SCM_ARG2, FUNC_NAME,
		   type_smob_name);
  type1_smob = (type_smob *) SCM_SMOB_DATA (type1_scm);
  type2_smob = (type_smob *) SCM_SMOB_DATA (type2_scm);
  type1 = type1_smob->type;
  type2 = type2_smob->type;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      result = types_deeply_equal (type1, type2);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return scm_from_bool (result);
}

/* Return the type named TYPE_NAME in BLOCK.
   Returns NULL if not found.
   This routine does not throw an error.  */

static struct type *
tyscm_lookup_typename (const char *type_name, const struct block *block)
{
  struct type *type = NULL;
  volatile struct gdb_exception except;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      if (!strncmp (type_name, "struct ", 7))
	type = lookup_struct (type_name + 7, NULL);
      else if (!strncmp (type_name, "union ", 6))
	type = lookup_union (type_name + 6, NULL);
      else if (!strncmp (type_name, "enum ", 5))
	type = lookup_enum (type_name + 5, NULL);
      else
	type = lookup_typename (current_language, get_current_arch (),
				type_name, block, 0);
    }
  if (except.reason < 0)
    return NULL;

  return type;
}

/* (lookup-type name [#:block <gdb:block>]) -> <gdb:type>
   TODO: legacy template support left out until needed.  */

static SCM
gdbscm_lookup_type (SCM name_scm, SCM rest)
{
  SCM keywords[] = { tyscm_block_keyword, SCM_BOOL_F };
  char *name;
  SCM block_scm = SCM_BOOL_F;
  int block_arg_pos = -1;
  const struct block *block = NULL;
  struct type *type;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, keywords, "s#O",
			      name_scm, &name,
			      rest, &block_arg_pos, &block_scm);

  if (block_arg_pos != -1)
    {
      SCM exception;

      block = bkscm_scm_to_block (block_scm, block_arg_pos, FUNC_NAME,
				  &exception);
      if (block == NULL)
	{
	  xfree (name);
	  gdbscm_throw (exception);
	}
    }
  type = tyscm_lookup_typename (name, block);
  xfree (name);

  if (type != NULL)
    return tyscm_scm_from_type_unsafe (type);
  return SCM_BOOL_F;
}

/* Initialize the Scheme type code.  */

#define ENTRY(X) { #X, X }

static const scheme_integer_constant type_integer_constants[] =
{
  ENTRY (TYPE_CODE_BITSTRING),
  ENTRY (TYPE_CODE_PTR),
  ENTRY (TYPE_CODE_ARRAY),
  ENTRY (TYPE_CODE_STRUCT),
  ENTRY (TYPE_CODE_UNION),
  ENTRY (TYPE_CODE_ENUM),
  ENTRY (TYPE_CODE_FLAGS),
  ENTRY (TYPE_CODE_FUNC),
  ENTRY (TYPE_CODE_INT),
  ENTRY (TYPE_CODE_FLT),
  ENTRY (TYPE_CODE_VOID),
  ENTRY (TYPE_CODE_SET),
  ENTRY (TYPE_CODE_RANGE),
  ENTRY (TYPE_CODE_STRING),
  ENTRY (TYPE_CODE_ERROR),
  ENTRY (TYPE_CODE_METHOD),
  ENTRY (TYPE_CODE_METHODPTR),
  ENTRY (TYPE_CODE_MEMBERPTR),
  ENTRY (TYPE_CODE_REF),
  ENTRY (TYPE_CODE_CHAR),
  ENTRY (TYPE_CODE_BOOL),
  ENTRY (TYPE_CODE_COMPLEX),
  ENTRY (TYPE_CODE_TYPEDEF),
  ENTRY (TYPE_CODE_NAMESPACE),
  ENTRY (TYPE_CODE_DECFLOAT),
  ENTRY (TYPE_CODE_INTERNAL_FUNCTION),

  END_INTEGER_CONSTANTS
};

#undef ENTRY

static const scheme_function type_functions[] =
{
  { "type?", 1, 0, 0, gdbscm_type_p,
    "\
Return #t if the object is a <gdb:type> object." },

  { "lookup-type", 1, 0, 1, gdbscm_lookup_type,
    "\
Return the <gdb:type> object representing string or #f if not found.\n\
If block is given then the type is looked for in that block.\n\
\n\
  Arguments: string [#:block <gdb:block>]" },

  { "type-code", 1, 0, 0, gdbscm_type_code,
    "\
Return the code of the type" },

  { "type-fields", 1, 0, 0, gdbscm_type_fields,
    "\
Return the list of <gdb:field> objects of fields of the type." },

  { "type-tag", 1, 0, 0, gdbscm_type_tag,
    "\
Return the tag name of the type, or #f if there isn't one." },

  { "type-sizeof", 1, 0, 0, gdbscm_type_sizeof,
    "\
Return the size of the type, in bytes." },

  { "type-strip-typedefs", 1, 0, 0, gdbscm_type_strip_typedefs,
    "\
Return a type formed by stripping the type of all typedefs." },

  { "type-array", 2, 1, 0, gdbscm_type_array,
    "\
Return a type representing an array of objects of the type.\n\
\n\
  Arguments: <gdb:type> [low-bound] high-bound\n\
    If low-bound is not provided zero is used.\n\
    N.B. If only the high-bound parameter is specified, it is not\n\
    the array size.\n\
    Valid bounds for array indices are [low-bound,high-bound]." },

  { "type-vector", 2, 1, 0, gdbscm_type_vector,
    "\
Return a type representing a vector of objects of the type.\n\
Vectors differ from arrays in that if the current language has C-style\n\
arrays, vectors don't decay to a pointer to the first element.\n\
They are first class values.\n\
\n\
  Arguments: <gdb:type> [low-bound] high-bound\n\
    If low-bound is not provided zero is used.\n\
    N.B. If only the high-bound parameter is specified, it is not\n\
    the array size.\n\
    Valid bounds for array indices are [low-bound,high-bound]." },

  { "type-pointer", 1, 0, 0, gdbscm_type_pointer,
    "\
Return a type of pointer to the type." },

  { "type-range", 1, 0, 0, gdbscm_type_range,
    "\
Return (low high) representing the range for the type." },

  { "type-reference", 1, 0, 0, gdbscm_type_reference,
    "\
Return a type of reference to the type." },

  { "type-target", 1, 0, 0, gdbscm_type_target,
    "\
Return the target type of the type." },

  { "type-const", 1, 0, 0, gdbscm_type_const,
    "\
Return a const variant of the type." },

  { "type-volatile", 1, 0, 0, gdbscm_type_volatile,
    "\
Return a volatile variant of the type." },

  { "type-unqualified", 1, 0, 0, gdbscm_type_unqualified,
    "\
Return a variant of the type without const or volatile attributes." },

  { "type-string", 1, 0, 0, gdbscm_type_string,
    "\
Return the name of the type as a string." },

  { "type-equal?", 2, 0, 0, gdbscm_type_equal_p,
    "\
Return #t if the two types are equal." },

  { "type-length", 1, 0, 0, gdbscm_type_length,
    "\
Return the number of fields of the type." },

  { "type-field", 2, 0, 0, gdbscm_type_field,
    "\
Return the field named by string of the type.\n\
\n\
  Arguments: <gdb:type> string" },

  { "type-has-field?", 2, 0, 0, gdbscm_type_has_field_p,
    "\
Return #t if the type has field named string.\n\
\n\
  Arguments: <gdb:type> string" },

  { "field?", 1, 0, 0, gdbscm_field_p,
    "\
Return #t if the object is a <gdb:field> object." },

  { "make-field-iterator", 1, 0, 0, gdbscm_make_field_iterator,
    "\
Return a <gdb:iterator> object for iterating over the fields of the type." },

  { "field-name", 1, 0, 0, gdbscm_field_name,
    "\
Return the name of the field." },

  { "field-type", 1, 0, 0, gdbscm_field_type,
    "\
Return the type of the field." },

  { "field-enumval", 1, 0, 0, gdbscm_field_enumval,
    "\
Return the enum value represented by the field." },

  { "field-bitpos", 1, 0, 0, gdbscm_field_bitpos,
    "\
Return the offset in bits of the field in its containing type." },

  { "field-bitsize", 1, 0, 0, gdbscm_field_bitsize,
    "\
Return the size of the field in bits." },

  { "field-artificial?", 1, 0, 0, gdbscm_field_artificial_p,
    "\
Return #t if the field is artificial." },

  { "field-baseclass?", 1, 0, 0, gdbscm_field_baseclass_p,
    "\
Return #t if the field is a baseclass." },

  END_FUNCTIONS
};

void
gdbscm_initialize_types (void)
{
  type_smob_tag = gdbscm_make_smob_type (type_smob_name, sizeof (type_smob));
  scm_set_smob_mark (type_smob_tag, tyscm_mark_type_smob);
  scm_set_smob_free (type_smob_tag, tyscm_free_type_smob);
  scm_set_smob_print (type_smob_tag, tyscm_print_type_smob);
  scm_set_smob_equalp (type_smob_tag, tyscm_equal_p_type_smob);

  field_smob_tag = gdbscm_make_smob_type (field_smob_name,
					  sizeof (field_smob));
  scm_set_smob_mark (field_smob_tag, tyscm_mark_field_smob);
  scm_set_smob_print (field_smob_tag, tyscm_print_field_smob);

  gdbscm_define_integer_constants (type_integer_constants, 1);
  gdbscm_define_functions (type_functions, 1);

  tyscm_next_field_x_scm =
    scm_c_define_gsubr ("%type-next-field!", 1, 0, 0,
			gdbscm_type_next_field_x);

  tyscm_block_keyword = scm_from_latin1_keyword ("block");

  /* Register an objfile "free" callback so we can properly copy types
     associated with the objfile when it's about to be deleted.  */
  tyscm_objfile_data_key =
    register_objfile_data_with_cleanup (save_objfile_types, NULL);
}
