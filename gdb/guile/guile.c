/* General GDB/Guile code.

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

#include "defs.h"
#include <stdarg.h>
#include "breakpoint.h"
#include "cli/cli-cmds.h"
#include "cli/cli-script.h"
#include "cli/cli-utils.h"
#include "command.h"
#include "gdbcmd.h"
#include "interps.h"
#include "scripting.h"
#include "utils.h"
#include "version.h"
#ifdef HAVE_GUILE
#include "guile.h"
#include "guile-internal.h"
#endif

/* Declared constants and enum for guile exception printing.  */
const char gdbscm_print_excp_none[] = "none";
const char gdbscm_print_excp_full[] = "full";
const char gdbscm_print_excp_message[] = "message";

/* "set guile print-stack" choices.  */
static const char *const guile_print_excp_enums[] =
  {
    gdbscm_print_excp_none,
    gdbscm_print_excp_full,
    gdbscm_print_excp_message,
    NULL
  };

/* The exception printing variable.  'full' if we want to print the
   error message and stack, 'none' if we want to print nothing, and
   'message' if we only want to print the error message.  'message' is
   the default.  */
const char *gdbscm_print_excp = gdbscm_print_excp_message;

#ifdef HAVE_GUILE

static void gdbscm_finish_initialization (void);
static int gdbscm_initialized (void);
static void gdbscm_eval_from_control_command (struct command_line *);
static script_sourcer_func gdbscm_source_script;

int gdb_scheme_initialized;

/* Symbol for setting documentation strings.  */
SCM gdbscm_documentation_symbol;

/* Keywords used by various functions.  */
static SCM from_tty_keyword;
static SCM to_string_keyword;

/* The name of the various modules (without the surrounding parens).  */
const char gdbscm_module_name[] = "gdb";
const char gdbscm_init_module_name[] = "gdb init";

/* The name of the bootstrap file.  */
static const char boot_scm_filename[] = "boot.scm";

/* Name of the variable containing GDB's data-directory.  */
static const char data_directory_name[] = "*data-directory*";

/* The interface between gdb proper and guile scripting.  */

const struct script_language_interface guile_scripting_interface =
{
  gdbscm_finish_initialization,
  gdbscm_initialized,

  gdbscm_source_script,
  gdbscm_source_objfile_script,
  gdbscm_auto_load_enabled,

  gdbscm_eval_from_control_command,

  NULL, /* gdbscm_start_type_printers, */
  NULL, /* gdbscm_apply_type_printers, */
  NULL, /* gdbscm_free_type_printers, */

  gdbscm_apply_val_pretty_printer,

  NULL, /* gdbscm_apply_frame_filter, */

  gdbscm_preserve_values,

  gdbscm_breakpoint_has_cond,
  gdbscm_breakpoint_cond_says_stop,

  NULL, /* gdbscm_check_quit_flag, */
  NULL, /* gdbscm_clear_quit_flag, */
  NULL, /* gdbscm_set_quit_flag, */
};

/* Implementation of the gdb "guile-interactive" command.  */

static void
guile_interactive_command (char *arg, int from_tty)
{
  struct cleanup *cleanup;

  cleanup = make_cleanup_restore_integer (&interpreter_async);
  interpreter_async = 0;

  arg = skip_spaces (arg);

  /* This explicitly rejects any arguments for now.
     "It is easier to relax a restriction than impose one after the fact."
     We would *like* to be able to pass arguments to the interactive shell
     but that's not what python-interactive does.  Until there is time to
     sort it out, we forbid arguments.  */

  if (arg && *arg)
    error (_("guile-interactive currently does not take any arguments."));
  else
    {
      dont_repeat ();
      gdbscm_enter_repl ();
    }

  do_cleanups (cleanup);
}

/* Implementation of the gdb "guile" command.
   Note: Contrary to the Python version this displays the result.
   Have to see which is better.

   TODO: Add the result to Guile's history?  */

static void
guile_command (char *arg, int from_tty)
{
  struct cleanup *cleanup;

  cleanup = make_cleanup_restore_integer (&interpreter_async);
  interpreter_async = 0;

  arg = skip_spaces (arg);

  if (arg && *arg)
    {
      char *msg = gdbscm_safe_eval_string (arg, 1);

      if (msg != NULL)
	{
	  make_cleanup (xfree, msg);
	  error ("%s", msg);
	}
    }
  else
    {
      struct command_line *l = get_command_line (guile_control, "");

      make_cleanup_free_command_lines (&l);
      execute_control_command_untraced (l);
    }

  do_cleanups (cleanup);
}

/* Given a command_line, return a command string suitable for passing
   to Guile.  Lines in the string are separated by newlines.  The return
   value is allocated using xmalloc and the caller is responsible for
   freeing it.  */

static char *
compute_scheme_string (struct command_line *l)
{
  struct command_line *iter;
  char *script = NULL;
  int size = 0;
  int here;

  for (iter = l; iter; iter = iter->next)
    size += strlen (iter->line) + 1;

  script = xmalloc (size + 1);
  here = 0;
  for (iter = l; iter; iter = iter->next)
    {
      int len = strlen (iter->line);

      strcpy (&script[here], iter->line);
      here += len;
      script[here++] = '\n';
    }
  script[here] = '\0';
  return script;
}

/* Take a command line structure representing a "guile" command, and
   evaluate its body using the Guile interpreter.
   This is the script_lang.eval_from_control_command "method".  */

static void
gdbscm_eval_from_control_command (struct command_line *cmd)
{
  char *script, *msg;
  struct cleanup *cleanup;

  if (cmd->body_count != 1)
    error (_("Invalid \"guile\" block structure."));

  cleanup = make_cleanup (null_cleanup, NULL);

  script = compute_scheme_string (cmd->body_list[0]);
  msg = gdbscm_safe_eval_string (script, 0);
  xfree (script);
  if (msg != NULL)
    {
      make_cleanup (xfree, msg);
      error ("%s", msg);
    }

  do_cleanups (cleanup);
}

/* Read a file as Scheme code.
   This is the script_lang.source_script "method".
   FILE is the file to run.  FILENAME is name of the file FILE.
   This does not throw any errors.  If an exception occurs an error message
   is printed.  */

static void
gdbscm_source_script (FILE *file, const char *filename)
{
  char *msg = gdbscm_safe_source_script (filename);

  if (msg != NULL)
    {
      fprintf_filtered (gdb_stderr, "%s\n", msg);
      xfree (msg);
    }
}

/* A Scheme function which evaluates a string using the gdb CLI.  */

static SCM
gdbscm_execute_gdb_command (SCM command_scm, SCM rest)
{
  int from_tty_arg_pos = -1, to_string_arg_pos = -1;
  int from_tty = 0, to_string = 0;
  volatile struct gdb_exception except;
  const SCM keywords[] = { from_tty_keyword, to_string_keyword, SCM_BOOL_F };
  char *command;
  char *result = NULL;
  struct cleanup *cleanups;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, keywords, "s#tt",
			      command_scm, &command, rest,
			      &from_tty_arg_pos, &from_tty,
			      &to_string_arg_pos, &to_string);

  /* Note: The contents of "command" may get modified while it is
     executed.  */
  cleanups = make_cleanup (xfree, command);

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      struct cleanup *inner_cleanups;

      inner_cleanups = make_cleanup_restore_integer (&interpreter_async);
      interpreter_async = 0;

      prevent_dont_repeat ();
      if (to_string)
	result = execute_command_to_string (command, from_tty);
      else
	{
	  execute_command (command, from_tty);
	  result = NULL;
	}

      /* Do any commands attached to breakpoint we stopped at.  */
      bpstat_do_actions ();

      do_cleanups (inner_cleanups);
    }
  do_cleanups (cleanups);
  GDBSCM_HANDLE_GDB_EXCEPTION (except);

  if (result)
    {
      SCM r = gdbscm_scm_from_c_string (result);
      xfree (result);
      return r;
    }
  return SCM_UNSPECIFIED;
}

#else /* ! HAVE_GUILE */

/* Dummy implementation of the gdb "guile-interactive" and "guile"
   commands. */

static void
guile_interactive_command (char *arg, int from_tty)
{
  arg = skip_spaces (arg);
  if (arg && *arg)
    error (_("guile-interactive currently does not take any arguments."));
  error (_("Guile scripting is not supported in this copy of GDB."));
}

static void
guile_command (char *arg, int from_tty)
{
  arg = skip_spaces (arg);
  if (arg && *arg)
    error (_("Guile scripting is not supported in this copy of GDB."));
  else
    {
      /* Even if Guile isn't enabled, we still have to slurp the
	 command list to the corresponding "end".  */
      struct command_line *l = get_command_line (guile_control, "");
      struct cleanup *cleanups = make_cleanup_free_command_lines (&l);

      execute_control_command_untraced (l);
      do_cleanups (cleanups);
    }
}

#endif /* ! HAVE_GUILE */

/* Lists for 'set,show,info guile' commands.  */

static struct cmd_list_element *set_guile_list;
static struct cmd_list_element *show_guile_list;
static struct cmd_list_element *info_guile_list;

/* Function for use by 'set guile' prefix command.  */

static void
set_guile_command (char *args, int from_tty)
{
  help_list (set_guile_list, "set guile ", all_commands, gdb_stdout);
}

/* Function for use by 'show guile' prefix command.  */

static void
show_guile_command (char *args, int from_tty)
{
  cmd_show_list (show_guile_list, from_tty, "");
}

/* The "info scheme" command is defined as a prefix, with
   allow_unknown 0.  Therefore, its own definition is called only for
   "info scheme" with no args.  */

static void
info_guile_command (char *args, int from_tty)
{
  printf_unfiltered (_("\"info guile\" must be followed"
		       " by the name of an info command.\n"));
  help_list (info_guile_list, "info guile ", -1, gdb_stdout);
}

/* Initialization.  */

#ifdef HAVE_GUILE

static const scheme_function misc_guile_functions[] =
{
  { "execute", 1, 0, 1, gdbscm_execute_gdb_command,
  "\
Execute the given GDB command.\n\
\n\
  Arguments: string [#:to-string boolean] [#:from-tty boolean]\n\
    If #:to-string is true then the result is returned as a string.\n\
    If #:from-tty is true then the command executes as if entered\n\
    from the keyboard.\n\
  Returns: The result of the command if #:to-string is true.\n\
    Otherwise returns unspecified." },

  END_FUNCTIONS
};

/* Load gdb/boot.scm, the Scheme side of GDB/Guile support.
   Note: This function assumes it's called within the gdb module.  */

static void
initialize_scheme_side (void)
{
  char *gdb_guile_dir = concat (gdb_datadir, SLASH_STRING, "guile", NULL);
  char *boot_scm_path = concat (gdb_guile_dir, SLASH_STRING, "gdb",
				SLASH_STRING, boot_scm_filename, NULL);
  char *msg;

  /* While scm_c_primitive_load works, the loaded code is not compiled,
     instead it is left to be interpreted.  Eh?
     Anyways, this causes a ~100x slowdown, so we only use it to load
     gdb/boot.scm, and then let boot.scm do the rest.
     We do however have to tell boot.scm where GDB's data-directory is.  */
  scm_c_define (data_directory_name, gdbscm_scm_from_c_string (gdb_datadir));
  scm_c_export (data_directory_name, NULL);

  msg = gdbscm_safe_source_script (boot_scm_path);

  if (msg != NULL)
    {
      fprintf_filtered (gdb_stderr, "%s", msg);
      xfree (msg);
      warning (_("\n"
		 "Could not complete Guile gdb module initialization from:\n"
		 "%s.\n"
		 "Limited Guile support is available.\n"
		 "Suggest passing --data-directory=/path/to/gdb/data-directory.\n"),
	       boot_scm_path);
    }

  xfree (gdb_guile_dir);
  xfree (boot_scm_path);
}

/* Install the gdb scheme module.
   The result is a boolean indicating success.
   If initializing the gdb module fails an error message is printed.
   Note: This function runs in the context of the gdb module.  */

static void
initialize_gdb_module (void *data)
{
  /* The documentation symbol needs to be defined before any calls to
     gdbscm_define_{variables,functions}.  */
  gdbscm_documentation_symbol = gdbscm_symbol_from_c_string ("documentation");

  /* The smob and exception support must be initialized early.  */
  gdbscm_initialize_smobs ();
  gdbscm_initialize_exceptions ();

  /* The rest are initialized in alphabetical order.  */
  gdbscm_initialize_arches ();
  gdbscm_initialize_auto_load ();
  gdbscm_initialize_blocks ();
  gdbscm_initialize_breakpoints ();
  gdbscm_initialize_disasm ();
  gdbscm_initialize_frames ();
  gdbscm_initialize_iterators ();
  gdbscm_initialize_lazy_strings ();
  gdbscm_initialize_math ();
  gdbscm_initialize_objfiles ();
  gdbscm_initialize_ports ();
  gdbscm_initialize_pretty_printers ();
  gdbscm_initialize_strings ();
  gdbscm_initialize_symbols ();
  gdbscm_initialize_symtabs ();
  gdbscm_initialize_types ();
  gdbscm_initialize_values ();

  gdbscm_define_functions (misc_guile_functions, 1);

  scm_c_define ("*gdb-version*", gdbscm_scm_from_c_string (version));
  scm_c_define ("*host-config*", gdbscm_scm_from_c_string (host_name));
  scm_c_define ("*target-config*", gdbscm_scm_from_c_string (target_name));
  scm_c_export ("*gdb-version*", "*host-config*", "*target-config*", NULL);

  from_tty_keyword = scm_from_latin1_keyword ("from-tty");
  to_string_keyword = scm_from_latin1_keyword ("to-string");

  initialize_scheme_side ();

  gdb_scheme_initialized = 1;
}

/* A callback to finish Guile initialization after gdb has finished all its
   initialization.
   This is the script_lang.finish_initialization "method".  */

static void
gdbscm_finish_initialization (void)
{
  /* Restore the environment to the user interaction one.  */
  scm_set_current_module (scm_interaction_environment ());
}

/* The script_lang.initialized "method".  */

static int
gdbscm_initialized (void)
{
  return gdb_scheme_initialized;
}

/* Enable or disable Guile backtraces.  */

static void
gdbscm_set_backtrace (int enable)
{
  static const char disable_bt[] = "(debug-disable 'backtrace)";
  static const char enable_bt[] = "(debug-enable 'backtrace)";

  if (enable)
    gdbscm_safe_eval_string (enable_bt, 0);
  else
    gdbscm_safe_eval_string (disable_bt, 0);
}

#endif /* HAVE_GUILE */

/* Install the various gdb commands used by Guile.  */

static void
install_gdb_commands (void)
{
  add_com ("guile-interactive", class_obscure,
	   guile_interactive_command,
#ifdef HAVE_GUILE
	   _("\
Start an interactive Guile prompt.\n\
\n\
To return to GDB, type the EOF character (e.g., Ctrl-D on an empty\n\
prompt).\n\
\n\
Alternatively, a single-line Guile command can be given as an\n\
argument, and if the command is an expression, the result will be\n\
printed.  For example:\n\
\n\
    (gdb) scheme-interactive (2 + 3)\n\
    5\n\
")
#else /* HAVE_GUILE */
	   _("\
Start a Guile interactive prompt.\n\
\n\
Guile scripting is not supported in this copy of GDB.\n\
This command is only a placeholder.")
#endif /* HAVE_GUILE */
	   );
  add_com_alias ("gi", "guile-interactive", class_obscure, 1);

  /* Since "help guile" is easy to type, and intuitive, we add general help
     in using GDB+Guile to this command.  */
  add_com ("guile", class_obscure, guile_command,
#ifdef HAVE_GUILE
	   _("\
Evaluate a Guile command.\n\
\n\
The command can be given as an argument, for instance:\n\
\n\
    guile (display 23)\n\
\n\
If no argument is given, the following lines are read and used\n\
as the Guile commands.  Type a line containing \"end\" to indicate\n\
the end of the command.\n\
\n\
The Guile GDB module must first be imported before it can be used.\n\
Do this with:\n\
(gdb) guile (use-modules (gdb))\n\
or if you want to import the (gdb) module with a prefix, use:\n\
(gdb) guile (use-modules ((gdb) #:renamer (symbol-prefix-proc 'gdb:)))\n\
\n\
The Guile interactive session, started with the \"guile-interactive\"\n\
command, provides extensive help and apropos capabilities.")
#else /* HAVE_GUILE */
	   _("\
Evaluate a Guile command.\n\
\n\
Guile scripting is not supported in this copy of GDB.\n\
This command is only a placeholder.")
#endif /* HAVE_GUILE */
	   );
  add_com_alias ("gu", "guile", class_obscure, 1);

  add_prefix_cmd ("guile", class_obscure, set_guile_command,
		  _("Prefix command for Guile preference settings."),
		  &set_guile_list, "set guile ", 0,
		  &setlist);
  add_alias_cmd ("gu", "guile", class_obscure, 1, &setlist);

  add_prefix_cmd ("guile", class_obscure, show_guile_command,
		  _("Prefix command for Guile preference settings."),
		  &show_guile_list, "show guile ", 0,
		  &showlist);
  add_alias_cmd ("gu", "guile", class_obscure, 1, &showlist);

  add_prefix_cmd ("guile", class_obscure, info_guile_command,
		  _("Prefix command for Guile info displays."),
		  &info_guile_list, "info guile ", 0,
		  &infolist);
  add_info_alias ("gu", "guile", 1);

  /* The name "print-stack" is carried over from Python.
     A better name is "print-exception".  */
  add_setshow_enum_cmd ("print-stack", no_class, guile_print_excp_enums,
			&gdbscm_print_excp, _("\
Set mode for Guile exception printing on error."), _("\
Show the mode of Guile exception printing on error."), _("\
none  == no stack or message will be printed.\n\
full == a message and a stack will be printed.\n\
message == an error message without a stack will be printed."),
			NULL, NULL,
			&set_guile_list, &show_guile_list);
}

/* Provide a prototype to silence -Wmissing-prototypes.  */
extern initialize_file_ftype _initialize_guile;

void
_initialize_guile (void)
{
  char *msg;

  install_gdb_commands ();

#if HAVE_GUILE
  /* The Guile docs say scm_init_guile isn't as portable as the other Guile
     initialization routines.  However, this is the easiest to use.
     We can switch to a more portable routine if/when the need arises
     and if it can be used with gdb.  */
  scm_init_guile ();

  /* The Python support puts the C side in module "_gdb", leaving the Python
     side to define module "gdb" which imports "_gdb".  There is evidently no
     similar convention in Guile so we skip this.  */

  /* The rest of the initialization is done by initialize_gdb_module.
     scm_c_define_module is used as it allows us to perform the initialization
     within the desired module.  */
  scm_c_define_module (gdbscm_module_name, initialize_gdb_module, NULL);

  /* Set Guile's backtrace to match the "set guile print-stack" default.
     [N.B. The two settings are still separate.]
     But only do this after we've initialized Guile, it's nice to see a
     backtrace if there's an error during initialization.
     OTOH, if the error is that gdb/init.scm wasn't found because gdb is being
     run from the build tree, the backtrace is more noise than signal.
     Sigh.  */
  gdbscm_set_backtrace (0);
#endif
}
