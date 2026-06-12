; init.c - x86_64 helpers
;
; Five functions used by the C init:
;
;   asm_memzero(void *dst, size_t n)
;     Zeroes n bytes at dst using REP STOSB/STOSQ with alignment fast-path.
;
;   asm_strcmp(const char *a, const char *b) -> int
;     Standard strcmp semantics.
;
;   asm_write_str(int fd, const char *s)
;     write(2) syscall directly — async-signal-safe, no stdio, usable
;     from panic() even with a corrupted heap.
;
;   asm_poll_read(int fd, int timeout_ms) -> int
;     poll(2) for POLLIN up to timeout_ms (-1 = forever), then read(2)
;     one byte.  Returns the byte (0-255), -1 on timeout, -2 on error
;     or EOF.  EINTR is retried.  This is the keyboard wait used by the
;     boot selector and init menu countdowns.
;
;   asm_usleep(unsigned long usec)
;     nanosleep(2), retrying with the remaining time on EINTR.  Used by
;     the keyboard-detection polling loop.
;
; Assembled with NASM. System V AMD64 ABI:
;   args:  rdi, rsi, rdx, rcx, r8, r9
;   return: rax
;   caller-saved: rax, rcx, rdx, rsi, rdi, r8-r11

SYS_READ      equ 0
SYS_WRITE     equ 1
SYS_POLL      equ 7
SYS_NANOSLEEP equ 35

POLLIN        equ 1
EINTR_NEG     equ -4

section .text

global asm_memzero
asm_memzero:
    test    rsi, rsi
    jz      .done

    ; Align dst to 8 bytes first
    mov     rcx, rdi
    and     rcx, 7
    jz      .aligned

.prefix_loop:
    test    rcx, rcx
    jz      .aligned
    dec     rcx
    test    rsi, rsi
    jz      .done
    xor     al, al
    stosb                   ; [rdi] = al; rdi++
    dec     rsi
    jmp     .prefix_loop

.aligned:
    xor     eax, eax        ; rax = 0
    mov     rcx, rsi
    shr     rcx, 3          ; rcx = n / 8
    jz      .tail
    rep     stosq

.tail:
    mov     rcx, rsi
    and     rcx, 7
    jz      .done
    rep     stosb

.done:
    ret


global asm_strcmp
asm_strcmp:
.loop:
    movzx   eax, byte [rdi]     ; al = *a
    movzx   ecx, byte [rsi]     ; cl = *b
    inc     rdi
    inc     rsi

    test    al, al
    jz      .end_of_a

    cmp     al, cl
    jne     .different

    test    cl, cl
    jnz     .loop

    xor     eax, eax
    ret

.end_of_a:
    test    cl, cl
    jz      .equal
    mov     eax, -1             ; *a == 0, *b != 0 → a < b
    ret

.different:
    sub     eax, ecx            ; *a - *b
    ret

.equal:
    xor     eax, eax
    ret


global asm_write_str
asm_write_str:
    mov     rdx, rsi            ; compute strlen into rdx
    test    rsi, rsi
    jz      .empty

.strlen_loop:
    cmp     byte [rdx], 0
    je      .strlen_done
    inc     rdx
    jmp     .strlen_loop

.strlen_done:
    sub     rdx, rsi            ; rdx = length
    test    rdx, rdx
    jz      .empty

.retry:
    mov     eax, SYS_WRITE      ; write(fd=rdi, buf=rsi, count=rdx)
    syscall
    cmp     rax, EINTR_NEG
    je      .retry
    ; Other errors ignored — in the panic path there is nothing to do.

.empty:
    ret


; int asm_poll_read(int fd, int timeout_ms)
global asm_poll_read
asm_poll_read:
    sub     rsp, 16
    mov     dword [rsp], edi    ; pollfd.fd
    mov     word  [rsp+4], POLLIN
    mov     word  [rsp+6], 0    ; revents
    movsxd  r8, esi             ; save timeout (poll clobbers arg regs)

.retry_poll:
    lea     rdi, [rsp]          ; fds
    mov     esi, 1              ; nfds
    mov     rdx, r8             ; timeout_ms (negative = infinite)
    mov     eax, SYS_POLL
    syscall
    cmp     rax, EINTR_NEG
    je      .retry_poll
    test    rax, rax
    js      .err                ; poll error
    jz      .timeout            ; 0 ready fds = timeout

.retry_read:
    mov     edi, dword [rsp]    ; fd
    lea     rsi, [rsp+8]        ; 1-byte buffer
    mov     edx, 1
    mov     eax, SYS_READ
    syscall
    cmp     rax, EINTR_NEG
    je      .retry_read
    cmp     rax, 1
    jne     .err                ; 0 = EOF, negative = error
    movzx   eax, byte [rsp+8]
    add     rsp, 16
    ret

.timeout:
    mov     eax, -1
    add     rsp, 16
    ret

.err:
    mov     eax, -2
    add     rsp, 16
    ret


; void asm_usleep(unsigned long usec)
global asm_usleep
asm_usleep:
    sub     rsp, 32             ; [rsp] req timespec, [rsp+16] rem timespec
    mov     rax, rdi
    xor     edx, edx
    mov     rcx, 1000000
    div     rcx                 ; rax = seconds, rdx = remaining usec
    mov     [rsp], rax          ; req.tv_sec
    imul    rdx, rdx, 1000      ; usec → nsec
    mov     [rsp+8], rdx        ; req.tv_nsec

.retry:
    lea     rdi, [rsp]          ; req
    lea     rsi, [rsp+16]       ; rem
    mov     eax, SYS_NANOSLEEP
    syscall
    cmp     rax, EINTR_NEG
    jne     .done
    mov     rax, [rsp+16]       ; req = rem; retry
    mov     [rsp], rax
    mov     rax, [rsp+24]
    mov     [rsp+8], rax
    jmp     .retry

.done:
    add     rsp, 32
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
