;; Various experimental utilities.
;; Anything in this file can change or disappear.
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

;; TODO: Split this file up by function?
;; E.g., (gdb experimental ports), etc.

(define-module (gdb experimental)
  #:use-module (gdb)
  #:use-module (gdb init))

;; These are defined in C.
(define-public with-gdb-output-to-port (@ (gdb) %with-gdb-output-to-port))
(define-public with-gdb-error-to-port (@ (gdb) %with-gdb-error-to-port))

(define-public (with-gdb-output-to-string thunk)
  "Calls THUNK and returns all GDB output as a string."
  (call-with-output-string
   (lambda (p) (with-gdb-output-to-port p thunk))))

(define-public (set-smob-converters! from-smob to-smob)
  "Set the GDB smob converters, *smob->scm* and *scm->smob*.

  It is not intended that different modules provide their own values.
  These hooks exist to provide a way to build something on top of GDB smobs,
  but this facility is experimental.

  Arguments: from-smob to-smob
    from-smob: a procedure of one argument, a GDB smob,
      and returns a form of the smob used by the application
    to-smob:   a procedure of one argument, a Scheme object returned
      by from-smob, and returns the original GDB smob.

  The result is unspecified."

  (let ((func-name 'set-smob-converters!)
	(pred (lambda (arg) (or (not arg) (procedure? arg)))))
    (%assert-type (pred from-smob) from-smob SCM_ARG1 func-name)
    (%assert-type (pred to-smob) to-smob SCM_ARG2 func-name))

  (set! (@@ (gdb) *smob->scm*) from-smob)
  (set! (@@ (gdb) *scm->smob*) to-smob)
  (if #f #f))

;; Iterators

(define-public (make-list-iterator l end-marker)
  "Return a <gdb:iterator> object for a list."
  (%assert-type (list? l) l SCM_ARG1 'make-list-iterator)
  (let ((next! (lambda (iter)
		 (let ((l (iterator-progress iter)))
		   (if (eq? l '())
		       end-marker
		       (begin
			 (set-iterator-progress! iter (cdr l))
			 (car l)))))))
    (make-iterator l l next!)))

(define-public (iterator-map proc iter end-marker)
  "Return a list of PROC applied to each element."
  (let loop ((proc proc)
	     (iter iter)
	     (result '()))
    (let ((next (iterator-next! iter)))
      (if (eq? next end-marker)
	  (reverse! result)
	  (loop proc iter (cons (proc next) result))))))

(define-public (iterator-for-each proc iter end-marker)
  "Apply PROC to each element.  The result is unspecified."
  (let ((next (iterator-next! iter)))
    (if (not (eq? next end-marker))
	(begin
	  (proc next)
	  (iterator-for-each proc iter end-marker)))))

(define-public (iterator-filter pred iter end-marker)
  "Return the elements that satify predicate PRED."
  (let loop ((result '()))
    (let ((next (iterator-next! iter)))
      (cond ((eq? next end-marker) (reverse! result))
	    ((pred next) (loop (cons next result)))
	    (else (loop result))))))

(define-public (iterator-until pred iter end-marker)
  "Run the iterator until the result of (pred element) is true.

  Returns:
    The result of the first (pred element) call that returns true,
    or #f if no element matches."
  (let loop ((next (iterator-next! iter)))
    (cond ((eq? next end-marker) #f)
	  ((pred next) => identity)
	  (else (loop (iterator-next! iter))))))
