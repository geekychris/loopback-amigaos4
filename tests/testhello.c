#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    IDOS->Printf("hello from testhello\n");
    return 0;
}
