; Example program for the reverse-engineering walkthrough (docs/walkthrough.md).
;
; It contains what the analysis is meant to recognise: strings, a table of string pointers, a jump table behind a computed
; JP (HL), tables of 16-bit numbers that are read word by word, a 16-bit variable, tail calls (JP print) and firmware calls.
; Without a ROM the firmware is stubbed: TXT_OUTPUT collects the text (protocol command "console").
;
;   build/z80tool asm examples/cpc/demo_cpc.asm -o demo_cpc.bin
;   python3 agent/z80_annotate.py --bin demo_cpc.bin --org 0x4000 --dry-run

TXT_OUTPUT  EQU &BB5A

        ORG &4000
main:   LD HL,banner
        CALL print
        XOR A               ; command 0: greeting
        CALL dispatch
        LD A,1              ; command 1: sum of the values
        CALL dispatch
        LD A,2              ; command 2: the names
        CALL dispatch
        LD HL,(runs)
        INC HL
        LD (runs),HL
        RET

; prints the zero-terminated text at HL
print:  LD A,(HL)
        OR A
        RET Z
        CALL TXT_OUTPUT
        INC HL
        JR print

; runs command A (0..2) through the table of handlers
dispatch:
        ADD A,A
        LD E,A
        LD D,0
        LD HL,commands
        ADD HL,DE
        LD E,(HL)
        INC HL
        LD D,(HL)
        EX DE,HL
        JP (HL)
commands:
        DW cmd_hello,cmd_sum,cmd_names

cmd_hello:
        LD HL,greeting
        JP print            ; tail call

cmd_sum:
        LD HL,sum_text
        CALL print
        LD HL,values
        LD B,5
        LD DE,0
cs_loop:
        LD C,(HL)
        INC HL
        LD A,(HL)
        INC HL
        PUSH HL
        LD H,A
        LD L,C
        ADD HL,DE
        EX DE,HL
        POP HL
        DJNZ cs_loop
        EX DE,HL
        CALL print_dec
        LD HL,crlf
        JP print            ; tail call

cmd_names:
        LD HL,names
        LD B,3
cn_loop:
        LD E,(HL)
        INC HL
        LD D,(HL)
        INC HL
        PUSH HL
        EX DE,HL
        CALL print
        POP HL
        DJNZ cn_loop
        RET

; prints HL as a decimal number without leading zeros
print_dec:
        PUSH HL
        LD HL,pow10
        LD B,5
        LD C,0              ; bit 0: a digit other than 0 was printed
pd_digit:
        LD E,(HL)
        INC HL
        LD D,(HL)
        INC HL
        EX (SP),HL          ; HL = the number, the table pointer goes to the stack
        LD A,'0'-1
pd_count:
        INC A
        OR A
        SBC HL,DE
        JR NC,pd_count
        ADD HL,DE
        EX (SP),HL
        CP '0'
        JR NZ,pd_nonzero
        BIT 0,C
        JR NZ,pd_print
        LD A,B
        CP 1
        LD A,'0'
        JR NZ,pd_skip       ; a leading zero, unless it is the last digit
        JR pd_print
pd_nonzero:
        SET 0,C
pd_print:
        CALL TXT_OUTPUT
pd_skip:
        DJNZ pd_digit
        POP HL
        RET

banner:     DB "Z80 demo",13,0
greeting:   DB "Hello from the Z80 example",13,0
sum_text:   DB "sum=",0
crlf:       DB 13,0
names:      DW n_one,n_two,n_three
n_one:      DB "one ",0
n_two:      DB "two ",0
n_three:    DB "three",13,0
values:     DW 10,20,30,40,50
pow10:      DW 10000,1000,100,10,1
runs:       DW 0
