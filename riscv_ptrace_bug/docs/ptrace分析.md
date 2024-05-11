# 0 说明

> linux-v6.9-rc6：[riscv - arch/riscv - Linux source code (v6.9-rc6) - Bootlin](https://elixir.bootlin.com/linux/v6.9-rc6/source/arch/riscv)
>
> 于2024/4/29.



[RISC-V Syscall 系列1：什么是 Syscall ? - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part1-usage/)

[RISC-V Syscall 系列 2：Syscall 过程分析 - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part2-procedure/#handle_exception)

- [x] [RISC-V Syscall 系列 3：什么是 vDSO？ - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part3-vdso-overview/)
- [x] [RISC-V Syscall 系列 4：vDSO 实现原理分析 - 泰晓科技 (tinylab.org)](https://tinylab.org/riscv-syscall-part4-vdso-implementation/)

---

- [ ] ptrace riscv内核分析

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

### 2) 流程分析: 以ptrace为例

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







#### 代码梳理

直接在代码里写注释，段注释用 `n:` 开头，行注释直接 `#`，这里把 `CONFIG_*` 都删了，只保留syscall调用链相关的代码。下面分析ptrace的整个代码流程。

##### 整体流程overview

```c
handle_exception
    +-> la ra, ret_from_exception
    +-> do_trap_ecall_u /* system call */
    	+-> long syscall = regs->a7;
            /* ??? */
            regs->epc += 4;
            regs->orig_a0 = regs->a0;
    	+-> syscall_enter_from_user_mode(regs, syscall);
		+-> if (syscall >= 0 && syscall < NR_syscalls)
				syscall_handler(regs, syscall);
		+-> syscall_exit_to_user_mode(regs);
	+-> ret_from_exception
        +-> sret //ret to user mode
     
// arch/riscv/include/asm/ptrace.h
struct pt_regs {
	unsigned long epc;
	unsigned long ra;
	unsigned long sp;
	unsigned long a0;
    
    //...
    
	/* a0 value before the syscall */
	unsigned long orig_a0;
};
```

以上，在调用 `syscall_enter_from_user_mode` 前，存在一个备份动作：`regs->orig_a0 = regs->a0`。目前陷入S mode后，`regs->a0` 保存的是用户态的a0值，在返回用户态时必须恢复这个a0，为什么提前备份 `regs->a0` 呢，需要搞清楚两个问题：

- [ ] 猜测 `regs->a0` 的值，在内核态的某些场景下会发生变化，具体什么场景？（a0保存的到底是什么？）
- [ ] 为什么只保存 `a0`，`pt_regs` 中的其他值不需要保存吗？

---

##### `syscall_enter_from_user_mode`

首先关注第一个问题，`a0` 究竟保存的是什么？一切都还要追溯到 `handle_exception` 中：

```assembly
// arch/riscv/kernel/entry.S
// 只关注a0相关的代码


/* n: 在head.S里配置给CSR_TVEC */
ENTRY(handle_exception)
	//...
_save_context:
	/*
	 * n: 保存sp到thread_info用户态指针的位置，如果是就在内核态，那么把内核栈
	 * sp保存到了thread_info用户态栈指针的位置。
	 *
	 * 异常或中断来自内核或者用户态，再下面合并处理。当来自用户态时，tp的值和
	 * scratch寄存器的值是一样的，所以这里不需要恢复tp。
	 */
	REG_S sp, TASK_TI_USER_SP(tp)
	/* n: sp换成内核栈 */
	REG_L sp, TASK_TI_KERNEL_SP(tp)
	/*
	 * n: 扩大栈的范围，扩大的范围用来保存相关的寄存器。移动sp其实就相当于在栈上分配空间。
	 *    sp移动之前的值是中断或者异常打断的上下文，也就是中断或异常处理完后要恢复的值。
	 */
	addi sp, sp, -(PT_SIZE_ON_STACK)
	/*
	 * n: 下面的一段代码把各个系统寄存器保存到栈上刚刚开辟出来的空间, 注意需要
	 * 特殊处理的是sp(x2)和tp(x4)。当前的sp，由于上面的变动已经不是需要保存的sp，
	 * 但是，之前我们已经把需要保存的sp放到了thread_info里，所以下面把thread_info
	 * 里的sp取出后再入栈。
	 */
    //...
    REG_S x10, PT_A0(sp)
        
    /*
	 * n: 上面已经保存了寄存器现场，下面可以使用系统寄存器了，s0保存用户态sp。
	 * 把task_struct里保存的用户态栈指针提取出来，然后在后面保存到内核栈上。
	 */
	REG_L s0, TASK_TI_USER_SP(tp)
	/* n: todo */
#ifdef CONFIG_CONTEXT_TRACKING
	/* If previous state is in user mode, call context_tracking_user_exit. */
	li   a0, SR_PP
	and a0, s1, a0
	bnez a0, skip_context_tracking
	call context_tracking_user_exit
skip_context_tracking:
    /* n: 上面可以知道sp就是pt_regs的地址，a0存放pt_regs的地址 */
	move a0, sp /* pt_regs */  
      
//... 
```

现在明确了：目前 `a0` 寄存器上保存的是 `pt_regs` 的地址值，`regs->a0` 保存的是用户态的a0值。

---

接下来，看`regs->a0` 在内核态下什么场景会被修改？`syscall_enter_from_user_mode` 代码如下：

```c
syscall_enter_from_user_mode(regs, syscall);
+-> enter_from_user_mode(regs);
	+-> arch_enter_from_user_mode(regs);
+-> local_irq_enable();
+-> syscall_enter_from_user_mode_work(regs, syscall);
	+-> //...

```

可以看到，`syscall_enter_from_user_mode` 中调用了 `local_irq_enable` 使能了本地S mode中断，这意味着，中断上下文是可以抢占当前内核态执行流的，由于 `stvec` 从未修改，当S mode interrupt触发时控制流跳转至 `handle_exception`，该函数开始时会对当前的trap来自于哪个特权级进行判断，

```assembly
ENTRY(handle_exception)
	/*
	 * If coming from userspace, preserve the user thread pointer and load
	 * the kernel thread pointer.  If we came from the kernel, the scratch
	 * register will contain 0, and we should continue on the current TP.
	 */
	/*
	 * n: 交换tp寄存器和scratch寄存器里的值。如果exception从用户态进来，scratch
	 * 的值应该是task_struct(下面在离开内核的时候，会把用户进程的task_struct
	 * 指针赋给tp)，如果exception从内核态进来，tp应该是当前线程的task_struct
	 * 指针，scratch应该是0。
	 *
	 * 所以，下面紧接着的处理中，如果从内核进来，要恢复下tp。
	 *
	 * 总结下在用户态和内核态时tp和scratch的值是什么：
	 *
	 *  +----------+--------------+---------------+
	 *  |          | user space   |   kernel      |
	 *  +----------+--------------+---------------+
	 *  | tp:      | tls base     |   task_struct |
	 *  +----------+--------------+---------------+
	 *  | scratch: | task_struct  |   0           |
	 *  +----------+--------------+---------------+
	 *
	 * 注意：配置tp为tls地址的函数是：copy_thread，如果用户态没有使用TLS，tp
	 *       在用户态的值是？
	 */
	csrrw tp, CSR_SCRATCH, tp
	/* n: tp不为0，异常来自用户态，直接跳到上下文保存的地方 */
	bnez tp, _save_context

_restore_kernel_tpsp:
	/*
	 * n: csrr伪指令，把scratch寄存器的值写入tp，上面为了判断是否在内核把tp
	 * 的值和CSR_SCRATCH的值做了交换。这里恢复tp寄存器。
	 */
	csrr tp, CSR_SCRATCH
	/* n: 把内核sp保存到内核thread_info上 */
	REG_S sp, TASK_TI_KERNEL_SP(tp)
_save_context:
	/*
	 * n: 保存sp到thread_info用户态指针的位置，如果是就在内核态，那么把内核栈
	 * sp保存到了thread_info用户态栈指针的位置。
	 *
	 * 异常或中断来自内核或者用户态，再下面合并处理。当来自用户态时，tp的值和
	 * scratch寄存器的值是一样的，所以这里不需要恢复tp。
	 */
	REG_S sp, TASK_TI_USER_SP(tp)
	/* n: sp换成内核栈 */
	REG_L sp, TASK_TI_KERNEL_SP(tp)
	
	/* 和U mode->S mode一样，S mode->S mode把这些寄存器值保存到内核栈空间的pt_regs位置处 */
	REG_S x10, PT_A0(sp)
	
	
```

- [ ] 和 `U mode -> S mode` 一样，`S mode -> S mode` 也需要把这些寄存器值保存到内核栈，这需要重新开辟一块 `PT_SIZE_ON_STACK` 大小的栈空间，同时我们把原来 `pt_regs` 的地址值 (即 `regs->a0`) 备份至。。。



##### `sys_ptrace` 分支处理

```c
asmlinkage long sys_ptrace(long request, long pid, unsigned long addr,
			   unsigned long data);

SYSCALL_DEFINE4(ptrace, long, request, long, pid, unsigned long, addr,
		unsigned long, data)
{
	struct task_struct *child;
	long ret;

	if (request == PTRACE_TRACEME) {
		ret = ptrace_traceme();
		goto out;
	}

	child = find_get_task_by_vpid(pid);
	if (!child) {
		ret = -ESRCH;
		goto out;
	}

	if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
		ret = ptrace_attach(child, request, addr, data);
		goto out_put_task_struct;
	}

	ret = ptrace_check_attach(child, request == PTRACE_KILL ||
				  request == PTRACE_INTERRUPT);
	if (ret < 0)
		goto out_put_task_struct;

	ret = arch_ptrace(child, request, addr, data);
	if (ret || request != PTRACE_DETACH)
		ptrace_unfreeze_traced(child);

 out_put_task_struct:
	put_task_struct(child);
 out:
	return ret;
}
```

# 2 ptrace自顶向下

[articles/20220829-ptrace.md · aosp-riscv/working-group - Gitee.com](https://gitee.com/aosp-riscv/working-group/blob/master/articles/20220829-ptrace.md#5-single-step-单步执行)

[linux-沙盒入门，ptrace从0到1-腾讯云开发者社区-腾讯云 (tencent.com)](https://cloud.tencent.com/developer/article/1799705)

[Linux内核原理分析：利用ptrace系统调用实现一个软件调试器_linux ptrace-CSDN博客](https://blog.csdn.net/qq_42935201/article/details/122672822)

## 2.0 ptrace from 0 to 1

### 1) 初识

ptrace在linux 反调试技术中的地位就如同nc在安全界的地位，ptrace使用场景：

* 编写动态分析工具，如：`gdb`、`strace`；
* 反追踪，一个进程只能被一个进程追踪 (一个进程能同时追踪多个进程)，若此进程已被追踪，其他基于ptrace的追踪器将无法再追踪此进程，更进一步可以实现子母进程双线执行，动态解密代码等更高级的反分析技术；
* 代码注入，往其他进程里注入代码；
* 不退出进程，进行在线升级；

Ptrace 可以让父进程控制子进程运行，并可以检查和改变子进程的核心image的功能（Peek and Poke 在系统编程中是很知名的叫法，指的是直接读写内存内容）。ptrace 主要跟踪的是进程运行时的状态，直到收到一个终止信号结束进程，这里的信号如果是我们给程序设置的断点，则进程被中止，并通知其父进程，在进程中止的状态下，进程的内存空间可以被读写。当然父进程还可以使子进程继续执行，并选择是否忽略引起中止的信号，ptrace 可以让一个进程监视和控制另一个进程的执行，并修改被监视进程的内存、寄存器等，主要应用于断点调试和系统调用跟踪，strace和gdb工具就是基于ptrace编写的。

> 注：ptrace 是linux的一种系统调用，所以当我们用gdb进行attach其他进程的时候，需要root权限。

在Linux系统中，进程状态除了我们所熟知的TASK_RUNNING，TASK_INTERRUPTIBLE，TASK_STOPPED等，还有一个TASK_TRACED，而TASK_TRACED将调试程序断点成为可能。

1. **R (TASK_RUNNING)，可执行状态。**
2. **S (TASK_INTERRUPTIBLE)，可中断的睡眠状态。**
3. **D (TASK_UNINTERRUPTIBLE)，不可中断的睡眠状态。**
4. **T (TASK_STOPPED or TASK_TRACED)，暂停状态或跟踪状态。**

当使用了 ptrace 跟踪后，所有发送给被跟踪的子进程的信号 (除了SIGKILL)，都会被转发给父进程，而子进程则会被阻塞，这时子进程的状态就会被系统标注为 `TASK_TRACED`，而父进程收到信号后，就可以对停止下来的子进程进行检查和修改，然后让子进程继续运行。

>***什么是信号？***
>
>一个信号就是一条消息，它通知进程系统中发生了一个某种类型的事件，信号是多种多样的，并且一个信号对应一个事件，这样才能做到当进程收到一个信号后，知道到底是一个什么事件，应该如何处理（但是要保证必须识别这个信号），个人理解信号就是操作系统跟进程沟通的一个有特殊含义的语句。
>
>我们可以直接通过 `kill -l` ，来查看信息的种类：
>
><img src="https://ask.qcloudimg.com/http-save/yehe-8119233/rlxk48n3k5.png" alt="img" style="zoom:67%;" />
>
>一共62种，其中1~31是非可靠信号，34~64是可靠信号 (非可靠信号是早期Unix系统中的信号，后来又添加了可靠信号，方便用户自定义信号，这二者之间具体的区别在下文中会提到)

---

```c
#include <sys/ptrace.h>       
long ptrace(enum __ptrace_request request, pid_t pid, void *addr, void *data);
```

- `request`: 表示要执行的操作类型。反调试会用到 `PT_DENY_ATTACH`，调试会用到 `PTRACE_ATTACH`；
- `pid`: 要操作的目标进程ID；
- `addr`: 要监控的目标内存地址；
- `data`: 保存读取出或者要写入的数据；

### 2) 内核实现

ptrace的内核实现在 `kernel/ptrace.c` 文件中，直接看内核接口是 `SYSCALL_DEFINE4(ptrace, ...)`，代码如下：

```c
SYSCALL_DEFINE4(ptrace, long, request, long, pid, unsigned long, addr,unsigned long, data)
{
    struct task_struct *child;
    long ret;
       
    if (request == PTRACE_TRACEME)
        {
      ret = ptrace_traceme();
      if (!ret)
        arch_ptrace_attach(current);
        goto out;
    }
       
    child = ptrace_get_task_struct(pid);
    if (IS_ERR(child))
        {
      ret = PTR_ERR(child);
      goto out;
    }
       
    if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
      ret = ptrace_attach(child, request, addr, data);
      /*
       * Some architectures need to do book-keeping after
       * a ptrace attach.
       */
      if (!ret)
        arch_ptrace_attach(child);
      goto out_put_task_struct;
    }
       
    ret = ptrace_check_attach(child, request == PTRACE_KILL ||request == PTRACE_INTERRUPT);
    if (ret < 0)
      goto out_put_task_struct;
    ret = arch_ptrace(child, request, addr, data);
    if (ret || request != PTRACE_DETACH)
      ptrace_unfreeze_traced(child);
    
     out_put_task_struct:
      put_task_struct(child);
     out:
      return ret;
}
```





## 2.1 ptrace uapi intro

> NAME ptrace - process trace
>
> SYNOPSIS #include <sys/ptrace.h>
>
> ```c
>   long ptrace(enum __ptrace_request request, pid_t pid,
>               void *addr, void *data);
> ```

---

ptrace 从名字上看是用于进程跟踪，它提供了一个进程 A 观察和控制另外一个进程 B 执行的能力，这里的进程 A 在术语上称之为 `tracer`，而进程 B 则称之为 `tracee`。

ptrace 函数原型中，参数 `request` 是一系列以 `PTRACE_` 为前缀的枚举值，用于通知内核执行 ptrace 的操作类型，具体可以查看 man 手册。pid 是指定接受 ptrace request 的对象（严格说是线程 ID，因为按照 man 手册的说法 `"tracee" always means "(one) thread", never "a (possibly multithreaded) process"`），addr 和 data 的具体含义视 request 的不同而不同，具体要查看 man 手册。

如果要让 `tracer` 能够 tracing `tracee`，首先要在两者之间建立 tracing 关系。`tracer` 进程和 `tracee` 进程之间，可以采取以下两种方式之一建立 tracing 关系：

- **第一种方式是：**tracer 进程利用 fork 创建子进程，子进程首先调用 `ptrace(PTRACE_TRACEME, 0, ...)` 成为 tracee，建立了与父进程 tracer 的 tracing 关系，然后 tracee 子进程再调用 `execve` 加载需要被 trace 的程序。本文 example-code 主要采用这种方式。
- **第二种方式是：**tracer 进程可以利用 `ptrace(PTRACE_ATTACH，pid,...)` 指定 tracee 的进程号来建立 tracing 关系。一旦 attach 成功，tracer 变成 tracee 的父进程 (用 ps 可以看到)。tracing 关系建立后可以调用 `ptrace(PTRACE_DETACH，pid,...)` 解除。注意 attach 进程时的权限问题，如一个非 root 权限的进程是不能 attach 到一个 root 权限的进程上的。

当 tracing 关系建立后，tracer 可以控制 tracee 的执行，在 tracer 的控制下（即调用各种 ptrace request），内核通过向 tracee 发送信号的方式将其暂停，即 tracee 发生阻塞，任务状态就会被系统标注为 `TASK_TRACED`，即使这个信号被 tracee 忽略（ignore）也无法阻止 tracee 被暂停；但有个信号例外，就是 SIGKILL，也就是说如果 tracee 收到 SIGKILL 仍然会按照默认的方式处理，如果没有注册自己的 handler 默认 tracee 仍然会被杀死。

一旦 tracee 被暂停，tracer 可以通过调用 `waitpid` 获知 tracee 状态的变化，以及状态变化的原因（通过 `waitpid` 返回的 `wstatus`）。此时 tracer 就可以对停止下来的 tracee 进行检查，甚至修改当前寄存器上下文和内存的值，或者让 tracee 继续运行。

---

先写一个简单的 tracee 程序，为排除干扰实现一个干净的程序，我们这里采用汇编，总共 8 条指令，期间调用两次系统调用，第一次调用 `write` 在 stdout 上打印 "Hello\n" 一共 6 个字符，第二次调用 `exit` 结束进程。

```assembly
	.section .rodata
msg:
	.string "Hello\n"

	.text
	.globl _start
_start:
	# write(int fd, const void *buf, size_t count);
	movq $1, %rax		# __NR_write
	movq $1, %rdi		# fd = 1
	leaq msg(%rip), %rsi	# buf = msg
	movq $6, %rdx		# count = 6
	syscall
	
	# exit(int status);
	movq $60, %rax		# __NR_exit
	xor %rdi, %rdi		# status = 0
	syscall
```

tracer 程序如下：

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>

#include <sys/ptrace.h>
#include <sys/user.h> // user_regs_struct 
#include <linux/elf.h> // NT_PRSTATUS
#include <sys/uio.h> // struct iovec

void info_registers(pid_t pid)
{
	long retval;
	struct user_regs_struct regs;
	struct iovec pt_iov = {
		.iov_base = &regs,
		.iov_len = sizeof(regs),
	};
	retval = ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &pt_iov);
	assert(-1 != retval);

	// just print some important registers we care about
	printf( "--------------------------\n"
		"rax: 0x%llx\n"
		"rdi: 0x%llx\n"
		"rsi: 0x%llx\n"
		"rdx: 0x%llx\n",
		regs.rax,
		regs.rdi,
		regs.rsi,
		regs.rdx);
}

int main(void)
{
	int wstatus;
	long retval;
	pid_t pid;

	pid = fork();
	assert(-1 != pid);

	if (0 == pid) {
		// child process starts ......
		retval = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		assert(-1 != retval);

		// a successful call of execl will make tracee received
		// a SIGTRAP, which gives the parent tracer a chance to 
		// gain control before the new program begins execution.
		retval = execl("./tracee", "tracee", NULL);
		assert(-1 != retval);
	}

	// parent process goes here

	// wait no-timeout till status of child is changed
	retval = waitpid(pid, &wstatus, 0);
	assert(-1 != retval);
	
	assert(WIFSTOPPED(wstatus));
	printf("tracer: tracee got a signal and was stopped: %s\n", strsignal(WSTOPSIG(wstatus)));

	printf("tracer: request to query more information from the tracee\n");
	info_registers(pid);
	
	// continue the tracee
	printf("tracer: request tracee to continue ...\n");
	retval = ptrace(PTRACE_CONT, pid, NULL, NULL);
	assert(-1 != retval);

	return 0;
}
```

- 其中 tracer 在收到内核的通知（实际是通过 waitpid 的方式等待内核的通知）知道 tracee 被停止后，可以通过 `ptrace(PTRACE_GETREGSET, ...)` 查询 tracee 进程的寄存器状态。这里有点类似我们在 gdb 中程序暂停后输入 `info registers`。
- 最后 tracer 通过调用 `ptrace(PTRACE_CONT, ...` 通知内核继续执行 tracee，所以最后我们看到 “Hello” 输出，这里就好比 gdb 里输入 `continue` 命令。
- 这个例子里 tracee 之所以能够暂停，实际上是内核的一种缺省行为，即 tracee 在调用 `ptrace(PTRACE_TRACEME, ...)` 申请自己成为 tracee 后，再调用 `excec` 这些系统调用，则内核默认会在实际执行新程序（这里是 tracee）之前发送一个 SIGTRAP 信号给 tracee 先将 tracee 进程挂起，这里我们在 tracer 程序里也验证了这个信号。

## 2.2 example: code/test.c

prtace既能用作调试，也能用作反调试，当传入的request不同时，就可以切换到不同的功能了。在使用ptrace之前，需要在两个进程间建立追踪关系，其中tracee可以不做任何事，也可使用prctl和PTRACE_TRACEME来进行设置，ptrace编程的主要部分是tracer，它可以通过附着的方式与tracee建立追踪关系，建立之后，可以控制tracee在特定的时候暂停并向tracer发送相应信号，而tracer则通过循环等待 `waitpid` 来处理tracee发来的信号，如下图所示：

<img src="https://ask.qcloudimg.com/http-save/yehe-8119233/e2yfg93jx2.png" alt="img" style="zoom:80%;" />

在进行追踪前，需要先建立追踪关系，相关request有如下4个：

> * `PTRACE_TRACEME`：tracee表明自己想要被追踪，这会自动与父进程建立追踪关系，这也是唯一能被tracee使用的request，其他的request都由tracer指定；
> * `PTRACE_ATTACH`：tracer用来附着一个进程tracee，以建立追踪关系，并向其发送SIGSTOP信号使其暂停；
> * `PTRACE_SEIZE`：像PTRACE_ATTACH附着进程，但它不会让tracee暂停，addr参数须为0，data参数指定一位ptrace选项； 
> * `PTRACE_DETACH`：解除追踪关系，tracee将继续运行。

其中建立关系时，tracer使用如下方法：

```c
ptrace(PTRACE_ATTACH, pid, 0, 0);
/*或*/
ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_flags); /*指定追踪选项立即生效*/
```

---

`execl()` 函数，对应的系统调用为 `__NR_execve`，系统调用值为 59。库函数 `execve` 调用链如下：

<img src="https://ask.qcloudimg.com/http-save/yehe-8119233/50w0yc84m8.png" alt="img" style="zoom: 67%;" />

---

以下代码采用 `PTRACE_TRACEME` 的方式建立trace关系，同时利用tracer修改tracee寄存器的值，代码如下：

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <asm/ptrace.h>
#include <sys/uio.h>
#include <linux/elf.h> 
#include <sys/ptrace.h> /* ptrace(2), PTRACE_*, */
#include <sys/types.h>  /* pid_t, size_t, */
#include <stdlib.h>     /* NULL, */
#include <stddef.h>     /* offsetof(), */
#include <sys/user.h>   /* struct user*, */
#include <errno.h>      /* errno, */
#include <assert.h>     /* assert(3), */
#include <sys/wait.h>   /* waitpid(2), */
#include <string.h>     /* memcpy(3), */
#include <stdint.h>     /* uint*_t, */
#include <sys/uio.h>    /* process_vm_*, struct iovec, */
#include <unistd.h>     /* sysconf(3), */
#include <sys/mman.h>

int main(int argc, char* argv[])
{       pid_t pid = fork();
        if(pid < 0)
        {
                perror("fork error");
                exit(-1);
        }
        else if (pid == 0)
        {
                static char *newargv[] = { NULL, "hello", "world", NULL };
                static char *newenviron[] = { "test env", "hello new env", NULL };
                long ret_val = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                if(ret_val <0 )
                {
                        perror("ptrace");
                        exit(-1);
                }
                kill(getpid(),SIGSTOP); //sig self to get traced
                for(int i=0;i<10;i++)
                {
                    int x = getpid();
                    if(x<0) 
                    {
                        printf("yes!\n");
                    }
                }
                newargv[0] = "argv0_progarm_name_test";
                execve("./test", newargv, newenviron);
                exit(0);
        }
        else 
        {
            while(1)
            {
                    int status;
                    pid_t wait_pid = waitpid(-1, &status, 0);
                    if(wait_pid < 0)
                    {
                            perror("waitpid");
                            exit(-1);
                    }
                    if(WIFEXITED(status))
                    {
                            printf("child exit success!\n");
                            exit(0);
                    }
                    else if (WIFSTOPPED(status))
                    {
                            struct user_regs_struct _regs;
                            struct iovec regs;
                            regs.iov_base = &_regs;
                            regs.iov_len  = sizeof(_regs);
                            long ret_val=ptrace(PTRACE_GETREGSET,wait_pid,NT_PRSTATUS,&regs);
                            if(ret_val < 0)
                            {
                                    perror("ptrace can't get arguments");
                                    kill(wait_pid, SIGKILL);
                                    exit(-1);
                            }
                            if(_regs.a7 == 172) //getpid syscall
                            {
                                    _regs.a7 = 0;
                                    ret_val = ptrace(PTRACE_SETREGSET,wait_pid,NT_PRSTATUS,&regs);
                                    if(ret_val <0 )
                                    {
                                            perror("ptrace can't set arguments");
                                            kill(wait_pid, SIGKILL);
                                            exit(-1);
                                    }
                            }
                            else if (_regs.a7 == 221)  //execve syscall
                            {
                                    static int is_first = 1;
                                    if(is_first)
                                    {
                                            is_first = !is_first;
                                            _regs.a0 = 0;
                                            //_regs.a1 = 0;
                                            _regs.a2 = 0;
                                            //_regs.a3 = 0;
                                            //_regs.a4 = 0;
                                            //_regs.a5 = 0;
                                            //_regs.a6 = 0;
                                            ret_val = ptrace(PTRACE_SETREGSET,wait_pid,NT_PRSTATUS,&regs);
                                            if(ret_val <0 )
                                            {
                                                    perror("ptrace can't set arguments");
                                                    kill(wait_pid, SIGKILL);
                                                    exit(-1);
                                            }
                                    }
                            }
                            ptrace(PTRACE_SYSCALL, wait_pid, 0, 0);
                    }
                    else
                    {
                            printf("child exit excption!\n");
                            exit(-1);
                    }
            }
        }
        return 0;
}
```

---

```c
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>


int main(int argc, char* argv[], char* envp[])
{
        printf("\n================================\n");
        for(int i=0;i<argc;i++)
        {
                printf("%s ",argv[i]);
        }
        printf("\n================================\n");
        char** ptr = envp;
        printf("\n==================envp==========\n");
        for(;(*ptr) != 0;)
        {
                printf("%s ",*ptr);
                ptr++;
        }
        printf("\n================================\n");

        return 0;
}
```



































































