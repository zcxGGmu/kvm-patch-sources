# README Summary 

**This is a brief readme introduction on how to properly run and operate `proot` on `openEuler RISCV 23.09`.  Before compiling and running, there is a `bug with RISCV Linux ptrace` that needs to be addressed.**



# Bug summary

**A ptrace bug in riscv linux , when tracer want to change tracee's a0 register in option PTRACE_SYSCALL to stop the tracee**



# Bug descrption

- **Hardware Environment : Qemu 7.2.0, qemu-system-riscv64**
- **OS Distribution: openEuler 23.09 RISCV community** 
- **Linux Kernel Version: 6.4.0**
- **Bug Description : when i am trying to use ptrace for tracer to change the tracee's a0 register in riscv linux when tracee is trace stopped by `PTRACE_SYSCALL`, actually , it doesn't change a0 , but it actually can change a1 ... a7 and other registers**



# Bug  analysis

- **Take a look at this patch [[PATCH\] riscv: entry: Save a0 prior syscall_enter_from_user_mode() (kernel.org)](https://lore.kernel.org/lkml/20230403-crisping-animosity-04ed8a45c625@spud/T/)** 

- **we need to know that where will sleep in when tracee signal self in syscall enter. take a look at file in linux  `/arch/riscv/kernel/traps.c`  , the function  `do_trap_ecall_u` ,  every time process execute syscall will get in the function , and will be blocked in `syscall_enter_from_user_mode` if it is traced for syscall enter and exit . and you will know that regs->orig_a0 is be assigned before `syscall_enter_from_user_mode` . and if  we use ptrace for tracer to change the register , we can't change orig_a0 , we can only change a0, because riscv ptrace USERSPACE don't support the orig_a0 change.  so, actually , we can't change orig_a0 use PTRACE_SYSCALL option.** 



# Bug Reproduction

### compile code.c and test.c in riscv linux

```c
gcc code.c -o a.out
gcc test.c -o test
```

### put a.out test in the same directory

### execute a.out

```c
./a.out
```

### what do "a.out"  and "test" do ?

- **`a.out`** 
  - **`fork` and setup tracee relationship between father and son** 
  - **`father process` :  take  `waitpid` to get tracee and  use `ptrace syscall` to change a0  to NULL when tracee is executing  `execve syscall`**  
  - **`son process` :  first `claim itself can be traced` and `signal self` to be trace stopped , then execute `./test` ELF**
- **`test`**
  - **print all argument passed by `execve syscall` , also the `envp`**
- **if ptrace execute success , `test` will not be excuted , but actually , `./test` works well .**



# hot to fix bug

- **add orig_a0 to USERSPACE  : This behavior can affect many modules, including some user space header files such as asm/ptrace.h, etc.  I have not submitted the corresponding patch, but rather reported it as a bug to the Linux community.**



# simple way to support ptrace and run proot

- **Note : The methods for all these options are only applicable for temporarily modifying the kernel code locally.**
  - **insert the code "`if(regs->orig_a0 != regs->a0 ) regs->orig_a0 = regs->a0`" at the end of the `syscall_trace_enter` function in the file `/kernel/entry/common.c`.**
  - ![](add.png)**The places marked with red lines are where you need to add the corresponding code.**
  - **The addition of this code is rather crude:**
    - **It insert RISCV architecture code into the generic architecture code.**
    - **It does not effectively resolve the BUG mentioned in the [PATCH](https://lore.kernel.org/lkml/20230403-crisping-animosity-04ed8a45c625@spud/T/). it merely reduces the likelihood of the BUG occurring**
    - **However, it allows proot to function properly.**



# testcase for riscv proot

**I employed a temporary solution by recompiling the openEuler kernel locally, and this is the result of my proot test cases.**

https://github.com/Jer6y/rv_linux_bug/assets/88422053/578bf9f6-5227-4c63-a282-eb4ae1af23da

**Compared to running on `x86`, there were 7 additional failures.** 

**This could be due to the `ptrace still having bugs`, or it might be that the ported code requires further improvement.**



# use proot on openEuler riscv to chroot 

https://github.com/Jer6y/rv_linux_bug/assets/88422053/4f343c1e-24ab-4c78-bade-29c190f30a16
