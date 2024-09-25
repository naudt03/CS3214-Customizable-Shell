#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int
main(int ac, char *av[])
{
    char *s = ac > 1 ? av[1] : "y";
    long b = strlen(s);
    long hm = 2L*1024*1024*1024/(b+1); 
    while (hm-- > 0) {
        if (fwrite(s, b, 1, stdout) != 1)
            break;
        if (fwrite("\n", 1, 1, stdout) != 1)
            break;
    }
    fflush(stdout);
}
