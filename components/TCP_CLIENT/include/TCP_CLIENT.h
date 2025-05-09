#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TCP_CLIENT_CORE 0

void initTcpClientSocket(QueueHandle_t connectionStateQueueHandler);

void initTcpServerSocket(QueueHandle_t connectionStateQueueHandler);


#endif