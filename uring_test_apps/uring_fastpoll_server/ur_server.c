#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <liburing.h>  

// this program needs IORING_FEAT_FAST_POLL feature. kernel 5.7 required
#define IORING_FEAT_FAST_POLL (1U << 5)

//net
#define LISTEN_PORT 7777
#define LISTEN_BACKLOG 10000

//net app
#define CLIENT_MESSAGE_SIZE 1024
#define CONNECTIONS_POOL_SIZE 10000 

//io
#define IO_URING_LEN 32768


enum socket_state {
    ACCEPT,
    READ,
    WRITE,
};

typedef struct {
    unsigned socket;
    enum socket_state state;    
} 
io_connection_data;

typedef struct {
    struct io_uring uring;
    io_connection_data conn_pool[CONNECTIONS_POOL_SIZE]; 
    char messages_buffer[CONNECTIONS_POOL_SIZE][CLIENT_MESSAGE_SIZE];  
} 
ur_thread_context;

typedef struct {
   unsigned listener_socket;
   unsigned thread_num;
}
thread_params;


void io_accept(ur_thread_context* context, int socket, struct sockaddr *cli_addr, socklen_t *addr_len);
void io_read(ur_thread_context* context, int socket, size_t size);
void io_write(ur_thread_context* context, int socket, size_t size);



void* launch_uring(void *arg) {

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

    struct io_uring_params p;
    struct sockaddr_in cli_addr;
    ur_thread_context* context;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    context = malloc(sizeof(ur_thread_context));
    memset(context, 0, sizeof(ur_thread_context));

    memset(&p, 0, sizeof(p));
    memset(&cli_addr, 0, addr_len);
    


    //init uring interface
    if (io_uring_queue_init_params(IO_URING_LEN, &context->uring, &p) < 0) {
        perror("io_uring_init failed. \n");
        return NULL;
    }

    if (!(p.features & IORING_FEAT_FAST_POLL)) {
        perror("IORING_FEAT_FAST_POLL not supported. kernel 5.7 needed. \n");
        return NULL;
    }


    // add 1st accept sqe
    io_accept(context, sock_listen, (struct sockaddr *)&cli_addr, &addr_len);



    // main io loop
    while (1)
    {
        io_uring_submit_and_wait(&context->uring, 1);

        struct io_uring_cqe *cqes[IO_URING_LEN];
        int total_cqes = io_uring_peek_batch_cqe(&context->uring, cqes, IO_URING_LEN);

        // iterate through CQEs
        for (int i = 0; i < total_cqes; i++)
        {
            struct io_uring_cqe *cqe = cqes[i];
            io_connection_data *cqe_data = (io_connection_data*)io_uring_cqe_get_data(cqe);


            switch(cqe_data->state) {
                case ACCEPT:
                    res = cqe->res; //new socket FD

                    //printf("ACCEPT SOCKET# %i in thread# %i \n", res, thread_num);
                    //fflush(stdout);

                    io_uring_cqe_seen(&context->uring, cqe);

                    if (res > 0) {
                        io_read(context, res, CLIENT_MESSAGE_SIZE);
                    }

                    io_accept(context, sock_listen, (struct sockaddr *)&cli_addr, &addr_len);
                    break;

                case READ:
                    res = cqe->res; //bytes read

                    if (res <= 0) {
                       //connection was closed
                       io_uring_cqe_seen(&context->uring, cqe);
                       close(cqe_data->socket);
                    }
                    else {
                       io_uring_cqe_seen(&context->uring, cqe);
                       io_write(context, cqe_data->socket, res);
                    }
                    break;
                case WRITE:
                    io_uring_cqe_seen(&context->uring, cqe);
                    io_read(context, cqe_data->socket, CLIENT_MESSAGE_SIZE);
                    break;
            }
        }
    }
}


void io_accept(ur_thread_context* context, int socket, struct sockaddr *cli_addr, socklen_t *addr_len)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&context->uring);

    io_uring_prep_accept(sqe, socket, cli_addr, addr_len, 0);

    io_connection_data *conn_data = &context->conn_pool[socket];
    conn_data->socket = socket;
    conn_data->state = ACCEPT;

    io_uring_sqe_set_data(sqe, conn_data);
}

void io_read(ur_thread_context* context, int socket, size_t size)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&context->uring);
    io_uring_prep_recv(sqe, socket, &context->messages_buffer[socket], size, 0);

    io_connection_data *conn_data = &context->conn_pool[socket];
    conn_data->socket = socket;
    conn_data->state = READ;

    io_uring_sqe_set_data(sqe, conn_data);
}

void io_write(ur_thread_context* context, int socket, size_t size)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&context->uring);
    io_uring_prep_send(sqe, socket, &context->messages_buffer[socket], size, 0);

    io_connection_data *conn_data = &context->conn_pool[socket];
    conn_data->socket = socket;
    conn_data->state = WRITE;

    io_uring_sqe_set_data(sqe, conn_data);
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
       pthread_create(&t_ids[i], NULL, &launch_uring, (void*)&tp_arr[i]);
    }    

    printf("server running...\n");

    int* ret;
    for (int i=0; i<threads; i++) {
       pthread_join(t_ids[i], (void**)&ret);
    }

}



