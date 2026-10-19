        org $C100
        lib flexequ.txt

dump    bra  start               skip version info
        fcb  1                   version

saddr   fdb  0                   place to save current pointer
nLine   fcb  16                  current lines remaining to output
eFlag   fcb 0                    temp place to save Echo Flag for

start   lda  LSTTRM              get last terminating character
        cmpa #$0d                if it's CR, no address supplied
        beq  dodump              use the value that is default (0000)

        jsr  GETHEX              otherwise get the user supplied address
nxt16   stx  saddr               save the pointer

dodump  jsr  PCRLF               give the operator a new line on the screen

        ldb  #16                 set columns remaining counter to 16
        ldx  #saddr              point to the pointer for output
        jsr  OUTADR              show the user what address we are doing
        lda  #$20                and a space
        jsr  PUTCHR

        ldx  saddr               get the ponter
loop    jsr  OUTHEX              output the byte pointed to by pointer
        lda  #$20                and a space
        jsr  PUTCHR
        leax 1,x                 bump the pointer
        decb                     decrement the columns remaining counter
        bne  loop                not done outputting bytes

        lda  #$20                do a couple of spaces between hex and ascii
        jsr  PUTCHR
        jsr  PUTCHR

        ldb  #16                 reset the columns remaining counter
        ldx  saddr               reset pointer to start of these 16 bytes
doascii lda  0,x                 get the character (hex)
        cmpa #$20                see if it's between $20 and $7F (printable)
        blt  dodot               no - output a period to the ascii part
        anda #$80                see if it's above $7f
        bne  dodot               yes - do the period

        lda  0,x                 no - this is printable - get it again
dochr   jsr  PUTCHR              output it
        bra  nxtasc              loop on doing the rest of the 16 chars

dodot   lda  #'.                 output a period because the byte is not
        bra  dochr               a printable ascii character

nxtasc  leax 1,x                 bump the pointer
        decb                     decrement the counter
        bne  doascii             and loop if not finished with this line

        lda  nLine               get the lines remaining
        deca                     decrement the counter
        sta  nLine               save it again
        bne  nxt16               loop if we have not done 16 lines yet

        lda  #16                 we have done 16 lines - reset the counter
        sta  nLine               to 16 again

        stx  saddr               set pointer to point to the next 16 bytes

        lda  $CC2F               get the echo flag
        sta  eFlag               save it
        lda  #0                  turn it off
        sta  $CC2F               for the GetChar to be without ECHO
        jsr  GETCHR
        psha                     save the character we got
        lda  eFlag               get the saved echo flag
        sta  $CC2F               put it back
        pula                     restore the character to A
        cmpa #$20                if it's a space - do it all again

        beq  dodump

        jmp  WARMS               otherwise - get out by going back to FLEX

        end  dump
