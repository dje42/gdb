;; Copyright (C) 2010-2013 Free Software Foundation, Inc.
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

;; This file is part of the GDB testsuite.
;; Exercise Goops support.
;; This feature is currently experimental.

(use-modules ((gdb)) ((gdb experimental)))
(use-modules ((oop goops)))

(define-class <my-value> () (value #:init-keyword #:value #:getter get-value))

;; SMOB will always be a gdb smob.
(define (smob->scm smob)
  (let ((kind (gsmob-kind smob)))
    (case kind
      ((<gdb:value>) (make <my-value> #:value smob))
      (else #f))))

;; N.B.: SCM can be any value, not necessarily the result of smob->scm.
(define (scm->smob scm)
  (let ((kind (class-of scm)))
    (cond
     ((eq? kind <my-value>) (get-value scm))
     (else #f))))

;; Do this to install the converters.
;;(set-smob-converters! smob->scm scm->smob)

;; Versions of converters that throw errors to verify GDB recovers.

;; SMOB will always be a gdb smob.
(define (bad:smob->scm smob)
  (misspelled-doesnt-exist smob))

;; N.B.: SCM can be any value, not necessarily the result of smob->scm.
(define (bad:scm->smob scm)
  (misspelled-doesnt-exist scm))

;; Do this to install the converters.
;;(set-smob-converters! bad:smob->scm bad:scm->smob)
