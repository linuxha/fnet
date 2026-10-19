 NAM RLIST
*
* REMOTE DIRECTORY LISTING THROUGH NETPC
*
*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 js New vectors
* ---------------------------------------------------------------

 LIB FLEXEQU
*
* SEPARATOR IN THE OUTPUT STREAM
SEP EQU CR

* LINES PER SCREEN
SCREEN EQU 20

 ORG $C100
 BRA COLD
* VERSION NUMBER
VN FCB 2,2
* TEMP STORAGE
COUNT RMB 1 LINE COUNTER
PTR RMB 2 POINTER
*
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

*INITIALIZE THE BUFFER
 LDX #BUFFER
 STX PTR INITIALIZE POINTER

* PUT THE "RLIST" COMMAND
 LDA #'I
 JSR PUTIT

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
* ADDED CODE
* SEND A SPACE TO START THE FIRST LINE
 LDA A #$20
 JSR schar
 BCC NOWORK

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
 FCC /Press spacebar to continue, ESC to stop /
 fcb 0

TIMOUT FCB CR,LF
 FCC /Communication time-out error/
 FCB 0

nodrv FCC CR,LF,/NETDRV is not loaded in memory, no action taken./,0

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst








* BUFFER AREA
BUFFER RMB 256

 END $C100
