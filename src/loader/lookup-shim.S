/* Lookup inherited methods within our legacy worker only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * System V x86-64: three saved registers keep the stack aligned for calls.
 * The parent fills the last two pointer slots while ShellUI is stopped.
 */
.text
.globl home_lookup_shim
.type home_lookup_shim,@function
home_lookup_shim:
    push %rbx
    push %r12
    push %r13
    mov %rdi,%rbx
    mov %rsi,%r12
    mov %edx,%r13d
    xor %eax,%eax
1:
    test %rbx,%rbx
    jz 2f
    mov %rbx,%rdi
    mov %r12,%rsi
    mov %r13d,%edx
    call *3f(%rip)
    test %rax,%rax
    jnz 2f
    mov %rbx,%rdi
    call *4f(%rip)
    mov %rax,%rbx
    jmp 1b
2:
    pop %r13
    pop %r12
    pop %rbx
    ret
3:  .quad 0
4:  .quad 0
.size home_lookup_shim,.-home_lookup_shim
