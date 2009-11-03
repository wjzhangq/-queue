#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <malloc/malloc.h>
#include <fcntl.h>
#include <pthread.h>

#define MAX_IFNUM 5
#define MAX_MSG_LENGTH 1024

#define ON_OFF(flag) ((flag) ? "on":"off")

struct _settings {
  int port; /* UDP port to listen on */
  int daemon; /* daemon mode */
  char* ipaddr; /* address to bind */
  int localonly;  /* only allow local msg */
  int workernum;  /* max num of workers */
  int queuesize;  /* max ids in queue */
  char* exepath; /* script path */
  int verbose;
};

char* ipaddrs[MAX_IFNUM];
int ifnum = 0;
struct _settings settings;
volatile int* id_queue;
volatile int scan_index = 0;
volatile int concurrent_workers = 0;
/* msg thread (main) and flush thread (scan_worker) may change id list */
pthread_mutex_t lock;
pthread_cond_t cond;
pthread_mutex_t workerlock;
pthread_cond_t workercond;
pthread_mutex_t reaperlock;
pthread_cond_t reapercond;

extern int errno;
extern char *optarg;

int getaddrs(char **);
int is_local_ip(const char*);
int daemonize();
int start_worker(int);
void* scan_worker(void*);
void* reaper_worker(void*);
void handle_msg(char*, int, const char*);
void settings_init(void);
void showusage(void);
int close_stdio(int);

int main(int argc, char **argv)
{
  int c;
  int     sockfd;
  struct sockaddr_in servaddr, cliaddr;
  int n;
  socklen_t len;
  char msg[MAX_MSG_LENGTH];
  int i;

  settings_init();

  /* process arguments */
  while (-1 != (c = getopt(argc, argv, "hp:dl:Lc:n:e:v"))) {
    switch (c) {
    case 'h':
      showusage();
      exit(EXIT_SUCCESS);
      break;
    case 'p':
      settings.port = atoi(optarg);
      break;
    case 'd':
      settings.daemon = 1;
      break;
    case 'l':
      settings.ipaddr = strdup(optarg);
      break;
    case 'L':
      settings.localonly = 1;
      break;
    case 'c':
      settings.workernum = atoi(optarg);
      break;
    case 'n':
      settings.queuesize = atoi(optarg);
      break;
    case 'e':
      settings.exepath = strdup(optarg);
      break;
    case 'v':
      settings.verbose++;
      break;
    default:
      //fprintf(stderr, "unknown option -- '%c'\n", c);
      return 1;
    }
  }

  if (settings.port < 1) {
    fprintf(stderr, "please use '-p' to specify a port(>1) to listen to\n");
    exit(-1);
  }

  if (settings.workernum < 1) {
    fprintf(stderr, "please use '-c' to specify max num(>1) of concurrent workers\n");
    exit(-1);
  }

  if (settings.queuesize < 1) {
    fprintf(stderr, "please use '-n' to specify queue size(>1)\n");
    exit(-1);
  }

  
  if (settings.exepath == NULL || settings.exepath[0] == '\0') {
    fprintf(stderr, "empty 'script path', use '-e' to specify it\n");
    exit(-1);
  }
  if (access(settings.exepath, R_OK | X_OK) != 0) {
    fprintf(stderr, "script not readble or executable: %s\n", strerror(errno));
    exit(-1);
  }
  
  /* print setttngs */
  printf("settings:\n");
  printf("\tlisten port:  %d\n", settings.port);
  printf("\tdaemon mode:  %s\n", ON_OFF(settings.daemon));
  printf("\tlisten addr:  %s\n",
         (settings.ipaddr != NULL) ? settings.ipaddr : "INADDR_ANY");
  printf("\tlocal only:   %s\n", ON_OFF(settings.localonly));
  printf("\tworker num    %d\n", settings.workernum);
  printf("\tqueue size    %d\n", settings.queuesize);
  printf("\tscript path:  '%s'\n", settings.exepath);
  printf("\tverbose:      %s\n", ON_OFF(settings.verbose));

  /* allocate space for queue */
  id_queue = (int*)malloc(sizeof(int) * settings.queuesize);
  if (id_queue == NULL) {
    fprintf(stderr, "can not allocate memory for id queue\n");
    exit(-1);
  }
  
  for (i = 0; i < MAX_IFNUM; i++) {
    ipaddrs[i] = (char*)malloc(20);
    bzero(ipaddrs[i], 20);
  }
  ifnum = getaddrs(ipaddrs);
  if (ifnum <= 0) {
    fprintf(stderr, "can not get ip address of interface(s)\n");
    exit(EXIT_FAILURE);
  } else if (settings.verbose > 0) {
    printf("ip address of interface(s):\n");
    for (i = 0; i < ifnum; i++) {
      printf("\t%s\n", ipaddrs[i]);
    }
  }

  if (settings.daemon) {
    if (daemonize()) {
      fprintf(stderr, "can't run as daemon\n");
      exit(EXIT_FAILURE);
    }
    printf("run as daemon\n");
  }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket error: ");
    exit(EXIT_FAILURE);
  }

  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  if (settings.ipaddr != NULL) {
    if (inet_aton(settings.ipaddr, &(servaddr.sin_addr)) == 0) {
      fprintf(stderr, "invalid ip address to listen: %s\n", settings.ipaddr);
      exit(EXIT_FAILURE);
    }
  } else {
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  servaddr.sin_port = htons(settings.port);

  if (bind(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr))) {
    perror("bind error: ");
    exit(EXIT_FAILURE);
  }

  pthread_mutex_init(&lock, NULL);
  pthread_cond_init(&cond, NULL);
  pthread_mutex_init(&workerlock, NULL);
  pthread_cond_init(&workercond, NULL);
  pthread_mutex_init(&reaperlock, NULL);
  pthread_cond_init(&reapercond, NULL);
  
  /* start thread to reap children */
  do {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_create(&t, &attr, reaper_worker, NULL) != 0) {
      perror("pthread_create error: ");
      exit(EXIT_FAILURE);
    }
  } while (0);

  /* start thread to scan queue */
  do {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_create(&t, &attr, scan_worker, NULL) != 0) {
      perror("pthread_create error: ");
      exit(EXIT_FAILURE);
    }
  } while (0);

  /* close stdio if verbose == 0*/
  if (close_stdio(settings.verbose)) {
    fprintf(stderr, "can't close fd: 0, 1, 2\n");
    exit(EXIT_FAILURE);
  }

  len = sizeof(cliaddr);
  while (1) {
    n = recvfrom(sockfd, msg, MAX_MSG_LENGTH, 0, (struct sockaddr *)&cliaddr, &len);
    if (n < 1) {
      if (settings.verbose > 0) {
        fprintf(stderr, "recvfrom error\n");
      }
      continue;
    }
    if (settings.localonly) {
      char *from = inet_ntoa(cliaddr.sin_addr);
      if (is_local_ip(from)) {
        if (settings.verbose > 0) {
          fprintf(stderr, "deny msg from %s\n", from);
        }
        continue;        
      }
    }
    msg[n] = '\0';
    if (settings.verbose > 0) {
      handle_msg(msg, n, inet_ntoa(cliaddr.sin_addr));
    } else {
      handle_msg(msg, n, NULL);
    }
    /*
    sendto(sockfd, msg, n, 0, (struct sockaddr *)&cliaddr, len);
    */
  }

  return 0;
}

void settings_init(void)
{
  settings.port = 0;
  settings.daemon = 0;
  settings.ipaddr = NULL;
  settings.localonly = 0;
  settings.workernum = 0;
  settings.queuesize = 0;
  settings.exepath = NULL;
  settings.verbose = 0;
}

void* reaper_worker(void *args)
{
  int status;
  
  while (1) {
    if (settings.verbose > 1) {
      printf("[reaper] wait\n");
    }

    if (wait(&status) == -1) {
      if (settings.verbose > 0) {
        fprintf(stderr, "[reaper] cond_wait as : %s\n", strerror(errno));
      }
      pthread_mutex_lock(&reaperlock);
      pthread_cond_wait(&reapercond, &reaperlock);
      pthread_mutex_unlock(&reaperlock);
      if (settings.verbose > 0) {
        fprintf(stderr, "[reaper] cond_wait_done\n");
      }
    } else {
      if (settings.verbose > 1) {
        printf("[reaper] get a child\n");
      }
      pthread_mutex_lock(&workerlock);
      concurrent_workers--;
      if (concurrent_workers < 0)
        concurrent_workers = 0;
      pthread_cond_signal(&workercond);
      pthread_mutex_unlock(&workerlock);
    }
  }
  return NULL;
}

void* scan_worker(void *args)
{
  int current_id;

  while (1) {
    current_id = 0;
    pthread_mutex_lock(&lock);
    if (id_queue[scan_index] <= 0) {
      /* queue is empty */
      if (settings.verbose > 0) {
        printf("[scan_worker] cond_wait id\n");
      }
      pthread_cond_wait(&cond, &lock);
      if (settings.verbose > 0) {
        printf("[scan_worker] cond_wait_done id\n");
      }
    }
    if (id_queue[scan_index] > 0) {
      current_id = id_queue[scan_index];
      id_queue[scan_index] = 0;
      scan_index++;
      if (scan_index >= settings.queuesize)
        scan_index = 0;
    }
    pthread_mutex_unlock(&lock);
    
    /* now check current id */
    if (current_id >0) {
      pthread_mutex_lock(&workerlock);
      if (concurrent_workers >= settings.workernum) {
        if (settings.verbose > 0) {
          printf("[scan_worker] cond_wait worker\n");
        }
        pthread_cond_wait(&workercond, &workerlock);
        if (settings.verbose > 0) {
          printf("[scan_worker] cond_wait_done worker\n");
        }
      }
      
      if (concurrent_workers < settings.workernum) {
        concurrent_workers++;
        if (settings.verbose > 1) {
          printf("[scan_worker] start a worker (%d), num %d max%d\n", current_id,
                 concurrent_workers, settings.workernum);
        }
      } else {
        current_id = 0;
      }
      pthread_mutex_unlock(&workerlock);
      if (current_id > 0)
        start_worker(current_id);
      
      current_id = 0;
    }
  }
  return NULL;
}

int start_worker(int id)
{
  pid_t pid;
  if ((pid = fork()) < 0) {
    return -1;
  } else if (pid > 0) {
    /* notify reaper */
    pthread_mutex_lock(&reaperlock);
    if (settings.verbose > 1)
      printf("[start_worker] cond_signal\n");
    pthread_cond_signal(&reapercond);
    pthread_mutex_unlock(&reaperlock);
    return 0;
  } else {
    /* worker */
    char* args[3];
    char idstr[20];
    snprintf(idstr, 20, "%d", id);
    idstr[19] = '\0';
    args[0] = settings.exepath;
    args[1] = idstr;
    args[2] = NULL;
	if (settings.verbose > 1){
		printf("id:%s is start runing!\n", idstr);
	}
    execv(settings.exepath, args);
	if (settings.verbose > 1){
		printf("id:%s is end running!\n", idstr);
	}
    exit(-1);
  }
  
  return 0;
}

/* handle received message */
void handle_msg(char *msg, int length, const char *from)
{
  static int current_index = 0;
  int recv_id;
  int i;

  if (msg == NULL || length < 1)
    return;
  
  if (settings.verbose > 1) {
    printf("[msg] recv from %s: ", from);
    fwrite(msg, length, 1, stdout);
    printf("\n");
  }

  /* only allow positive id */
  for (i = 0; i < length; i++) {
    if (msg[i] < '0' || msg[i] > '9') {
      return;
    }
  }

  recv_id = atoi(msg);
  if (recv_id < 1)
    return;
  
  pthread_mutex_lock(&lock);
  /* override old id */
  if (id_queue[current_index] >= 1) {
    /* update scan_index so we can process id in original sequence */
    scan_index = current_index + 1;
    if (scan_index >= settings.queuesize)
      scan_index = 0;
  }
  id_queue[current_index] = recv_id;
  if (settings.verbose > 1) {
    printf("[msg_handler] add id [%d] = %d\n", current_index, recv_id);
  }
  current_index++;
  if (current_index >= settings.queuesize)
    current_index = 0;
  if (settings.verbose > 1)
    printf("[msg_handler] cond signal\n");
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
}

/* show usage */
void showusage(void)
{
  printf("%s",
         "-h           print this help and exit\n"
         "-p <num>     UDP port number to linsten on\n"
         "-d           run as daemon\n"
         "-l <ip_addr> interface to listen on (default: INADDR_ANY)\n"
         "-L           only allow message from local address\n"
         "-c <num>     max concurrent worker number (>1)\n"
         "-n <num>     max queue size (>1)\n"
         "-e <exepath> full path of script that handle received id\n"
         "-v           verbose\n"
         );
}

/** get ip addresses of all interfaces */
int getaddrs(char **addrvec)
{
  int fd;
  struct ifreq buf[MAX_IFNUM];
  struct ifconf ifc;
  int interfacenum;
  int i, m;

  m = 0;

  //  for (i = 0; i < MAX_IFNUM; i++)
  //    memset(addrvec[i], 0, 16); /* xxx.xxx.xxx.xxx */
  
  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
    ifc.ifc_len = sizeof buf;
    ifc.ifc_buf = (caddr_t) buf;
    if (!ioctl(fd, SIOCGIFCONF, (char *) &ifc)) {
      interfacenum = ifc.ifc_len / sizeof(struct ifreq);

      for (i = (interfacenum - 1); i >= 0; i--) {
/*         fprintf(stderr, "%s ", buf[i].ifr_name); */
        /* skip loop interface
        ioctl(fd, SIOCGIFFLAGS, (char *) &buf[i]);
        if (buf[i].ifr_flags & IFF_LOOPBACK) {
          interfacenum--;
          continue;
        }
        */
        ioctl(fd, SIOCGIFADDR, (char *)&buf[i]);
        strcpy(addrvec[m], 
               inet_ntoa(((struct sockaddr_in *)&buf[i].ifr_addr)->sin_addr));

        /*Get HW ADDRESS of the net card */
        /*
        ioctl(fd, SIOCGIFHWADDR, (char *) &buf[i]);
        for (j = 0; j < 6; j++) {
          addrvec[m][2+j] = (unsigned char)buf[i].ifr_hwaddr.sa_data[j];
          fprintf(stderr,"%02x", (unsigned char)buf[i].ifr_hwaddr.sa_data[j]);
        }
        fprintf(stderr, "\n");
        */
        m++;
      }

    } else {
      interfacenum = -1;
    }
  } else {
    interfacenum = -2;
  }
  close(fd);
  if (interfacenum >=0)
    interfacenum = m;
  return interfacenum;
}

/**
 * test if given ip address match a local interface
 * \return 0 ok, non-zero fail
 */
int is_local_ip(const char* ip)
{
  int i;
  for (i = 0; i < ifnum; i++) {
    if (strcmp(ip, ipaddrs[i]) == 0)
      return 0;
  }
  return -1;
}

/**
 * run as daemon
 * \return 0 on success
 */
int daemonize(void)
{
  switch (fork()) {
  case -1:
    return (-1);
  case 0:
    break;
  default:
    _exit(EXIT_SUCCESS);
  }

  if (setsid() == -1)
    return (-1);

  /*
  if (nochdir == 0) {
    if(chdir("/") != 0) {
      perror("chdir");
      return (-1);
    }
  }
  */
  return 0;
}

int close_stdio(int noclose) {
  int fd;

  if (noclose == 0 && (fd = open("/dev/null", O_RDWR, 0)) != -1) {
    if(dup2(fd, STDIN_FILENO) < 0) {
      perror("dup2 stdin");
      return (-1);
    }
    if(dup2(fd, STDOUT_FILENO) < 0) {
      perror("dup2 stdout");
      return (-1);
    }
    if(dup2(fd, STDERR_FILENO) < 0) {
      perror("dup2 stderr");
      return (-1);
    }

    if (fd > STDERR_FILENO) {
      if(close(fd) < 0) {
        perror("close");
        return (-1);
      }
    }
  }
  return (0);

}
