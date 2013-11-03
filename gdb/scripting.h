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

#ifndef SCRIPTING_H
#define SCRIPTING_H

#include "mi/mi-cmds.h" /* For PRINT_NO_VALUES, etc.  */

struct breakpoint_object;
struct command_line;
struct frame_info;
struct language_defn;
struct objfile;
struct type;
struct ui_file;
struct ui_out;
struct value;
struct value_print_options;

/* The main type describing a descripting language (opaque).  */
struct script_language;

typedef void script_sourcer_func (FILE *, const char *);
typedef void objfile_script_sourcer_func (struct objfile *, FILE *,
					  const char *);

/* Each entry in .debug_gdb_scripts begins with a byte that is used to
   identify the entry.  This byte is to use as we choose.
   0 is reserved so that's never used (to catch errors).
   It is recommended to avoid ASCII values 32-127 to help catch (most) cases
   of forgetting to include this byte.
   Other unused values needn't specify different scripting languages,
   but we have no need for anything else at the moment.  */

enum section_script_id
  {
    SECTION_SCRIPT_ID_NEVER_USE = 0,
    SECTION_SCRIPT_ID_PYTHON = 1,
    SECTION_SCRIPT_ID_GUILE = 2,
    /* Native GDB scripts are *not* supported in .debug_gdb_scripts,
       but we reserve a value for it so we have something to put in the
       script_language table.  */
    SECTION_SCRIPT_ID_GDB = 3
  };

/* Script frame-filter status return values.  */

enum script_bt_status
  {
    /* Return when an error has occurred in processing frame filters,
       or when printing the stack.  */
    SCR_BT_ERROR = -1,

    /* Return from internal routines to indicate that the function
       succeeded.  */
    SCR_BT_OK = 1,

    /* Return when the frame filter process is complete, and all
       operations have succeeded.  */
    SCR_BT_COMPLETED = 2,

    /* Return when the frame filter process is complete, but there
       were no filter registered and enabled to process. */
    SCR_BT_NO_FILTERS = 3
  };

/* Flags to pass to apply_frame_filter.  */

enum frame_filter_flags
  {
    /* Set this flag if frame level is to be printed.  */
    PRINT_LEVEL = 1,

    /* Set this flag if frame information is to be printed.  */
    PRINT_FRAME_INFO = 2,

    /* Set this flag if frame arguments are to be printed.  */
    PRINT_ARGS = 4,

    /* Set this flag if frame locals are to be printed.  */
    PRINT_LOCALS = 8,
  };

/* A choice of the different frame argument printing strategies that
   can occur in different cases of frame filter instantiation.  */

enum script_frame_args
  {
    /* Print no values for arguments when invoked from the MI. */
    NO_VALUES = PRINT_NO_VALUES,

    MI_PRINT_ALL_VALUES = PRINT_ALL_VALUES,

    /* Print only simple values (what MI defines as "simple") for
       arguments when invoked from the MI. */
    MI_PRINT_SIMPLE_VALUES = PRINT_SIMPLE_VALUES,

    /* Print only scalar values for arguments when invoked from the CLI. */
    CLI_SCALAR_VALUES,

    /* Print all values for arguments when invoked from the CLI. */
    CLI_ALL_VALUES
  };

/* Table of type printers associated with the global typedef table.  */

struct script_type_printers
{
  /* Type-printers from Python.  */
  void *py_type_printers;

#if 0 /*TODO*/
  /* Type-printers from Guile.  */
  void *scm_type_printers;
#endif
};

/* The interface for making calls from GDB to an external scripting
   language.  */

struct script_language_interface
{
  void (*finish_initialization) (void);

  int (*initialized) (void);

  script_sourcer_func *script_sourcer;
  objfile_script_sourcer_func *objfile_script_sourcer;
  int (*auto_load_enabled) (void);

  void (*eval_from_control_command) (struct command_line *);

  void (*start_type_printers) (struct script_type_printers *);
  /* This function has a bit of a funny name, since it actually applies
     recognizers, but this seemed clearer given the start_type_printers
     and free_type_printers functions.  */
  char *(*apply_type_printers) (const struct script_type_printers *,
				struct type *);
  void (*free_type_printers) (struct script_type_printers *);

  int (*apply_val_pretty_printer)
    (struct type *type, const gdb_byte *valaddr,
     int embedded_offset, CORE_ADDR address,
     struct ui_file *stream, int recurse,
     const struct value *val, const struct value_print_options *options,
     const struct language_defn *language);

  enum script_bt_status (*apply_frame_filter)
    (struct frame_info *frame, int flags, enum script_frame_args args_type,
     struct ui_out *out, int frame_low, int frame_high);

  void (*preserve_values) (struct objfile *, htab_t copied_types);

  int (*breakpoint_has_cond) (struct breakpoint *);
  int (*breakpoint_cond_says_stop) (struct breakpoint *);

  int (*check_quit_flag) (void);
  void (*clear_quit_flag) (void);
  void (*set_quit_flag) (void);
};

/* An opaque, to varobj.c, interface between varobj.c and the scripting
   language.  */

struct varobj_scripting_interface
{
};

extern const struct script_language *get_script_lang_gdb (void);
extern const struct script_language *get_script_lang_python (void);
extern const struct script_language *get_script_lang_guile (void);

/* Accessors for "public" attributes of struct script_language.  */

extern const char *script_lang_name (const struct script_language *);

extern const char *script_lang_capitalized_name
  (const struct script_language *);

extern const char *script_lang_suffix (const struct script_language *);

extern const char *script_lang_auto_load_suffix
  (const struct script_language *);

extern objfile_script_sourcer_func *script_lang_objfile_sourcer
   (const struct script_language *);

extern int script_lang_auto_load_enabled (const struct script_language *);

/* Wrappers for each scripting API function that iterate over all
   (external) scripting languages.  */

extern void finish_script_initialization (void);

extern script_sourcer_func *get_script_sourcer (const char *file);

extern int get_section_script_lang (int id,
				    const struct script_language **spec);

extern void eval_script_from_control_command (struct command_line *cmd);

extern void auto_load_ext_scripts_for_objfile (struct objfile *);

extern struct script_type_printers *start_script_type_printers (void);

extern char *apply_script_type_printers (struct script_type_printers *,
					 struct type *);

extern void free_script_type_printers (struct script_type_printers *);

extern int apply_val_script_pretty_printer
  (struct type *type, const gdb_byte *valaddr,
   int embedded_offset, CORE_ADDR address,
   struct ui_file *stream, int recurse,
   const struct value *val, const struct value_print_options *options,
   const struct language_defn *language);

extern enum script_bt_status apply_script_frame_filter
  (struct frame_info *frame, int flags, enum script_frame_args args_type,
   struct ui_out *out, int frame_low, int frame_high);

extern void preserve_script_values (struct objfile *, htab_t copied_types);

extern int breakpoint_has_script_cond (struct breakpoint *);

extern int breakpoint_script_cond_says_stop (struct breakpoint *);

/* Return non-zero if Python scripting successfully initialized.  */
extern int script_lang_python_initialized (void);

/* Return non-zero if Guile scripting successfully initialized.  */
extern int script_lang_guile_initialized (void);

#endif /* SCRIPTING_H */
