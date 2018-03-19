
/* Usermode application */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>

#define FORK_TEST_ITERS (2 * 250 * 1024 * 1024)

void fork_test(void)
{
   printf("Running infinite loop..\n");

   unsigned n = 1;
   int FORK_TEST_ITERS_hits_count = 0;
   bool inchild = false;
   bool exit_on_next_FORK_TEST_ITERS_hit = false;

   while (true) {

      if (!(n % FORK_TEST_ITERS)) {

         printf("[PID: %i] FORK_TEST_ITERS hit!\n", getpid());

         if (exit_on_next_FORK_TEST_ITERS_hit) {
            return;
         }

         FORK_TEST_ITERS_hits_count++;

         if (FORK_TEST_ITERS_hits_count == 1) {

            printf("forking..\n");

            int pid = fork();

            printf("Fork returned %i\n", pid);

            if (pid == 0) {
               printf("############## I'm the child!\n");
               inchild = true;
            } else {
               printf("############## I'm the parent, child's pid = %i\n", pid);
               printf("[parent] waiting the child to exit...\n");
               int wstatus=0;
               int p = waitpid(pid, &wstatus, 0);
               printf("[parent] child (pid: %i) exited with status: %i!\n", p, WEXITSTATUS(wstatus));
               exit_on_next_FORK_TEST_ITERS_hit = true;
            }

         }

         if (FORK_TEST_ITERS_hits_count == 2 && inchild) {
            printf("child: 2 iter hits, exit!\n");
            exit(123);
         }
      }

      n++;
   }
}

int bss_variable[32];

void bss_var_test(void)
{
   for (int i = 0; i < 32; i++) {
      if (bss_variable[i] != 0) {
         printf("%s: FAIL\n", __FUNCTION__);
         exit(1);
      }
   }

   printf("%s: OK\n", __FUNCTION__);
}

void args_test(int argc, char ** argv)
{
   printf("argc: %i\n", argc);

   for (int i = 0; i < argc; i++) {
      printf("argv[%i] = '%s'\n", i, argv[i]);
   }

   printf("env[OSTYPE] = '%s'\n", getenv("OSTYPE"));

   if (strcmp(argv[0], "init")) {
      printf("%s: FAIL\n", __FUNCTION__);
      exit(1);
   }

   if (strcmp(getenv("OSTYPE"), "linux-gnu")) {
      printf("%s: FAIL\n", __FUNCTION__);
      exit(1);
   }
}

void file_read_test(void)
{
   char buf[256];
   int fd;

   if (getenv("EXOS")) {
      fd = open("/EFI/BOOT/elf_kernel_stripped", O_RDONLY);
   } else {
      fd = open("build/sysroot/EFI/BOOT/elf_kernel_stripped", O_RDONLY);
   }

   printf("open() = %i\n", fd);

   if (fd < 0) {
      perror("Open failed");
      exit(1);
   }

   int r = read(fd, buf, 256);
   printf("user: read() returned %i\n", r);

   for (int i = 0; i < 16; i++) {
      printf("0x%02x ", (unsigned)buf[i]);
   }

   printf("\n");
   close(fd);
}

void test_read_stdin(void)
{

   char buf[256];

   while (1) {
      printf("Enter a string: ");
      fflush(stdout);

      int ret = read(0, buf, 256);
      buf[ret] = 0;

      if (!strcmp(buf, "stop\n"))
         break;
      for (int i = 0; i < ret; i++)
         buf[i] = toupper(buf[i]);
      printf("upper is: %s", buf);
   }

   printf("-- end --\n");

   // int n;
   // printf("Tell me a number: ");
   // fflush(stdout);
   // fscanf(stdin, "%i", &n);
   // printf("OK, %i * %i = %i\n", n, n, n * n);
}

int main(int argc, char **argv, char **env)
{
   if (getenv("EXOS")) {
      int in_fd = open("/dev/tty", O_RDONLY);
      int out_fd = open("/dev/tty", O_WRONLY);
      int err_fd = open("/dev/tty", O_WRONLY);
      //printf("in: %i, out: %i, err: %i\n", in_fd, out_fd, err_fd);
      (void)in_fd; (void)out_fd; (void)err_fd;
   }

   const char *hello_msg = "[direct write] Hello from init!\n";
   write(1, hello_msg, strlen(hello_msg));
   printf("MY PID IS %i\n", getpid());

   args_test(argc, argv);
   //file_read_test();
   //test_read_stdin();

   int shell_pid = fork();

   if (!shell_pid) {
      printf("[init forked child] running shell\n");
      execve("/bin/shell", NULL, NULL);
   }

   printf("[init] wait for the shell to exit\n");
   int wstatus;
   waitpid(shell_pid, &wstatus, 0);
   printf("[init] the shell exited with status: %d\n", WEXITSTATUS(wstatus));

   //bss_var_test();
   //fork_test();

   return 0;
}

