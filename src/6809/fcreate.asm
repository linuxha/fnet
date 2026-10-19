    NAM     FCREATE
    OPT     PAG
*
* REMOTE CREATE A DSK FILE THROUGH NETPC
*
*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 JS New vectors
* ---------------------------------------------------------------

    LIB     FLEXEQU
*
* SEPARATOR IN THE OUTPUT STREAM
* '*' SUGGESTED FOR DEBUG
* CR FOR NORMAL USE

cr      equ     $0d             carriage return character
nxtch   equ     $cd27           get next buffer character

SEP EQU $0D

        ORG     $C100
        BRA     COLD

* VERSION NUMBER

VN      FCB     2,2

* TEMP STORAGE

PTR     RMB     2               POINTER IN BUFFER
SAVPTR  RMB     2               SAVE POINTER
NUMFLG  RMB     1               NUMERIC FLAG

fnprsnt fcb     0               assume we are talking to NetPC
drv     fcb     3               default to drive 3
ndarray fdb     0               place to store address of array

* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (WARMS) is a dummy value
*
schar   JMP     WARMS
rchar   JMP     WARMS
*
COLD    EQU     *               START OF CODE
*
memend  equ     $cc2b           FLEX end of user RAM
*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of NETDRV is already loaded
*
search  ldx     memend          start of search
sear2   leax    1,x             Bump pointer
        cpx     #$c000          Finished?
        lbeq    noload          Yes, not found
        ldy     #sgnst          Point to target string
        clr     b               Reset byte counter

sear3   ldaa    b,x             Get byte from RAM
        cmpa    b,y             Same as signature?
        bne     sear2           No, bump and restart
        inc     b               Point to next byte
        cmpb    #len            Finished?
        bne     sear3           No, check next byte

* string was found, all is OK

* Set the address of the in/out vectors

        leax    len,x           point to schar routine
        stx     schar+1
        leax    3,x             point to rchar routine
        stx     rchar+1

* See if we are talking to FNETDRV. Y is still pointing
* at the first character of FLEXNet if it's there.

        leax    3,x             point at the reset vector
        leax    3,x             point at the FLEXNet 4.1.0 string

        ldy     #fnsgnt         Point to target string
        clr     b
sear5   ldaa    b,x
        cmpa    b,y
        bne     check           Not FNETDRV
        inc     b
        cmpb    #len2
        bne     sear5

* X is still pointing at the FLEXNet 4.1.0 string. We need
* to add B to it to get it to point at rge drvltr table.
* If we add 4 more to it we will be pinting at the netdrv array.

        abx                     add B to X to point at drvltrs
        leax    4,x             now pointing at the netdrv array
        stx     ndarray

        ldaa    #1
        staa    fnprsnt
*
* ---------------------------------------------------------------
*
*
check   LDX     #BUFFER
        STX     PTR             INITIALIZE POINTER

* PUT THE "C" COMMAND IN BUFFER

        LDAA    #'C

        ldab    fnprsnt
        cmpb    0
        beq     notfnet

        LDAA    #'c

notfnet JSR     PUTIT

        ldab    fnprsnt
        cmpb    0
        beq     allset

* Get the drive from the command line

getdrv  jsr     nxtch           skip leading spaces
        cmpa    #$20
        beq     getdrv

        cmpa    #cr             last character in line?
        beq     nodrv

        anda    #$03
        bra     gotdrv

nodrv   ldaa    #3              default to drive 3
        staa    drv

gotdrv  JSR     PUTIT

* PROMPT FOR PATH

allset  LDX     #STR1 PROMPT THE USER
        JSR     NAMIN GET NAME
        JSR     PUTSEP

* PROMPT FOR FILE NAME

        LDX     #STR2           PROMPT USER
        JSR     NAMIN           GET NAME
        JSR     PUTSEP

* PROMPT FOR VOLUME NUMBER

        LDY     #STR3
        JSR     GETDEC
        JSR     PUTSEP

* PROMPT FOR TRACKS

        LDY     #STR4
        JSR     GETDEC
        JSR     PUTSEP

* PROMPT FOR SECTORS

        LDY     #STR5
        JSR     GETDEC
        JSR     PUTSEP

* NOW TERMINATE THE STRING
        CLR     A
        JSR     PUTIT

* ASK FOR PERMISSION

        LDX     #GO
        JSR     PDATA

* GET RESPONSE

        JSR     GETCHR
        ANDA    #$5F
        CMPA    #'Y
        BNE     EXIT2

* NOW SEND OUT THE CONTENTS OF
* THE BUFFER AS ONE SERIAL STREAM

        LDX     #BUFFER
LOOP    LDAA    0,X+
        BEQ     SEREND
        JSR     schar
        BCC     NOWORK
        BRA     LOOP

SEREND EQU *

* DATA TRANSMISSION DONE, WAIT
* FOR RESPONSE (ACK OR NACK)

        ldb     #255

LP      pshs    b
        JSR     rchar
        puls    b
        BCS     CHKSTAT         got response - process it

        dec     b               try this thirty times
        bne     LP              for really big drives

        bra     NOWORK          TIME-OUT ERROR?

CHKSTAT CMP     A #$06          IS IT ACK ?
        BEQ     GOOD
        CMP     A #$15          IS IT NACK?
        BEQ     BAD
        BRA     LP

GOOD    ldab    fnprsnt
        cmpb    0
        beq     good1

        ldx     ndarray
        ldab    drv
        stab    b,x

good1   LDX     #GOODST
        BRA     EXIT

BAD     LDX     #BADST
        BRA     EXIT

noload  ldx     #nodrvms        Inform that drivers are not loaded
        bra     EXIT

NOWORK  LDX     #TIMOUT
EXIT    JSR     PDATA

EXIT2   JMP     WARMS           RETURN TO FLEX

* LOW-LEVEL ROUTINES
* PRINT A STRING

PDATA2  JSR     PUTCHR
PDATA   LDAA    0,X+
        BNE     PDATA2
        RTS

* PRINT A PROMPT STRING, THEN
* INPUT A NAME (OR NUMBER) IN BUFFER
* IF ANY NON-NUMERIC CHARACTER IS FOUND,
* SET "NUMFLG" TO NON-ZERO.
* OTHERWISE, ZERO MEANS ALL NUMERIC

NAMIN   JSR     PDATA           PRINT THE PROMPT
        CLR     NUMFLG          CLEAR "NUMERIC" FLAG
        CLR     B               CLEAR BYTE COUNTER
NAM3    JSR     [INCHNE]        GET A CHARACTER
        CMPA    #BS             IS IT BACKSPACE?
        BNE     NOBACK          NO, PROCEED

* BACKSPACE FOUND

        TST     B               FIRST CHARACTER?
        BEQ     NAM3            YES, DO NOTHING

* PROCESS BACKSPACE

        DEC     B               DECREMENT "GOOD" COUNTER

* NOW OVERWRITE THE BAD CHARACTER

        LDAA    #BS
        JSR     PUTCHR
        LDAA    #$20
        JSR     PUTCHR
        LDAA    #BS             RE-POSITION CURSOR
        JSR     PUTCHR

* DECREMENT BUFFER POINTER

        LDX     PTR
        DEX
        STX     PTR
        BRA     NAM3            GO FOR NEXT CHARACTER

* DISPLAY THE CHARACTER AND STORE IT

NOBACK  JSR     PUTCHR
        CMPA    #CR             DONE?
        BNE     NXT             NO, ADD TO BUFFER
        RTS

* ADD ONE CHARACTER TO BUFFER

NXT     JSR     PUTIT
        INC     B               INCREMENT GOOD COUNTER

* CHECK FOR DIGITS

        CMPA    #$30            BELOW ZERO?
        BLO     NOTNUM          NO, NOT NUMERIC
        CMPA    #$40            ABOVE 9?
        BHS     NOTNUM          YES, NOT NUMERIC
        BRA     NAM3            GO FOR NEXT CHARACTER

NOTNUM  INC     NUMFLG          SAY NON-NUMERIC
        BRA     NAM3            AND LOOP BACK

* PUT A SEPARATOR

PUTSEP  LDAA    #SEP            AND EXI THRU PUTIT

* PUT A CHARACTER IN BUFFER

PUTIT   LDX     PTR             GET POINTER
        STAA    0,X+            STORE BYTE AND BUMP PTR
        STX     PTR
        RTS

* PROMPT FOR A DECIMAL NUMBER
* AND PUT IT IN BUFFER
* ENTRY: Y POINTS TO PROMPT STRING

GETDEC  LDX     PTR             SAVE POINTER IN CASE OF ERROR
        STX     SAVPTR
ASKVOL  LDX     SAVPTR          RECALL POINTER
        STX     PTR
        TFR     Y,X             PROMPT USER
        JSR     NAMIN           GET NAME
        TST     NUMFLG          ALL NUMERIC INPUT?
        BNE     ASKVOL          NOT NUMERIC, RESTART
        RTS

* CHARACTER STRINGS

STR1    FCB     CR,LF
        FCC     /File path ? /
        FCB     0

STR2    FCB     CR,LF
        FCC     /File name ? /
        FCB     0

STR3    FCB     CR,LF
        FCC     /Disk number (in decimal) ? /
        FCB     0

STR4    FCB     CR,LF
        FCC     /Number of tracks (in decimal) ? /
        FCB     0

STR5    FCB     CR,LF
        FCC     /Sectors per track (in decimal) ? /
        FCB     0

GOODST  FCB     CR,LF
        fcc    /Command executed OK./
        FCB     0

BADST   FCB     CR,LF
        FCC     /Error message received from NETPC./
        FCB     0

GO      FCB     CR,LF
        FCC     /OK to proceed ? /
        FCB     0

TIMOUT  FCB     CR,LF
        FCC     /Communication time-out error/
        FCB     0

nodrvms FCB     CR,LF
        FCC     /NETDRV is not loaded in memory, no action taken./
        FCB     0

* signature string

sgnst   fcc     'netUUdrv'
len     equ     *-sgnst

fnsgnt  fcc     'FLEXNet 4.0.1'
len2    equ     *-fnsgnt

* BUFFER AREA

BUFFER  RMB     256

        END     $C100
