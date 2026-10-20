;*-------------------------------------------------------------------
;*
;* MEMDUMP - Paged hex and ASCII dump of memory
;*
;*    Port of src/6809/memdump.asm. Not part of FlexNet. Shows 16
;*    lines of 16 bytes; SP shows the next 16 lines, any other key
;*    returns to FLEX.
;*
;*    Usage: MEMDUMP [addr]    hex address, default 0000
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 MEMDUMP 1
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
dump	bra  start
	fcb  1		;* version number
;
saddr	fdb  0		;* address of the current line
nline	rmb  1		;* lines left on the screen
;
start	ldaa LSTTRM	;* address given?
	cmpa #CR
	beq  page	;* no, use the default
	jsr  GETHEX
	stx  saddr
;
page	ldaa #16	;* 16 lines a screen
	staa nline
line	jsr  PCRLF
	ldx  #saddr	;* address
	jsr  OUTADR
	ldaa #' '
	jsr  PUTCHR
;
	ldx  saddr	;* 16 bytes in hex
	ldab #16
hex	jsr  OUTHEX	;* preserves B and X
	ldaa #' '
	jsr  PUTCHR
	inx
	decb
	bne  hex
	ldaa #' '	;* two spaces between hex and ASCII
	jsr  PUTCHR
	ldaa #' '
	jsr  PUTCHR
;
	ldx  saddr	;* the same 16 bytes as ASCII
	ldab #16
asc	ldaa 0,x
	cmpa #' '	;* printable ($20-$7E)?
	bcs  dot
	cmpa #$7F
	bcs  chr
dot	ldaa #'.'	;* no, show a dot
chr	jsr  PUTCHR
	inx
	decb
	bne  asc
	stx  saddr	;* next line
	dec  nline
	bne  line
;
	ldab ECHOFL	;* wait for a key, without echo
	clr  ECHOFL
	jsr  GETCHR	;* preserves B
	stab ECHOFL
	cmpa #' '	;* SP: next screen
	beq  page
	jmp  WARMS	;* otherwise back to FLEX
;
	if   * > UTLEND
	error "MEMDUMP does not fit in the utility command space"
	endif
	end  dump
