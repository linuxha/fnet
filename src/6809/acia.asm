        NAM ACIA

* This file is the only one which must be
* system-dependent (i.e. it must be edited
* to match the address of your serial port).

* Adaptation for Mike's system:
*
*   ACIA on port #0

port    equ     0
BOARD   EQU     16*port+$E000

aciac   EQU     BOARD               ACIA CONTROL REGISTER
aciad   EQU     aciac+1             ACIA DATA REGISTER

