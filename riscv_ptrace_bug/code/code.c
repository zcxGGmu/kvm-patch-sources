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