; hello.asm - Simple "HELLO WORLD" COM program for MagiDOS
; Assemble with: nasm -f obj hello.asm -o hello.obj
; Link with: wlink format dos com file hello.obj name hello.com

BITS 16
ORG 100h

start:
    ; Set video mode to text 80x25 (already set by BIOS)
    ; Print "HELLO WORLD" using DOS INT 21h

    mov ah, 09h         ; Display string function
    mov dx, msg         ; Point to message
    int 21h             ; Call DOS

    ; Wait for key press
    mov ah, 00h         ; Get keystroke function
    int 16h             ; Call BIOS keyboard

    ; Exit to DOS
    mov ax, 4C00h       ; Exit program function
    int 21h             ; Call DOS

msg:
    db "HELLO WORLD$"

end start
