;; Scheme side of the gdb module.
;;
;; Copyright (C) 2013 Free Software Foundation, Inc.
;;
;; This file is part of GDB.
;;
;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3 of the License, or
;; (at your option) any later version.
;;
;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.
;;
;; You should have received a copy of the GNU General Public License
;; along with this program.  If not, see <http://www.gnu.org/licenses/>.

(define-module (gdb init)
  #:use-module (gdb))

(define-public SCM_ARG1 1)
(define-public SCM_ARG2 2)

;; The original i/o ports.  In case the user wants them back.
(define %orig-input-port #f)
(define %orig-output-port #f)
(define %orig-error-port #f)

;; Keys for GDB-generated exceptions.
;; gdb:with-stack is handled separately.

(define %exception-keys '(gdb:error
			  gdb:invalid-object-error
			  gdb:memory-error
			  gdb:pp-type-error))

;; Printer for gdb exceptions, used when Scheme tries to print them directly.

(define (%error-printer port key args default-printer)
  (apply (case-lambda
	  ((subr msg args . rest)
	   (if subr
	       (format port "In procedure ~a: " subr))
	   (apply format port msg (or args '())))
	  (_ (default-printer)))
	 args))

;; Print the message part of a gdb:with-stack exception.
;; The arg list is the way it is because it's also passed to
;; set-exception-printer!.
;; We don't print a backtrace here because when invoked by Guile it will have
;; already printed a backtrace.

(define (%print-with-stack-exception-message port key args default-printer)
  (let ((real-key (car args))
	(real-args (cddr args)))
    (%error-printer port real-key real-args default-printer)))

;; Copy of Guile's print-exception that tweaks the output for our purposes.

(define (%print-exception-worker port frame key args)
  (define (default-printer)
    (format port "Throw to key `~a' with args `~s'." key args))
  (format port "ERROR: ")
  ;; Pass #t for tag to catch all errors.
  (catch #t
	 (lambda ()
	   (%error-printer port key args default-printer))
	 (lambda (k . args)
	   (format port "Error while printing gdb exception: ~a ~s."
		   k args)))
  (newline port)
  (force-output port))

;; Print a gdb:with-stack exception, including the backtrace.
;; This is a special exception that wraps the real exception and includes
;; the stack.  It is used to record the stack at the point of the exception,
;; but defer printing it until now.

(define (%print-with-stack-exception port key args)
  (let ((real-key (car args))
	(stack (cadr args))
	(real-args (cddr args)))
    (display "Backtrace:\n" port)
    (display-backtrace stack port #f #f '())
    (newline port)
    (%print-exception port (stack-ref stack 0) real-key real-args)))

;; Called from the C code to print an exception.
;; Guile prints them a little differently than we want.
;; See boot-9.scm:print-exception.

(define (%print-exception port frame key args)
  (cond ((eq? key 'gdb:with-stack)
	 (%print-with-stack-exception port key args))
	((memq key %exception-keys)
	 (%print-exception-worker port frame key args))
	(else
	 (print-exception port frame key args))))

;; Internal utility to check the type of an argument, akin to SCM_ASSERT_TYPE.
;; It's public so other gdb modules can use it.

(define-public (%assert-type test-result arg pos func-name)
  (if (not test-result)
      (scm-error 'wrong-type-arg func-name
		 "Wrong type argument in position ~a: ~s"
		 (list pos arg) (list arg))))

;; Internal utility called during startup to initialize this GDB+Guile.

(define (%initialize)
  (add-to-load-path (string-append *data-directory*
				   file-name-separator-string "guile"))

  (for-each (lambda (key)
	      (set-exception-printer! key %error-printer))
	    %exception-keys)
  (set-exception-printer! 'gdb:with-stack %print-with-stack-exception-message)

  (set! %orig-input-port (set-current-input-port (input-port)))
  (set! %orig-output-port (set-current-output-port (output-port)))
  (set! %orig-error-port (set-current-error-port (error-port))))

;; Public routines.

(define-public (orig-input-port) %orig-input-port)
(define-public (orig-output-port) %orig-output-port)
(define-public (orig-error-port) %orig-error-port)
