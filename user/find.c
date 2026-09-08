#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char*
filename(char *path)
{
  char *p;

  for(p = path + strlen(path); p >= path && *p != '/'; p--)
    ;

  return p + 1;
}

void
runexec(char **cmd, char *path)
{
  char *args[MAXARG];
  int i;

  for(i = 0; cmd[i] != 0 && i < MAXARG - 2; i++)
    args[i] = cmd[i];

  args[i++] = path;
  args[i] = 0;

  if(fork() == 0){
    exec(args[0], args);

    // 只有 exec 失败才会执行到这里
    fprintf(2, "find: exec %s failed\n", args[0]);
    exit(1);
  }

  wait(0);
}

void
find(char *path, char *target, char **cmd)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){

  case T_DEVICE:
  case T_FILE:
    if(strcmp(filename(path), target) == 0){
      if(cmd == 0){
        printf("%s\n", path);
      } else {
        runexec(cmd, path);
      }
    }
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
      fprintf(2, "find: path too long\n");
      break;
    }

    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;

      char name[DIRSIZ + 1];

      memmove(name, de.name, DIRSIZ);
      name[DIRSIZ] = '\0';

      if(strcmp(name, ".") == 0 ||
         strcmp(name, "..") == 0)
        continue;

      strcpy(p, name);

      find(buf, target, cmd);
    }

    break;
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "usage: find path filename [-exec cmd ...]\n");
    exit(1);
  }

  // 普通 find
  if(argc == 3){
    find(argv[1], argv[2], 0);
    exit(0);
  }

  // find ... -exec ...
  if(argc >= 5 && strcmp(argv[3], "-exec") == 0){
    find(argv[1], argv[2], &argv[4]);
    exit(0);
  }

  fprintf(2, "usage: find path filename [-exec cmd ...]\n");
  exit(1);
}