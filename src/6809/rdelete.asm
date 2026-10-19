 NAM RDELETE
*
* "NEW DELETE" COMMAND
* WITH COMMAND-LINE PARAMETERS
*
*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 JS New vectors
*       02.03   2002-11-26 js Remove extra slash in display of
*                             file name
* ---------------------------------------------------------------
 LIB FLEXEQU
*
 ORG $C100
 BRA COLD
* VERSION NUMBER
VN FCB 2,3
* TEMP STORAGE
PTR RMB 2 POINTER IN BUFFER
EOLTMP RMB 1 STORE TTYEOL BYTE
NAMPTR RMB 2 POINTER TO NAME
CHRCNT RMB 1 NAME LENGTH
*
* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (WARMS) is a dummy value
*
schar JMP WARMS
rchar JMP WARMS
*
COLD EQU * START OF CODE

memend  equ     $cc2b           FLEX end of user RAM
*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of NETDRV is already loaded
*
search ldx memend  start of search
sear2 leax 1,x Bump pointer
 cpx #$c000 Finished?
 lbeq noload Yes, not found
 ldy #sgnst Point to target string
 clr b Reset byte counter
sear3 ldaa b,x Get byte from RAM
 cmpa b,y Same as signature?
 bne sear2 No, bump and restart
 inc b Point to next byte
 cmpb #len Finished?
 bne sear3 No, check next byte
*
* string was found, all is OK

* Set the address of the in/out vectors
 leax len,x
 stx schar+1
 leax 3,x
 stx rchar+1
*
* ---------------------------------------------------------------
*
* SAVE TTY END OF LINE BYTE
 LDAA TTYEOL
 STAA EOLTMP
 CLR TTYEOL

* INITIALIZE THE BUFFER
 LDX #BUFFER
 STX PTR INITIALIZE POINTER

* MEASURE THE LENGTH OF THE FILE NAME
* TO PROTECT AGAINST MISSING PARAMS
 CLR CHRCNT
 LDX TTYLBP
* SKIP SPACES
CHK4 LDA 0,X+
 CMPA #$20
 BEQ CHK4
* COUNT BYTES
CHK8 CMPA #CR
 BEQ CHK12
 INC CHRCNT
 LDAA 0,X+
 BRA CHK8
CHK12 LDAA CHRCNT
 CMPA #1
 LBLO BADNAM

* SEND THE "WHERE" COMMAND
 LDA #'?
 JSR schar
 LBCC NOWORK

* RECEIVE ONE LINE
LP1 JSR rchar GET CHAR
 LBCC NOWORK
 CMPA #CR FINISHED?
 BEQ WTA
* STORE THE CHARACTER
 JSR PUTIT
 BRA LP1

* WAIT FOR ACK
WTA JSR rchar
 LBCC NOWORK
 CMP A #ACK
 BNE WTA


* STORE THE POINTER TO MARK THE END
* OF THE PATH NAME
 LDX PTR
 STX NAMPTR

 LDAA #'\
 JSR PUTIT


* PRINT THE PROMPT LINE
 LDX #ASK
 JSR PDATA

* READ THE FILE NAME FROM THE FLEX LINE
* BUFFER
* SKIP LEADING SPACES
MAIN04 JSR NXTCH
 CMP A #$20
 BEQ MAIN04

MAIN08 CMPA #CR END OF LINE?
 BEQ MAIN12
 JSR PUTIT STORE IT IN BUFFER
 JSR NXTCH
 BRA MAIN08

* ADD THE EXTENSION
MAIN12 EQU *
 LDAA #'.
 JSR PUTIT
 LDAA #'D
 JSR PUTIT
 LDAA #'S
 JSR PUTIT
 LDAA #'K
 JSR PUTIT

*TERMINATE THE BUFFER
 CLR A
 JSR PUTIT

* DISPLAY BUFFER ON THE SCREEN
 LDX #BUFFER
 JSR PDATA

* PROMPT FOR ANSWER
 LDX #YESNO
 JSR PDATA

* GET USER'S RESPONSE
PROMPT JSR GETCHR
 ANDA #$5F
 CMPA #'Y
 BEQ DOIT
 CMPA #'N
 BEQ NODEX
 LDA A #BS
 JSR PUTCHR
 BRA PROMPT

* USER OK, PERFORM DELETE
*
* PUT A "D" IN BUFFER
DOIT LDX NAMPTR
 LDA A #'D
 STA A 0,X

* SEND OUT THE BUFFER AS ONE STREAM
SENDLP LDA A 0,X+
 BEQ DOCR
 JSR schar
 BCC NOWORK
 BRA SENDLP

* SEND A [CR]
DOCR LDA A #CR
 JSR schar
 BCC NOWORK

* WAIT FOR ACK
WTACK JSR rchar
 BCC NOWORK
 CMPA #NACK
 BEQ ERROR
 CMP A #ACK
 BNE WTACK

 LDX #DELOK
 BRA EXIT

noload   ldx #nodrv  Inform that drivers are not loaded
         bra EXIT

BADNAM LDX #BAD
 BRA EXIT

ERROR LDX #ERR
 BRA EXIT

NODEX LDX #NODEL
 BRA EXIT

NOWORK LDX #TIMOUT
EXIT JSR PDATA

* RESTORE TTYEOL AND EXIT
 LDAA EOLTMP
 STAA TTYEOL
 JMP WARMS

* LOW-LEVEL ROUTINES
* PRINT A STRING
PDATA2 JSR PUTCHR
PDATA LDA A 0,X+
 BNE PDATA2
 RTS

* PUT A CHARACTER IN BUFFER
PUTIT LDX PTR
 STA A 0,X+
 STX PTR
 RTS

* CHARACTER STRINGS

ASK FCB CR,LF
 FCC /Delete the file  /
 fcb 0

TIMOUT FCB CR,LF
 FCC /Communication time-out error/
 fcb 0

NODEL FCB CR,LF
 FCC /File not deleted./
 fcb 0

YESNO FCC ' (Y/N)? '
 FCB 0

ERR FCB CR,LF
 FCC /Error in processing file./
 FCB 0

DELOK FCB CR,LF
 fcc    /Command executed OK./
 FCB 0

BAD FCB CR,LF
 FCC /Illegal file name (the syntax is RDELETE <filename>)./
 FCB 0

nodrv FCC /NETDRV is not loaded in memory, no action taken./
 FCB 0

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst

* BUFFER AREA
BUFFER RMB 256

 END $C100
