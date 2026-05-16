; init.c - x86_64
;
; Three functions used by the C init:
;
;   asm_memzero(void *dst, size_t n)
;     Zeroes n bytes at dst using REP STOSB with alignment fast-path.
;     Used to zero InitState (large struct) at startup.
;
;   asm_strcmp(const char *a, const char *b) -> int
;     Standard strcmp semantics. Used in hot paths (stratum name
;     comparison in the enable loop) to avoid function call overhead
;     of libc strcmp when inlining is not possible across TUs.
;
;   asm_write_str(int fd, const char *s)
;     Calls the write(2) syscall directly, without going through
;     stdio or libc. This is async-signal-safe and usable from
;     panic() even after a corrupted heap.
;
; Assembled with NASM. Linked into the C init via the Makefile.
; All functions follow the System V AMD64 ABI:
;   args:  rdi, rsi, rdx, rcx, r8, r9
;   return: rax
;   caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
;   callee-saved: rbx, rbp, r12-r15

section .text

global asm_memzero
asm_memzero:
    test    rsi, rsi
    jz      .done

    ; Try to align dst to 8 bytes first
    mov     rcx, rdi
    and     rcx, 7
    jz      .aligned

    ; Unaligned prefix: zero byte-by-byte until aligned
    xor     al, al
.prefix_loop:
    test    rcx, rcx
    jz      .aligned
    dec     rcx
    test    rsi, rsi
    jz      .done
    stosb                   ; [rdi] = al; rdi++
    dec     rsi
    jmp     .prefix_loop

.aligned:
    ; Zero 8 bytes at a time with QWORD stores
    xor     eax, eax        ; rax = 0
    mov     rcx, rsi
    shr     rcx, 3          ; rcx = n / 8
    jz      .tail
    rep     stosq           ; [rdi] = rax (8 bytes); rdi += 8; rcx--

.tail:
    ; Zero remaining bytes (n % 8)
    mov     rcx, rsi
    and     rcx, 7
    jz      .done
    rep     stosb

.done:
    ret


global asm_strcmp
asm_strcmp:
    ; Load bytes in a loop — unrolling not worth it for short strings
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

    ; Both terminated at same point — they're equal
    xor     eax, eax
    ret

.end_of_a:
    ; a ended — if b also ended, equal; else a < b
    test    cl, cl
    jz      .equal
    ; *a == 0, *b != 0 → a < b
    mov     eax, -1
    ret

.different:
    ; al != cl
    sub     eax, ecx    ; result = *a - *b
    ret

.equal:
    xor     eax, eax
    ret


global asm_write_str
asm_write_str:
    ; Compute strlen(s): scan for NUL byte
    mov     rdx, rsi        ; rdx = s (we'll compute length into rdx)
    test    rsi, rsi
    jz      .empty

.strlen_loop:
    cmp     byte [rdx], 0
    je      .strlen_done
    inc     rdx
    jmp     .strlen_loop

.strlen_done:
    sub     rdx, rsi        ; rdx = length = end - start

    test    rdx, rdx
    jz      .empty

    ; write(fd, s, len)
    ;   syscall number: 1 (SYS_write)
    ;   rdi = fd       (already set)
    ;   rsi = buf      (already set)
    ;   rdx = count    (computed above)
    mov     rax, 1          ; SYS_write
    syscall
    ; Return value ignored — in panic path, nothing we can do anyway.

.empty:
    ret
