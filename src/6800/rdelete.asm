;*-------------------------------------------------------------------
;*
;* RDELETE - Delete a .DSK image in the host's current directory
;*
;*    Port of src/6809/rdelete.asm (2.03, js). FNETDRV must be
;*    loaded. Shows the full name and asks Y/N before deleting.
;*
;*    Usage: RDELETE <name>[.DSK]
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 RDELETE 2.03, with fixes:
;*                          - .DSK is added only if the name has no
;*                            extension (D8);
;*                          - the shown path uses the host's / instead
;*                            of \.
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,3	;* version number
;
BUFFER	equ  SYSFCB	;* "cwd/name.DSK" buffer
BUFEND	equ  BUFFER+255	;* last byte, kept for the terminator
;
ptr	rmb  2		;* pointer in the buffer
namptr	rmb  2		;* where the name starts, less one
eoltmp	rmb  1		;* saved EOL character
dotflg	rmb  1		;* non-zero if the name has an extension
;
cold	jsr  netfind	;* find FNETDRV
	bcs  cold2
	jmp  noload
cold2	ldaa TTYEOL	;* save the EOL character
	staa eoltmp
	clr  TTYEOL
	jsr  argcnt	;* name given?
	bne  del01
	jmp  badnam	;* no
;
;* Get the current directory with ?
;
del01	ldx  #BUFFER
	stx  ptr
	ldaa #'?'
	jsr  schar
	bcs  del02
dnowk	jmp  nowork
del02	jsr  rchar	;* collect it up to the CR
	bcc  dnowk
	cmpa #CR
	beq  del04
	jsr  putit
	bra  del02
del04	jsr  rchar	;* then wait for ACK
	bcc  dnowk
	cmpa #ACK
	bne  del04
;
;* Add a / unless the directory ends with one. namptr points at
;*    the /, which becomes the D command when the name is sent.
;
	ldx  ptr
	cpx  #BUFFER
	beq  del06
	dex
	ldaa 0,x
	cmpa #'/'
	bne  del06
	stx  namptr	;* ends with /
	bra  del08
del06	ldx  ptr
	stx  namptr
	ldaa #'/'
	jsr  putit
;
;* Add the name from the command line, and .DSK if it has no
;*    extension
;
del08	ldx  #ask
	jsr  pdata
	clr  dotflg
del10	jsr  NXTCH	;* skip leading spaces
	cmpa #' '
	beq  del10
del12	cmpa #CR	;* end of the name?
	beq  del14
	cmpa #'.'
	bne  del13
	inc  dotflg
del13	jsr  putit
	jsr  NXTCH
	bra  del12
;
del14	tst  dotflg
	bne  del16
	ldx  #dskext	;* add .DSK
del15	ldaa 0,x
	beq  del16
	stx  namp2
	jsr  putit
	ldx  namp2
	inx
	bra  del15
;
del16	ldx  ptr	;* show it
	clr  0,x
	ldx  #BUFFER
	jsr  pdata
	ldx  #yesno
	jsr  pdata
prompt	jsr  GETCHR	;* get the answer
	anda #$5F
	cmpa #'Y'
	beq  doit
	cmpa #'N'
	beq  nodex
	ldaa #BS
	jsr  PUTCHR
	bra  prompt
;
;* Send D, the name and CR
;
doit	ldx  namptr
	ldaa #'D'
	staa 0,x
dlp	ldaa 0,x
	beq  docr
	jsr  schar
	bcc  nowork
	inx
	bra  dlp
docr	ldaa #CR
	jsr  schar
	bcc  nowork
wtack	jsr  rchar	;* ACK or NAK
	bcc  nowork
	cmpa #NAK
	beq  error
	cmpa #ACK
	bne  wtack
	ldx  #delok
	bra  exit
;
noload	ldx  #nodrv	;* FNETDRV is not loaded
	jsr  pdata
	jmp  WARMS
badnam	ldx  #bad
	bra  exit
error	ldx  #err
	bra  exit
nodex	ldx  #nodel
	bra  exit
nowork	ldx  #timout
exit	jsr  pdata
	ldaa eoltmp	;* restore the EOL character
	staa TTYEOL
	jmp  WARMS	;* back to FLEX
;
;* putit - Put A in the buffer, if there is room
;
putit	ldx  ptr
	cpx  #BUFEND
	beq  put02
	staa 0,x
	inx
	stx  ptr
put02	rts
;
namp2	rmb  2		;* pointer in dskext
;
;* Messages
;
dskext	fcc  ".DSK"
	fcb  0
ask	fcb  CR,LF
	fcc  "Delete the file  "
	fcb  0
timout	fcb  CR,LF
	fcc  "Communication time-out error"
	fcb  0
nodel	fcb  CR,LF
	fcc  "File not deleted."
	fcb  0
yesno	fcc  " (Y/N)? "
	fcb  0
err	fcb  CR,LF
	fcc  "Error in processing file."
	fcb  0
delok	fcb  CR,LF
	fcc  "Command executed OK."
	fcb  0
bad	fcb  CR,LF
	fcc  "Illegal file name (the syntax is RDELETE <filename>)."
	fcb  0
nodrv	fcb  CR,LF
	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  0
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "RDELETE does not fit in the utility command space"
	endif
	end  start
