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