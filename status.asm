%define STATUS_KNOWN 11
%define ENTRY_SIZE 24
%define OFFSET_MATCH 0
%define OFFSET_ICON 8
%define OFFSET_COLOR 16

extern status_map
extern strstr

default rel

global status_icon
section .text
status_icon:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    lea rbx, [status_map]
    mov r12, rdi
    xor ecx, ecx
.loop:
    cmp ecx, STATUS_KNOWN
    jge .default
    mov rsi, [rbx + OFFSET_MATCH]
    mov rdi, r12
    push rcx
    push rbx
    call strstr wrt ..plt
    pop rbx
    pop rcx
    test rax, rax
    jnz .found
    add rbx, ENTRY_SIZE
    inc ecx
    jmp .loop
.found:
    mov rax, [rbx + OFFSET_ICON]
    jmp .done
.default:
    mov rax, [rbx + OFFSET_ICON]
.done:
    pop r12
    pop rbx
    pop rbp
    ret

global status_color
status_color:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    lea rbx, [status_map]
    mov r12, rdi
    xor ecx, ecx
.loop2:
    cmp ecx, STATUS_KNOWN
    jge .default2
    mov rsi, [rbx + OFFSET_MATCH]
    mov rdi, r12
    push rcx
    push rbx
    call strstr wrt ..plt
    pop rbx
    pop rcx
    test rax, rax
    jnz .found2
    add rbx, ENTRY_SIZE
    inc ecx
    jmp .loop2
.found2:
    mov eax, dword [rbx + OFFSET_COLOR]
    jmp .done2
.default2:
    mov eax, dword [rbx + OFFSET_COLOR]
.done2:
    pop r12
    pop rbx
    pop rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
