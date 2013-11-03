/* Interface between gdb and its scripting languages.

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

/* Note: With few exceptions, external functions and variables in this file
   have "script" in the name, and no other symbol in gdb does.  */

#include "defs.h"
#include "auto-load.h"
#include "breakpoint.h"
#include "scripting.h"
#include "cli/cli-script.h"
#include "guile/guile.h"
#include "python/python.h"

/* High level description of a scripting language.
   One entry for each of Python and Guile is compiled into GDB regardless of
   whether the support is present.  This is done so that we can issue
   meaningful errors if the support is not compiled in.  */

struct script_language
{
  const char *name;
  const char *capitalized_name;
  const char *suffix;

  /* The suffix of per-objfile scripts to auto-load scripts.
     E.g. When the program loads libfoo.so, look for libfoo-gdb.${lang}.  */
  const char *auto_load_suffix;

  enum section_script_id section_script_id;

  /* Note: IWBN to record the auto-load script suffix here, but it is not
     because we also auto-load GDB scripts, and we don't want to fill/stub out
     the rest of the interface for GDB scripts just for that.
     So it is recorded elsewhere, in objfile_script_lang_interface.  */

  enum command_control_type cli_control_type;

  /* A function that will throw an "unsupported" error if called.
     This is used to implement script_ext_{soft,hard}.  */
  script_sourcer_func *source_script_unsupported;

  /* Either a pointer to the interface to the scripting language or NULL
     if the support is not compiled into GDB.  */
  const struct script_language_interface *interface;
};

static void source_python_unsupported (FILE *f, const char *file);
static void source_guile_unsupported (FILE *f, const char *file);

/* GDB's own scripting language.
   This exists to support auto-loading ${prog}-gdb.gdb scripts.  */

static const struct script_language script_lang_gdb =
{
  "gdb",
  "GDB",
  /* We fall back to interpreting a script as a GDB script if it doesn't
     match the other scripting languages, but for consistency's sake,
     give it a formal suffix.  */
  ".gdb",
  "-gdb.gdb",
  SECTION_SCRIPT_ID_GDB,
  /* cli_control_type: This is never used.  */
  invalid_control,
  /* source_script_unsupported: This is never used.  */
  NULL,
  /* The rest of the scripting interface isn't supported by GDB's
     builtin scripting language.  */
  NULL
};

/* The main struct describing GDB's interface to the Python
   scripting language.  */

static const struct script_language script_lang_python =
{
  "python",
  "Python",
  ".py",
  "-gdb.py",
  SECTION_SCRIPT_ID_PYTHON,
  python_control,
  source_python_unsupported,
#ifdef HAVE_PYTHON
  &python_scripting_interface
#else
  NULL
#endif
};

/* The main struct describing GDB's interface to the Guile
   scripting language.  */

static const struct script_language script_lang_guile =
{
  "guile",
  "Guile",
  ".scm",
  "-gdb.scm",
  SECTION_SCRIPT_ID_GUILE,
  guile_control,
  source_guile_unsupported,
#ifdef HAVE_GUILE
  &guile_scripting_interface
#else
  NULL
#endif
};

/* Table of all external (non-native) scripting languages.  */

static const struct script_language * const external_scripting_languages[] =
{
  /* Python appears before Guile to avoid breaking existing code if there are
     problems in the new Guile support.  E.g. if there's a Python
     pretty-printer then it has priority.  */
  &script_lang_python,
  &script_lang_guile,
  NULL
};

const struct script_language *
get_script_lang_gdb (void)
{
  return &script_lang_gdb;
}

const struct script_language *
get_script_lang_python (void)
{
  return &script_lang_python;
}

const struct script_language *
get_script_lang_guile (void)
{
  return &script_lang_guile;
}

/* Accessors for "public" attributes of struct script_language.

   IWBN if we could use slang_foo here and elsewhere, but we can't for fear of
   confusing someone into thinking we might be referring to the Slang
   programming language.  */

const char *
script_lang_name (const struct script_language *slang)
{
  return slang->name;
}

const char *
script_lang_capitalized_name (const struct script_language *slang)
{
  return slang->capitalized_name;
}

const char *
script_lang_suffix (const struct script_language *slang)
{
  return slang->suffix;
}

const char *
script_lang_auto_load_suffix (const struct script_language *slang)
{
  return slang->auto_load_suffix;
}

/* objfile_script_sourcer_func for native gdb scripts.
   Load GDB auto-script FILE,FILENAME for OBJFILE.  */

static void
source_gdb_script_for_objfile (struct objfile *objfile, FILE *file,
			       const char *filename)
{
  volatile struct gdb_exception e;

  TRY_CATCH (e, RETURN_MASK_ALL)
    {
      script_from_file (file, filename);
    }
  exception_print (gdb_stderr, e);
}

objfile_script_sourcer_func *
script_lang_objfile_sourcer (const struct script_language *slang)
{
  if (slang == &script_lang_gdb)
    return source_gdb_script_for_objfile;

  return slang->interface->objfile_script_sourcer;
}

int
script_lang_auto_load_enabled (const struct script_language *slang)
{
  if (slang->interface == NULL)
    return 0;
  return slang->interface->auto_load_enabled ();
}

/* Functions that iterate over all (external) scripting languages.  */

/* Iterate over all external scripting languages, regardless of whether the
   support has been compiled in or not.  */
#define ALL_EXT_SCRIPTING_LANGUAGES(i, slang) \
  for (/*int*/ i = 0, slang = external_scripting_languages[0]; \
       slang != NULL; \
       slang = external_scripting_languages[++i])

/* Iterate over all external scripting languages that are supported.  */
#define ALL_EXT_ENABLED_SCRIPTING_LANGUAGES(i, slang) \
  for (/*int*/ i = 0, slang = external_scripting_languages[0]; \
       slang != NULL; \
       slang = external_scripting_languages[++i]) \
    if (slang->interface != NULL)

void
finish_script_initialization (void)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      slang->interface->finish_initialization ();
    }
}

/* Throw UNSUPPORTED_ERROR if called.
   This is used to implement script_ext_{soft,hard}.  */

static void
source_python_unsupported (FILE *f, const char *file)
{
  throw_error (UNSUPPORTED_ERROR,
	       _("Python scripting is not supported in this copy of GDB."));
}

/* Throw UNSUPPORTED_ERROR if called.
   This is used to implement script_ext_{soft,hard}.  */

static void
source_guile_unsupported (FILE *f, const char *file)
{
  throw_error (UNSUPPORTED_ERROR,
	       _("Guile scripting is not supported in this copy of GDB."));
}

/* Return TRUE if FILE has extension EXTENSION.  */

static int
has_extension (const char *file, const char *extension)
{
  int file_len = strlen (file);
  int extension_len = strlen (extension);

  return (file_len > extension_len
	  && strcmp (&file[file_len - extension_len], extension) == 0);
}

/* Return a function to load FILE.
   If FILE specifies a scripting language we support but which is not
   enabled then return a function that throws UNSUPPORTED_ERROR.
   Otherwise return NULL.

   Note: This could be a lot cleaner if not for script_ext_soft.  */

script_sourcer_func *
get_script_sourcer (const char *file)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_SCRIPTING_LANGUAGES (i, slang)
    {
      if (has_extension (file, slang->suffix))
	{
	  if (slang->interface != NULL)
	    return slang->interface->script_sourcer;
	  return slang->source_script_unsupported;
	}
    }

  return NULL;
}

/* Store in *SPEC the data for loading a script in .debug_gdb_scripts with
   id ID.  ID is the first byte of the "record".
   If ID is recognized, returns non-zero.  Otherwise zero is returned.  */

int
get_section_script_lang (int id, const struct script_language **spec)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->section_script_id == id)
	{
	  *spec = slang;
	  return 1;
	}
    }

  return 0;
}

static const char *
script_lang_name_from_control_command (struct command_line *cmd)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->cli_control_type == cmd->control_type)
	return slang->capitalized_name;
    }

  gdb_assert_not_reached ("invalid scripting language in cli command");
}

void
eval_script_from_control_command (struct command_line *cmd)
{
  int i;
  const struct script_language *slang;
  const char *script_lang_name;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->cli_control_type == cmd->control_type)
	{
	  slang->interface->eval_from_control_command (cmd);
	  return;
	}
    }

  /* This requested scripting language is not supported.  */

  error (_("%s scripting is not supported in this copy of GDB."),
	 script_lang_name_from_control_command (cmd));
}

/* Load scripts for OBJFILE written in external languages (Python/Guile).  */

void
auto_load_ext_scripts_for_objfile (struct objfile *objfile)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (script_lang_auto_load_enabled (slang))
	auto_load_objfile_script (objfile, slang);
    }
}

struct script_type_printers *
start_script_type_printers (void)
{
  struct script_type_printers *printers =
    XZALLOC (struct script_type_printers);
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->start_type_printers != NULL)
	slang->interface->start_type_printers (printers);
    }

  return printers;
}

char *
apply_script_type_printers (struct script_type_printers *printers,
			    struct type *type)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      char *result = NULL;

      if (slang->interface->apply_type_printers != NULL)
	result = slang->interface->apply_type_printers (printers, type);

      if (result != NULL)
	return result;
    }

  return NULL;
}

void
free_script_type_printers (struct script_type_printers *printers)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->free_type_printers != NULL)
	slang->interface->free_type_printers (printers);
    }

  xfree (printers);
}

int
apply_val_script_pretty_printer (struct type *type, const gdb_byte *valaddr,
				 int embedded_offset, CORE_ADDR address,
				 struct ui_file *stream, int recurse,
				 const struct value *val,
				 const struct value_print_options *options,
				 const struct language_defn *language)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->apply_val_pretty_printer (type, valaddr,
						      embedded_offset, address,
						      stream, recurse, val,
						      options, language))
	return 1;
    }

  return 0;
}

enum script_bt_status
apply_script_frame_filter (struct frame_info *frame, int flags,
			   enum script_frame_args args_type,
			   struct ui_out *out,
			   int frame_low, int frame_high)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      enum script_bt_status status;

      if (slang->interface->apply_frame_filter == NULL)
	continue;
      status = slang->interface->apply_frame_filter (frame, flags,
						     args_type, out,
						     frame_low, frame_high);
      /* We use the filters from the first scripting language that has
	 applicable filters.  */
      if (status != SCR_BT_NO_FILTERS)
	return status;
    }

  return SCR_BT_NO_FILTERS;
}

void
preserve_script_values (struct objfile *objfile, htab_t copied_types)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      slang->interface->preserve_values (objfile, copied_types);
    }
}

int
breakpoint_has_script_cond (struct breakpoint *b)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->breakpoint_has_cond (b))
	return 1;
    }

  return 0;
}

int
breakpoint_script_cond_says_stop (struct breakpoint *b)
{
  int i;
  const struct script_language *slang;
  int stop = 0;

  /* N.B.: All conditions must be executed, even if, e.g., the first one
     says "stop".  Conditions may have side-effects.  */

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->breakpoint_has_cond (b))
	stop |= slang->interface->breakpoint_cond_says_stop (b);
    }

  return stop;
}

/* ^C/SIGINT support.
   This requires cooperation with the scripting languages so the support
   is defined here.
   The prototypes for these are in defs.h.

   TODO: Guile support.  */

/* Nonzero means a quit has been requested.
   This flag tracks quit requests but it's only used if a scripting language
   doesn't provide the necessary support.  */
static int quit_flag;

/* Clear the quit flag.  */

void
clear_quit_flag (void)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->clear_quit_flag != NULL)
	slang->interface->clear_quit_flag ();
    }

  quit_flag = 0;
}

/* Set the quit flag.  */

void
set_quit_flag (void)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->set_quit_flag != NULL)
	slang->interface->set_quit_flag ();
    }

  quit_flag = 1;
}

/* Return true if the quit flag has been set, false otherwise.
   Scripting languages may need their own control over whether SIGINT has
   been seen.  So scripting languages take priority over our quit_flag.  */

int
check_quit_flag (void)
{
  int i;
  const struct script_language *slang;

  ALL_EXT_ENABLED_SCRIPTING_LANGUAGES (i, slang)
    {
      if (slang->interface->check_quit_flag != NULL)
	return slang->interface->check_quit_flag ();
    }

  /* This is written in a particular way to avoid races.  */
  if (quit_flag)
    {
      quit_flag = 0;
      return 1;
    }

  return 0;
}

/* Some code (MI) wants to know if a particular scripting language
   successfully initialized.  */

/* Return non-zero if Python scripting successfully initialized.  */

int
script_lang_python_initialized (void)
{
  if (script_lang_python.interface != NULL)
    return script_lang_python.interface->initialized ();
  return 0;
}

/* Return non-zero if Guile scripting successfully initialized.  */

int
script_lang_guile_initialized (void)
{
  if (script_lang_guile.interface != NULL)
    return script_lang_guile.interface->initialized ();
  return 0;
}
