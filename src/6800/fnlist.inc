;*-------------------------------------------------------------------
;* fnlist.inc - Paged listing from the FlexNet host (RDIR, RLIST)
;*
;*    Call listit after the listing command has been sent. The host
;*    sends one line per SP it receives and ACK at the end. Lines are
;*    collected in the system FCB, then printed, 20 to a screen.
;*    Never returns: exits to WARMS.
;*-------------------------------------------------------------------
SCREEN	equ  20		;* lines per screen
BUFFER	equ  SYSFCB	;* line buffer
BUFEND	equ  BUFFER+255	;* last byte, kept for the terminator
;
listit	ldaa #SCREEN	;* start a new screen
	staa count
;
lst02	ldx  #BUFFER	;* receive one line
	stx  ptr
lst04	jsr  rchar	;* get a character
	bcc  nowork
	cmpa #ACK	;* finished?
	beq  lstend
	ldx  ptr	;* store it, if there is room
	cpx  #BUFEND
	beq  lst06
	staa 0,x
	inx
	stx  ptr
lst06	cmpa #LF	;* end of the line?
	bne  lst04
;
	ldx  ptr	;* print the line
	clr  0,x
	ldx  #BUFFER
	jsr  pdata
	dec  count	;* end of the screen?
	beq  lst08
	ldaa #' '	;* no, ask for the next line
	jsr  schar
	bcc  nowork
	bra  lst02
;
lst08	ldx  #askusr	;* ask the user
	jsr  pdata
	jsr  GETCHR
	cmpa #ESC
	beq  lst10
	cmpa #' '
	bne  lst08
	jsr  schar	;* SP: go for another screen
	bcc  nowork
	bra  listit
;
lst10	jsr  schar	;* ESC: pass it on to the host
	bcc  nowork
	jsr  PCRLF
lst12	jsr  rchar	;* and wait for ACK
	bcc  nowork
	cmpa #ACK
	bne  lst12
lstend	jmp  WARMS
;
nowork	ldx  #timout	;* time-out
	jsr  pdata
	jmp  WARMS
;
askusr	fcb  CR
	fcc  "Press spacebar to continue, ESC to stop "
	fcb  0
timout	fcb  CR,LF
	fcc  "Communication time-out error"
	fcb  0
;
ptr	rmb  2		;* pointer in the line buffer
count	rmb  1		;* lines left on the screen
