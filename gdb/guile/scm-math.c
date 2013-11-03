/* GDB/Scheme support for math operations on values.

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
#include "charset.h"
#include "cp-abi.h"
#include "doublest.h" /* Needed by dfp.h.  */
#include "expression.h" /* Needed by dfp.h.  */
#include "dfp.h"
#include "gdb_assert.h"
#include "symtab.h" /* Needed by language.h.  */
#include "language.h"
#include "valprint.h"
#include "value.h"
#include "guile-internal.h"

/* Note: Use target types here to remain consistent with the values system in
   GDB (which uses target arithmetic).  */

enum valscm_unary_opcode
{
  VALSCM_NOT,
  VALSCM_NEG,
  VALSCM_NOP,
  VALSCM_ABS,
  /* Note: This is Scheme's "logical not", not GDB's.
     GDB calls this UNOP_COMPLEMENT.  */
  VALSCM_LOGNOT
};

enum valscm_binary_opcode
{
  VALSCM_ADD,
  VALSCM_SUB,
  VALSCM_MUL,
  VALSCM_DIV,
  VALSCM_REM,
  VALSCM_MOD,
  VALSCM_POW,
  VALSCM_LSH,
  VALSCM_RSH,
  VALSCM_MIN,
  VALSCM_MAX,
  VALSCM_BITAND,
  VALSCM_BITOR,
  VALSCM_BITXOR
};

/* If TYPE is a reference, return the target; otherwise return TYPE.  */
#define STRIP_REFERENCE(TYPE) \
  ((TYPE_CODE (TYPE) == TYPE_CODE_REF) ? (TYPE_TARGET_TYPE (TYPE)) : (TYPE))

/* Returns a value object which is the result of applying the operation
   specified by OPCODE to the given argument.
   If there's an error a Scheme exception is thrown.  */

static SCM
vlscm_unop (enum valscm_unary_opcode opcode, SCM x, const char *func_name)
{
  struct gdbarch *gdbarch = get_current_arch ();
  const struct language_defn *language = current_language;
  struct value *arg1;
  SCM result = SCM_BOOL_F;
  struct value *res_val = NULL;
  SCM except_scm;
  struct cleanup *cleanups;
  volatile struct gdb_exception except;

  cleanups = make_cleanup_value_free_to_mark (value_mark ());

  arg1 = vlscm_convert_value_from_scheme (x, NULL, SCM_UNDEFINED,
					  SCM_ARG1, func_name, &except_scm,
					  gdbarch, language);
  if (arg1 == NULL)
    {
      do_cleanups (cleanups);
      gdbscm_throw (except_scm);
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      switch (opcode)
	{
	case VALSCM_NOT:
	  /* Alas gdb and guile use the opposite meaning for "logical not".  */
	  {
	    struct type *type = language_bool_type (language, gdbarch);
	    res_val =
	      value_from_longest (type, (LONGEST) value_logical_not (arg1));
	  }
	  break;
	case VALSCM_NEG:
	  res_val = value_neg (arg1);
	  break;
	case VALSCM_NOP:
	  /* Seemingly a no-op, but if X was a Scheme value it is now
	     a <gdb:value> object.  */
	  res_val = arg1;
	  break;
	case VALSCM_ABS:
	  if (value_less (arg1, value_zero (value_type (arg1), not_lval)))
	    res_val = value_neg (arg1);
	  else
	    res_val = arg1;
	  break;
	case VALSCM_LOGNOT:
	  res_val = value_complement (arg1);
	  break;
	default:
	  gdb_assert_not_reached ("unsupported operation");
	}
    }
  GDBSCM_HANDLE_GDB_EXCEPTION_WITH_CLEANUPS (except, cleanups);

  gdb_assert (res_val != NULL);
  result = vlscm_scm_from_value (res_val);

  do_cleanups (cleanups);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  return result;
}

/* Returns a value object which is the result of applying the operation
   specified by OPCODE to the given arguments.
   If there's an error a Scheme exception is thrown.  */

static SCM
vlscm_binop (enum valscm_binary_opcode opcode, SCM x, SCM y,
	     const char *func_name)
{
  struct gdbarch *gdbarch = get_current_arch ();
  const struct language_defn *language = current_language;
  struct value *arg1, *arg2;
  SCM result = SCM_BOOL_F;
  struct value *res_val = NULL;
  SCM except_scm;
  struct cleanup *cleanups;
  volatile struct gdb_exception except;

  cleanups = make_cleanup_value_free_to_mark (value_mark ());

  arg1 = vlscm_convert_value_from_scheme (x, NULL, SCM_UNDEFINED,
					  SCM_ARG1, func_name, &except_scm,
					  gdbarch, language);
  if (arg1 == NULL)
    {
      do_cleanups (cleanups);
      gdbscm_throw (except_scm);
    }
  arg2 = vlscm_convert_value_from_scheme (y, NULL, SCM_UNDEFINED,
					  SCM_ARG2, func_name, &except_scm,
					  gdbarch, language);
  if (arg2 == NULL)
    {
      do_cleanups (cleanups);
      gdbscm_throw (except_scm);
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      switch (opcode)
	{
	case VALSCM_ADD:
	  {
	    struct type *ltype = value_type (arg1);
	    struct type *rtype = value_type (arg2);

	    CHECK_TYPEDEF (ltype);
	    ltype = STRIP_REFERENCE (ltype);
	    CHECK_TYPEDEF (rtype);
	    rtype = STRIP_REFERENCE (rtype);

	    if (TYPE_CODE (ltype) == TYPE_CODE_PTR
		&& is_integral_type (rtype))
	      res_val = value_ptradd (arg1, value_as_long (arg2));
	    else if (TYPE_CODE (rtype) == TYPE_CODE_PTR
		     && is_integral_type (ltype))
	      res_val = value_ptradd (arg2, value_as_long (arg1));
	    else
	      res_val = value_binop (arg1, arg2, BINOP_ADD);
	  }
	  break;
	case VALSCM_SUB:
	  {
	    struct type *ltype = value_type (arg1);
	    struct type *rtype = value_type (arg2);

	    CHECK_TYPEDEF (ltype);
	    ltype = STRIP_REFERENCE (ltype);
	    CHECK_TYPEDEF (rtype);
	    rtype = STRIP_REFERENCE (rtype);

	    if (TYPE_CODE (ltype) == TYPE_CODE_PTR
		&& TYPE_CODE (rtype) == TYPE_CODE_PTR)
	      {
		/* A ptrdiff_t for the target would be preferable here.  */
		res_val =
		  value_from_longest (builtin_type (gdbarch)->builtin_long,
				      value_ptrdiff (arg1, arg2));
	      }
	    else if (TYPE_CODE (ltype) == TYPE_CODE_PTR
		     && is_integral_type (rtype))
	      res_val = value_ptradd (arg1, - value_as_long (arg2));
	    else
	      res_val = value_binop (arg1, arg2, BINOP_SUB);
	  }
	  break;
	case VALSCM_MUL:
	  res_val = value_binop (arg1, arg2, BINOP_MUL);
	  break;
	case VALSCM_DIV:
	  res_val = value_binop (arg1, arg2, BINOP_DIV);
	  break;
	case VALSCM_REM:
	  res_val = value_binop (arg1, arg2, BINOP_REM);
	  break;
	case VALSCM_MOD:
	  res_val = value_binop (arg1, arg2, BINOP_MOD);
	  break;
	case VALSCM_POW:
	  res_val = value_binop (arg1, arg2, BINOP_EXP);
	  break;
	case VALSCM_LSH:
	  res_val = value_binop (arg1, arg2, BINOP_LSH);
	  break;
	case VALSCM_RSH:
	  res_val = value_binop (arg1, arg2, BINOP_RSH);
	  break;
	case VALSCM_MIN:
	  res_val = value_binop (arg1, arg2, BINOP_MIN);
	  break;
	case VALSCM_MAX:
	  res_val = value_binop (arg1, arg2, BINOP_MAX);
	  break;
	case VALSCM_BITAND:
	  res_val = value_binop (arg1, arg2, BINOP_BITWISE_AND);
	  break;
	case VALSCM_BITOR:
	  res_val = value_binop (arg1, arg2, BINOP_BITWISE_IOR);
	  break;
	case VALSCM_BITXOR:
	  res_val = value_binop (arg1, arg2, BINOP_BITWISE_XOR);
	  break;
	default:
	  gdb_assert_not_reached ("unsupported operation");
	}
    }
  GDBSCM_HANDLE_GDB_EXCEPTION_WITH_CLEANUPS (except, cleanups);

  gdb_assert (res_val != NULL);
  result = vlscm_scm_from_value (res_val);

  do_cleanups (cleanups);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  return result;
}

/* (value-add x y) -> <gdb:value> */

static SCM
gdbscm_value_add (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_ADD, x, y, FUNC_NAME);
}

/* (value-sub x y) -> <gdb:value> */

static SCM
gdbscm_value_sub (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_SUB, x, y, FUNC_NAME);
}

/* (value-mul x y) -> <gdb:value> */

static SCM
gdbscm_value_mul (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_MUL, x, y, FUNC_NAME);
}

/* (value-div x y) -> <gdb:value> */

static SCM
gdbscm_value_div (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_DIV, x, y, FUNC_NAME);
}

/* (value-rem x y) -> <gdb:value> */

static SCM
gdbscm_value_rem (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_REM, x, y, FUNC_NAME);
}

/* (value-mod x y) -> <gdb:value> */

static SCM
gdbscm_value_mod (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_MOD, x, y, FUNC_NAME);
}

/* (value-pow x y) -> <gdb:value> */

static SCM
gdbscm_value_pow (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_POW, x, y, FUNC_NAME);
}

/* (value-neg x) -> <gdb:value> */

static SCM
gdbscm_value_neg (SCM x)
{
  return vlscm_unop (VALSCM_NEG, x, FUNC_NAME);
}

/* (value-pos x) -> <gdb:value> */

static SCM
gdbscm_value_pos (SCM x)
{
  return vlscm_unop (VALSCM_NOP, x, FUNC_NAME);
}

/* (value-abs x) -> <gdb:value> */

static SCM
gdbscm_value_abs (SCM x)
{
  return vlscm_unop (VALSCM_ABS, x, FUNC_NAME);
}

/* (value-lsh x y) -> <gdb:value> */

static SCM
gdbscm_value_lsh (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_LSH, x, y, FUNC_NAME);
}

/* (value-rsh x y) -> <gdb:value> */

static SCM
gdbscm_value_rsh (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_RSH, x, y, FUNC_NAME);
}

/* (value-min x y) -> <gdb:value> */

static SCM
gdbscm_value_min (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_MIN, x, y, FUNC_NAME);
}

/* (value-max x y) -> <gdb:value> */

static SCM
gdbscm_value_max (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_MAX, x, y, FUNC_NAME);
}

/* (value-not x) -> <gdb:value> */

static SCM
gdbscm_value_not (SCM x)
{
  return vlscm_unop (VALSCM_NOT, x, FUNC_NAME);
}

/* (value-lognot x) -> <gdb:value> */

static SCM
gdbscm_value_lognot (SCM x)
{
  return vlscm_unop (VALSCM_LOGNOT, x, FUNC_NAME);
}

/* (value-logand x y) -> <gdb:value> */

static SCM
gdbscm_value_logand (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_BITAND, x, y, FUNC_NAME);
}

/* (value-logior x y) -> <gdb:value> */

static SCM
gdbscm_value_logior (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_BITOR, x, y, FUNC_NAME);
}

/* (value-logxor x y) -> <gdb:value> */

static SCM
gdbscm_value_logxor (SCM x, SCM y)
{
  return vlscm_binop (VALSCM_BITXOR, x, y, FUNC_NAME);
}

/* Utility to perform all value comparisons.
   If there's an error a Scheme exception is thrown.  */

static SCM
vlscm_rich_compare (int op, SCM x, SCM y, const char *func_name)
{
  struct gdbarch *gdbarch = get_current_arch ();
  const struct language_defn *language = current_language;
  struct value *v1, *v2;
  int result = 0;
  SCM except_scm;
  struct cleanup *cleanups;
  volatile struct gdb_exception except;

  cleanups = make_cleanup_value_free_to_mark (value_mark ());

  v1 = vlscm_convert_value_from_scheme (x, NULL, SCM_UNDEFINED,
					SCM_ARG1, func_name, &except_scm,
					gdbarch, language);
  if (v1 == NULL)
    {
      do_cleanups (cleanups);
      gdbscm_throw (except_scm);
    }
  v2 = vlscm_convert_value_from_scheme (y, NULL, SCM_UNDEFINED,
					SCM_ARG2, func_name, &except_scm,
					gdbarch, language);
  if (v2 == NULL)
    {
      do_cleanups (cleanups);
      gdbscm_throw (except_scm);
    }

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      switch (op)
	{
        case BINOP_LESS:
	  result = value_less (v1, v2);
	  break;
	case BINOP_LEQ:
	  result = (value_less (v1, v2)
		    || value_equal (v1, v2));
	  break;
	case BINOP_EQUAL:
	  result = value_equal (v1, v2);
	  break;
	case BINOP_NOTEQUAL:
	  gdb_assert_not_reached ("not-equal not implemented");
        case BINOP_GTR:
	  result = value_less (v2, v1);
	  break;
	case BINOP_GEQ:
	  result = (value_less (v2, v1)
		    || value_equal (v1, v2));
	  break;
	default:
	  gdb_assert_not_reached ("invalid <gdb:value> comparison");
      }
    }
  do_cleanups (cleanups);
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  return scm_from_bool (result);
}

/* (value=? x y) -> boolean
   There is no "not-equal?" function (value!= ?) on purpose.
   We're following string=?, etc. as our Guide here.  */

static SCM
gdbscm_value_eq_p (SCM x, SCM y)
{
  return vlscm_rich_compare (BINOP_EQUAL, x, y, FUNC_NAME);
}

/* (value<? x y) -> boolean */

static SCM
gdbscm_value_lt_p (SCM x, SCM y)
{
  return vlscm_rich_compare (BINOP_LESS, x, y, FUNC_NAME);
}

/* (value<=? x y) -> boolean */

static SCM
gdbscm_value_le_p (SCM x, SCM y)
{
  return vlscm_rich_compare (BINOP_LEQ, x, y, FUNC_NAME);
}

/* (value>? x y) -> boolean */

static SCM
gdbscm_value_gt_p (SCM x, SCM y)
{
  return vlscm_rich_compare (BINOP_GTR, x, y, FUNC_NAME);
}

/* (value>=? x y) -> boolean */

static SCM
gdbscm_value_ge_p (SCM x, SCM y)
{
  return vlscm_rich_compare (BINOP_GEQ, x, y, FUNC_NAME);
}

/* Subroutine of vlscm_convert_value_from_scheme to simplify it.
   Convert OBJ, a Scheme number, to a <gdb:value> object.

   TYPE, if non-NULL, is the result type.  Otherwise, if OBJ is an integer,
   then the smallest int that will hold the value in the following progression
   is chosen:
   int, unsigned int, long, unsigned long, long long, unsigned long long.
   Otherwise, if OBJ is a real number, the it is converted to a double.

   If the number isn't representable, e.g. it's too big, a <gdb:exception>
   object is stored in *EXCEPT_SCMP and NULL is returned.
   The conversion may throw a gdb error, e.g., if TYPE is invalid.  */

static struct value *
vlscm_convert_number (SCM obj, struct type *type,
		      int arg_pos, const char *func_name,
		      struct gdbarch *gdbarch, SCM *except_scmp)
{
  const struct builtin_type *bt = builtin_type (gdbarch);

  if (scm_is_signed_integer (obj, LONG_MIN, LONG_MAX))
    {
      if (scm_is_signed_integer (obj, INT_MIN, INT_MAX))
	return value_from_longest (bt->builtin_int, scm_to_int (obj));
      if (scm_is_unsigned_integer (obj, 0, UINT_MAX))
	{
	  return value_from_longest (bt->builtin_unsigned_int,
				     scm_to_uint (obj));
	}
      return value_from_longest (bt->builtin_long, scm_to_long (obj));
    }
  else if (scm_is_unsigned_integer (obj, 0, ULONG_MAX))
    {
      return value_from_longest (bt->builtin_unsigned_long,
				 scm_to_uint (obj));
    }
  else if (sizeof (long long) > sizeof (long)
	   && sizeof (scm_t_intmax) >= sizeof (long long)
	   && scm_is_signed_integer (obj, LLONG_MIN, LLONG_MAX))
    {
      return value_from_longest (bt->builtin_long_long,
				 gdbscm_scm_to_longest (obj));
    }
  else if (sizeof (scm_t_uintmax) >= sizeof (unsigned long long)
	   && scm_is_unsigned_integer (obj, 0, ULLONG_MAX))
    {
      return value_from_longest (bt->builtin_unsigned_long_long,
				 gdbscm_scm_to_ulongest (obj));
    }
  else if (scm_is_real (obj))
    {
      return value_from_double (bt->builtin_double,
				scm_to_double (obj));
    }

  *except_scmp = gdbscm_make_type_error (func_name, arg_pos, obj, NULL);
  return NULL;
}

/* Subroutine of vlscm_convert_value_from_scheme to simplify it.
   Convert BV, a Scheme bytevector, to a <gdb:value> object.

   TYPE, if non-NULL, is the result type.  Otherwise, a vector of type
   uint8_t is used.
   TYPE_SCM is Scheme object wrapping TYPE, used in exception text,
   or #f if TYPE is NULL.

   If the bytevector isn't the same size as the type, then a <gdb:exception>
   object is stored in *EXCEPT_SCMP, and NULL is returned.  */

static struct value *
vlscm_convert_bytevector (SCM bv, struct type *type, SCM type_scm,
			  int arg_pos, const char *func_name,
			  SCM *except_scmp, struct gdbarch *gdbarch)
{
  LONGEST length = SCM_BYTEVECTOR_LENGTH (bv);
  struct value *value;

  if (type == NULL)
    {
      type = builtin_type (gdbarch)->builtin_uint8;
      type = lookup_array_range_type (type, 0, length);
      make_vector_type (type);
    }
  type = check_typedef (type);
  if (TYPE_LENGTH (type) != length)
    {
      *except_scmp = gdbscm_make_out_of_range_error (func_name, arg_pos,
						     type_scm,
			"size of type does not match size of bytevector");
      return NULL;
    }

  value = value_from_contents (type,
			       (gdb_byte *) SCM_BYTEVECTOR_CONTENTS (bv));
  return value;
}

/* Try to convert a Scheme value to a GDB value.

   TYPE, if non-NULL, is the result type which must be compatible with
   the value being converted.
   If TYPE is NULL then a suitable default type is chosen.
   TYPE_SCM is Scheme object wrapping TYPE, used in exception text,
   or SCM_UNDEFINED if TYPE is NULL.

   If the value cannot be converted, NULL is returned and a gdb:exception
   object is stored in *EXCEPT_SCMP.
   Otherwise the new value is returned, added to the all_values chain.  */

struct value *
vlscm_convert_value_from_scheme (SCM obj, struct type *type, SCM type_scm,
				 int arg_pos, const char *func_name,
				 SCM *except_scmp, struct gdbarch *gdbarch,
				 const struct language_defn *language)
{
  struct value *value = NULL;
  SCM except_scm = SCM_BOOL_F;
  volatile struct gdb_exception except;

  *except_scmp = SCM_BOOL_F;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      SCM scm;

      scm = vlscm_scm_to_value_gsmob (obj);
      if (vlscm_is_value (scm))
	{
	  value = value_copy (vlscm_scm_to_value (scm));
	}
      else if (gdbscm_is_exception (scm))
	{
	  except_scm = scm;
	  value = NULL;
	}
      else if (gdbscm_is_true (scm_bytevector_p (obj)))
	{
	  value = vlscm_convert_bytevector (obj, type, type_scm,
					    arg_pos, func_name,
					    &except_scm, gdbarch);
	}
      else if (gdbscm_is_bool (obj)) 
	{
	  value = value_from_longest (language_bool_type (language, gdbarch),
				      gdbscm_is_true (obj));
	}
      else if (scm_is_number (obj))
	{
	  value = vlscm_convert_number (obj, type, arg_pos, func_name, gdbarch,
					&except_scm);
	}
      else if (scm_is_string (obj))
	{
	  char *s;
	  size_t len;
	  struct cleanup *cleanup;

	  /* TODO: Provide option to select non-strict conversion?  */
	  s = gdbscm_scm_to_string (obj, &len,
				    target_charset (gdbarch), 1 /*strict*/,
				    &except_scm);
	  if (s != NULL)
	    {
	      cleanup = make_cleanup (xfree, s);
	      value =
		value_cstring (s, len,
			       language_string_char_type (language, gdbarch));
	      do_cleanups (cleanup);
	    }
	  else
	    value = NULL;
	}
      /* Note: scm is assigned to here.  */
      else if (lsscm_is_lazy_string (scm =
				     lsscm_scm_to_lazy_string_gsmob (obj)))
	{
	  SCM string = lsscm_safe_call_lazy_string_to_value (obj);

	  if (gdbscm_is_exception (string))
	    {
	      except_scm = string;
	      value = NULL;
	    }
	  else
	    value = value_copy (vlscm_scm_to_value (string));
	}
      else if (gdbscm_is_exception (scm))
	{
	  except_scm = scm;
	  value = NULL;
	}
      else
	{
	  except_scm = gdbscm_make_type_error (func_name, arg_pos, obj, NULL);
	  value = NULL;
	}
    }
  if (except.reason < 0)
    except_scm = gdbscm_scm_from_gdb_exception (except);

  if (gdbscm_is_true (except_scm))
    {
      gdb_assert (value == NULL);
      *except_scmp = except_scm;
    }

  return value;
}

/* Initialize value math support.  */

static const scheme_function math_functions[] =
{
  { "value-add", 2, 0, 0, gdbscm_value_add,
    "\
Return a + b." },

  { "value-sub", 2, 0, 0, gdbscm_value_sub,
    "\
Return a - b." },

  { "value-mul", 2, 0, 0, gdbscm_value_mul,
    "\
Return a * b." },

  { "value-div", 2, 0, 0, gdbscm_value_div,
    "\
Return a / b." },

  { "value-rem", 2, 0, 0, gdbscm_value_rem,
    "\
Return a % b." },

  { "value-mod", 2, 0, 0, gdbscm_value_mod,
    "\
Return a mod b.  See Knuth 1.2.4." },

  { "value-pow", 2, 0, 0, gdbscm_value_pow,
    "\
Return pow (x, y)." },

  { "value-not", 1, 0, 0, gdbscm_value_not,
    "\
Return !a." },

  { "value-neg", 1, 0, 0, gdbscm_value_neg,
    "\
Return -a." },

  { "value-pos", 1, 0, 0, gdbscm_value_pos,
    "\
Return a." },

  { "value-abs", 1, 0, 0, gdbscm_value_abs,
    "\
Return abs (a)." },

  { "value-lsh", 2, 0, 0, gdbscm_value_lsh,
    "\
Return a << b." },

  { "value-rsh", 2, 0, 0, gdbscm_value_rsh,
    "\
Return a >> b." },

  { "value-min", 2, 0, 0, gdbscm_value_min,
    "\
Return min (a, b)." },

  { "value-max", 2, 0, 0, gdbscm_value_max,
    "\
Return max (a, b)." },

  { "value-lognot", 1, 0, 0, gdbscm_value_lognot,
    "\
Return ~a." },

  { "value-logand", 2, 0, 0, gdbscm_value_logand,
    "\
Return a & b." },

  { "value-logior", 2, 0, 0, gdbscm_value_logior,
    "\
Return a | b." },

  { "value-logxor", 2, 0, 0, gdbscm_value_logxor,
    "\
Return a ^ b." },

  { "value=?", 2, 0, 0, gdbscm_value_eq_p,
    "\
Return a == b." },

  { "value<?", 2, 0, 0, gdbscm_value_lt_p,
    "\
Return a < b." },

  { "value<=?", 2, 0, 0, gdbscm_value_le_p,
    "\
Return a <= b." },

  { "value>?", 2, 0, 0, gdbscm_value_gt_p,
    "\
Return a > b." },

  { "value>=?", 2, 0, 0, gdbscm_value_ge_p,
    "\
Return a >= b." },

  END_FUNCTIONS
};

void
gdbscm_initialize_math (void)
{
  gdbscm_define_functions (math_functions, 1);
}
