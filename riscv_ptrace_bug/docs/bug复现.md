# 0 计划

> [Jer6y/rv_linux_bug: a descrption for a riscv linux bug (github.com)](https://github.com/Jer6y/rv_linux_bug)
>
> [218701 – tracer can't change a0 in tracee when use ptrace syscall option in riscv (kernel.org)](https://bugzilla.kernel.org/show_bug.cgi?id=218701)

1. bug产生环境如下：

   > - **Hardware Environment : Qemu 7.2.0, qemu-system-riscv64**
   > - **OS Distribution: openEuler 23.09 RISCV community**
   > - **Linux Kernel Version: 6.4.0**
   > - **Bug Description : when i am trying to use ptrace for tracer to change the tracee's a0 register in riscv linux when tracee is trace stopped by `PTRACE_SYSCALL`, actually , it doesn't change a0 , but it actually can change a1 ... a7 and other registers**

   [通过 QEMU 仿真 RISC-V 环境并启动 openEuler RISC-V 系统](https://www.openeuler.org/zh/blog/phoebe/2023-09-26-Run-openEuler-RISC-V-On-Qemu.html)

   - [ ] 换内核；
   - [ ] 或者不用Euler环境，6.4.0内核即可？

2. bug复现：

   > compile code.c and test.c in riscv linux：
   >
   > ```
   > gcc code.c -o a.out
   > gcc test.c -o test
   > ```
   >
   > put a.out test in the same directory，execute a.out：
   >
   > ```
   > ./a.out
   > ```
   >
   > what do "a.out" and "test" do ?
   >
   > - `a.out`
   >   - **`fork` and setup tracee relationship between father and son**
   >   - **`father process` : take `waitpid` to get tracee and use `ptrace syscall` to change a0 to NULL when tracee is executing `execve syscall`**
   >   - **`son process` : first `claim itself can be traced` and `signal self` to be trace stopped , then execute `./test` ELF**
   > - `test`
   >   - **print all argument passed by `execve syscall` , also the `envp`**
   > - **if ptrace execute success , `test` will not be excuted , but actually , `./test` works well .**

---

# 1 log

修改 test 运行时寄存器，场景如下：

* 将 `a0 ~ a6` 置0：

  ![image-20240428170606653](https://cdn.jsdelivr.net/gh/MaskerDad/BlogImage@main/202404281706727.png)

* 只将 `a0` 置0，其余不变：

  ![image-20240428170839825](https://cdn.jsdelivr.net/gh/MaskerDad/BlogImage@main/202404281708851.png)

* 将 `a0/a1` 置为0，其余不变：

  ![image-20240428171118802](https://cdn.jsdelivr.net/gh/MaskerDad/BlogImage@main/202404281711828.png)

* 将 `a0/a2` 置为0，其余不变：

  ![image-20240428171248450](https://cdn.jsdelivr.net/gh/MaskerDad/BlogImage@main/202404281712474.png)

