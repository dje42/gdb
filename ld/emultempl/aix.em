# This shell script emits a C file. -*- C -*-
# It does some substitutions.
if [ -z "$MACHINE" ]; then
  OUTPUT_ARCH=${ARCH}
else
  OUTPUT_ARCH=${ARCH}:${MACHINE}
fi
cat >e${EMULATION_NAME}.c <<EOF
/* This file is is generated by a shell script.  DO NOT EDIT! */

/* AIX emulation code for ${EMULATION_NAME}
   Copyright 1991, 1993, 1995, 1996, 1997, 1998, 2000, 2001
   Free Software Foundation, Inc.
   Written by Steve Chamberlain <sac@cygnus.com>
   AIX support by Ian Lance Taylor <ian@cygnus.com>
   AIX 64 bit support by Tom Rix <trix@redhat.com>

This file is part of GLD, the Gnu Linker.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#define TARGET_IS_${EMULATION_NAME}

#include "bfd.h"
#include "sysdep.h"
#include "libiberty.h"
#include "safe-ctype.h"
#include "getopt.h"
#include "obstack.h"
#include "bfdlink.h"

#include "ld.h"
#include "ldmain.h"
#include "ldmisc.h"
#include "ldexp.h"
#include "ldlang.h"
#include "ldfile.h"
#include "ldemul.h"
#include "ldctor.h"
#include "ldgram.h"

#include "coff/internal.h"
#include "coff/xcoff.h"

static void gld${EMULATION_NAME}_before_parse PARAMS ((void));
static int gld${EMULATION_NAME}_parse_args PARAMS ((int, char **));
static void gld${EMULATION_NAME}_after_open PARAMS ((void));
static void gld${EMULATION_NAME}_before_allocation PARAMS ((void));
static void gld${EMULATION_NAME}_read_file PARAMS ((const char *, boolean));
static void gld${EMULATION_NAME}_free PARAMS ((PTR));
static void gld${EMULATION_NAME}_find_relocs
  PARAMS ((lang_statement_union_type *));
static void gld${EMULATION_NAME}_find_exp_assignment PARAMS ((etree_type *));
static char *gld${EMULATION_NAME}_get_script PARAMS ((int *isfile));
static boolean gld${EMULATION_NAME}_unrecognized_file
  PARAMS ((lang_input_statement_type *));

/* The file alignment required for each section.  */
static unsigned long file_align;

/* The maximum size the stack is permitted to grow.  This is stored in
   the a.out header.  */
static unsigned long maxstack;

/* The maximum data size.  This is stored in the a.out header.  */
static unsigned long maxdata;

/* Whether to perform garbage collection.  */
static int gc = 1;

/* The module type to use.  */
static unsigned short modtype = ('1' << 8) | 'L';

/* Whether the .text section must be read-only (i.e., no relocs
   permitted).  */
static int textro;

/* Whether to implement Unix like linker semantics.  */
static int unix_ld;

/* Structure used to hold import file list.  */

struct filelist
{
  struct filelist *next;
  const char *name;
};

/* List of import files.  */
static struct filelist *import_files;

/* List of export symbols read from the export files.  */

struct export_symbol_list
{
  struct export_symbol_list *next;
  const char *name;
};

static struct export_symbol_list *export_symbols;

/* Maintains the 32 or 64 bit mode state of import file */
static unsigned int symbol_mode = 0x04;

/* This routine is called before anything else is done.  */

static void
gld${EMULATION_NAME}_before_parse ()
{
#ifndef TARGET_			/* I.e., if not generic.  */
  const bfd_arch_info_type *arch = bfd_scan_arch ("${OUTPUT_ARCH}");
  if (arch)
    {
      ldfile_output_architecture = arch->arch;
      ldfile_output_machine = arch->mach;
      ldfile_output_machine_name = arch->printable_name;
    }
  else
    ldfile_output_architecture = bfd_arch_${ARCH};
#endif /* not TARGET_ */
  config.has_shared = true;

  /*
   * The link_info.[init|fini]_functions are initialized in ld/lexsup.c.
   * Override them here so we can use the link_info.init_function as a
   * state flag that lets the backend know that -binitfini has been done.
   */
  link_info.init_function = NULL;
  link_info.fini_function = NULL;

}

/* Handle AIX specific options.  */

static int
gld${EMULATION_NAME}_parse_args (argc, argv)
     int argc;
     char **argv;
{
  int prevoptind = optind;
  int prevopterr = opterr;
  int indx;
  int longind;
  int optc;
  bfd_signed_vma val;
  char *end;

  enum {
    OPTION_IGNORE = 300,
    OPTION_AUTOIMP,
    OPTION_ERNOTOK,
    OPTION_EROK,
    OPTION_EXPORT,
    OPTION_IMPORT,
    OPTION_INITFINI,
    OPTION_LOADMAP,
    OPTION_MAXDATA,
    OPTION_MAXSTACK,
    OPTION_MODTYPE,
    OPTION_NOAUTOIMP,
    OPTION_NOSTRCMPCT,
    OPTION_PD,
    OPTION_PT,
    OPTION_STRCMPCT,
    OPTION_UNIX
  };

  /*
    b64 is an empty option.  The native linker uses -b64 for xcoff64 support
    Our linker uses -m aixppc64 for xcoff64 support. The choice for the
    correct emulation is done in collect2.c via the environmental varible
    LDEMULATION.

    binitfini has special handling in the linker backend.  The native linker
    uses the arguemnts to generate a table of init and fini functions for
    the executable.  The important use for this option is to support aix 4.2+
    c++ constructors and destructors.  This is tied into gcc via collect2.c.
    The function table is accessed by the runtime linker/loader by checking if
    the first symbol in the loader symbol table is "__rtinit".  The native
    linker generates this table and the loader symbol.  The gnu linker looks
    for the symbol "__rtinit" and makes it the first loader symbol.  It is the
    responsiblity of the user to define the __rtinit symbol.  The format for
    __rtinit is given by the aix system file /usr/include/rtinit.h.  You can
    look at collect2.c to see an example of how this is done for 32 and 64 bit.
    Below is an exmaple of a 32 bit assembly file that defines __rtinit.

    .file   "my_rtinit.s"

    .csect .data[RW],3
    .globl __rtinit
    .extern init_function
    .extern fini_function

    __rtinit:
            .long 0
            .long f1i - __rtinit
            .long f1f - __rtinit
            .long f2i - f1i
            .align 3
    f1i:    .long init_function
            .long s1i - __rtinit
            .long 0
    f2i:    .long 0
            .long 0
            .long 0
    f1f:    .long fini_function
            .long s1f - __rtinit
            .long 0
    f2f:    .long 0
            .long 0
            .long 0
            .align 3
    s1i:    .string "init_function"
            .align 3
    s1f:    .string "fini_function"

   */

  static const struct option longopts[] = {
    {"basis", no_argument, NULL, OPTION_IGNORE},
    {"bautoimp", no_argument, NULL, OPTION_AUTOIMP},
    {"bcomprld", no_argument, NULL, OPTION_IGNORE},
    {"bcrld", no_argument, NULL, OPTION_IGNORE},
    {"bcror31", no_argument, NULL, OPTION_IGNORE},
    {"bD", required_argument, NULL, OPTION_MAXDATA},
    {"bE", required_argument, NULL, OPTION_EXPORT},
    {"bernotok", no_argument, NULL, OPTION_ERNOTOK},
    {"berok", no_argument, NULL, OPTION_EROK},
    {"berrmsg", no_argument, NULL, OPTION_IGNORE},
    {"bexport", required_argument, NULL, OPTION_EXPORT},
    {"bf", no_argument, NULL, OPTION_ERNOTOK},
    {"bgc", no_argument, &gc, 1},
    {"bh", required_argument, NULL, OPTION_IGNORE},
    {"bhalt", required_argument, NULL, OPTION_IGNORE},
    {"bI", required_argument, NULL, OPTION_IMPORT},
    {"bimport", required_argument, NULL, OPTION_IMPORT},
    {"binitfini", required_argument, NULL, OPTION_INITFINI},
    {"bl", required_argument, NULL, OPTION_LOADMAP},
    {"bloadmap", required_argument, NULL, OPTION_LOADMAP},
    {"bmaxdata", required_argument, NULL, OPTION_MAXDATA},
    {"bmaxstack", required_argument, NULL, OPTION_MAXSTACK},
    {"bM", required_argument, NULL, OPTION_MODTYPE},
    {"bmodtype", required_argument, NULL, OPTION_MODTYPE},
    {"bnoautoimp", no_argument, NULL, OPTION_NOAUTOIMP},
    {"bnodelcsect", no_argument, NULL, OPTION_IGNORE},
    {"bnoentry", no_argument, NULL, OPTION_IGNORE},
    {"bnogc", no_argument, &gc, 0},
    {"bnso", no_argument, NULL, OPTION_NOAUTOIMP},
    {"bnostrcmpct", no_argument, NULL, OPTION_NOSTRCMPCT},
    {"bnotextro", no_argument, &textro, 0},
    {"bnro", no_argument, &textro, 0},
    {"bpD", required_argument, NULL, OPTION_PD},
    {"bpT", required_argument, NULL, OPTION_PT},
    {"bro", no_argument, &textro, 1},
    {"bS", required_argument, NULL, OPTION_MAXSTACK},
    {"bso", no_argument, NULL, OPTION_AUTOIMP},
    {"bstrcmpct", no_argument, NULL, OPTION_STRCMPCT},
    {"btextro", no_argument, &textro, 1},
    {"b64", no_argument, NULL, 0},
    {"static", no_argument, NULL, OPTION_NOAUTOIMP},
    {"unix", no_argument, NULL, OPTION_UNIX},
    {NULL, no_argument, NULL, 0}
  };

  /* Options supported by the AIX linker which we do not support: -f,
     -S, -v, -Z, -bbindcmds, -bbinder, -bbindopts, -bcalls, -bcaps,
     -bcror15, -bdebugopt, -bdbg, -bdelcsect, -bex?, -bfilelist, -bfl,
     -bgcbypass, -bglink, -binsert, -bi, -bloadmap, -bl, -bmap, -bnl,
     -bnobind, -bnocomprld, -bnocrld, -bnoerrmsg, -bnoglink,
     -bnoloadmap, -bnl, -bnoobjreorder, -bnoquiet, -bnoreorder,
     -bnotypchk, -bnox, -bquiet, -bR, -brename, -breorder, -btypchk,
     -bx, -bX, -bxref.  */

  /* If the current option starts with -b, change the first : to an =.
     The AIX linker uses : to separate the option from the argument;
     changing it to = lets us treat it as a getopt option.  */
  indx = optind;
  if (indx == 0)
    {
      indx = 1;
    }

  if (indx < argc && strncmp (argv[indx], "-b", 2) == 0)
    {
      char *s;

      for (s = argv[indx]; *s != '\0'; s++)
	{
	  if (*s == ':')
	    {
	      *s = '=';
	      break;
	    }
	}
    }


  /* We add s and u so to the short options list so that -s and -u on
     the command line do not match -static and -unix.  */

  opterr = 0;
  optc = getopt_long_only (argc, argv, "-D:H:KT:zsu", longopts, &longind);
  opterr = prevopterr;

  switch (optc)
    {
    case 's':
    case 'u':
    default:
      optind = prevoptind;
      return 0;

    case 0:
      /* Long option which just sets a flag.  */
      break;

    case 'D':
      val = strtoll (optarg, &end, 0);
      if (*end != '\0')
	einfo ("%P: warning: ignoring invalid -D number %s\n", optarg);
      else if (val != -1)
	lang_section_start (".data", exp_intop (val));
      break;

    case 'H':
      val = strtoul (optarg, &end, 0);
      if (*end != '\0'
	  || (val & (val - 1)) != 0)
	einfo ("%P: warning: ignoring invalid -H number %s\n", optarg);
      else
	file_align = val;
      break;

    case 'K':
    case 'z':
      /* FIXME: This should use the page size for the target system.  */
      file_align = 4096;
      break;

    case 'T':
      /* On AIX this is the same as GNU ld -Ttext.  When we see -T
         number, we assume the AIX option is intended.  Otherwise, we
         assume the usual GNU ld -T option is intended.  We can't just
         ignore the AIX option, because gcc passes it to the linker.  */
      val = strtoull (optarg, &end, 0);
      if (*end != '\0')
	{
	  optind = prevoptind;
	  return 0;
	}
      lang_section_start (".text", exp_intop (val));
      break;

    case OPTION_IGNORE:
      break;

    case OPTION_INITFINI:
      {
	/*
	 * The aix linker init fini has the format :
	 *
	 * -binitfini:[ Initial][:Termination][:Priority]
	 *
	 * it allows the Termination and Priority to be optional.
	 *
	 * Since we support only one init/fini pair, we ignore the Priority.
	 *
	 * Define the special symbol __rtinit.
	 *
	 * strtok does not correctly handle the case of -binitfini::fini: so
	 * do it by hand
	 */
	char *t, *i, *f;

	i = t = optarg;
	while (*t && ':' != *t)
	  t++;
	if (*t)
	  *t++ = 0;

	if (0 != strlen (i))
	  {
	    link_info.init_function = i;
	  }

	f = t;
	while (*t && ':' != *t)
	  t++;
	*t = 0;

	if (0 != strlen (f))
	  {
	    link_info.fini_function = f;
	  }
      }
    break;

    case OPTION_AUTOIMP:
      link_info.static_link = false;
      break;

    case OPTION_ERNOTOK:
      force_make_executable = false;
      break;

    case OPTION_EROK:
      force_make_executable = true;
      break;

    case OPTION_EXPORT:
      gld${EMULATION_NAME}_read_file (optarg, false);
      break;

    case OPTION_IMPORT:
      {
	struct filelist *n;
	struct filelist **flpp;

	n = (struct filelist *) xmalloc (sizeof (struct filelist));
	n->next = NULL;
	n->name = optarg;
	flpp = &import_files;
	while (*flpp != NULL)
	  flpp = &(*flpp)->next;
	*flpp = n;
      }
      break;

    case OPTION_LOADMAP:
      config.map_filename = optarg;
      break;

    case OPTION_MAXDATA:
      val = strtoull (optarg, &end, 0);
      if (*end != '\0')
	einfo ("%P: warning: ignoring invalid -bmaxdata number %s\n", optarg);
      else
	maxdata = val;
      break;

    case OPTION_MAXSTACK:
      val = strtoull (optarg, &end, 0);
      if (*end != '\0')
	einfo ("%P: warning: ignoring invalid -bmaxstack number %s\n", optarg);
      else
	maxstack = val;
      break;

    case OPTION_MODTYPE:
      if (*optarg == 'S')
	{
	  link_info.shared = true;
	  ++optarg;
	}
      if (*optarg == '\0' || optarg[1] == '\0')
	einfo ("%P: warning: ignoring invalid module type %s\n", optarg);
      else
	modtype = (*optarg << 8) | optarg[1];
      break;

    case OPTION_NOAUTOIMP:
      link_info.static_link = true;
      break;

    case OPTION_NOSTRCMPCT:
      link_info.traditional_format = true;
      break;

    case OPTION_PD:
      /* This sets the page that the .data section is supposed to
         start on.  The offset within the page should still be the
         offset within the file, so we need to build an appropriate
         expression.  */
      val = strtoull (optarg, &end, 0);
      if (*end != '\0')
	einfo ("%P: warning: ignoring invalid -pD number %s\n", optarg);
      else
	{
	  etree_type *t;

	  t = exp_binop ('+',
			 exp_intop (val),
			 exp_binop ('&',
				    exp_nameop (NAME, "."),
				    exp_intop (0xfff)));
	  t = exp_binop ('&',
			 exp_binop ('+', t, exp_intop (31)),
			 exp_intop (~ (bfd_vma) 31));
	  lang_section_start (".data", t);
	}
      break;

    case OPTION_PT:
      /* This set the page that the .text section is supposed to start
         on.  The offset within the page should still be the offset
         within the file.  */
      val = strtoull (optarg, &end, 0);
      if (*end != '\0')
	einfo ("%P: warning: ignoring invalid -pT number %s\n", optarg);
      else
	{
	  etree_type *t;

	  t = exp_binop ('+',
			 exp_intop (val),
			 exp_nameop (SIZEOF_HEADERS, NULL));
	  t = exp_binop ('&',
			 exp_binop ('+', t, exp_intop (31)),
			 exp_intop (~ (bfd_vma) 31));
	  lang_section_start (".text", t);
	}
      break;

    case OPTION_STRCMPCT:
      link_info.traditional_format = false;
      break;

    case OPTION_UNIX:
      unix_ld = true;
      break;
    }

  return 1;
}

/* This is called when an input file can not be recognized as a BFD
   object or an archive.  If the file starts with #!, we must treat it
   as an import file.  This is for AIX compatibility.  */

static boolean
gld${EMULATION_NAME}_unrecognized_file (entry)
     lang_input_statement_type *entry;
{
  FILE *e;
  boolean ret;

  e = fopen (entry->filename, FOPEN_RT);
  if (e == NULL)
    return false;

  ret = false;

  if (getc (e) == '#' && getc (e) == '!')
    {
      struct filelist *n;
      struct filelist **flpp;

      n = (struct filelist *) xmalloc (sizeof (struct filelist));
      n->next = NULL;
      n->name = entry->filename;
      flpp = &import_files;
      while (*flpp != NULL)
	flpp = &(*flpp)->next;
      *flpp = n;

      ret = true;
      entry->loaded = true;
    }

  fclose (e);

  return ret;
}

/* This is called after the input files have been opened.  */

static void
gld${EMULATION_NAME}_after_open ()
{
  boolean r;
  struct set_info *p;

  /* Call ldctor_build_sets, after pretending that this is a
     relocateable link.  We do this because AIX requires relocation
     entries for all references to symbols, even in a final
     executable.  Of course, we only want to do this if we are
     producing an XCOFF output file.  */
  r = link_info.relocateable;
  if (strstr (bfd_get_target (output_bfd), "xcoff") != NULL)
    link_info.relocateable = true;
  ldctor_build_sets ();
  link_info.relocateable = r;

  /* For each set, record the size, so that the XCOFF backend can
     output the correct csect length.  */
  for (p = sets; p != (struct set_info *) NULL; p = p->next)
    {
      bfd_size_type size;

      /* If the symbol is defined, we may have been invoked from
	 collect, and the sets may already have been built, so we do
	 not do anything.  */
      if (p->h->type == bfd_link_hash_defined
	  || p->h->type == bfd_link_hash_defweak)
	continue;

      if (p->reloc != BFD_RELOC_CTOR)
	{
	  /* Handle this if we need to.  */
	  abort ();
	}

      size = (p->count + 2) * 4;
      if (! bfd_xcoff_link_record_set (output_bfd, &link_info, p->h, size))
	einfo ("%F%P: bfd_xcoff_link_record_set failed: %E\n");
    }
}

/* This is called after the sections have been attached to output
   sections, but before any sizes or addresses have been set.  */

static void
gld${EMULATION_NAME}_before_allocation ()
{
  struct filelist *fl;
  struct export_symbol_list *el;
  char *libpath;
  asection *special_sections[XCOFF_NUMBER_OF_SPECIAL_SECTIONS];
  int i;

  /* Handle the import and export files, if any.  */
  for (fl = import_files; fl != NULL; fl = fl->next)
    gld${EMULATION_NAME}_read_file (fl->name, true);
  for (el = export_symbols; el != NULL; el = el->next)
    {
      struct bfd_link_hash_entry *h;

      h = bfd_link_hash_lookup (link_info.hash, el->name, false, false, false);
      if (h == NULL)
	einfo ("%P%F: bfd_link_hash_lookup of export symbol failed: %E\n");
      if (! bfd_xcoff_export_symbol (output_bfd, &link_info, h))
	einfo ("%P%F: bfd_xcoff_export_symbol failed: %E\n");
    }

  /* Track down all relocations called for by the linker script (these
     are typically constructor/destructor entries created by
     CONSTRUCTORS) and let the backend know it will need to create
     .loader relocs for them.  */
  lang_for_each_statement (gld${EMULATION_NAME}_find_relocs);

  /* We need to build LIBPATH from the -L arguments.  If any -rpath
     arguments were used, though, we use -rpath instead, as a GNU
     extension.  */
  if (command_line.rpath != NULL)
    libpath = command_line.rpath;
  else if (search_head == NULL)
    libpath = (char *) "";
  else
    {
      size_t len;
      search_dirs_type *search;

      len = strlen (search_head->name);
      libpath = xmalloc (len + 1);
      strcpy (libpath, search_head->name);
      for (search = search_head->next; search != NULL; search = search->next)
	{
	  size_t nlen;

	  nlen = strlen (search->name);
	  libpath = xrealloc (libpath, len + nlen + 2);
	  libpath[len] = ':';
	  strcpy (libpath + len + 1, search->name);
	  len += nlen + 1;
	}
    }

  /* Let the XCOFF backend set up the .loader section.  */
  if (! bfd_xcoff_size_dynamic_sections (output_bfd, &link_info, libpath,
					 entry_symbol, file_align,
					 maxstack, maxdata,
					 gc && ! unix_ld ? true : false,
					 modtype,
					 textro ? true : false,
					 unix_ld,
					 special_sections))
    einfo ("%P%F: failed to set dynamic section sizes: %E\n");

  /* Look through the special sections, and put them in the right
     place in the link ordering.  This is especially magic.  */
  for (i = 0; i < XCOFF_NUMBER_OF_SPECIAL_SECTIONS; i++)
    {
      asection *sec;
      lang_output_section_statement_type *os;
      lang_statement_union_type **pls;
      lang_input_section_type *is;
      const char *oname;
      boolean start;

      sec = special_sections[i];
      if (sec == NULL)
	continue;

      /* Remove this section from the list of the output section.
	 This assumes we know what the script looks like.  */
      is = NULL;
      os = lang_output_section_find (sec->output_section->name);
      if (os == NULL) {
	einfo ("%P%F: can't find output section %s\n",
	       sec->output_section->name);
      }

      for (pls = &os->children.head; *pls != NULL; pls = &(*pls)->header.next)
	{

	  if ((*pls)->header.type == lang_input_section_enum &&
	      (*pls)->input_section.section == sec)
	    {
	      is = (lang_input_section_type *) *pls;
	      *pls = (*pls)->header.next;
	      break;
	    }

	  if ((*pls)->header.type == lang_wild_statement_enum)
	    {
	      lang_statement_union_type **pwls;

	      for (pwls = &(*pls)->wild_statement.children.head;
		   *pwls != NULL;
		   pwls = &(*pwls)->header.next)
		{

		  if ((*pwls)->header.type == lang_input_section_enum &&
		      (*pwls)->input_section.section == sec)
		    {

		      is = (lang_input_section_type *) *pwls;
		      *pwls = (*pwls)->header.next;
		      break;
		    }
		}

	      if (is != NULL)
		break;
	    }
	}

      if (is == NULL)
	{
	  einfo ("%P%F: can't find %s in output section\n",
		 bfd_get_section_name (sec->owner, sec));
	}

      /* Now figure out where the section should go.  */
      switch (i)
	{

	default: /* to avoid warnings */
	case XCOFF_SPECIAL_SECTION_TEXT:
	  /* _text */
	  oname = ".text";
	  start = true;
	  break;

	case XCOFF_SPECIAL_SECTION_ETEXT:
	  /* _etext */
	  oname = ".text";
	  start = false;
	  break;

	case XCOFF_SPECIAL_SECTION_DATA:
	  /* _data */
	  oname = ".data";
	  start = true;
	  break;

	case XCOFF_SPECIAL_SECTION_EDATA:
	  /* _edata */
	  oname = ".data";
	  start = false;
	  break;

	case XCOFF_SPECIAL_SECTION_END:
	case XCOFF_SPECIAL_SECTION_END2:
	  /* _end and end */
	  oname = ".bss";
	  start = false;
	  break;
	}

      os = lang_output_section_find (oname);

      if (start)
	{
	  is->header.next = os->children.head;
	  os->children.head = (lang_statement_union_type *) is;
	}
      else
	{
	  is->header.next = NULL;
	  lang_statement_append (&os->children,
				 (lang_statement_union_type *) is,
				 &is->header.next);
	}
    }
}

static int change_symbol_mode (char *input)
{
  /*
   * 1 : state changed
   * 0 : no change
   */

  char *symbol_mode_string[] = {
    "# 32",   /* 0x01 */
    "# 64",   /* 0x02 */
    "# no32", /* 0x04 */
    "# no64", /* 0x08 */
    NULL,
  };
  unsigned int bit;
  char *string;

  for (bit = 0; ; bit++)
    {
      string = symbol_mode_string[bit];
      if (NULL == string)
	return 0;

      if (0 == strcmp (input, string))
	{
	  symbol_mode = (1 << bit);
	  return 1;
	}
    }
  /* should not be here */
  return 0;
}

static int is_syscall(char *input, unsigned int *flag)
{
  /*
   * 1 : yes
   * 0 : ignore
   * -1 : error, try something else
   */
  unsigned int bit;
  char *string;
  
  struct sc {
    char *syscall_string;
    unsigned int flag;
  } s [] = {
    { "svc"	    /* 0x01 */, XCOFF_SYSCALL32 },
    { "svc32"	    /* 0x02 */, XCOFF_SYSCALL32 },
    { "svc3264"     /* 0x04 */, XCOFF_SYSCALL32 | XCOFF_SYSCALL64 },
    { "svc64"	    /* 0x08 */, XCOFF_SYSCALL64 },
    { "syscall"     /* 0x10 */, XCOFF_SYSCALL32 },
    { "syscall32"   /* 0x20 */, XCOFF_SYSCALL32 },
    { "syscall3264" /* 0x40 */, XCOFF_SYSCALL32 | XCOFF_SYSCALL64 },
    { "syscall64"   /* 0x80 */, XCOFF_SYSCALL64 },
    { NULL, 0 },
  };

  *flag = 0;

  for (bit = 0; ;bit++) {
    
    string = s[bit].syscall_string;
    if (NULL == string) {
      return -1;
    }

    if (0 == strcmp(input, string)) {
      if (1 << bit & ${SYSCALL_MASK}) {
	*flag = s[bit].flag;
	return 1;
      } else {
	return 0;
      }
    }
  }
  /* should not be here */
  return -1;
}

/* Read an import or export file.  For an import file, this is called
   by the before_allocation emulation routine.  For an export file,
   this is called by the parse_args emulation routine.  */

static void
gld${EMULATION_NAME}_read_file (filename, import)
     const char *filename;
     boolean import;
{
  struct obstack *o;
  FILE *f;
  int lineno;
  int c;
  boolean keep;
  const char *imppath;
  const char *impfile;
  const char *impmember;

  o = (struct obstack *) xmalloc (sizeof (struct obstack));
  obstack_specify_allocation (o, 0, 0, xmalloc, gld${EMULATION_NAME}_free);

  f = fopen (filename, FOPEN_RT);
  if (f == NULL)
    {
      bfd_set_error (bfd_error_system_call);
      einfo ("%F%s: %E\n", filename);
    }

  keep = false;

  imppath = NULL;
  impfile = NULL;
  impmember = NULL;

  lineno = 0;

  /*
   * default to 32 and 64 bit mode
   * symbols at top of /lib/syscalls.exp do not have a mode modifier and they
   * are not repeated, assume 64 bit routines also want to use them.
   * See the routine change_symbol_mode for more information.
   */
  symbol_mode = 0x04;

  while ((c = getc (f)) != EOF)
    {
      char *s;
      char *symname;
      unsigned int syscall_flag = 0;
      bfd_vma address;
      struct bfd_link_hash_entry *h;

      if (c != '\n')
	{
	  obstack_1grow (o, c);
	  continue;
	}

      obstack_1grow (o, '\0');
      ++lineno;

      s = (char *) obstack_base (o);
      while (ISSPACE (*s))
	++s;
      if (*s == '\0'
	  || *s == '*'
	  || change_symbol_mode (s)
	  || (*s == '#' && s[1] == ' ')
	  || (! import && *s == '#' && s[1] == '!'))
	{
	  obstack_free (o, obstack_base (o));
	  continue;
	}

      if (*s == '#' && s[1] == '!')
	{
	  s += 2;
	  while (ISSPACE (*s))
	    ++s;
	  if (*s == '\0')
	    {
	      imppath = NULL;
	      impfile = NULL;
	      impmember = NULL;
	      obstack_free (o, obstack_base (o));
	    }
	  else if (*s == '(')
	    einfo ("%F%s%d: #! ([member]) is not supported in import files\n",
		   filename, lineno);
	  else
	    {
	      char cs;
	      char *file;

	      (void) obstack_finish (o);
	      keep = true;
	      imppath = s;
	      file = NULL;
	      while (! ISSPACE (*s)
		     && *s != '(' && *s != '\0')
		{
		  if (*s == '/')
		    file = s + 1;
		  ++s;
		}
	      if (file != NULL)
		{
		  file[-1] = '\0';
		  impfile = file;
		  if (imppath == file - 1)
		    imppath = "/";
		}
	      else
		{
		  impfile = imppath;
		  imppath = "";
		}
	      cs = *s;
	      *s = '\0';
	      while (ISSPACE (cs))
		{
		  ++s;
		  cs = *s;
		}
	      if (cs != '(')
		{
		  impmember = "";
		  if (cs != '\0')
		    einfo ("%s:%d: warning: syntax error in import file\n",
			   filename, lineno);
		}
	      else
		{
		  ++s;
		  impmember = s;
		  while (*s != ')' && *s != '\0')
		    ++s;
		  if (*s == ')')
		    *s = '\0';
		  else
		    einfo ("%s:%d: warning: syntax error in import file\n",
			   filename, lineno);
		}
	    }

	  continue;
	}

      if (symbol_mode & ${SYMBOL_MODE_MASK})
	{
	  /* This is a symbol to be imported or exported.  */
	  symname = s;
	  syscall_flag = 0;
	  address = (bfd_vma) -1;

	  while (! ISSPACE (*s) && *s != '\0')
	    ++s;
	  if (*s != '\0')
	    {
	      char *se;

	      *s++ = '\0';

	      while (ISSPACE (*s))
		++s;

	      se = s;
	      while (! ISSPACE (*se) && *se != '\0')
		++se;
	      if (*se != '\0')
		{
		  *se++ = '\0';
		  while (ISSPACE (*se))
		    ++se;
		  if (*se != '\0')
		    einfo ("%s%d: warning: syntax error in import/export file\n",
			   filename, lineno);
		}

	      if (s != se)
		{
		  int status;
		  char *end;

		  status = is_syscall(s, &syscall_flag);
	      
		  if (0 > status) {
		    /* not a system call, check for address */
		    address = strtoul (s, &end, 0);

		    /* not a system call, check for address */
		    address = strtoul (s, &end, 0);
		    if (*end != '\0')
		      {
			einfo ("%s:%d: warning: syntax error in import/export file\n",
			       filename, lineno);
			
		      }
		  }
		}
	    }

	  if (! import)
	    {
	      struct export_symbol_list *n;

	      ldlang_add_undef (symname);
	      n = ((struct export_symbol_list *)
		   xmalloc (sizeof (struct export_symbol_list)));
	      n->next = export_symbols;
	      n->name = xstrdup (symname);
	      export_symbols = n;
	    }
	  else
	    {
	      h = bfd_link_hash_lookup (link_info.hash, symname, false, false,
					true);
	      if (h == NULL || h->type == bfd_link_hash_new)
		{
		  /* We can just ignore attempts to import an unreferenced
		     symbol.  */
		}
	      else
		{
		  if (! bfd_xcoff_import_symbol (output_bfd, &link_info, h,
						 address, imppath, impfile,
						 impmember, syscall_flag))
		    einfo ("%X%s:%d: failed to import symbol %s: %E\n",
			   filename, lineno, symname);
		}
	    }
	}
      obstack_free (o, obstack_base (o));
    }

  if (obstack_object_size (o) > 0)
    {
      einfo ("%s:%d: warning: ignoring unterminated last line\n",
	     filename, lineno);
      obstack_free (o, obstack_base (o));
    }

  if (! keep)
    {
      obstack_free (o, NULL);
      free (o);
    }
}

/* This routine saves us from worrying about declaring free.  */

static void
gld${EMULATION_NAME}_free (p)
     PTR p;
{
  free (p);
}

/* This is called by the before_allocation routine via
   lang_for_each_statement.  It looks for relocations and assignments
   to symbols.  */

static void
gld${EMULATION_NAME}_find_relocs (s)
     lang_statement_union_type *s;
{
  if (s->header.type == lang_reloc_statement_enum)
    {
      lang_reloc_statement_type *rs;

      rs = &s->reloc_statement;
      if (rs->name == NULL)
	einfo ("%F%P: only relocations against symbols are permitted\n");
      if (! bfd_xcoff_link_count_reloc (output_bfd, &link_info, rs->name))
	einfo ("%F%P: bfd_xcoff_link_count_reloc failed: %E\n");
    }

  if (s->header.type == lang_assignment_statement_enum)
    gld${EMULATION_NAME}_find_exp_assignment (s->assignment_statement.exp);
}

/* Look through an expression for an assignment statement.  */

static void
gld${EMULATION_NAME}_find_exp_assignment (exp)
     etree_type *exp;
{
  struct bfd_link_hash_entry *h;

  switch (exp->type.node_class)
    {
    case etree_provide:
      h = bfd_link_hash_lookup (link_info.hash, exp->assign.dst,
				false, false, false);
      if (h == NULL)
	break;
      /* Fall through.  */
    case etree_assign:
      if (strcmp (exp->assign.dst, ".") != 0)
	{
	  if (! bfd_xcoff_record_link_assignment (output_bfd, &link_info,
						  exp->assign.dst))
	    einfo ("%P%F: failed to record assignment to %s: %E\n",
		   exp->assign.dst);
	}
      gld${EMULATION_NAME}_find_exp_assignment (exp->assign.src);
      break;

    case etree_binary:
      gld${EMULATION_NAME}_find_exp_assignment (exp->binary.lhs);
      gld${EMULATION_NAME}_find_exp_assignment (exp->binary.rhs);
      break;

    case etree_trinary:
      gld${EMULATION_NAME}_find_exp_assignment (exp->trinary.cond);
      gld${EMULATION_NAME}_find_exp_assignment (exp->trinary.lhs);
      gld${EMULATION_NAME}_find_exp_assignment (exp->trinary.rhs);
      break;

    case etree_unary:
      gld${EMULATION_NAME}_find_exp_assignment (exp->unary.child);
      break;

    default:
      break;
    }
}

static char *
gld${EMULATION_NAME}_get_script (isfile)
     int *isfile;
EOF

if test -n "$COMPILE_IN"
then
# Scripts compiled in.

# sed commands to quote an ld script as a C string.
sc="-f ${srcdir}/emultempl/ostring.sed"

cat >>e${EMULATION_NAME}.c <<EOF
{
  *isfile = 0;

  if (link_info.relocateable == true && config.build_constructors == true)
    return
EOF
sed $sc ldscripts/${EMULATION_NAME}.xu                     >> e${EMULATION_NAME}.c
echo '  ; else if (link_info.relocateable == true) return' >> e${EMULATION_NAME}.c
sed $sc ldscripts/${EMULATION_NAME}.xr                     >> e${EMULATION_NAME}.c
echo '  ; else if (!config.text_read_only) return'         >> e${EMULATION_NAME}.c
sed $sc ldscripts/${EMULATION_NAME}.xbn                    >> e${EMULATION_NAME}.c
echo '  ; else if (!config.magic_demand_paged) return'     >> e${EMULATION_NAME}.c
sed $sc ldscripts/${EMULATION_NAME}.xn                     >> e${EMULATION_NAME}.c
echo '  ; else return'                                     >> e${EMULATION_NAME}.c
sed $sc ldscripts/${EMULATION_NAME}.x                      >> e${EMULATION_NAME}.c
echo '; }'                                                 >> e${EMULATION_NAME}.c

else
# Scripts read from the filesystem.

cat >>e${EMULATION_NAME}.c <<EOF
{
  *isfile = 1;

  if (link_info.relocateable == true && config.build_constructors == true)
    return "ldscripts/${EMULATION_NAME}.xu";
  else if (link_info.relocateable == true)
    return "ldscripts/${EMULATION_NAME}.xr";
  else if (!config.text_read_only)
    return "ldscripts/${EMULATION_NAME}.xbn";
  else if (!config.magic_demand_paged)
    return "ldscripts/${EMULATION_NAME}.xn";
  else
    return "ldscripts/${EMULATION_NAME}.x";
}
EOF

fi

cat >>e${EMULATION_NAME}.c <<EOF

struct ld_emulation_xfer_struct ld_${EMULATION_NAME}_emulation =
{
  gld${EMULATION_NAME}_before_parse,
  syslib_default,
  hll_default,
  after_parse_default,
  gld${EMULATION_NAME}_after_open,
  after_allocation_default,
  set_output_arch_default,
  ldemul_default_target,
  gld${EMULATION_NAME}_before_allocation,
  gld${EMULATION_NAME}_get_script,
  "${EMULATION_NAME}",
  "${OUTPUT_FORMAT}",
  0,	/* finish */
  0,	/* create_output_section_statements */
  0,	/* open_dynamic_archive */
  0,	/* place_orphan */
  0,	/* set_symbols */
  gld${EMULATION_NAME}_parse_args,
  gld${EMULATION_NAME}_unrecognized_file,
  NULL, /* list_options */
  NULL, /* recognized_file */
  NULL, /* find potential_libraries */
};
EOF
