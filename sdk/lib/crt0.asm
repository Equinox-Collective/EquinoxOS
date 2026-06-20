[bits 64]
[extern main]
[extern exit]
[extern __libc_init_env]
[extern __libc_init_array]   ; Импортируем функцию вызова конструкторов C++
[global _start]

_start:
    ; Ядро передаёт: rdi=argc, rsi=argv, rdx=envp.
    ; 1. Выравниваем стек по 16 байтам (требование ABI)
    and rsp, -16

    ; 2. Сохраняем argc/argv/envp в callee-saved регистры на время вызовов
    mov r13, rdi          ; argc
    mov r14, rsi          ; argv
    mov r15, rdx          ; envp

    ; 3. Инициализируем окружение: environ = envp
    mov rdi, r15
    call __libc_init_env

    ; 4. Инициализируем глобальные конструкторы (C++)
    call __libc_init_array

    ; 5. Вызываем main(argc, argv, envp)
    mov rdi, r13
    mov rsi, r14
    mov rdx, r15
    call main

    ; 6. Выход
    mov rdi, rax
    call exit

    jmp $