//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

// find the mmap'd region containing virtual address va.
static struct vma*
findvma(struct proc *p, uint64 va)
{
  struct vma *v;
  int i;

  for(i = 0; i < NVMA; i++){
    v = &p->vma[i];
    if(v->used && va >= v->addr && va < v->addr + v->len)
      return v;
  }
  return 0;
}

// handle a page fault in an mmap'd region: allocate a page of physical
// memory, read the corresponding bytes of the mapped file into it, and
// map it into the process's page table.  Returns the physical address
// of the page, or 0 if va is not in an mmap'd region (or if something
// went wrong).
uint64
vmafault(struct proc *p, uint64 va, int write)
{
  struct vma *v;
  char *mem;
  int perm;

  if((v = findvma(p, va)) == 0)
    return 0;

  // a store to a read-only mapping is fatal.
  if(write && (v->prot & PROT_WRITE) == 0){
    setkilled(p);
    return 0;
  }

  va = PGROUNDDOWN(va);
  if(ismapped(p->pagetable, va))
    return 0;

  if((mem = kalloc()) == 0)
    return 0;
  memset(mem, 0, PGSIZE);

  if(v->f->type == FD_INODE){
    // readi() returns fewer bytes than requested past end of file,
    // leaving the rest of the page zero, as mmap should.
    ilock(v->f->ip);
    readi(v->f->ip, 0, (uint64)mem, v->off + (va - v->addr), PGSIZE);
    iunlock(v->f->ip);
  }

  perm = PTE_U | PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return 0;
  }
  return (uint64)mem;
}

// write back modifications to the file for any allocated pages in
// vma v's range [addr, addr+len), if mapped MAP_SHARED, and unmap them.
// len must be a multiple of PGSIZE and addr page-aligned, as is the
// case in all of mmaptest.
static void
unmapvma(struct vma *v, pagetable_t pagetable, uint64 addr, uint64 len)
{
  uint64 a;
  pte_t *pte;
  int off, n;

  if(v->flags & MAP_SHARED){
    for(a = PGROUNDDOWN(addr); a < addr + len; a += PGSIZE){
      if((pte = walk(pagetable, a, 0)) == 0 || (*pte & PTE_V) == 0)
        continue;   // never faulted in; nothing to write back
      off = v->off + (a - v->addr);
      begin_op();
      ilock(v->f->ip);
      // don't write past the end of the file, or the file would
      // be extended by bytes beyond the mapped data.
      n = v->f->ip->size - off;
      if(n > PGSIZE)
        n = PGSIZE;
      if(n > 0)
        writei(v->f->ip, 0, PTE2PA(*pte), off, n);
      iunlock(v->f->ip);
      end_op();
    }
  }
  uvmunmap(pagetable, PGROUNDDOWN(addr), PGROUNDUP(len)/PGSIZE, 1);
}

// unmap all of the current process's mmap'd regions, as if munmap()
// had been called on each one.  Used on exit and exec.
void
munmapall(pagetable_t pagetable)
{
  struct proc *p = myproc();
  int i;

  for(i = 0; i < NVMA; i++){
    struct vma *v = &p->vma[i];
    if(v->used){
      unmapvma(v, pagetable, v->addr, v->len);
      fileclose(v->f);
      v->used = 0;
    }
  }
}

uint64
sys_mmap(void)
{
  uint64 addr, va;
  int len, prot, flags, offset;
  struct file *f;
  struct proc *p = myproc();
  struct vma *v;
  int i;

  argaddr(0, &addr);
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(5, &offset);
  if(argfd(4, 0, &f) < 0)
    return -1;

  if(addr != 0 || len <= 0 || offset != 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;
  // a shared writable mapping requires a file opened for writing.
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && f->writable == 0)
    return -1;

  for(i = 0; i < NVMA; i++)
    if(!p->vma[i].used)
      break;
  if(i == NVMA)
    return -1;
  v = &p->vma[i];

  // map the file just above the process's heap and all of its
  // other mmap'd regions, so that regions never overlap.
  va = PGROUNDUP(p->sz);
  for(i = 0; i < NVMA; i++)
    if(p->vma[i].used && p->vma[i].addr + p->vma[i].len > va)
      va = PGROUNDUP(p->vma[i].addr + p->vma[i].len);

  v->used = 1;
  v->addr = va;
  v->len = len;
  v->prot = prot;
  v->flags = flags;
  v->off = offset;
  v->f = filedup(f);

  return va;
}

uint64
sys_munmap(void)
{
  uint64 addr, end;
  int len;
  struct proc *p = myproc();
  struct vma *v;
  int n;

  argaddr(0, &addr);
  argint(1, &len);
  if(len <= 0)
    return -1;

  if((v = findvma(p, addr)) == 0)
    return -1;

  // unmap [addr, addr+len), writing back MAP_SHARED modifications.
  // the lab assumes munmap removes pages at the start or end of the
  // region, or the whole region, so no hole forms in the middle.
  end = addr + len;
  if(end > v->addr + v->len)
    end = v->addr + v->len;
  unmapvma(v, p->pagetable, addr, end - addr);

  if(addr <= v->addr && end >= v->addr + v->len){
    // whole region unmapped
    fileclose(v->f);
    v->used = 0;
  } else if(addr <= v->addr){
    // removed a chunk at the start of the region
    n = end - v->addr;
    v->addr = end;
    v->len -= n;
    v->off += n;
  } else {
    // removed a chunk at the end of the region
    v->len = addr - v->addr;
  }
  return 0;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
