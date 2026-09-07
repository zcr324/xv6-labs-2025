#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
select(char *str)
{
  int n;

  n = atoi(str);

  if(n % 5 == 0 || n % 6 == 0)
    return n;

  return -1;
}

void
sixfive(int fd)
{
  char c;
  char num[32];
  int len = 0;
  int valid = 1;
  int sf;

  while(read(fd, &c, 1) == 1){

    // 当前字符是数字
    if('0' <= c && c <= '9'){
      if(valid){
        if(len < sizeof(num) - 1){
          num[len] = c;
          len++;
        }
      }
    }

    // 当前字符是合法分隔符
    else if(strchr(" -\r\t\n./,", c)){
      if(valid && len > 0){
        num[len] = '\0';

        sf = select(num);

        if(sf != -1)
          printf("%d\n", sf);
      }

      // 分隔符之后可以开始新的合法数字
      valid = 1;
      len = 0;
    }

    // 既不是数字，也不是合法分隔符
    else{
      valid = 0;
      len = 0;
    }
  }

  // EOF 也被视为一个隐式分隔符
  if(valid && len > 0){
    num[len] = '\0';

    sf = select(num);

    if(sf != -1)
      printf("%d\n", sf);
  }
}

int
main(int argc, char *argv[])
{
  int fd;
  int i;

  if(argc < 2){
    fprintf(2, "usage: sixfive file...\n");
    exit(1);
  }

  for(i = 1; i < argc; i++){
    fd = open(argv[i], O_RDONLY);

    if(fd < 0){
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      continue;
    }

    sixfive(fd);
    close(fd);
  }

  exit(0);
}