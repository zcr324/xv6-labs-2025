#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define NPORTS 64
#define NPACKETS 16

struct netpacket {
  char *buf;
  int len;
  int payload_offset;
  uint32 src;
  uint16 sport;
};

struct portqueue {
  int used;
  uint16 port;
  int head;
  int count;
  struct netpacket packets[NPACKETS];
};

static struct portqueue portqueues[NPORTS];

// netlock must be held.
static struct portqueue *
findqueue(uint16 port)
{
  for(int i = 0; i < NPORTS; i++)
    if(portqueues[i].used && portqueues[i].port == port)
      return &portqueues[i];
  return 0;
}

void
netinit(void)
{
  initlock(&netlock, "netlock");
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);
  if(port < 0 || port > 0xffff)
    return -1;

  acquire(&netlock);
  if(findqueue((uint16)port) != 0){
    release(&netlock);
    return -1;
  }
  for(int i = 0; i < NPORTS; i++){
    if(!portqueues[i].used){
      portqueues[i].used = 1;
      portqueues[i].port = port;
      portqueues[i].head = 0;
      portqueues[i].count = 0;
      release(&netlock);
      return 0;
    }
  }
  release(&netlock);

  return -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  int port;
  argint(0, &port);
  if(port < 0 || port > 0xffff)
    return -1;

  acquire(&netlock);
  struct portqueue *q = findqueue((uint16)port);
  if(q == 0){
    release(&netlock);
    return -1;
  }
  while(q->count > 0){
    struct netpacket *pkt = &q->packets[q->head];
    kfree(pkt->buf);
    q->head = (q->head + 1) % NPACKETS;
    q->count--;
  }
  q->used = 0;
  wakeup(q);
  release(&netlock);
  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  struct proc *p = myproc();
  int dport, maxlen;
  uint64 srcaddr, sportaddr, bufaddr;
  argint(0, &dport);
  argaddr(1, &srcaddr);
  argaddr(2, &sportaddr);
  argaddr(3, &bufaddr);
  argint(4, &maxlen);
  if(dport < 0 || dport > 0xffff || maxlen < 0)
    return -1;

  acquire(&netlock);
  struct portqueue *q = findqueue((uint16)dport);
  if(q == 0){
    release(&netlock);
    return -1;
  }
  while(q->count == 0){
    if(killed(p) || !q->used || q->port != (uint16)dport){
      release(&netlock);
      return -1;
    }
    sleep(q, &netlock);
  }

  struct netpacket pkt = q->packets[q->head];
  q->head = (q->head + 1) % NPACKETS;
  q->count--;
  release(&netlock);

  int n = pkt.len;
  if(n > maxlen)
    n = maxlen;
  int error = 0;
  if(copyout(p->pagetable, srcaddr, (char *)&pkt.src, sizeof(pkt.src)) < 0 ||
     copyout(p->pagetable, sportaddr, (char *)&pkt.sport, sizeof(pkt.sport)) < 0 ||
     copyout(p->pagetable, bufaddr, pkt.buf + pkt.payload_offset, n) < 0)
    error = -1;
  kfree(pkt.buf);
  return error < 0 ? -1 : n;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  if(len < 0)
    return -1;
  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  if(e1000_transmit(buf, total) < 0){
    kfree(buf);
    return -1;
  }

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  if(len < sizeof(struct eth) + sizeof(struct ip)){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);
  int ip_header_len = (ip->ip_vhl & 0x0f) * 4;
  if((ip->ip_vhl >> 4) != 4 || ip_header_len < sizeof(struct ip) ||
     ip->ip_p != IPPROTO_UDP || ntohl(ip->ip_dst) != local_ip ||
     len < sizeof(struct eth) + ip_header_len + sizeof(struct udp)){
    kfree(buf);
    return;
  }

  int ip_len = ntohs(ip->ip_len);
  if(ip_len < ip_header_len + sizeof(struct udp) ||
     ip_len > len - sizeof(struct eth)){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)((char *)ip + ip_header_len);
  int udp_len = ntohs(udp->ulen);
  if(udp_len < sizeof(struct udp) ||
     udp_len > ip_len - ip_header_len){
    kfree(buf);
    return;
  }

  uint16 dport = ntohs(udp->dport);
  acquire(&netlock);
  struct portqueue *q = findqueue(dport);
  if(q == 0 || q->count == NPACKETS){
    release(&netlock);
    kfree(buf);
    return;
  }

  int tail = (q->head + q->count) % NPACKETS;
  q->packets[tail].buf = buf;
  q->packets[tail].len = udp_len - sizeof(struct udp);
  q->packets[tail].payload_offset =
    sizeof(struct eth) + ip_header_len + sizeof(struct udp);
  q->packets[tail].src = ntohl(ip->ip_src);
  q->packets[tail].sport = ntohs(udp->sport);
  q->count++;
  wakeup(q);
  release(&netlock);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  if(e1000_transmit(buf, sizeof(*eth) + sizeof(*arp)) < 0)
    kfree(buf);

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
