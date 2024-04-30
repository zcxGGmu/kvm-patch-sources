# 0 说明

> linux-v6.9-rc6：[riscv - arch/riscv - Linux source code (v6.9-rc6) - Bootlin](https://elixir.bootlin.com/linux/v6.9-rc6/source/arch/riscv)
>
> 于2024/4/29.



[RISC-V Syscall 系列1：什么是 Syscall ? - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part1-usage/)

[RISC-V Syscall 系列 2：Syscall 过程分析 - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part2-procedure/#handle_exception)

- [ ] [RISC-V Syscall 系列 3：什么是 vDSO？ - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part3-vdso-overview/)
- [ ] [RISC-V Syscall 系列 4：vDSO 实现原理分析 - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part4-vdso-implementation/)

# 1 syscall分析

## 1.1 syscall通识

Syscall 又称为系统调用，它是操作系统内核给用户态程序提供的一组 API，可以用来访问系统资源和内核提供的服务。比如用户态程序申请内存、读写文件等都需要通过 Syscall 完成。

通过 Linux 源码里可以看到(include/linux/syscalls.h)，大约有 400 多个 Syscall。其中一部分是兼容 [POSIX](https://en.wikipedia.org/wiki/POSIX) 标准，另一些是 Linux 特有的。

<img src="https://tinylab.org/wp-content/uploads/2022/03/riscv-linux/images/riscv_syscall/linux_api.svg" alt="Linux_API" style="zoom: 33%;" />

## 1.2 调用syscall的方式

### 1) 直接调用

通过系统调用，往标准输出上打印一串字符。代码如下：

```assembly
.data
msg:
    .ascii "Hello, world!\n"
.text
    .global _start
_start:
    li a7, 64    # linux write syscall
    li a0, 1     # stdout
    la a1, msg   # address of string
    li a2, 14    # length of string
    ecall        # call linux syscall
    li a7, 93    # linux exit syscall
    li a0, 0     # return value
    ecall        # call linux syscall
---------------------------------------------
$ riscv64-linux-gnu-gcc -c test.S
$ riscv64-linux-gnu-ld -o test test.o
$ qemu-riscv64 test
Hello, world!
```

RISC-V 中通过 `ecall` 指令进行 Syscall 的调用。 `ecall` 指令会将 CPU 从用户态转换到内核态，并跳转到 Syscall 的入口处。通过 a7 寄存器来标识是哪个 Syscall。至于调用 Syscall 要传递的参数则可以依次使用 a0-a5 这 6 个寄存器来存储。

`write` 的系统调用号为 64，所以上述代码里将 64 存储到 a7 中。`write` 系统调用的参数有 3 个，第一个是文件描述符，第二个是要打印的字符串地址，第三个是字符串的长度，上述代码中将这三个参数分别存入到 a0、a1、a2 这三个寄存器中。

系统调用号列表可以在 Linux 源码中进行查看：

```c
//include/uapi/asm-generic/unistd.h

#define __NR_write 64
#define __NR_exit 93
```

系统调用函数声明源码位置：

```c
//include/linux/syscalls.h

asmlinkage long sys_write(unsigned int fd, const char __user *buf, size_t count);
asmlinkage long sys_exit(int error_code);
```

### 2) 标准C库

直接使用汇编调用syscall，显然是繁琐的，[C 标准库](https://en.wikipedia.org/wiki/C_standard_library)提供了对 Syscall 的封装。下面用一段 C 代码例子看看如何使用 Syscall：

```c
#include <unistd.h>
int main() {
    write(1, "Hello, world!\n", 14);
    return 0;
}
----------------------------------------
$ riscv64-linux-gnu-gcc -static testc.c -o testc
$ qemu-riscv64 testc
Hello, world!
```

往往write函数，将调用到sys_write，该函数内部将执行ecall指令，并携带sycall_id和其他参数陷入内核。

## 1.3 syscall内核过程分析🎉

### 1) riscv特权模型

#### 特权级

CPU在PC处取指令，然后执行指令，指令改变CPU状态，如此循环。指令改变CPU状态，被改变的有CPU的寄存器以及内存，被改变的寄存器包括PC，这样CPU就可以完成计算以及指令跳转。

这个同步变化的模型，可能被中断或者异常打断。实际上，异常的整体运行情况也符合上面的模型，指令在执行的时候，会遇到各种各样的异常，当异常发生的时候，硬件通过一组寄存器，向软件提供异常发生时的硬件状态，同时硬件改变PC的状态，使其指向一段预先放好的代码。中断异步的打断CPU正在执行的指令，通过一组寄存器向软件提供中断发生时的硬件状态，同时硬件改变PC的状态，使其指向一段预先放好的代码。

当中断或异常发生的时候，硬件可以把CPU切换到一个正交的状态上，这些状态对于CPU的寄存器有着不同的访问权限。目前在riscv处理器上，有U, S, M等CPU模式，它们的编码分别是：0，1，3。

#### 寄存器以及访问方式

CPU有通用寄存器，也有系统寄存器，其中系统寄存器也叫CSR，control system register。一般指令可以访问通用寄存器，但是只有特殊的指令才能访问系统寄存器，在riscv上，有一堆这样的csr指令。这些指令以 `csrxxx` 的方式命名，通过寄存器编号做各种读写操作。

对于CSR的域段，riscv spec上定义了三个标准的读写属性，分别是：WPRI/WLRL/WARL，其中W是write，R是read，P是preserve，I是ignore，L是legal。所以，每个域段含义是：

* WPRI对于写来说，不改变值，如果WPRI是preserve的域段，需要写成0，软件读出这个字段需要忽略掉；
* 对于WLRL的域段，必须写入一个合法的值，否则可能报非法指令异常，如果上一次写是一个合法值，那么下次读就是这个合法值，如果上一次写是一个非法值，那么下次读返回一个随机值，但是这个随机值和写入的非法值和写入非法值之前的值有关系；
* 对于WARL，可以写入任何值，但是，读出的总是一个合法值，这个性质可以用来做特性检测，比如misa的extension域段，它表示extension的使能情况，软件是可以写这个bit把相关的特性关掉的，当一个特性被关掉时，我们可以尝试写相关的bit，看看能否写入，如果可以写入，那么这个特性是存在的，只是被关掉，如果写入不了，那么平台本来就不支持这个特性。

#### M mode寄存器

M mode下的寄存器大概可以分为若干类，一类是描述机器的寄存器，一类是和中断异常以及内存相关的寄存器，一类是时间相关的寄存器，另外还有PMU相关的寄存器。

* **描述机器的寄存器有：**misa，mvendorid，marchid，mimpid，mhartid。

  从名字就可以看出，这些寄存器是机器相关特性的描述，其中最后一个是core的编号。misa描述当前硬件支持的特性，riscv很多特性是可选的，这个寄存器就相当于一个能力寄存器。

* **中断异常相关的寄存器有：**mstatus, mepc, mtvec, mtval, medeleg/mideleg, mie/mip, mcause。

  S mode的相关寄存器和这些寄存器基本一样，只是以s开头。这些寄存器，我们可以用中断异常处理的流程把他们串起来，在下面S mode里描述。比较特殊的是medeleg/mideleg寄存器，默认情况下，riscv的中断和异常都在M mode下处理，可以通过配置这两个寄存器，把异常或中断委托在S mode下处理。

* **内存管理相关的寄存器有:** PMP相关寄存器。

* **时间管理相关的寄存器：**mtime，mtimecmp。CPU内时钟相关的寄存器。

我们依次看看这些寄存器的具体定义，在看的时候，尽可能发掘一下这些寄存器定义背后的逻辑。

---

##### mstatus

status是机器相关的全局状态寄存器。riscv上，虽然各个mode的status寄存器是独立的，但各个mode的status的各个域段是正交的，基本上是每个域段在每个mode上都有一份。mstatus各个域段的说明，如下：

```sh
MIE MPIE MPP SIE SPIE SPP SXL UXL MBE SBE UBE MPRV MXR SUM TVM TW TSR FS VS XS SD
```

* `MIE/MPIE/MPP/SIE/SPIE/SPP` 在spec里是一起描述的。

  * xIE表示，全局中断的使能情况
  * xPIE表示，特权级切换之前x mode全局中断的使能情况
  * xPP表示。特权级切换到x mode之前CPU所处在的mode。

  下面具体讲下SIE/SPIE/SPP，MIE/MPIE/MPP的逻辑是一样的：

  > XIE控制中断的全局使能，这里比较有意思的逻辑是，如果CPU当前运行在x mode，比x mode特权级低的中断都关了，和更低特权级的中断使能没有关系，比x mode更高的中断都会上报，和更高优先级的中断使能位没有关系，即高特权中断在任何情况下都可抢占低特权级CPU。
  >
  > 在中断或异常时，导致处理器切到S mode，硬件会把SIE清0，这个就是关S mode的全局中断，这样SIE的原始信息就需要被另外保存到SPIE这个bit上，只有这样才不会丢信息。SPIE也用来提供sret返回时，用来恢复SIE的值，硬件在执行sret的时候会把SPIE的值写入SI，同时把SPIE写1。
  >
  > SPP的作用也是类似的，从哪个mode切到目前的mode也是一个重要信息，需要提供给软件。同样，SPP也用来给sret提供返回mode，硬件取SPP的值作为返回mode，同时更新SPP为系统支持的最低优先级。
  >
  > 可以看到，SPIE和SPP的语意，只在切换进S mode时是有固定一个意义的，比如，sret返回新模式，SPP会写为最低优先级，这个显然不能指示新模式的之前模式是最低优先级。

* `SXL/UXL` 描述的是S mode/U mode的寄存器宽度，riscv上这个可以是WARL也可以实现成只读，riscv上搞的太灵活，但是实现的时候，搞成只读就好。

* `MBE/SBE/UBE` 是配置M mode、S mode以及U mode下，数据load/store的大小端，指令总是小端的。CPU对内存还有隐式的访问，比如page walk的时候访问页表，对于这种访问，CPU怎么理解对应数据的大小端，S mode页表的大小端也是SBE定义。

* `MPRV(modify privilege)` 改变特定特权级下load/store的行为，当MPRV是0时，当前特权级load/store内存的规则不变；当MPRV是1时，使用MPP的值作为load/store内存的特权级，sret/mret返回到非M mode是，顺带把MPRV清0。

  看起来MPRV只在M mode改变load/store的行为，难道从S mode陷入M mode(这时MPP是S mode)，如果把MPRV配置成1，M mode就可以用S mode定义的页表去访问？

  查了下opensbi的代码，在处理misaligned load/store时还真有这样用的，貌似是从内核读引起misaligned load/store的对应指令到opensbi。(相关代码在opensbi的lib/sbi/sbi_unpriv.c里)

* `MXR(Make executable readable)` 本质上用来抑制某些异常的产生，MXR是0时load操作只可以操作readable的地址；MXR是1，把这个约束放松到load操作也可以作用readable和executable的地址，这个就允许load text段的指令，这个配置在opensbi里是配合上面的MPRV一起用的，可以看到要在opensbi里load misaligned的load/store指令，就需要有这个配置。

  `MPRV/MXR` 是在基本逻辑上，对访存操作的一些限制做放松，从而方便一些M mode的操作。这个在riscv的spec里有明确的提及，其实就是我们上面举的例子，比如，没有MPRV，M mode也可以通过软件做page talk得到PA，有了MPRV硬件就可以帮助做这个工作，效率更高。

* `SUM(permit supervisor user memory access)`，控制S mode对U mode内存的访问。默认情况SUM=0不容许内核态直接访问用户态，SUM=1时，去掉这个限制。对于MPRV=1/MPP=S mode的情况，SUM同样是适用的，MPRV实际上是在原来的隔离机制上打了洞，不过由于MPP只能是更低的特权级，本来高特权级就可以访问低特权级的资源，MRPV是实现的一个优化，但是，作为协议设计，只要开了这样的口，以后每个特性与之相关的时候，就都要考虑一下。

* `TVM(trap virtual memory)` 控制S mode的satp访问，以及sfence.vma/sinval.vma指令是否有效。默认(TVM=0)是有效的，TVM=1可以关掉S mode如上的功能。目前，还没有看到opensbi里使用这个bit。

* `TW(timeout wait)` 控制wfi指令的行为，默认(TW=0)情况，wfi可能一直等在当前指令运行的特权级，当TW=1时，wfi如果超时，会直接触发非法指令异常，如果这个超时时间是0，那么wfi立马就触发一个非法指令异常。

  spec上说，wfi的这个特性可以用在虚拟化上，看起来是用wfi陷入异常，然后可以换一个新的guest进来。目前，还没有看到opensbi里有使用这个bit。另外，目前的定义是，如果U mode的wfi超时，会有非法指令异常被触发。

* `TSR(trap sret)` 控制sret的行为，默认(TSR=0)下，S mode的sret指令是正常执行的，但在TSR=1时，S mode执行sret会触发非法指令异常。spec的解释是，sret触发非法指令异常，是为了在不支持H扩展的机器上模拟H扩展。目前，还没有看到opensbi里有使用这个bit。

* `FS/VS/XS/SD` 给是软件优化的hint，FS/VS/XS每个都是2bit，描述的是浮点扩展/向量扩展/所有扩展的状态，因为扩展有可能涉及到相关计算用的寄存器，那么在CPU上下文切换的时候，就要做相关寄存器的保存和恢复，FS/VS/XS的状态有off/init/clean/dirty，这样增加了更多的状态，软件就可以依次做更加精细的管理和优化，SD只有一个bit，表示FS/VS/XS有没有dirty。riscv的spec给出了这些状态的详细定义。

---

##### mtvec

`mtvec` 用来存放中断异常时，PC的跳转地址。mtvec的最低两个bit用来描述不同的跳转方式，目前有两个，一个是直接跳转到mtvec base，一个是跳转到 `base + 中断号 * 4` 的地址，后面这种只是对中断起作用。

---

##### medeleg/mideleg

`medeleg/mideleg` 可以把特定的异常和中断，委托到S mode处理。riscv spec里提到，如果把一个异常委托到S mode，那么发生异常时，要更新的是mstatus里的SPIE/SIE/SPP，但异常在S mode模式处理，也看不到mstatus里的SPIE/SIE/SPP啊？

从qemu的实现上看，mstatus和sstatus其实内部实现就是一个寄存器，只不过结合权限控制对外表现成两个寄存器，那么这里写mstatus的SPIE/SIE/SPP，在S mode读sstatus的SPIE/SIE/SPP，其实读写的都是相同的地方。

软件可以把相应的位置写1，然后再读相关的位置，以此查看相关中断或者异常的代理寄存器有没有支持，medeleg/mideleg的域段属性是WARL，这个就是说，可以随便写入，但是总是返回合法值，当对应域段没有实现时，1是无法写入的，所以得到的还是0，反之读到的是1。

> medeleg/mideleg不存在默认代理值，不能有只读1的bit存在。

不能从高特权级向低特权级做trap，比如，已经把非法指令异常委托到S mode了，但是在M mode的时候出现指令异常，那还是在M mode响应这个异常。同级trap是可以的。

中断一旦被代理，就必须在代理mode处理。从这里看中断还是和特权级关系比较近的，在定义中断的时候，其实已经明确定义了是哪个特权级处理的中断，称之为由X-Mode响应的中断。

---

##### mip/mie

`mip.MEIP/mip.MTIP` 是只读，由外部中断控制器或者timer配置和清除。mip.MSIP也是只读，一个核写寄存器，触发另外核的mip.MSIP。`mip.SEIP/mip.STIP` 可以通过外接中断控制器或者timer写1，也可以在M mode对他们写1，以此触发S mode的外部中断和timer中断。riscv spec提到mip.SSIP也可以由平台相关的中断控制器触发。

这里又出现了和status寄存器一样的问题，mip的SEIP/STIP/SSIP域段的位置和sip域段上SEIP/STIP/SSIP的位置是一样的，所以，riscv spec有提到，如果中断被委托到S mode, sip/sie和mip里对应域段的值是一样的。

中断的优先级是，先从mode划分，M mode的最高，在同一级中从高到低依次是：external interrupt，software interrupt, timer interrupt。

---

##### mepc/mcause/mtval/mscratch

这些寄存器的功能相对比较简单，具体的描述在下面S mode里介绍。

---

#### S mode寄存器

S mode存在和中断异常以及内存相关的寄存器。我们主要从S mode出发来整理梳理下。

> * 中断异常相关的寄存器有：sstatus, sepc, stvec, stval, sie, sip, scratch, scause
> * 内存管理相关的寄存器有: satp

* `sstatus` 是S mode的status寄存器，具体的域段有SD/UXL/MXR/SUM/XS/FS/SPP/SPIE/UPIE/SIE/UIE，其中很多都在mstatus寄存器中已经有介绍。之前没有涉及到的，UIE/UPIE和用户态中断相关。
* `sie` 是riscv上定义的各种具体中断的使能状态，每种中断一个bit。`sip` 是对应各种中断的pending状态，每种中断一个bit。基本逻辑和M mode的是一样的，只不过控制的是S mode和U mode的逻辑。

* `stvec` 是存放中断异常的跳转地址，当中断或异常发生时，硬件在做相应状态的改动后，就直接跳到stvec保存的地址，从这里取指令执行。基于这样的定义，软件可以把中断异常向量表的地址，配置到这个寄存器里，中断或者异常的时候，硬件就会把PC跳到中断异常向量表。当然软件也可以把其他的地址配置到stvec，借用它完成跳转的功能。

* `sepc` 是S mode exception PC，就是硬件用来给软件报告发生异常或者中断时的PC的，当异常发生时，sepc就是异常指令对应的PC，当中断发生的时候，sepc是被中断的指令对应的PC。比如有A、B两条指令，中断发生在AB之间、或者和B同时发生导致B没有执行，sepc保存B指令的PC。

* `scause` 报告中断或异常发生的原因，当这个寄存器的最高bit是1时表示中断，0表示异常。
* `stval` 报告中断或异常的参数，当发生非法指令异常时，这个寄存器里存放非法指令的指令编码，当发生访存异常时，这个寄存器存放的是被访问内存的地址。

* `scratch` 寄存器是留给软件使用的一个寄存器。Linux内核使用这个寄存器判断，中断或者异常发生的时候，CPU是在用户态还是内核态，当scratch是0时，表示在内核态，否则在用户态。
* `satp` 是存放页表的基地址，riscv内核态和用户态，分时使用这个页表基地址寄存器，这个寄存器的最高bit表示是否启用页表。如果启用页表，硬件在执行访存指令的时候，在TLB没有命中时，就会通过 `satp` 做page table walk，以此来找虚拟地址对应的物理地址。

到此为止，中断或异常发生时需要用到的寄存器都有了。我们下面通过具体的中断或者异常流程，把整个过程串起来。

#### Trap处理流程

中断异常向量表的地址，会提前配置到 `stvec` 中。`medeleg/mideleg` 寄存器也需要提前配置好，把需要在S mode下处理的异常和中断对应的bit配置上。

当中断或异常发生的时候，硬件把SIE的值copy到SPIE，当前处理器mode写入SPP，SIE清0。`sepc` 存入异常指令地址、或者中断指令地址，`scause` 写入中断或者异常的原因，`stval` 写入中断或异常的参数，然后通过 `stvec` 得到中断异常向量的地址。随后，硬件从中断异常向量地址取指令执行。

以Linux内核为例，riscv的中断异常处理流程，先保存中断或异常发生时的寄存器上下文，然后根据 `scause` 的信息，找见具体的中断或异常处理函数执行。具体的软件流程分析可以参考：[Linux内核riscv entry.S分析](https://wangzhou.github.io/Linux内核riscv-entry-S分析/)。

当异常或中断需要返回时，软件可以使用sret指令，sret指令在执行的时候，会把 `sepc` 寄存器里保存的地址作为返回地址，使用SPP寄存器里的值作为CPU的mode，把SPIE的值恢复到SIE上，SPIE写1，SPP写入U mode编号。可见，在调用sret前，软件要配置好sepc、SPP、SPIE寄存器的值。

#### 一些特权指令

ecall或者ebreak异常。ecall异常又可以分为ecall from U mode、ecall from S mode，分别表示ecall是在CPU U mode还是在S mode发起的。在Linux上，从U mode发起的ecall就是一个系统调用，软件把系统调用需要的参数先摆到系统寄存器上，然后触发ecall指令，硬件依照上述的异常流程改变CPU的状态，最终软件执行系统调用代码，参数从系统寄存器上获取。

> Syscall 的调用参数和返回值传递通过遵循如下约定实现：
>
> - 调用参数
>   - `a7` 寄存器存放系统调用号，区分是哪个 Syscall
>   - `a0-a5` 寄存器依次用来表示 Syscall 编程接口中定义的参数
> - 返回值
>   - `a0` 寄存器存放 Syscall 的返回值

机器相关的指令：reset、wfi。reset复位整个riscv机器。wfi执行的时候会挂起CPU，直到CPU收到中断，一般是用来降低功耗的。

内存屏障相关的指令：sfence.vma。sfence.vma，和其他架构下的TLB flush指令类似，用来清空TLB，这个指令可以带ASID或address参数，表示清空对应参数标记的TLB，当ASID或者address的寄存器是X0时，表示对应的参数是无效的。

### 2) 流程分析

[Linux内核riscv head.S分析 | Sherlock's blog (wangzhou.github.io)](https://wangzhou.github.io/Linux内核riscv-head-S分析/)

[Linux内核riscv entry.S分析 | Sherlock's blog (wangzhou.github.io)](https://wangzhou.github.io/Linux内核riscv-entry-S分析/)

---

#### 一些前置知识

##### riscv汇编/kernel

```assembly
#define __ASM_STR(x)	x
#define __REG_SEL(a, b)	__ASM_STR(a)
#define REG_S		__REG_SEL(sd, sw)
#define REG_L		__REG_SEL(ld, lw)
ld rd, offset(rs1) 		# x[rd] = M[x[rs1] + sext(offset)][63:0]
sd rs2, offset(rs1) M[x[rs1] + sext(offset) = x[rs2][63: 0]

/*
	arch/riscv/kernel/asm-offsets.c定义了很多struct相对于首地址的偏移量，
	其目的是利用sp/tp指针保存/恢复context.
	OFFSET宏定义在: include/linux/kbuild.h
	参考: https://blog.csdn.net/gzxb1995/article/details/105066070
*/
#define DEFINE(sym, val) \
	asm volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))
#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem))
#define offsetof(TYPE, MEMBER)	__builtin_offsetof(TYPE, MEMBER)

/* PT_*(sp) */
OFFSET(PT_RA, pt_regs, ra); => #define PT_RA(x) //...

/* TASK_TI_*(tp) */
OFFSET(TASK_TI_KERNEL_SP, task_struct, thread_info.kernel_sp);
OFFSET(TASK_TI_USER_SP, task_struct, thread_info.user_sp);

li rd, immediate	 	# x[rd] = immediate
la rd, symbol 			# x[rd] = &symbol
j offset 				# pc += sext(offset)
jr rs1 					# pc = x[rs1]
jal rd, offset 			# x[rd] = pc+4; pc += sext(offset)
jalr rd, offset(rs1) 	# t =pc+4; pc=(x[rs1]+sext(offset))&~1; x[rd]=t
tail symbol 			# pc = &symbol; clobber x[6]
add rd, rs1, rs2 		# x[rd] = x[rs1] + x[rs2]

csrrw rd, csr, zimm[4:0] # (csrrw tp, CSR, tp) t = CSRs[csr]; CSRs[csr] = x[rs1]; x[rd] = t
csrrc rd, csr, rs1 		 # t = CSRs[csr]; CSRs[csr] = t &~x[rs1]; x[rd] = t
csrr rd, csr 			 # x[rd] = CSRs[csr]
csrw csr, rs1 			 # CSRs[csr] = x[rs1]

addi rd, rs1, immediate  # x[rd] = x[rs1] + sext(immediate)
andi rd, rs1, immediate  # x[rd] = x[rs1] & sext(immediate)
srli rd, rs1, shamt 	 # x[rd] = (x[rs1] ≫𝑢 shamt)
slli rd, rs1, shamt      # x[rd] = x[rs1] ≪ shamt

bnez rs1, offset 		 # if (rs1 ≠ 0) pc += sext(offset) <=>  bne rs1, x0, offset
bge rs1, rs2, offset     # if (rs1 ≥s rs2) pc += sext(offset)
bgeu rs1, rs2, offset 	 # if (rs1 ≥u rs2) pc += sext(offset)
```

##### task_struct/pt_regs/kernel_stack

```c
struct task_struct {
    struct thread_info		thread_info;
    /* CPU-specific state of this task: */
	struct thread_struct		thread;
}

// arch/riscv/include/asm/thread_info.h
/*
 * low level task data that entry.S needs immediate access to
 * - this struct should fit entirely inside of one cache line
 * - if the members of this struct changes, the assembly constants
 *   in asm-offsets.c must be updated accordingly
 * - thread_info is included in task_struct at an offset of 0.  This means that
 *   tp points to both thread_info and task_struct.
 */
struct thread_info {
	unsigned long   flags;		/* low level flags */
	int             preempt_count;  /* 0=>preemptible, <0=>BUG */
	/*
	 * These stack pointers are overwritten on every system call or
	 * exception.  SP is also saved to the stack it can be recovered when
	 * overwritten.
	 */
	long			kernel_sp;	/* Kernel stack pointer */
	long			user_sp;	/* User stack pointer */
	int				cpu;
	unsigned long	syscall_work;	/* SYSCALL_WORK_ flags */
};

/* CPU-specific state of a task */
struct thread_struct {
	/* Callee-saved registers */
	unsigned long ra;
	unsigned long sp;	/* Kernel mode stack */
	unsigned long s[12];	/* s[0]: frame pointer */
	unsigned long bad_cause;
	unsigned long align_ctl;
};

// arch/riscv/include/asm/ptrace.h
struct pt_regs {
	unsigned long epc;
	unsigned long ra;
	unsigned long sp;
	unsigned long gp;
	unsigned long tp;
	unsigned long t0;
	unsigned long t1;
	unsigned long t2;
	unsigned long s0;
	unsigned long s1;
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
	unsigned long a5;
	unsigned long a6;
	unsigned long a7;
	unsigned long s2;
	unsigned long s3;
	unsigned long s4;
	unsigned long s5;
	unsigned long s6;
	unsigned long s7;
	unsigned long s8;
	unsigned long s9;
	unsigned long s10;
	unsigned long s11;
	unsigned long t3;
	unsigned long t4;
	unsigned long t5;
	unsigned long t6;
	/* Supervisor/Machine CSRs */
	unsigned long status;
	unsigned long badaddr;
	unsigned long cause;
	/* a0 value before the syscall */
	unsigned long orig_a0;
};
```







#### 代码分析

直接在代码里写注释，段注释用 `n:` 开头，行注释直接 `#`，这里把 `CONFIG_*` 都删了，只保留syscall调用链相关的代码。

##### head.S

- [ ] 系统启动时，scratch/tp寄存器如何设置的？



##### entry.S

```assembly
SYM_CODE_START(handle_exception)
	/*
	 * If coming from userspace, preserve the user thread pointer and load
	 * the kernel thread pointer.  If we came from the kernel, the scratch
	 * register will contain 0, and we should continue on the current TP.
	 */
	csrrw tp, CSR_SCRATCH, tp # 
	bnez tp, .Lsave_context

.Lrestore_kernel_tpsp:
	csrr tp, CSR_SCRATCH
	REG_S sp, TASK_TI_KERNEL_SP(tp)

.Lsave_context:
	REG_S sp, TASK_TI_USER_SP(tp)
	REG_L sp, TASK_TI_KERNEL_SP(tp)
	addi sp, sp, -(PT_SIZE_ON_STACK)
	REG_S x1,  PT_RA(sp)
	REG_S x3,  PT_GP(sp)
	REG_S x5,  PT_T0(sp)
	save_from_x6_to_x31

	/*
	 * Disable user-mode memory access as it should only be set in the
	 * actual user copy routines.
	 *
	 * Disable the FPU/Vector to detect illegal usage of floating point
	 * or vector in kernel space.
	 */
	li t0, SR_SUM | SR_FS_VS

	REG_L s0, TASK_TI_USER_SP(tp)
	csrrc s1, CSR_STATUS, t0
	csrr s2, CSR_EPC
	csrr s3, CSR_TVAL
	csrr s4, CSR_CAUSE
	csrr s5, CSR_SCRATCH
	REG_S s0, PT_SP(sp)
	REG_S s1, PT_STATUS(sp)
	REG_S s2, PT_EPC(sp)
	REG_S s3, PT_BADADDR(sp)
	REG_S s4, PT_CAUSE(sp)
	REG_S s5, PT_TP(sp)

	/*
	 * Set the scratch register to 0, so that if a recursive exception
	 * occurs, the exception vector knows it came from the kernel
	 */
	csrw CSR_SCRATCH, x0

	/* Load the global pointer */
	load_global_pointer

	/* Load the kernel shadow call stack pointer if coming from userspace */
	scs_load_current_if_task_changed s5

	move a0, sp /* pt_regs */
	la ra, ret_from_exception

	/*
	 * MSB of cause differentiates between
	 * interrupts and exceptions
	 */
	bge s4, zero, 1f

	/* Handle interrupts */
	tail do_irq
1:
	/* Handle other exceptions */
	slli t0, s4, RISCV_LGPTR
	la t1, excp_vect_table
	la t2, excp_vect_table_end
	add t0, t1, t0
	/* Check if exception code lies within bounds */
	bgeu t0, t2, 1f
	REG_L t0, 0(t0)
	jr t0
1:
	tail do_trap_unknown
SYM_CODE_END(handle_exception)
ASM_NOKPROBE(handle_exception)
        
/*
 * The ret_from_exception must be called with interrupt disabled. Here is the
 * caller list:
 *  - handle_exception
 *  - ret_from_fork
 */
SYM_CODE_START_NOALIGN(ret_from_exception)
	REG_L s0, PT_STATUS(sp)
#ifdef CONFIG_RISCV_M_MODE
	/* the MPP value is too large to be used as an immediate arg for addi */
	li t0, SR_MPP
	and s0, s0, t0
#else
	andi s0, s0, SR_SPP
#endif
	bnez s0, 1f

	/* Save unwound kernel stack pointer in thread_info */
	addi s0, sp, PT_SIZE_ON_STACK
	REG_S s0, TASK_TI_KERNEL_SP(tp)

	/* Save the kernel shadow call stack pointer */
	scs_save_current

	/*
	 * Save TP into the scratch register , so we can find the kernel data
	 * structures again.
	 */
	csrw CSR_SCRATCH, tp
1:
	REG_L a0, PT_STATUS(sp)
	/*
	 * The current load reservation is effectively part of the processor's
	 * state, in the sense that load reservations cannot be shared between
	 * different hart contexts.  We can't actually save and restore a load
	 * reservation, so instead here we clear any existing reservation --
	 * it's always legal for implementations to clear load reservations at
	 * any point (as long as the forward progress guarantee is kept, but
	 * we'll ignore that here).
	 *
	 * Dangling load reservations can be the result of taking a trap in the
	 * middle of an LR/SC sequence, but can also be the result of a taken
	 * forward branch around an SC -- which is how we implement CAS.  As a
	 * result we need to clear reservations between the last CAS and the
	 * jump back to the new context.  While it is unlikely the store
	 * completes, implementations are allowed to expand reservations to be
	 * arbitrarily large.
	 */
	REG_L  a2, PT_EPC(sp)
	REG_SC x0, a2, PT_EPC(sp)

	csrw CSR_STATUS, a0
	csrw CSR_EPC, a2

	REG_L x1,  PT_RA(sp)
	REG_L x3,  PT_GP(sp)
	REG_L x4,  PT_TP(sp)
	REG_L x5,  PT_T0(sp)
	restore_from_x6_to_x31

	REG_L x2,  PT_SP(sp)

#ifdef CONFIG_RISCV_M_MODE
	mret
#else
	sret
#endif
SYM_CODE_END(ret_from_exception)
ASM_NOKPROBE(ret_from_exception)
```



















































