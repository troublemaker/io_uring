#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>


//net
#define LISTEN_PORT 7777
#define LISTEN_BACKLOG 10000

//net app
#define MAX_EVENTS 10000
#define CLIENT_MESSAGE_SIZE 1024


typedef struct {
   unsigned listener_socket;
   unsigned thread_num;
}
thread_params;




void* launch_epoll(void *arg) {

    int sock_listen = ((int*)arg)[0];
    int thread_num = ((int*)arg)[1]; 

    cpu_set_t cpuset;
    pthread_t thread;

    thread = pthread_self();
    CPU_ZERO(&cpuset);
    CPU_SET(thread_num, &cpuset);

    int res = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (res != 0) {
       perror("pthread_setaffinity_np failed. \n");
       return NULL;
    }

    struct sockaddr_in cli_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    memset(&cli_addr, 0, addr_len);
    
    char buffer[CLIENT_MESSAGE_SIZE];
    memset(buffer, 0, sizeof(buffer));


    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];
    int epollfd;
    
    epollfd = epoll_create(1);
    if (epollfd < 0) {
        perror("Creating epoll FD failed \n");
    }

    ev.events = EPOLLIN;
    ev.data.fd = sock_listen;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock_listen, &ev) == -1) {
        perror("Adding listening socket failed \n");
    }

    int total_events;
    int new_sock;
    int bytes_read;

    // main io loop
    while (1)
    {
        total_events = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        
        if (total_events == -1) {
            perror("epoll_wait failed \n");
        }

        for (int i = 0; i < total_events; ++i) {
            if (events[i].data.fd == sock_listen) {
                //printf("ACCEPT SOCKET# %i in thread# %i \n", res, thread_num);
                //fflush(stdout);

                new_sock = accept4(sock_listen, (struct sockaddr *)&cli_addr, &addr_len, SOCK_NONBLOCK);
                
                if (new_sock > 0) {
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = new_sock;
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, new_sock, &ev) == -1){
                        perror("epoll_ctl ADD failed \n");
                    }
                }


            }
            else {
                new_sock = events[i].data.fd;
                bytes_read = recv(new_sock, buffer, CLIENT_MESSAGE_SIZE, 0);
                if (bytes_read <= 0){
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, new_sock, NULL);
                    shutdown(new_sock, SHUT_RDWR);
                }
                else{
                    send(new_sock, buffer, bytes_read, 0);
                }
            }
        }
    }
}




int main(int argc, char* argv[])
{
    // parse params
    int opt; 
    long threads = 0;

    while((opt = getopt(argc, argv, "t:h")) != -1)  
    {  
        switch(opt)  
        {  
            case 't':  
                threads = strtol(optarg, NULL, 10); 
                if (threads < 1 || threads > 64) {
                   printf("Threads value must be > 0 and < 64 \n");
                   return 1;
                }
                break;  
            case 'h':  
                printf("usage -t: number of threads. defaults to # of CPUs in the system \n"); 
                return 0;  
        }  
    }


    long total_cpu = sysconf(_SC_NPROCESSORS_ONLN);

    printf("IO_URING test echo server. \n");
    printf("Number of CPUs: %li \n", total_cpu);

    if (threads == 0) {
       threads = total_cpu;
    } 
 
    printf("Launching with %li threads. \n", threads);


    struct sockaddr_in srv_addr;
    int sock_listen;
    int reuse_val = 1;

    memset(&srv_addr, 0, sizeof(srv_addr));


    // create main listening socket
    sock_listen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &reuse_val, sizeof(reuse_val));

    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(LISTEN_PORT);
    srv_addr.sin_addr.s_addr = INADDR_ANY;


    // bind main listening socket
    if (bind(sock_listen, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("binding socket failed \n");
        return 1;
    }

    // start listening
    if (listen(sock_listen, LISTEN_BACKLOG) < 0) {
        perror("listening failed\n");
        return 1;
    }


    //launch IO threads
    thread_params* tp_arr;
    pthread_t* t_ids; 

    tp_arr = malloc(sizeof(thread_params) * threads);
    t_ids = malloc(sizeof(pthread_t) * threads);    

    for (int i=0; i<threads; i++) {
       tp_arr[i].listener_socket = sock_listen;
       tp_arr[i].thread_num = i;
       pthread_create(&t_ids[i], NULL, &launch_epoll, (void*)&tp_arr[i]);
    }    

    printf("server running...\n");

    int* ret;
    for (int i=0; i<threads; i++) {
       pthread_join(t_ids[i], (void**)&ret);
    }

}