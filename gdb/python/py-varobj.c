/* Implementation of the GDB variable objects API.

   Copyright (C) 1999-2013 Free Software Foundation, Inc.

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

#include "defs.h"
#include "exceptions.h"
#include "gdb_vecs.h"
#include "gdbtypes.h"
#include "symtab.h"
#include "language.h"
#include "valprint.h"
#include "varobj.h"
#include "python.h"
#include "python-internal.h"

/* Dynamic part of varobj.  */

struct varobj_dynamic
{
  /* Whether the children of this varobj were requested.  This field is
     used to decide if dynamic varobj should recompute their children.
     In the event that the frontend never asked for the children, we
     can avoid that.  */
  int children_requested;

  /* The pretty-printer constructor.  If NULL, then the default
     pretty-printer will be looked up.  If None, then no
     pretty-printer will be installed.  */
  PyObject *constructor;

  /* The pretty-printer that has been constructed.  If NULL, then a
     new printer object is needed, and one will be constructed.  */
  PyObject *pretty_printer;

  /* The iterator returned by the printer's 'children' method, or NULL
     if not available.  */
  PyObject *child_iter;

  /* We request one extra item from the iterator, so that we can
     report to the caller whether there are more items than we have
     already reported.  However, we don't want to install this value
     when we read it, because that will mess up future updates.  So,
     we stash it here instead.  */
  PyObject *saved_item;
};

/* Helper function to install a Python environment suitable for
   use during operations on VAR.  */

static struct cleanup *
varobj_ensure_python_env (struct varobj *var)
{
  struct expression *exp = varobj_get_gdb_expression (var);

  return ensure_python_env (exp->gdbarch,
			    exp->language_defn);
}

/* Helper function for construct_visualizer.  Instantiate a
   pretty-printer for a given value.  */

static PyObject *
instantiate_pretty_printer (PyObject *constructor, struct value *value)
{
  PyObject *val_obj = NULL; 
  PyObject *printer;

  val_obj = value_to_value_object (value);
  if (! val_obj)
    return NULL;

  printer = PyObject_CallFunctionObjArgs (constructor, val_obj, NULL);
  Py_DECREF (val_obj);
  return printer;
}

/* A helper function to install a constructor function and visualizer
   in a varobj.  */

static void
install_visualizer (struct varobj *var, PyObject *constructor,
		    PyObject *visualizer)
{
  struct varobj_dynamic *dyn = var->dynamic;

  Py_XDECREF (dyn->constructor);
  dyn->constructor = constructor;

  Py_XDECREF (dyn->pretty_printer);
  dyn->pretty_printer = visualizer;

  Py_XDECREF (dyn->child_iter);
  dyn->child_iter = NULL;
}

/* Instantiate and install a visualizer for VAR using CONSTRUCTOR to
   make a new object.  */

static void
construct_visualizer (struct varobj *var, PyObject *constructor)
{
  PyObject *pretty_printer;

  Py_INCREF (constructor);
  if (constructor == Py_None)
    pretty_printer = NULL;
  else
    {
      pretty_printer = instantiate_pretty_printer (constructor, var->value);
      if (! pretty_printer)
	{
	  gdbpy_print_stack ();
	  Py_DECREF (constructor);
	  constructor = Py_None;
	  Py_INCREF (constructor);
	}

      if (pretty_printer == Py_None)
	{
	  Py_DECREF (pretty_printer);
	  pretty_printer = NULL;
	}
    }

  install_visualizer (var, constructor, pretty_printer);
}

/* Look up and install VISUALIZER.
   Returns zero if Python is not initialized and this can't work,
   non-zero for success.
   An exception is thrown if there's a problem evaluating VISUALIZER.  */

int
gdbpy_varobj_set_visualizer (struct varobj *var, const char *visualizer)
{
  PyObject *mainmod, *globals, *constructor;
  struct cleanup *back_to;

  if (!gdb_python_initialized)
    return 0;

  back_to = varobj_ensure_python_env (var);

  mainmod = PyImport_AddModule ("__main__");
  globals = PyModule_GetDict (mainmod);
  Py_INCREF (globals);
  make_cleanup_py_decref (globals);

  constructor = PyRun_String (visualizer, Py_eval_input, globals, globals);

  if (! constructor)
    {
      gdbpy_print_stack ();
      error (_("Could not evaluate visualizer expression: %s"), visualizer);
    }

  construct_visualizer (var, constructor);
  Py_XDECREF (constructor);

  do_cleanups (back_to);

  return 1;
}

/* Install the default visualizer for VAR, if enabled.
   This is called when no constructor has been set.  */

static void
install_default_visualizer (struct varobj *var)
{
  PyObject *pretty_printer = NULL;

  if (var->value)
    {
      pretty_printer = gdbpy_get_varobj_pretty_printer (var->value);
      if (! pretty_printer)
	{
	  gdbpy_print_stack ();
	  error (_("Cannot instantiate printer for default visualizer"));
	}
    }

  if (pretty_printer == Py_None)
    {
      Py_DECREF (pretty_printer);
      pretty_printer = NULL;
    }

  install_visualizer (var, NULL, pretty_printer);
}

/* Install the visualizer for a new value.
   We rerun the constructor or re-lookup the pretty-printer in case the
   type of the value has changed (e.g. the object is a pointer to a different
   kind of derived c++ class).  */

void
gdbpy_install_new_value_visualizer (struct varobj *var,
				    int default_visualizer_enabled)
{
  struct cleanup *cleanup;
  struct varobj_dynamic *dyn = var->dynamic;

  if (!gdb_python_initialized)
    return;

  cleanup = varobj_ensure_python_env (var);

  if (dyn->constructor)
    {
      /* If the constructor has been explicitly set to None, then we want
	 the raw value.  */
      if (dyn->constructor != Py_None)
	construct_visualizer (var, dyn->constructor);
    }
  else
    {
      /* Don't find a visualizer the default way if it has been disabled.  */
      if (default_visualizer_enabled)
	install_default_visualizer (var);
    }

  do_cleanups (cleanup);
}

/* This is the script_language.update_dynamic_varobj_children "method".
   The result is the (possibly new) number of children or -1 if there's
   an error.  */

int
gdbpy_update_dynamic_varobj_children (struct varobj *var,
				      struct dynamic_child_data *child_data,
				      int update_children,
				      int from, int to)
{
  struct cleanup *back_to;
  PyObject *children;
  int i;
  struct varobj_dynamic *dyn = var->dynamic;
  PyObject *printer = dyn->pretty_printer;

  back_to = varobj_ensure_python_env (var);

  if (!PyObject_HasAttr (printer, gdbpy_children_cst))
    {
      do_cleanups (back_to);
      return -1;
    }

  if (update_children || !dyn->child_iter)
    {
      children = PyObject_CallMethodObjArgs (printer, gdbpy_children_cst,
					     NULL);

      if (!children)
	{
	  gdbpy_print_stack ();
	  error (_("Null value returned for children"));
	}

      make_cleanup_py_decref (children);

      Py_XDECREF (dyn->child_iter);
      dyn->child_iter = PyObject_GetIter (children);
      if (!dyn->child_iter)
	{
	  gdbpy_print_stack ();
	  error (_("Could not get children iterator"));
	}

      Py_XDECREF (dyn->saved_item);
      dyn->saved_item = NULL;

      i = 0;
    }
  else
    i = VEC_length (varobj_p, var->children);

  /* We ask for one extra child, so that MI can report whether there
     are more children.  */
  for (; to < 0 || i < to + 1; ++i)
    {
      PyObject *item;
      int force_done = 0;

      /* See if there was a leftover from last time.  */
      if (dyn->saved_item)
	{
	  item = dyn->saved_item;
	  dyn->saved_item = NULL;
	}
      else
	item = PyIter_Next (dyn->child_iter);

      if (!item)
	{
	  /* Normal end of iteration.  */
	  if (!PyErr_Occurred ())
	    break;

	  /* If we got a memory error, just use the text as the
	     item.  */
	  if (PyErr_ExceptionMatches (gdbpy_gdb_memory_error))
	    {
	      PyObject *type, *value, *trace;
	      char *name_str, *value_str;

	      PyErr_Fetch (&type, &value, &trace);
	      value_str = gdbpy_exception_to_string (type, value);
	      Py_XDECREF (type);
	      Py_XDECREF (value);
	      Py_XDECREF (trace);
	      if (!value_str)
		{
		  gdbpy_print_stack ();
		  break;
		}

	      name_str = xstrprintf ("<error at %d>", i);
	      item = Py_BuildValue ("(ss)", name_str, value_str);
	      xfree (name_str);
	      xfree (value_str);
	      if (!item)
		{
		  gdbpy_print_stack ();
		  break;
		}

	      force_done = 1;
	    }
	  else
	    {
	      /* Any other kind of error.  */
	      gdbpy_print_stack ();
	      break;
	    }
	}

      /* We don't want to push the extra child on any report list.  */
      if (to < 0 || i < to)
	{
	  PyObject *py_v;
	  const char *name;
	  struct value *v;
	  struct cleanup *inner;
	  int can_mention = from < 0 || i >= from;

	  inner = make_cleanup_py_decref (item);

	  if (!PyArg_ParseTuple (item, "sO", &name, &py_v))
	    {
	      gdbpy_print_stack ();
	      error (_("Invalid item from the child list"));
	    }

	  v = convert_value_from_python (py_v);
	  if (v == NULL)
	    gdbpy_print_stack ();
	  varobj_install_dynamic_child (var,
					can_mention ? child_data : NULL,
					i, xstrdup (name), v);
	  do_cleanups (inner);
	}
      else
	{
	  Py_XDECREF (dyn->saved_item);
	  dyn->saved_item = item;

	  /* We want to truncate the child list just before this
	     element.  */
	  break;
	}

      if (force_done)
	break;
    }

  do_cleanups (back_to);

  return i;
}

/* This is the script_language.get_varobj_display_hint "method".  */

char *
gdbpy_get_varobj_display_hint (struct varobj *var)
{
  char *result = NULL;
  struct cleanup *back_to;
  struct varobj_dynamic *dyn = var->dynamic;

  if (!gdb_python_initialized)
    return NULL;

 xyzdje: // TODO: var here is a varobj

  back_to = varobj_ensure_python_env (var);

  if (dyn->pretty_printer)
    result = gdbpy_get_display_hint (dyn->pretty_printer);

  do_cleanups (back_to);

  return result;
}

/* This is the script_language get_print_value "method".
   The result is non-zero for success, zero for failure.  */

int
gdbpy_get_varobj_print_value (struct varobj *var,
			      enum varobj_display_formats format,
			      struct value *value, struct ui_file *stb)
{
  struct cleanup *old_chain;
  char *thevalue = NULL;
  struct value_print_options opts;
  struct type *type = NULL;
  long len = 0;
  char *encoding = NULL;
  /* Initialize it just to avoid a GCC false warning.  */
  CORE_ADDR str_addr = 0;
  int string_print = 0;
  PyObject *value_formatter = var->dynamic->pretty_printer;
  struct value *replacement;
  PyObject *output;

  if (!gdb_python_initialized)
    return 0;
  if (value_formatter == NULL)
    return 0;

  old_chain = varobj_ensure_python_env (var);

  /* First check to see if we have any children at all.  If so,
     we simply return {...}.  */
  if (PyObject_HasAttr (value_formatter, gdbpy_children_cst))
    {
      fputs_filtered ("{...}", stb);
      do_cleanups (old_chain);
      return 1;
    }

  /* A pretty-printer with neither to_string nor children methods is kinda
     broken, but the previous code didn't flag it, so we can't.  */
  if (!PyObject_HasAttr (value_formatter, gdbpy_to_string_cst))
    {
      do_cleanups (old_chain);
      return 0;
    }

  output = apply_varobj_pretty_printer (value_formatter, &replacement, stb);

  /* If we have string like output ...  */
  if (output)
    {
      make_cleanup_py_decref (output);

      /* If this is a lazy string, extract it.  For lazy
	 strings we always print as a string, so set
	 string_print.  */
      if (gdbpy_is_lazy_string (output))
	{
	  gdbpy_extract_lazy_string (output, &str_addr, &type,
				     &len, &encoding);
	  make_cleanup (free_current_contents, &encoding);
	  string_print = 1;
	}
      else
	{
	  /* If it is a regular (non-lazy) string, extract
	     it and copy the contents into THEVALUE.  If the
	     hint says to print it as a string, set
	     string_print.  Otherwise just return the extracted
	     string as a value.  */

	  char *s = python_string_to_target_string (output);

	  if (s)
	    {
	      struct gdbarch *gdbarch;
	      char *hint;

	      hint = gdbpy_get_display_hint (value_formatter);
	      if (hint)
		{
		  if (!strcmp (hint, "string"))
		    string_print = 1;
		  xfree (hint);
		}

	      if (!string_print)
		{
		  fputs_filtered (s, stb);
		  do_cleanups (old_chain);
		  return 1;
		}

	      thevalue = s;
	      gdbarch = get_type_arch (value_type (value));
	      type = builtin_type (gdbarch)->builtin_char;

	      make_cleanup (xfree, thevalue);
	    }
	  else
	    gdbpy_print_stack ();
	}
    }
  /* If the printer returned a replacement value, set VALUE
     to REPLACEMENT.  If there is not a replacement value,
     just use the value passed to this function.  */
  if (replacement)
    value = replacement;

  varobj_raw_formatted_print_options (&opts, format);

  /* If the THEVALUE has contents, it is a regular string.  */
  if (thevalue)
    LA_PRINT_STRING (stb, type, (gdb_byte *) thevalue, len, encoding, 0, &opts);
  else if (string_print)
    /* Otherwise, if string_print is set, and it is not a regular
       string, it is a lazy string.  */
    val_print_string (type, encoding, str_addr, len, stb, &opts);
  else
    /* All other cases.  */
    common_val_print (value, stb, 0, &opts, current_language);

  do_cleanups (old_chain);
  return 1;
}

void
gdbpy_varobj_alloc_variable (struct varobj *var)
{
  struct varobj_dynamic *dyn = xmalloc (sizeof (*dyn));

  dyn->children_requested = 0;
  dyn->constructor = NULL;
  dyn->pretty_printer = NULL;
  dyn->child_iter = NULL;
  dyn->saved_item = NULL;

  var->dynamic = dyn;
}

void
gdbpy_varobj_free_variable (struct varobj *var)
{
  struct varobj_dynamic *dyn = var->dynamic;

  if (dyn->pretty_printer != NULL)
    {
      struct cleanup *cleanup = varobj_ensure_python_env (var);

      Py_XDECREF (dyn->constructor);
      Py_XDECREF (dyn->pretty_printer);
      Py_XDECREF (dyn->child_iter);
      Py_XDECREF (dyn->saved_item);

      do_cleanups (cleanup);
    }

  xfree (dyn);
  var->dynamic = NULL;
}

/* Set the flag to indicate children have been requested.  */

void
gdbpy_varobj_set_children_requested (struct varobj *var)
{
  var->dynamic->children_requested = 1;
}

/* Return non-zero if children have been requested.  */

int
gdbpy_varobj_children_requested_p (struct varobj *var)
{
  return var->dynamic->children_requested;
}

/* Return true if VAR has a saved item.  */

int
gdbpy_varobj_has_saved_item (struct varobj *var)
{
  return var->dynamic->saved_item != NULL;
}

/* Return non-zero if VAR has been pretty-printed.  */

int
gdbpy_varobj_pretty_printed_p (struct varobj *var)
{
  return var->dynamic->pretty_printer != NULL;
}
