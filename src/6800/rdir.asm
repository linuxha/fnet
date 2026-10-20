;*-------------------------------------------------------------------
;*
;* RDIR - List the .DSK files in the host's current directory
;*
;*    Port of src/6809/rdir.asm (2.02, js). FNETDRV must be loaded.
;*    Shows 20 lines at a time; SP shows the next 20, ESC stops.
;*
;*    Usage: RDIR [pattern]    e.g. RDIR A2*
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 RDIR 2.02
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,2	;* version number
;
cold	jsr  netfind	;* find FNETDRV
	bcc  noload
	ldaa #'A'	;* send A (directory) command
	jsr  schar
	bcc  nwrkng
	jsr  sndarg	;* then the pattern and CR
	bcc  nwrkng
	jmp  listit	;* show the listing
;
nwrkng	jmp  nowork	;* time-out
;
noload	ldx  #nodrv	;* FNETDRV is not loaded
	jsr  pdata
	jmp  WARMS
;
nodrv	fcb  CR,LF
	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  0
;
	include "fnlist.inc"
	include "fnutil.inc"
;
	if   * > UTLEND
	error "RDIR does not fit in the utility command space"
	endif
	end  start
