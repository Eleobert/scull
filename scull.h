
#define SCULL_QUANTUM 4000
#define SCULL_QSET    1000

struct scull_qset
{
    void** data;
    struct scull_qset* next;
};
