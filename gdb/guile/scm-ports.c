/* Support for connecting Guile's stdio to GDB's.
   as well as r/w memory via ports.

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

/* TODO: Disable ability to import Guile's readline.
   It would be nice to keep its tab-completion support though.
   If Guile could publish the API between its readline.c and readline.scm
   in a way that apps could provide what readline.c provides, that would
   be one solution.  */

#include "defs.h"
#include "interps.h"
#include "target.h"
#include "guile-internal.h"

/* A ui-file for sending output to Guile.  */

typedef struct
{
  int *magic;
  SCM port;
} ioscm_file_port;

/* Data for a memory port.  */

typedef struct
{
  /* Bounds of memory range this port is allowed to access.
     To simplify overflow handling, an END of 0xff..ff is not allowed.
     This also means a start address of 0xff..ff is also allowed.
     I can live with that.  */
  CORE_ADDR start, end;

  /* (end - size + 1), recorded for convenience.  */
  ULONGEST size;

  /* Think of this as the lseek value maintained by the kernel.
     This value is always in the range [0, size].  */
  ULONGEST current;

  /* The size of the internal r/w buffers.
     Scheme ports aren't a straightforward mapping to memory r/w.
     Generally the user specifies how much to r/w and all access is
     unbuffered.  We don't try to provide equivalent access, but we allow
     the user to specify these values to help get something similar.  */
  unsigned read_buf_size, write_buf_size;
} ioscm_memory_port;

/* Copies of the original system input/output/error ports.
   These are recorded for debugging purposes.  */
static SCM orig_input_port_scm;
static SCM orig_output_port_scm;
static SCM orig_error_port_scm;

/* This is the stdio port descriptor, scm_ptob_descriptor.  */
static scm_t_bits stdio_port_desc;

/* Note: scm_make_port_type takes a char * instead of a const char *.  */
static /*const*/ char stdio_port_desc_name[] = "gdb:stdio-port";

/* Names of each gdb port.  */
static const char input_port_name[] = "gdb:stdin";
static const char output_port_name[] = "gdb:stdout";
static const char error_port_name[] = "gdb:stderr";

/* This is the actual port used from Guile.
   We don't expose these to the user though, to ensure they're not
   overwritten.  */
static SCM input_port_scm;
static SCM output_port_scm;
static SCM error_port_scm;

/* Magic number to identify port ui-files.
   Actually, the address of this variable is the magic number.  */
static int file_port_magic;

/* Internal enum for specifying output port.  */
enum oport { GDB_STDOUT, GDB_STDERR };

/* This is the memory port descriptor, scm_ptob_descriptor.  */
static scm_t_bits memory_port_desc;

/* Note: scm_make_port_type takes a char * instead of a const char *.  */
static /*const*/ char memory_port_desc_name[] = "gdb:memory-port";

/* The default amount of memory to fetch for each read/write request.
   Scheme ports don't provide a way to specify the size of a read,
   which is important to us to minimize the number of inferior interactions,
   which over a remote link can be important.  To compensate we augment the
   port API with a new function that let's the user specify how much the next
   read request should fetch.  This is the initial value for each new port.  */
static const unsigned default_read_buf_size = 16;
static const unsigned default_write_buf_size = 16;

/* Arbitrarily limit memory port buffers to 1 byte to 4K.  */
static const unsigned min_memory_port_buf_size = 1;
static const unsigned max_memory_port_buf_size = 4096;

/* "out of range" error message for buf sizes.  */
static char *out_of_range_buf_size;

/* Keywords used by open-memory.  */
static SCM mode_keyword;
static SCM start_keyword;
static SCM size_keyword;

/* Input support (gdb_stdin).
   TODO: readline */

static int
ioscm_fill_input (SCM port)
{
  /* Borrowed from libguile/fports.c.  */
  long count;
  scm_t_port *pt = SCM_PTAB_ENTRY (port);

  /* If we're called on stdout,stderr, punt.  */
  if (scm_is_eq (port, output_port_scm)
      || scm_is_eq (port, error_port_scm))
    return 0; /* Set errno and return -1?  */

  gdb_flush (gdb_stdout);
  gdb_flush (gdb_stderr);

  count = ui_file_read (gdb_stdin, (char *) pt->read_buf, pt->read_buf_size);
  if (count == -1)
    scm_syserror ("ioscm_fill_input_stdin");
  if (count == 0)
    return (scm_t_wchar) EOF;

  pt->read_pos = pt->read_buf;
  pt->read_end = pt->read_buf + count;
  return *pt->read_buf;
}

/* Like fputstrn_filtered, but don't escape characters, except nul.
   Also like fputs_filtered, but a length is specified.  */

static void
fputsn_filtered (const char *s, size_t size, struct ui_file *stream)
{
  size_t i;

  for (i = 0; i < size; ++i)
    {
      if (s[i] == '\0')
	fputs_filtered ("\\000", stream);
      else
	fputc_filtered (s[i], stream);
    }
}

/* Write to gdb's stdout or stderr.  */

static void
ioscm_write (SCM port, const void *data, size_t size)
{
  volatile struct gdb_exception except;

  /* If we're called on stdin, punt.  */
  if (scm_is_eq (port, input_port_scm))
    return;

  TRY_CATCH (except, RETURN_MASK_ALL)
    {
      if (scm_is_eq (port, error_port_scm))
	fputsn_filtered (data, size, gdb_stderr);
      else
	fputsn_filtered (data, size, gdb_stdout);
    }
  GDBSCM_HANDLE_GDB_EXCEPTION (except);
}

/* Flush gdb's stdout or stderr.  */

static void
ioscm_flush (SCM port)
{
  /* If we're called on stdin, punt.  */
  if (scm_is_eq (port, input_port_scm))
    return;

  if (scm_is_eq (port, error_port_scm))
    gdb_flush (gdb_stderr);
  else
    gdb_flush (gdb_stdout);
}

/* GDB's ports are accessed via functions to keep them read-only.  */

/* (input-port) -> port */

static SCM
gdbscm_input_port (void)
{
  return input_port_scm;
}

/* (output-port) -> port */

static SCM
gdbscm_output_port (void)
{
  return output_port_scm;
}

/* (error-port) -> port */

static SCM
gdbscm_error_port (void)
{
  return error_port_scm;
}

/* Support for sending GDB I/O to Guile ports.  */

static void
ioscm_file_port_delete (struct ui_file *file)
{
  ioscm_file_port *stream = ui_file_data (file);

  if (stream->magic != &file_port_magic)
    internal_error (__FILE__, __LINE__,
		    _("ioscm_file_port_delete: bad magic number"));
  xfree (stream);
}

static void
ioscm_file_port_rewind (struct ui_file *file)
{
  ioscm_file_port *stream = ui_file_data (file);

  if (stream->magic != &file_port_magic)
    internal_error (__FILE__, __LINE__,
		    _("ioscm_file_port_rewind: bad magic number"));

  scm_truncate_file (stream->port, 0);
}

static void
ioscm_file_port_put (struct ui_file *file,
		     ui_file_put_method_ftype *write,
		     void *dest)
{
  ioscm_file_port *stream = ui_file_data (file);

  if (stream->magic != &file_port_magic)
    internal_error (__FILE__, __LINE__,
		    _("ioscm_file_port_put: bad magic number"));

  /* This function doesn't meld with ports very well.  */
}

static void
ioscm_file_port_write (struct ui_file *file,
		       const char *buffer,
		       long length_buffer)
{
  ioscm_file_port *stream = ui_file_data (file);

  if (stream->magic != &file_port_magic)
    internal_error (__FILE__, __LINE__,
		    _("ioscm_pot_file_write: bad magic number"));

  scm_c_write (stream->port, buffer, length_buffer);
}

/* Return a ui_file that writes to PORT.  */

static struct ui_file *
ioscm_file_port_new (SCM port)
{
  ioscm_file_port *stream = XMALLOC (ioscm_file_port);
  struct ui_file *file = ui_file_new ();

  set_ui_file_data (file, stream, ioscm_file_port_delete);
  set_ui_file_rewind (file, ioscm_file_port_rewind);
  set_ui_file_put (file, ioscm_file_port_put);
  set_ui_file_write (file, ioscm_file_port_write);
  stream->magic = &file_port_magic;
  stream->port = port;

  return file;
}

/* Return the file mode bits stored in desc.  */

static int
get_mode_bits (int desc)
{
  return desc & (SCM_OPN | SCM_RDNG | SCM_WRTNG | SCM_BUF0 | SCM_BUFLINE);
}

/* Subclass an fport, installing our methods.  */

static scm_t_bits
ioscm_subclass_fport (SCM orig_port)
{
  int orig_port_type, port_type;
  scm_t_bits port_desc;

  orig_port_type = SCM_PTOBNUM (orig_port);

  port_desc = scm_make_port_type (stdio_port_desc_name, NULL, NULL);
  port_type = SCM_TC2PTOBNUM (port_desc);

  /* Copy fport methods into our "vtable".
     Note: scm_ptobs is deprecated in 2.2.  Need to see how this will
     need to change.  */
  scm_ptobs[port_type] = scm_ptobs[orig_port_type];
  /* Preserve the name.  */
  scm_ptobs[port_type].name = stdio_port_desc_name;
  /* There is no scm_set_port_fill_input.  */
  scm_ptobs[port_type].fill_input = ioscm_fill_input;
  /* There is no scm_set_port_write.  */
  scm_ptobs[port_type].write = ioscm_write;
  scm_set_port_flush (port_desc, ioscm_flush);

  return port_desc;
}

/* Helper routine for with-{output,error}-to-port.  */

static SCM
ioscm_with_output_to_port_worker (SCM port, SCM thunk, enum oport oport,
				  const char *func_name)
{
  struct ui_file *port_file;
  struct cleanup *cleanups;
  SCM result;

  SCM_ASSERT_TYPE (gdbscm_is_true (scm_output_port_p (port)), port,
		   SCM_ARG1, func_name, "output port");
  SCM_ASSERT_TYPE (gdbscm_is_true (scm_thunk_p (thunk)), thunk,
		   SCM_ARG2, func_name, "thunk");

  cleanups = set_batch_flag_and_make_cleanup_restore_page_info ();

  make_cleanup_restore_integer (&interpreter_async);
  interpreter_async = 0;

  port_file = ioscm_file_port_new (port);

  make_cleanup_ui_file_delete (port_file);

  if (oport == GDB_STDERR)
    {
      make_cleanup_restore_ui_file (&gdb_stderr);
      gdb_stderr = port_file;
    }
  else
    {
      make_cleanup_restore_ui_file (&gdb_stdout);

      if (ui_out_redirect (current_uiout, port_file) < 0)
	warning (_("Current output protocol does not support redirection"));
      else
	make_cleanup_ui_out_redirect_pop (current_uiout);

      gdb_stdout = port_file;
    }

  result = gdbscm_safe_call_0 (thunk, NULL);

  do_cleanups (cleanups);

  if (gdbscm_is_exception (result))
    gdbscm_throw (result);

  return result;
}

/* (%with-gdb-output-to-port port thunk) -> object
   This function is experimental.
   IWBN to not include "gdb" in the name, but it would collide with a standard
   procedure, and it's common to import the gdb module without a prefix.
   There are ways around this, but they're more cumbersome.

   This has % in the name because it's experimental, and we want the
   user-visible version to come from module (gdb experimental).  */

static SCM
gdbscm_percent_with_gdb_output_to_port (SCM port, SCM thunk)
{
  return ioscm_with_output_to_port_worker (port, thunk, GDB_STDOUT, FUNC_NAME);
}

/* (%with-gdb-error-to-port port thunk) -> object
   This function is experimental.
   IWBN to not include "gdb" in the name, but it would collide with a standard
   procedure, and it's common to import the gdb module without a prefix.
   There are ways around this, but they're more cumbersome.

   This has % in the name because it's experimental, and we want the
   user-visible version to come from module (gdb experimental).  */

static SCM
gdbscm_percent_with_gdb_error_to_port (SCM port, SCM thunk)
{
  return ioscm_with_output_to_port_worker (port, thunk, GDB_STDERR, FUNC_NAME);
}

/* Support for r/w memory via ports.  */

/* Perform an "lseek" to OFFSET,WHENCE on memory port IOMEM.
   OFFSET must be in the range [0,size].
   The result is non-zero for success, zero for failure.  */

static int
ioscm_lseek_address (ioscm_memory_port *iomem, LONGEST offset, int whence)
{
  CORE_ADDR new_current;

  gdb_assert (iomem->current <= iomem->size);

  switch (whence)
    {
    case SEEK_CUR:
      /* Catch over/underflow.  */
      if ((offset < 0 && iomem->current + offset > iomem->current)
	  || (offset >= 0 && iomem->current + offset < iomem->current))
	return 0;
      new_current = iomem->current + offset;
      break;
    case SEEK_SET:
      new_current = offset;
      break;
    case SEEK_END:
      if (offset == 0)
	{
	  new_current = iomem->size;
	  break;
	}
      /* TODO: Not supported yet.  */
      return 0;
    default:
      return 0;
    }

  if (new_current > iomem->size)
    return 0;
  iomem->current = new_current;
  return 1;
}

/* "fill_input" method for memory ports.  */

static int
gdbscm_memory_port_fill_input (SCM port)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);
  size_t to_read;

  /* "current" is the offset of the first byte we want to read.  */
  if (iomem->current >= iomem->size)
    return EOF;

  /* Don't read outside the allowed memory range.  */
  to_read = pt->read_buf_size;
  if (to_read > iomem->size - iomem->current)
    to_read = iomem->size - iomem->current;

  if (target_read_memory (iomem->start + iomem->current, pt->read_buf,
			  to_read) != 0)
    gdbscm_memory_error (FUNC_NAME, "error reading memory", SCM_EOL);

  pt->read_pos = pt->read_buf;
  pt->read_end = pt->read_buf + to_read;
  iomem->current += to_read;
  return *pt->read_buf;
}

/* "end_input" method for memory ports.
   Clear the read buffer and adjust the file position for unread bytes.  */

static void
gdbscm_memory_port_end_input (SCM port, int offset)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);
  size_t remaining = pt->read_end - pt->read_pos;

  /* Note: Use of "int offset" is specified by Guile ports API.  */
  if ((offset < 0 && remaining + offset > remaining)
      || (offset > 0 && remaining + offset < remaining))
    {
      gdbscm_out_of_range_error (FUNC_NAME, 0, scm_from_int (offset),
				 "overflow in offset calculation");
    }
  offset += remaining;

  if (offset > 0)
    {
      pt->read_pos = pt->read_end;
      /* Throw error if unread-char used at beginning of file
	 then attempting to write.  Seems correct.  */
      if (!ioscm_lseek_address (iomem, -offset, SEEK_CUR))
	{
	  gdbscm_out_of_range_error (FUNC_NAME, 0, scm_from_int (offset),
				     "bad offset");
	}
    }

  pt->rw_active = SCM_PORT_NEITHER;
}

/* "flush" method for memory ports.  */

static void
gdbscm_memory_port_flush (SCM port)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);
  size_t to_write = pt->write_pos - pt->write_buf;

  if (to_write == 0)
    return;

  /* There's no way to indicate a short write, so if the request goes past
     the end of the port's memory range, flag an error.  */
  if (to_write > iomem->size - iomem->current)
    {
      gdbscm_out_of_range_error (FUNC_NAME, 0,
				 gdbscm_scm_from_ulongest (to_write),
				 "writing beyond end of memory range");
    }

  if (target_write_memory (iomem->start + iomem->current, pt->write_buf,
			   to_write) != 0)
    gdbscm_memory_error (FUNC_NAME, "error writing memory", SCM_EOL);

  iomem->current += to_write;
  pt->write_pos = pt->write_buf;
  pt->rw_active = SCM_PORT_NEITHER;
}

/* "write" method for memory ports.  */

static void
gdbscm_memory_port_write (SCM port, const void *data, size_t size)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);
  const char *input = (char *) data;

  /* We could get fancy here, and try to buffer the request since we're
     buffering anyway.  But there's currently no need.  */

  /* First flush what's currently buffered.  */
  gdbscm_memory_port_flush (port);

  /* There's no way to indicate a short write, so if the request goes past
     the end of the port's memory range, flag an error.  */
  if (size > iomem->size - iomem->current)
    {
      gdbscm_out_of_range_error (FUNC_NAME, 0, gdbscm_scm_from_ulongest (size),
				 "writing beyond end of memory range");
    }

  if (target_write_memory (iomem->start + iomem->current, data, size) != 0)
    gdbscm_memory_error (FUNC_NAME, "error writing memory", SCM_EOL);

  iomem->current += size;
}

/* "seek" method for memory ports.  */

static scm_t_off
gdbscm_memory_port_seek (SCM port, scm_t_off offset, int whence)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);
  CORE_ADDR result;
  int rc;

  if (pt->rw_active == SCM_PORT_WRITE)
    {
      if (offset != 0 || whence != SEEK_CUR)
	{
	  gdbscm_memory_port_flush (port);
	  rc = ioscm_lseek_address (iomem, offset, whence);
	  result = iomem->current;
	}
      else
	{
	  /* Read current position without disturbing the buffer,
	     but flag an error if what's in the buffer goes outside the
	     allowed range.  */
	  CORE_ADDR current = iomem->current;
	  size_t delta = pt->write_pos - pt->write_buf;

	  if (current + delta < current
	      || current + delta > iomem->size + 1)
	    rc = 0;
	  else
	    {
	      result = current + delta;
	      rc = 1;
	    }
	}
    }
  else if (pt->rw_active == SCM_PORT_READ)
    {
      if (offset != 0 || whence != SEEK_CUR)
	{
	  scm_end_input (port);
	  rc = ioscm_lseek_address (iomem, offset, whence);
	  result = iomem->current;
	}
      else
	{
	  /* Read current position without disturbing the buffer
	     (particularly the unread-char buffer).  */
	  CORE_ADDR current = iomem->current;
	  size_t remaining = pt->read_end - pt->read_pos;

	  if (current - remaining > current
	      || current - remaining < iomem->start)
	    rc = 0;
	  else
	    {
	      result = current - remaining;
	      rc = 1;
	    }

	  if (rc != 0 && pt->read_buf == pt->putback_buf)
	    {
	      size_t saved_remaining = pt->saved_read_end - pt->saved_read_pos;

	      if (result - saved_remaining > result
		  || result - saved_remaining < iomem->start)
		rc = 0;
	      else
		result -= saved_remaining;
	    }
	}
    }
  else /* SCM_PORT_NEITHER */
    {
      rc = ioscm_lseek_address (iomem, offset, whence);
      result = iomem->current;
    }

  if (rc == 0)
    {
      gdbscm_out_of_range_error (FUNC_NAME, 0,
				 gdbscm_scm_from_longest (offset),
				 "bad seek");
    }

  /* TODO: The Guile API doesn't support 32x64.  We can't fix that here,
     and there's no need to throw an error if the new address can't be
     represented in a scm_t_off.  But we could return something less
     clumsy.  */
  return result;
}

/* "close" method for memory ports.  */

static int
gdbscm_memory_port_close (SCM port)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);

  gdbscm_memory_port_flush (port);

  if (pt->read_buf == pt->putback_buf)
    pt->read_buf = pt->saved_read_buf;
  xfree (pt->read_buf);
  xfree (pt->write_buf);
  scm_gc_free (iomem, sizeof (*iomem), "memory port");

  return 0;
}

/* "free" method for memory ports.  */

static size_t
gdbscm_memory_port_free (SCM port)
{
  gdbscm_memory_port_close (port);

  return 0;
}

/* "print" method for memory ports.  */

static int
gdbscm_memory_port_print (SCM exp, SCM port, scm_print_state *pstate)
{
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (exp);
  char *type = SCM_PTOBNAME (SCM_PTOBNUM (exp));

  scm_puts ("#<", port);
  scm_print_port_mode (exp, port);
  /* scm_print_port_mode includes a trailing space.  */
  gdbscm_printf (port, "%s %s-%s", type,
		 hex_string (iomem->start), hex_string (iomem->end));
  scm_putc ('>', port);
  return 1;
}

/* Create the port type used for memory.  */

static scm_t_bits
ioscm_create_memory_port_type (char *name)
{
  scm_t_bits port_desc;

  port_desc = scm_make_port_type (name, gdbscm_memory_port_fill_input,
				  gdbscm_memory_port_write);

  scm_set_port_end_input (port_desc, gdbscm_memory_port_end_input);
  scm_set_port_flush (port_desc, gdbscm_memory_port_flush);
  scm_set_port_seek (port_desc, gdbscm_memory_port_seek);
  scm_set_port_close (port_desc, gdbscm_memory_port_close);
  scm_set_port_free (port_desc, gdbscm_memory_port_free);
  scm_set_port_print (port_desc, gdbscm_memory_port_print);

  return port_desc;
}

/* Helper for gdbscm_open_memory to parse the mode bits.
   An exception is thrown if MODE is invalid.  */

static long
ioscm_parse_mode_bits (const char *func_name, const char *mode)
{
  const char *p;
  long mode_bits;

  if (*mode != 'r' && *mode != 'w')
    {
      gdbscm_out_of_range_error (func_name, 0,
				 gdbscm_scm_from_c_string (mode),
				 "bad mode string");
    }
  for (p = mode + 1; *p != '\0'; ++p)
    {
      switch (*p)
	{
	case 'b':
	case '+':
	  break;
	default:
	  gdbscm_out_of_range_error (func_name, 0,
				     gdbscm_scm_from_c_string (mode),
				     "bad mode string");
	}
    }

  /* Kinda awkward to convert the mode from SCM -> string only to have Guile
     convert it back to SCM, but that's the API we have to work with.  */
  mode_bits = scm_mode_bits ((char *) mode);

  return mode_bits;
}

/* Helper for gdbscm_open_memory to do the low level work
   of opening a port.  */

static SCM
ioscm_open_port (scm_t_bits port_type, long mode_bits)
{
  SCM port;

#if 0 /* TODO: Guile doesn't export this.  What to do?  */
  scm_i_scm_pthread_mutex_lock (&scm_i_port_table_mutex);
#endif

  port = scm_new_port_table_entry (port_type);

  SCM_SET_CELL_TYPE (port, port_type | mode_bits);

#if 0 /* TODO: Guile doesn't export this.  What to do?  */
  scm_i_pthread_mutex_unlock (&scm_i_port_table_mutex);
#endif

  return port;
}

/* Helper for gdbscm_open_memory to finish initializing the port.
   The port has address range [start,end].
   To simplify overflow handling, an END of 0xff..ff is not allowed.
   This also means a start address of 0xff..f is also not allowed.
   I can live with that.  */

static void
ioscm_init_memory_port (SCM port, CORE_ADDR start, CORE_ADDR end)
{
  scm_t_port *pt;
  ioscm_memory_port *iomem;

  gdb_assert (start <= end);
  gdb_assert (end < ~(CORE_ADDR) 0);

  iomem = (ioscm_memory_port *) scm_gc_malloc_pointerless (sizeof (*iomem),
							   "memory port");

  iomem->start = start;
  iomem->end = end;
  iomem->size = end - start + 1;
  iomem->current = 0;
  iomem->read_buf_size = default_read_buf_size;
  iomem->write_buf_size = default_write_buf_size;

  pt = SCM_PTAB_ENTRY (port);
  /* Match the expectation of `binary-port?'.  */
  pt->encoding = NULL;
  pt->rw_random = 1;
  pt->read_buf_size = iomem->read_buf_size;
  pt->read_buf = xmalloc (pt->read_buf_size);
  pt->read_pos = pt->read_end = pt->read_buf;
  pt->write_buf_size = iomem->write_buf_size;
  pt->write_buf = xmalloc (pt->write_buf_size);
  pt->write_pos = pt->write_buf;
  pt->write_end = pt->write_buf + pt->write_buf_size;

  SCM_SETSTREAM (port, iomem);
}

/* Re-initialize a memory port, updating its read/write buffer sizes.
   An exception is thrown if data is still buffered, except in the case
   where the buffer size isn't changing (since that's just a nop).  */

static void
ioscm_reinit_memory_port (SCM port, size_t read_buf_size,
			  size_t write_buf_size, const char *func_name)
{
  scm_t_port *pt = SCM_PTAB_ENTRY (port);
  ioscm_memory_port *iomem = (ioscm_memory_port *) SCM_STREAM (port);

  gdb_assert (read_buf_size >= min_memory_port_buf_size
	      && read_buf_size <= max_memory_port_buf_size);
  gdb_assert (write_buf_size >= min_memory_port_buf_size
	      && write_buf_size <= max_memory_port_buf_size);

  /* First check if anything is buffered.  */

  if (read_buf_size != pt->read_buf_size
      && pt->read_end != pt->read_buf)
    {
      scm_misc_error (func_name, "read buffer not empty: ~a",
		      scm_list_1 (port));
    }

  if (write_buf_size != pt->write_buf_size
      && pt->write_pos != pt->write_buf)
    {
      scm_misc_error (func_name, "write buffer not empty: ~a",
		      scm_list_1 (port));
    }

  /* Now we can update the buffer sizes, but only if the size has changed.  */

  if (read_buf_size != pt->read_buf_size)
    {
      iomem->read_buf_size = read_buf_size;
      pt->read_buf_size = read_buf_size;
      xfree (pt->read_buf);
      pt->read_buf = xmalloc (pt->read_buf_size);
      pt->read_pos = pt->read_end = pt->read_buf;
    }

  if (write_buf_size != pt->write_buf_size)
    {
      iomem->write_buf_size = write_buf_size;
      pt->write_buf_size = write_buf_size;
      xfree (pt->write_buf);
      pt->write_buf = xmalloc (pt->write_buf_size);
      pt->write_pos = pt->write_buf;
      pt->write_end = pt->write_buf + pt->write_buf_size;
    }
}

/* (open-memory [#:mode string] [#:start address] [#:size integer]) -> port
   Return a port that can be used for reading and writing memory.
   MODE is a string, and must be one of "r", "w", or "r+".
   For compatibility "b" (binary) may also be present, but we ignore it:
   memory ports are binary only.

   The chunk of memory that can be accessed can be bounded.
   If both START,SIZE are unspecified, all of memory can be accessed.
   If only START is specified, all of memory from that point on can be
   accessed.  If only SIZE if specified, all memory in [0,SIZE) can be
   accessed.  If both are specified, all memory in [START,START+SIZE) can be
   accessed.

   Note: If it becomes useful enough we can later add #:end as an alternative
   to #:size.  For now it is left out.

   The result is a Scheme port, and its semantics are a bit odd for accessing
   memory (e.g. unget), but we don't try hide this.  It's a port.

   N.B. Seeks on the port must be in the range [0,size).
   This is for similarity with bytevector ports, and so that one can seek
   to the first byte.  */

static SCM
gdbscm_open_memory (SCM rest)
{
  const SCM keywords[] = {
    mode_keyword, start_keyword, size_keyword, SCM_BOOL_F
  };
  char *mode = NULL;
  CORE_ADDR start = 0;
  CORE_ADDR end;
  int mode_arg_pos = -1, start_arg_pos = -1, size_arg_pos = -1;
  ULONGEST size;
  SCM port;
  long mode_bits;

  gdbscm_parse_function_args (FUNC_NAME, SCM_ARG1, keywords, "#sUU", rest,
			      &mode_arg_pos, &mode,
			      &start_arg_pos, &start,
			      &size_arg_pos, &size);

  scm_dynwind_begin (0);

  if (mode == NULL)
    mode = xstrdup ("r");
  scm_dynwind_free (mode);

  if (start == ~(CORE_ADDR) 0)
    {
      gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG1, scm_from_int (-1),
				 "start address of 0xff..ff not allowed");
    }

  if (size_arg_pos > 0)
    {
      if (size == 0)
	{
	  gdbscm_out_of_range_error (FUNC_NAME, 0, scm_from_int (0),
				     "zero size");
	}
      /* For now be strict about start+size overflowing.  If it becomes
	 a nuisance we can relax things later.  */
      if (start + size < start)
	{
	  gdbscm_out_of_range_error (FUNC_NAME, 0,
				scm_list_2 (gdbscm_scm_from_ulongest (start),
					    gdbscm_scm_from_ulongest (size)),
				     "start+size overflows");
	}
      end = start + size - 1;
      if (end == ~(CORE_ADDR) 0)
	{
	  gdbscm_out_of_range_error (FUNC_NAME, 0,
				scm_list_2 (gdbscm_scm_from_ulongest (start),
					    gdbscm_scm_from_ulongest (size)),
				     "end address of 0xff..ff not allowed");
	}
    }
  else
    end = (~(CORE_ADDR) 0) - 1;

  mode_bits = ioscm_parse_mode_bits (FUNC_NAME, mode);

  port = ioscm_open_port (memory_port_desc, mode_bits);

  ioscm_init_memory_port (port, start, end);

  scm_dynwind_end ();

  /* TODO: Set the file name as "memory-start-end"?  */
  return port;
}

/* Return non-zero if OBJ is a memory port.  */

static int
gdbscm_is_memory_port (SCM obj)
{
  return !SCM_IMP (obj) && (SCM_TYP16 (obj) == memory_port_desc);
}

/* (memory-port? obj) -> boolean */

static SCM
gdbscm_memory_port_p (SCM obj)
{
  return scm_from_bool (gdbscm_is_memory_port (obj));
}

/* (memory-port-range port) -> (start end) */

static SCM
gdbscm_memory_port_range (SCM port)
{
  ioscm_memory_port *iomem;

  SCM_ASSERT_TYPE (gdbscm_is_memory_port (port), port, SCM_ARG1, FUNC_NAME,
		   memory_port_desc_name);

  iomem = (ioscm_memory_port *) SCM_STREAM (port);
  return scm_list_2 (gdbscm_scm_from_ulongest (iomem->start),
		     gdbscm_scm_from_ulongest (iomem->end));
}

/* (memory-port-read-buffer-size port) -> integer */

static SCM
gdbscm_memory_port_read_buffer_size (SCM port)
{
  ioscm_memory_port *iomem;

  SCM_ASSERT_TYPE (gdbscm_is_memory_port (port), port, SCM_ARG1, FUNC_NAME,
		   memory_port_desc_name);

  iomem = (ioscm_memory_port *) SCM_STREAM (port);
  return scm_from_uint (iomem->read_buf_size);
}

/* (set-memory-port-read-buffer-size! port size) -> unspecified
   An exception is thrown if read data is still buffered.  */

static SCM
gdbscm_set_memory_port_read_buffer_size_x (SCM port, SCM size)
{
  ioscm_memory_port *iomem;

  SCM_ASSERT_TYPE (gdbscm_is_memory_port (port), port, SCM_ARG1, FUNC_NAME,
		   memory_port_desc_name);
  SCM_ASSERT_TYPE (scm_is_integer (size), size, SCM_ARG2, FUNC_NAME,
		   "integer");

  if (!scm_is_unsigned_integer (size, min_memory_port_buf_size,
				max_memory_port_buf_size))
    {
      gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG2, size,
				 out_of_range_buf_size);
    }

  iomem = (ioscm_memory_port *) SCM_STREAM (port);
  ioscm_reinit_memory_port (port, scm_to_uint (size), iomem->write_buf_size,
			    FUNC_NAME);

  return SCM_UNSPECIFIED;
}

/* (memory-port-write-buffer-size port) -> integer */

static SCM
gdbscm_memory_port_write_buffer_size (SCM port)
{
  ioscm_memory_port *iomem;

  SCM_ASSERT_TYPE (gdbscm_is_memory_port (port), port, SCM_ARG1, FUNC_NAME,
		   memory_port_desc_name);

  iomem = (ioscm_memory_port *) SCM_STREAM (port);
  return scm_from_uint (iomem->write_buf_size);
}

/* (set-memory-port-write-buffer-size! port size) -> unspecified
   An exception is thrown if write data is still buffered.  */

static SCM
gdbscm_set_memory_port_write_buffer_size_x (SCM port, SCM size)
{
  ioscm_memory_port *iomem;

  SCM_ASSERT_TYPE (gdbscm_is_memory_port (port), port, SCM_ARG1, FUNC_NAME,
		   memory_port_desc_name);
  SCM_ASSERT_TYPE (scm_is_integer (size), size, SCM_ARG2, FUNC_NAME,
		   "integer");

  if (!scm_is_unsigned_integer (size, min_memory_port_buf_size,
				max_memory_port_buf_size))
    {
      gdbscm_out_of_range_error (FUNC_NAME, SCM_ARG2, size,
				 out_of_range_buf_size);
    }

  iomem = (ioscm_memory_port *) SCM_STREAM (port);
  ioscm_reinit_memory_port (port, iomem->read_buf_size, scm_to_uint (size),
			    FUNC_NAME);

  return SCM_UNSPECIFIED;
}

/* Initialize gdb ports.  */

static const scheme_function port_functions[] =
{
  { "input-port", 0, 0, 0, gdbscm_input_port,
    "\
Return gdb's input port." },

  { "output-port", 0, 0, 0, gdbscm_output_port,
    "\
Return gdb's output port." },

  { "error-port", 0, 0, 0, gdbscm_error_port,
    "\
Return gdb's error port." },

#if 0 /* TODO */
  { "with-gdb-input-from-port", 2, 0, 0,
    gdbscm_percent_with_gdb_input_from_port,
    "\
Temporarily set GDB's input port to PORT and then invoke THUNK.\n\
\n\
  Arguments: port thunk\n\
  Returns: The result of calling THUNK.\n\
\n\
This procedure is experimental." },
#endif

  { "%with-gdb-output-to-port", 2, 0, 0,
    gdbscm_percent_with_gdb_output_to_port,
    "\
Temporarily set GDB's output port to PORT and then invoke THUNK.\n\
\n\
  Arguments: port thunk\n\
  Returns: The result of calling THUNK.\n\
\n\
This procedure is experimental." },

  { "%with-gdb-error-to-port", 2, 0, 0,
    gdbscm_percent_with_gdb_error_to_port,
    "\
Temporarily set GDB's error port to PORT and then invoke THUNK.\n\
\n\
  Arguments: port thunk\n\
  Returns: The result of calling THUNK.\n\
\n\
This procedure is experimental." },

  { "open-memory", 0, 0, 1, gdbscm_open_memory,
    "\
Return a port that can be used for reading/writing inferior memory.\n\
\n\
  Arguments: [#:mode string] [#:start address] [#:size integer]\n\
  Returns: A port object." },

  { "memory-port?", 1, 0, 0, gdbscm_memory_port_p,
    "\
Return #t if the object is a memory port." },

  { "memory-port-range", 1, 0, 0, gdbscm_memory_port_range,
    "\
Return the memory range of the port as (start end)." },

  { "memory-port-read-buffer-size", 1, 0, 0,
    gdbscm_memory_port_read_buffer_size,
    "\
Return the size of the read buffer for the memory port." },

  { "set-memory-port-read-buffer-size!", 2, 0, 0,
    gdbscm_set_memory_port_read_buffer_size_x,
    "\
Set the size of the read buffer for the memory port.\n\
\n\
  Arguments: port integer\n\
  Returns: unspecified." },

  { "memory-port-write-buffer-size", 1, 0, 0,
    gdbscm_memory_port_write_buffer_size,
    "\
Return the size of the write buffer for the memory port." },

  { "set-memory-port-write-buffer-size!", 2, 0, 0,
    gdbscm_set_memory_port_write_buffer_size_x,
    "\
Set the size of the write buffer for the memory port.\n\
\n\
  Arguments: port integer\n\
  Returns: unspecified." },

  END_FUNCTIONS
};

void
gdbscm_initialize_ports (void)
{
  /* What we're trying to do here is make a copy of stdin, etc. and replacing
     the few methods we need to, without affecting anything else.
     E.g. isatty? will still return the same value.  */

  orig_input_port_scm = scm_current_input_port ();
  orig_output_port_scm = scm_current_output_port ();
  orig_error_port_scm = scm_current_error_port ();

  stdio_port_desc = ioscm_subclass_fport (orig_input_port_scm);

  /* Set up stdin.  */

  input_port_scm =
    scm_fdes_to_port (0, isatty (0) ? "r0" : "r",
		      gdbscm_scm_from_c_string (input_port_name));
  SCM_SET_CELL_TYPE (input_port_scm,
		     stdio_port_desc
		     | get_mode_bits (SCM_CELL_TYPE (input_port_scm)));

  /* Set up stdout.  */

  output_port_scm =
    scm_fdes_to_port (1, isatty (1) ? "w0" : "w",
		      gdbscm_scm_from_c_string (output_port_name));
  SCM_SET_CELL_TYPE (output_port_scm,
		     stdio_port_desc
		     | get_mode_bits (SCM_CELL_TYPE (output_port_scm)));

  /* Set up stderr.  */

  error_port_scm =
    scm_fdes_to_port (2, isatty (2) ? "w0" : "w",
		      gdbscm_scm_from_c_string (error_port_name));
  SCM_SET_CELL_TYPE (error_port_scm,
		     stdio_port_desc
		     | get_mode_bits (SCM_CELL_TYPE (error_port_scm)));

  /* Set up memory ports.  */

  memory_port_desc =
    ioscm_create_memory_port_type (memory_port_desc_name);

  /* Install the accessor functions.  */

  gdbscm_define_functions (port_functions, 1);

  /* Keyword args for open-memory.  */

  mode_keyword = scm_from_latin1_keyword ("mode");
  start_keyword = scm_from_latin1_keyword ("start");
  size_keyword = scm_from_latin1_keyword ("size");

  /* Error message text for "out of range" memory port buffer sizes.  */

  out_of_range_buf_size = xstrprintf ("size not between %u - %u",
				      min_memory_port_buf_size,
				      max_memory_port_buf_size);
}
