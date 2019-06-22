; This is just Boost.Context in nasm
; https://github.com/boostorg/context


; ****************************************************************************************
; *                                                                                      *
; *  ----------------------------------------------------------------------------------  *
; *  |    0    |    1    |    2    |    3    |    4     |    5    |    6    |    7    |  *
; *  ----------------------------------------------------------------------------------  *
; *  |   0x0   |   0x4   |   0x8   |   0xc   |   0x10   |   0x14  |   0x18  |   0x1c  |  *
; *  ----------------------------------------------------------------------------------  *
; *  | fc_mxcsr|fc_x87_cw|        R12        |         R13        |        R14        |  *
; *  ----------------------------------------------------------------------------------  *
; *  ----------------------------------------------------------------------------------  *
; *  |    8    |    9    |   10    |   11    |    12    |    13   |    14   |    15   |  *
; *  ----------------------------------------------------------------------------------  *
; *  |   0x20  |   0x24  |   0x28  |  0x2c   |   0x30   |   0x34  |   0x38  |   0x3c  |  *
; *  ----------------------------------------------------------------------------------  *
; *  |        R15        |        RBX        |         RBP        |        RIP        |  *
; *  ----------------------------------------------------------------------------------  *
; *                                                                                      *
; ****************************************************************************************

section .text

; vgc_xfr_t vgc_make(void *sp, vgc_proc proc);
global vgc_make
vgc_make:
    mov rax, rdi                ; Grab the bottom of the context stack
    sub rax, 40h                ; Reserve space on the context stack

    stmxcsr [rax]               ; Save MMX control/status word
    fnstcw [rax + 4h]           ; Save x87 control word

    mov [rax + 28h], rsi        ; Store proc address at RBX

    lea rcx, [rel finish]       ; Calculate/store the address of finish at RBP
    mov [rax + 30h], rcx

    lea rcx, [rel trampoline]   ; Calculate/store the address of trampoline
    mov [rax + 38h], rcx

    ret

; Fix the stack before jumping into the passed vgc_proc
trampoline:
    push rbp                    ; Set finish as return addr
    jmp rbx                     ; Jump to the context function

; Kill the process if a context returns from the bottom stack frame
finish:
    xor rdi, rdi                ; Exit code is zero
    mov rax, 60d                ; Call _exit
    syscall
    hlt

; vgc_fiber vgc_jump(vgc_fiber)
global vgc_jump
vgc_jump:
    sub rsp, 38h                ; Allocate stack space

    stmxcsr [rsp]               ; Save MMX control/status word
    fnstcw [rsp + 4h]           ; Save x87 control word

    mov [rsp +  8h], r12        ; Save R12
    mov [rsp + 10h], r13        ; Save R13
    mov [rsp + 18h], r14        ; Save R14
    mov [rsp + 20h], r15        ; Save R15
    mov [rsp + 28h], rbx        ; Save RBX
    mov [rsp + 30h], rbp        ; Save RBP

    mov rax, rsp                ; Store the current stack pointer
    mov rsp, rdi                ; Switch into the destination stack

    ldmxcsr [rsp]               ; Restore MMX control/status word
    fldcw [rsp + 4h]            ; Restore x87 control word

    mov r12, [rsp +  8h]        ; Restore R12
    mov r13, [rsp + 10h]        ; Restore R13
    mov r14, [rsp + 18h]        ; Restore R14
    mov r15, [rsp + 20h]        ; Restore R15
    mov rbx, [rsp + 28h]        ; Restore RBX
    mov rbp, [rsp + 30h]        ; Restore RBP
    mov r8,  [rsp + 38h]        ; Restore return address

    ; If vgc_jump is being called to enter a function for the first time then
    ; the calling context and data pointer are passed as the arguments to the
    ; function. This enables the callee to yield back to the calling context.
    ;
    ; If vgc_jump is being called to resume a function (or yield to a caller)
    ; then the calling context and data pointer are returned from the vgc_jump
    ; that initiated the context switch.
    mov rdi, rax                ; Setup vgc_xfr_t argument
    mov rdx, rsi                ; Setup vgc_xfr_t return value

    ; Note, don't use:
    ; add rsp, 38h
    ; ret
    ; Because the Return Stack Buffer will miss every time due to the context
    ; switch. Much better to use the general indirect branch predictor and
    ; only miss _most_ of the time.
    add rsp, 40h                ; Prepare stack
    jmp r8
