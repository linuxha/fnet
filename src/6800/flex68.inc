;*-------------------------------------------------------------------
;* flex68.inc - FLEX 6800 equates for the FlexNet client programs
;*
;*    Values from src/6809/6809-analyze.md section 7. FLEX 6800 sits
;*    $2000 below FLEX09 for the system area and DOS entry points.
;*-------------------------------------------------------------------
;
;* System area
;
LINBUF	equ  $A080	;* line buffer
UTLORG	equ  $A100	;* utility command space (ORG of every utility)
SCNEND	equ  $A000	;* upper bound of the signature scan
TTYEOL	equ  $AC02	;* end-of-line character
LSTTRM	equ  $AC11	;* last terminator
TTYLBP	equ  $AC14	;* line buffer pointer
MEMEND	equ  $AC2B	;* end of user RAM
ECHOFL	equ  $AC2F	;* input echo flag (0 = GETCHR does not echo)
PRCNT	equ  $AC34	;* non-zero if the spooler task is running
;
;* DOS entry points
;
WARMS	equ  $AD03	;* warm start
GETCHR	equ  $AD15	;* get a character
PUTCHR	equ  $AD18	;* put a character
INBUF	equ  $AD1B	;* input a line into the line buffer
PSTRNG	equ  $AD1E	;* print CR/LF then an EOT-terminated string
CLASS	equ  $AD21	;* classify a character
PCRLF	equ  $AD24	;* print CR/LF
NXTCH	equ  $AD27	;* get the next line buffer character
GETFIL	equ  $AD2D	;* parse a file spec into an FCB
OUTDEC	equ  $AD39	;* print a decimal number
OUTHEX	equ  $AD3C	;* print a hex byte
GETHEX	equ  $AD42	;* get a hex number
OUTADR	equ  $AD45	;* print a hex address
INDEC	equ  $AD48	;* get a decimal number
;
;* Disk driver jump table (FLEX 3.0 SWTPC, see t/dskdrv.lst)
;
DRVTBL	equ  $BE80	;* READ, WRITE, VERIFY, RESTORE, DRIVE, CHKRDY, QUICK, ...
;
;* ASCII
;
EOT	equ  $04
ACK	equ  $06
BS	equ  $08
LF	equ  $0A
CR	equ  $0D
NAK	equ  $15
ESC	equ  $1B
