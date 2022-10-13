#ifndef socket_buffer_h
#define socket_buffer_h

#include <stdlib.h>

#define SOCKET_BUFFER_MEMORY 0     // 内存块，能明确知道内存大小
#define SOCKET_BUFFER_OBJECT 1     // 内存指针，需要做相应处理(send_object_init)才能知道数据的真实大小
#define SOCKET_BUFFER_RAWPOINTER 2 // 原始内存，对应lua userdata

struct socket_sendbuffer {
    int id;             // socket id
    int type;           // 要发送的数据类型（参见上面的宏定义）
    const void *buffer; // 数据指针（这里并非是真实要发送的数据的内存指针）
    size_t sz;          // 数据大小
};

#endif
