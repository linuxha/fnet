 NAM RDIR
*
* REMOTE DIRECTORY LISTING THROUGH NETPC
*
* SYNTAX: RDIR <CR> TO LIST ALL FILES
*         RDIR <PARAMETERS> TO PASS PARAMETERS TO
*              THE MS-DOS COMMAND LINE
*              (E.G. RDIR AB*  <CR>)

*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 js New vectors

 LIB FLEXEQU
*
* SEPARATOR IN THE OUTPUT STREAM
SEP EQU CR

* LINES PER SCREEN
SCREEN EQU 20

 ORG $C100
 BRA COLD

* TEMP STORAGE

vn fcb 2,2
PTR RMB 2 POINTER IN BUFFER
COUNT RMB 1 LINE COUNTER

* Serial input/output vectors
* set when utility is started
schar fcb $7E,0,0
rchar fcb $7E,0,0
*
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

*INITIALIZE THE BUFFER
 LDX #BUFFER
 STX PTR INITIALIZE POINTER

* PUT THE "DIR" COMMAND
 LDA #'A
 JSR PUTIT

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

MAIN12 EQU *

* SEND A <CR>
 LDA A #SEP
 JSR PUTIT

* NOW TERMINATE THE STRING
 CLR A
 JSR PUTIT

* NOW SEND OUT THE CONTENTS OF
* THE BUFFER AS ONE SERIAL STREAM
 LDX #BUFFER
LOOP LDA A 0,X+
 BEQ SEREND
 JSR schar
 BCC NOWORK
 BRA LOOP
SEREND EQU *
*
*
* START A NEW SCREEN
NEW LDA A #SCREEN
 STA A COUNT

* RECEIVE ONE LINE
ONELIN EQU *
 LDX #BUFFER INITIALIZE POINTER
 STX PTR
LP1 JSR rchar GET CHAR
 BCC NOWORK
 CMPA #ACK FINISHED?
 BEQ EXIT
* STORE THE CHARACTER
 JSR PUTIT
* LOOP IF NOT <LF>
 CMPA #LF
 BNE LP1

* LINE FEED RECEIVED, DISPLAY LINE ON CRT
 CLR A ADD TERMINATOR
 JSR PUTIT
 LDX #BUFFER
 JSR PDATA

* COUNT DOWN LINES
 DEC COUNT
 BEQ ASK

* SEND A SPACE FOR NEXT LINE
 LDA A #$20
 JSR schar
 BCC NOWORK
 BRA ONELIN

* ASK THE USER
ASK LDX #ASKUSR
 JSR PDATA
 JSR GETCHR
 CMP A #ESC
 BEQ EX1
 CMP A #$20
 BNE ASK

* GO FOR ANOTHER SCREEN
 JSR schar
 BRA NEW

* ESCAPE RECEIVED, SEND ESCAPE TO MSD0S
EX1 JSR schar
 BCC NOWORK
 JSR PCRLF

* WAIT FOR ACK
WTACK JSR rchar
 BCC NOWORK
 CMP A #ACK
 BNE WTACK
 BRA EXIT

noload   ldx #nodrv  Inform that drivers are not loaded
         bra finish

NOWORK LDX #TIMOUT
finish JSR PDATA

EXIT JMP WARMS

* LOW-LEVEL ROUTINES
* PRINT A STRING
PDATA2 JSR PUTCHR
PDATA LDA A 0,X+
 BNE PDATA2
 RTS

* PUT A CHARACTER IN BUFFER
PUTIT LDX PTR GET POINTER
 STA A 0,X+ STORE BYTE AND BUMP PTR
 STX PTR
 RTS

* CHARACTER STRINGS

ASKUSR FCB CR
 FCC /Press spacebar to continue, ESC to stop /,0

TIMOUT FCB CR,LF
 FCC /Communication time-out error/,0

nodrv FCC CR,LF,/NETDRV is not loaded in memory, no action taken./,0

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst


* BUFFER AREA
BUFFER RMB 256

 END $C100
