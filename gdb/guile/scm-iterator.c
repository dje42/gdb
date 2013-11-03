/* Simple iterators for GDB/Scheme.

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

/* These are an experiment, have to see what the data shows,
   and how they're used.  One issue is, of course, the use of assignment
   to update the iterator.  And the fact that this is non-standard.
   But it's simple, and let's me get other things working while we think
   about what kind of iteration support is desired.  */

/* See README file in this directory for implementation notes, coding
   conventions, et.al.  */

#include "defs.h"
#include "guile-internal.h"

/* A smob for iterating over something.
   Typically this is used when computing a list of everything is
   too expensive.
   The typedef for this struct is in guile-internal.h.  */

struct _iterator_smob {
  /* This always appears first.  */
  gdb_smob base;

  /* The object being iterated over.  */
  SCM object;

  /* An arbitrary object describing the progress of the iteration.
     This is used by next_x to track progress.  */
  SCM progress;

  /* A procedure of one argument, the iterator.
     It returns the next object in the iteration.
     How to signal "end of iteration" is up to next_x.  */
  SCM next_x;
};

static const char iterator_smob_name[] = "gdb:iterator";

/* The tag Guile knows the iterator smob by.  */
static scm_t_bits iterator_smob_tag;

const char *
itscm_iterator_smob_name (void)
{
  return iterator_smob_name;
}

SCM
itscm_iterator_smob_object (iterator_smob *i_smob)
{
  return i_smob->object;
}

SCM
itscm_iterator_smob_progress (iterator_smob *i_smob)
{
  return i_smob->progress;
}

void
itscm_set_iterator_smob_progress_x (iterator_smob *i_smob, SCM progress)
{
  i_smob->progress = progress;
}

/* Administrivia for iterator smobs.  */

/* The smob "mark" function for <gdb:iterator>.  */

static SCM
itscm_mark_iterator_smob (SCM self)
{
  iterator_smob *i_smob = (iterator_smob *) SCM_SMOB_DATA (self);

  scm_gc_mark (i_smob->object);
  scm_gc_mark (i_smob->progress);
  scm_gc_mark (i_smob->next_x);
  /* Do this last.  */
  return gdbscm_mark_gsmob (&i_smob->base);
}

/* The smob "print" function for <gdb:iterator>.  */

static int
itscm_print_iterator_smob (SCM self, SCM port, scm_print_state *pstate)
{
  iterator_smob *i_smob = (iterator_smob *) SCM_SMOB_DATA (self);

  gdbscm_printf (port, "#<%s ", iterator_smob_name);
  scm_write (i_smob->object, port);
  scm_puts (" ", port);
  scm_write (i_smob->progress, port);
  scm_puts (" ", port);
  scm_write (i_smob->next_x, port);
  scm_puts (">", port);

  scm_remember_upto_here_1 (self);

  /* Non-zero means success.  */
  return 1;
}

/* Low level routine to make a <gdb:iterator> object.
   Caller must verify correctness of arguments.
   No exceptions are thrown.  */

static SCM
itscm_make_iterator_smob (SCM object, SCM progress, SCM next)
{
  iterator_smob *i_smob = (iterator_smob *)
    scm_gc_malloc (sizeof (iterator_smob), iterator_smob_name);
  SCM i_scm;

  i_smob->object = object;
  i_smob->progress = progress;
  i_smob->next_x = next;
  i_scm = scm_new_smob (iterator_smob_tag, (scm_t_bits) i_smob);
  gdbscm_init_gsmob (&i_smob->base);

  return i_scm;
}

/* (make-iterator object object procedure) -> <gdb:iterator> */

SCM
gdbscm_make_iterator (SCM object, SCM progress, SCM next)
{
  SCM i_scm;

  SCM_ASSERT_TYPE (gdbscm_is_procedure (next), next, SCM_ARG3, FUNC_NAME,
		   "procedure");

  i_scm = itscm_make_iterator_smob (object, progress, next);

  return gdbscm_scm_from_gsmob_unsafe (i_scm);
}

/* Return non-zero if SCM is a <gdb:iterator> object.  */

int
itscm_is_iterator (SCM scm)
{
  return SCM_SMOB_PREDICATE (iterator_smob_tag, scm);
}

/* (iterator? object) -> boolean */

static SCM
gdbscm_iterator_p (SCM scm)
{
  return scm_from_bool (itscm_is_iterator (scm));
}

/* Returns the <gdb:iterator> object contained in SCM or #f if SCM is not a
   <gdb:iterator> object.
   Returns a <gdb:exception> object if there was a problem during the
   conversion.  */

SCM
itscm_scm_to_iterator_gsmob (SCM scm)
{
  return gdbscm_scm_to_gsmob_safe (scm, iterator_smob_tag);
}

/* Call the next! method on ITER, which must be a <gdb:iterator> object.
   Returns a <gdb:exception> object if an exception is thrown.
   OK_EXCPS is passed to gdbscm_safe_call_1.  */

SCM
itscm_safe_call_next_x (SCM iter, excp_matcher_func *ok_excps)
{
  iterator_smob *i_smob;

  gdb_assert (itscm_is_iterator (iter));

  i_smob = (iterator_smob *) SCM_SMOB_DATA (iter);
  return gdbscm_safe_call_1 (i_smob->next_x, iter, ok_excps);
}

/* Iterator methods.  */

/* Returns the <gdb:iterator> smob in SELF.
   Throws an exception if SELF is not an iterator smob
   (after passing it through *scm->smob*).  */

SCM
itscm_get_iterator_arg_unsafe (SCM self, int arg_pos, const char *func_name)
{
  SCM i_scm = itscm_scm_to_iterator_gsmob (self);

  if (gdbscm_is_exception (i_scm))
    gdbscm_throw (i_scm);

  SCM_ASSERT_TYPE (itscm_is_iterator (i_scm), self, arg_pos, func_name,
		   iterator_smob_name);

  return i_scm;
}

/* (iterator-object <gdb:iterator>) -> object */

static SCM
gdbscm_iterator_object (SCM self)
{
  SCM i_scm;
  iterator_smob *i_smob;

  i_scm = itscm_get_iterator_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  i_smob = (iterator_smob *) SCM_SMOB_DATA (i_scm);

  return i_smob->object;
}

/* (iterator-progress <gdb:iterator>) -> object */

static SCM
gdbscm_iterator_progress (SCM self)
{
  SCM i_scm;
  iterator_smob *i_smob;

  i_scm = itscm_get_iterator_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  i_smob = (iterator_smob *) SCM_SMOB_DATA (i_scm);

  return i_smob->progress;
}

/* (set-iterator-progress! <gdb:iterator> object) -> unspecified */

static SCM
gdbscm_set_iterator_progress_x (SCM self, SCM value)
{
  SCM i_scm;
  iterator_smob *i_smob;

  i_scm = itscm_get_iterator_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  i_smob = (iterator_smob *) SCM_SMOB_DATA (i_scm);

  i_smob->progress = value;
  return SCM_UNSPECIFIED;
}

/* (iterator-next! <gdb:iterator>) -> object
   The result is the next value in the iteration or some "end" marker.
   It is up to each iterator's next! function to specify what its end
   marker is.  */

static SCM
gdbscm_iterator_next_x (SCM self)
{
  SCM i_scm;
  iterator_smob *i_smob;

  i_scm = itscm_get_iterator_arg_unsafe (self, SCM_ARG1, FUNC_NAME);
  i_smob = (iterator_smob *) SCM_SMOB_DATA (i_scm);
  /* We leave type-checking of the procedure to gdbscm_safe_call_1.  */

  return gdbscm_safe_call_1 (i_smob->next_x, self, NULL);
}

/* Initialize the Scheme iterator code.  */

static const scheme_function iterator_functions[] =
{
  { "make-iterator", 3, 0, 0, gdbscm_make_iterator,
    "\
Create a <gdb:iterator> object.\n\
\n\
  Arguments: object progress next!\n\
    object:   The object to iterate over.\n\
    progress: An object to use to track progress of the iteration.\n\
    next!:    A procedure of one argument, the iterator.\n\
      Returns the next element in the iteration or an implementation-chosen\n\
      value to signify iteration is complete." },

  { "iterator?", 1, 0, 0, gdbscm_iterator_p,
    "\
Return #t if the object is a <gdb:iterator> object." },

  { "iterator-object", 1, 0, 0, gdbscm_iterator_object,
    "\
Return the object being iterated over." },

  { "iterator-progress", 1, 0, 0, gdbscm_iterator_progress,
    "\
Return the progress object of the iterator." },

  { "set-iterator-progress!", 2, 0, 0, gdbscm_set_iterator_progress_x,
    "\
Set the progress object of the iterator." },

  { "iterator-next!", 1, 0, 0, gdbscm_iterator_next_x,
    "\
Invoke the next! procedure of the iterator and return its result." },

  END_FUNCTIONS
};

void
gdbscm_initialize_iterators (void)
{
  iterator_smob_tag = gdbscm_make_smob_type (iterator_smob_name,
					     sizeof (iterator_smob));
  scm_set_smob_mark (iterator_smob_tag, itscm_mark_iterator_smob);
  scm_set_smob_print (iterator_smob_tag, itscm_print_iterator_smob);

  gdbscm_define_functions (iterator_functions, 1);
}
