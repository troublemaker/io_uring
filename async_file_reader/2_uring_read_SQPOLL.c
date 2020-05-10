#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <linux/membarrier.h>
#include <linux/io_uring.h> //kernel 5.1 required

#define URING_QUEUE_SIZE  1024

#define rmb() __asm__ __volatile__("lock; addl $0,0(%%rsp)":::"memory")
#define wmb() __asm__ __volatile__("lock; addl $0,0(%%rsp)":::"memory")

struct io_file_data {
    char* filename;
    off_t filesize;
    struct iovec* iov;      
};


struct app_sq_ring {
    unsigned* head;
    unsigned* tail;
    unsigned* ring_mask;
    unsigned* ring_entries;
    unsigned* flags;
    unsigned* array;
};

struct app_cq_ring {
    unsigned* head;
    unsigned* tail;
    unsigned* ring_mask;
    unsigned* ring_entries;
    struct io_uring_cqe* cqes;
};


//global app references to uring interface
int uring_fd;
struct app_sq_ring _sq;
struct app_cq_ring _cq;
struct io_uring_sqe* _sqes;


// these are not in the linux c library yet
int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int) syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,unsigned int min_complete, unsigned int flags)
{
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

int io_uring_register(int fd, unsigned int opcode, const void *arg, unsigned int nr_args) {
    return (int) syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

int membarrier(int cmd, int flags)
{
    return syscall(__NR_membarrier, cmd, flags);
}


//  init uring interface
int init_uring() {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));

    //setup for IORING_SETUP_SQPOLL
    p.flags = IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 99999999;

    uring_fd = io_uring_setup(URING_QUEUE_SIZE, &p); 
    if (uring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    void* ptr;

    //mapping submission queue 
    ptr = mmap(0, p.sq_off.array + p.sq_entries * sizeof(__u32), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, uring_fd, IORING_OFF_SQ_RING);
    if (ptr == MAP_FAILED) {
        perror("mmap failed for submission queue");
        return 1;
    }

    //save for easier reference
    _sq.head = ptr + p.sq_off.head;
    _sq.tail = ptr + p.sq_off.tail;
    _sq.ring_mask = ptr + p.sq_off.ring_mask;
    _sq.ring_entries = ptr + p.sq_off.ring_entries;
    _sq.flags = ptr + p.sq_off.flags;
    _sq.array = ptr + p.sq_off.array;


    //mapping submission queue entries
    _sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, uring_fd, IORING_OFF_SQES);
    if (_sqes == MAP_FAILED) {
        perror("mmap failed for submission queue entries");
        return 1;
    }


    //mapping completion queue 
    ptr = mmap(0, p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, uring_fd, IORING_OFF_CQ_RING);
    if (ptr == MAP_FAILED) {
        perror("mmap failed for completion queue");
        return 1;
    }

    //save for easier reference
    _cq.head = ptr + p.cq_off.head;
    _cq.tail = ptr + p.cq_off.tail;
    _cq.ring_mask = ptr + p.cq_off.ring_mask;
    _cq.ring_entries = ptr + p.cq_off.ring_entries;
    _cq.cqes = ptr + p.cq_off.cqes;

    return 0;
}




int submit_sqe(int fd, char* filename) {
    struct stat st;
    struct io_file_data* fdata;

    fdata = malloc(sizeof(struct io_file_data));
    fdata->iov = malloc(sizeof(struct iovec));

    fstat(fd, &st);
    if (st.st_size < 1) {
        return 1;
    }

    //we'll be using only one iovec struct / buffer
    fdata->filename = malloc(strlen(filename));
    strcpy(fdata->filename, filename);
    fdata->filesize = st.st_size;
    fdata->iov->iov_len = st.st_size;

    void* ptr;
    ptr = malloc(st.st_size);
    fdata->iov->iov_base = ptr;


    unsigned tail, index;

    //
    //add new submission to the tail
    //

    tail = *_sq.tail;
    index = tail & *_sq.ring_mask;
    struct io_uring_sqe *sqe = &_sqes[index];
   
    //because of IORING_SETUP_SQPOLL
    sqe->fd = 0; 
    sqe->flags = IOSQE_FIXED_FILE;
    //
    sqe->opcode = IORING_OP_READV;
    sqe->addr = (unsigned long) fdata->iov;
    sqe->len = 1; //only one iov element
    sqe->off = 0;
    sqe->user_data = (unsigned long long) fdata;
    _sq.array[index] = index;
    tail++;

    //tell the kernel new submission entry added
    wmb();
    *_sq.tail = tail;
    wmb();
    

    return 0;
}



int read_cqe() {
    struct io_file_data* fdata;
    struct io_uring_cqe* cqe;
    unsigned head;
    int entries_read = 0;
    
    head = *_cq.head;

    while (1) {
        rmb();

        if (head == *_cq.tail){
            break;
        }

        // read completed entry
        cqe = &_cq.cqes[head & *_cq.ring_mask];
        fdata = (struct io_file_data*) cqe->user_data;
        printf("File: %s, readv res=%d\n", fdata->filename, cqe->res);

        if (cqe->res < 0) {
            fprintf(stderr, "readv error: %s\n", strerror(cqe->res));
        } 
        else {
        
           //print out last 10 chars or less 
           char* buff = (char*)fdata->iov->iov_base;
           if (cqe->res > 10) {
  	       printf("%.10s", &buff[cqe->res-10]);   
           } else {
               printf("%.*s", cqe->res, buff);
           } 
           //
        }
        entries_read++;
        head++;
    }

    *_cq.head = head;
    wmb();

    return entries_read;
}

int ur_needs_waking() {
   wmb();
   //membarrier(MEMBARRIER_CMD_GLOBAL,0);   //something's off here. need to figure out.
   if (*_sq.flags & IORING_SQ_NEED_WAKEUP) {
      printf("waking up..\n");
      io_uring_enter(uring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP);
   }
   else {
      printf("not sleeping..\n");
   }
}





int main(int argc, char *argv[]) {

    int* filefds;

    if (argc < 2) {
        fprintf(stderr, "No files provided\n");
        return 1;
    }

    memset(&_sq, 0, sizeof(_sq));
    memset(&_cq, 0, sizeof(_cq));
    
    if(init_uring()) {
        fprintf(stderr, "io_uring_setup error\n");
        return 1;
    }

    int file_count = argc - 1;
    printf("Files to process: %i\n", file_count);


    //init fds storage
    filefds = malloc(sizeof(int)*file_count);

    for (int i = 1; i <= file_count; i++) {
       filefds[i-1] = open(argv[i], O_RDONLY);
       if (filefds[i-1] < 0) {
           fprintf(stderr,"file open error: %s \n", argv[i]);
           return 1;
       }
    }

    //register all fds with uring
    int r = io_uring_register(uring_fd, IORING_REGISTER_FILES, filefds, file_count);
    if (r < 0) {
        perror("io_uring_register_failed...\n");
        exit(1);
    }

    ur_needs_waking();

    //loop through files and add to SQE
    for (int i = 1; i <= file_count; i++) {
        if(submit_sqe(filefds[i-1], argv[i])) {
            fprintf(stderr, "Error reading file %s\n", argv[i]);
            return 1;
        }
        //sleep(1); //testing
        //ur_needs_waking(); //testing
    }
   

    int res = 0; 
    int entires_read = 0;
    //read results from CQE as they come
    while(1) {
       if (entires_read < file_count) {
          res = io_uring_enter(uring_fd, 0, 1, IORING_ENTER_GETEVENTS);
          if(res < 0) {
             perror("read: io_uring_enter error");
             return 1;
          }
          entires_read += read_cqe();
          printf("--- entries read: %i\n", entires_read);          
       }
       else {
          break;
       }
    }

    printf("\n...done\n");

    return 0;
}



