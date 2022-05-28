
#define SCULL_QUANTUM 1
#define SCULL_QSET    1

struct scull_qset
{
    void** data;
    struct scull_qset* next;
};