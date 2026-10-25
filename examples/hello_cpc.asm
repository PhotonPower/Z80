; hello_cpc.asm - prints a text with the Amstrad CPC firmware
;
;   z80tool asm examples/hello_cpc.asm -l -
;
; &BB5A is the firmware routine TXT OUTPUT (character in A).

TXT_OUTPUT  EQU &BB5A
CR          EQU 13
LF          EQU 10

            ORG &4000

start:      LD   HL,message         ; HL -> text
next:       LD   A,(HL)
            OR   A                  ; zero byte ends the text
            RET  Z
            CALL TXT_OUTPUT
            INC  HL
            JR   next

message:    DB   "Hello, Z80!",CR,LF,0
