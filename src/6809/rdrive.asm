*   File name:  RDRIVE.TXT
*
*   Syntax: RDRIVE <path>
*
*   Note: <path> may contain the whole directory path,
*         including the disk name, for example "C:\dir1\dir2\.."
*         In order to achieve this, the FLEX EOL character is
*         temporarily cleared. Thus, multiple commands on one
*         line can be used with this utility, only if RMOUNT
*         is the last command on the line.
*
*   Adapted from Bjarne Backstrom's source code for RMOUNT
*   by Joel Setton
*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 js New vectors
*       02.03   2002-11-23 js Longer delay if and only if the
*                             drive is A or B, i.e. a floppy
*       02.04   2002-11-24 js Do an "rwhich" command if no
*                             parameters are specified
* ---------------------------------------------------------------
*
*   FLEX equates.
*
warms   equ     $cd03           FLEX warm start
PUTCHR  equ     $cd18           print one character
pstrng  equ     $cd1e           write string to display
pcrlf   equ     $cd24           write cr/lf to display
nxtch   equ     $cd27           get next buffer character
memend  equ     $cc2b           memory end pointer
*
ttylbp  equ     $cc14           line buffer pointer location
ttyeol  equ     $cc02           end of line character location
*
* ---------------------------------------------------------------
*
ack     equ     $06             acknowledge character
cr      equ     $0d             carriage return character

*
* ***************************************************************
*
        org     $c100

start EQU *
        bra     COLD

versn   fcb     2,4             version number
PTR     rmb     2               Pointer in buffer
eoltmp  rmb     1               temp storage for EOL character
chrcnt  rmb     1               character counter
drvptr  rmb     2               drive pointer
temp    rmb     1               temp pointer for letter
*
* ---------------------------------------------------------------
* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (warms) is a dummy value
*
schar JMP warms
rchar JMP warms
*
COLD EQU * START OF CODE
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
* Set the drive letter pointer
 leax 6,x
 stx drvptr

*
* ---------------------------------------------------------------
*
*       jsr     pcrlf           (DELETED)

        lda     ttyeol          save EOL character
        sta     eoltmp

        clr     chrcnt          check file name length
        ldx     ttylbp

chec04  lda     ,x+             get character
        cmpa    #$20            skip leading spaces
        beq     chec04

        staa    temp            store the first character after spaces

chec08  cmpa    #cr             carriage return?
        beq     chec12
        inc     chrcnt          no, inc character count
        lda     ,x+             check next character
        bra     chec08

chec12  lda     chrcnt          check character count
        cmpa    #1              less than 2 characters?
        lblo    where           yes, do a "Where am I" command
*
* ---------------------------------------------------------------
*
        clr     ttyeol          disable TTYEOL

        lda     #'V             send V (driVe) command to remote host
        lbsr    schar
        lbcc    nwrkng          time out, communication not working
*
main04  jsr     nxtch           skip leading spaces
        cmpa    #$20
        beq     main04

main08  lbsr    schar           send one character to remote host
        lbcc    nwrkng          time out, communication not working
        cmpa    #cr             last character in line?
        beq     main12
        jsr     nxtch           get next character
        bra     main08
*
main12  lbsr    rchar           get response
        lbcc    nwrkng          time out, communication not working
        cmpa    #ack            got an ack?
        lbne    badfnm          no, report bad file name

*
*  Success!
*
*  store the drive letter
        ldaa temp
        dec a                   change a/b to @/a
        anda #$5e               make upper case
        staa [drvptr]           store it

        ldx     #succst         report success
        bra     finish

noload   ldx #nodrv  Inform that drivers are not loaded
         bra finis2
*
* ---------------------------------------------------------------
*
* No parameters were typed, do a "Where am I" command
*
where equ *

* SEND THE "WHERE" COMMAND
 LDA #'?

 JSR schar
 BCC nwrkng

* RECEIVE ONE LINE
 LDX #BUFFER INITIALIZE POINTER
 STX PTR
LP1 JSR rchar GET CHAR
 BCC nwrkng
 CMPA #cr FINISHED?
 BEQ DISP
* STORE THE CHARACTER
 JSR PUTIT
 BRA LP1

DISP EQU *
* LINE FEED RECEIVED, DISPLAY LINE ON CRT
 CLR A ADD TERMINATOR
 JSR PUTIT

* set the drive code
 ldaa BUFFER get the first character of string
 dec a
 anda #$5e
 staa [drvptr]

 LDX #CURDIR
 JSR pstrng

 LDX #BUFFER
 JSR PDATA

* WAIT FOR ACK
WTACK JSR rchar
 BCC nwrkng
 CMP A #ack
 BNE WTACK
 BRA exit2


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


*
*   Error/exit routines
*
badfnm  ldx     #badfst         bad name
        bra     finish
*
nwrkng  ldx     #nwrkst         communication is not working
        bra     finish
*
finish  lda     eoltmp          restore EOL character
        sta     ttyeol
finis2  jsr     pstrng          print string pointed to by XREG
exit2   jmp     warms           back to FLEX
*
* ---------------------------------------------------------------
*
CURDIR  FCC     /The current directory is  /,4
nwrkst  fcc     /Communication is not working!/,8,4
badfst  fcc     /Bad directory name!/,8,4
succst  fcc     /Command executed OK./,4
nodrv   FCC     /NETDRV is not loaded in memory, no action taken./,4

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst

* BUFFER AREA
BUFFER RMB 256
*
* ---------------------------------------------------------------
*
        end     start

