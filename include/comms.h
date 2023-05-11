#ifndef __COMMS_H__
#define __COMMS_H__

#include "stdio.h"

#define HEADER_COMMS 0xABC0

 typedef struct{
    uint32_t header;
    uint32_t kp;
    uint32_t ki;
    uint32_t kd;
    uint32_t checksum;
} pid_settings_t;

typedef struct{
    uint16_t header;
    uint16_t bat_voltage;
    uint16_t bat_percent;
    uint16_t batTemp;
    uint16_t temp_uc_control;
    uint16_t temp_uc_main;
    int16_t speedR;
    int16_t speedL;
    float   pitch;
    float   roll;
    float   yaw;
    float   centerAngle;
    uint16_t P;
    uint16_t I;
    uint16_t D;
    uint16_t orden_code;
    uint16_t error_code;
    uint16_t checksum;
}status_robot_t;

// enum{
//     ERROR_CODE_INIT,
//     ERROR_CODE_NORMAL,
//     ERROR_CODE_ERROR,
//     ERROR_CODE_UNKNOWN
// }error_code_enum;

// typedef struct{
//     uint16_t P;
//     uint16_t I;
//     uint16_t D;
//     uint8_t enable;
//     uint8_t orden_code;
//     uint8_t error_code;
// }rx_bt_app_t;

/**
 * @brief     handler for write and read
 */
typedef void (* spp_wr_task_cb_t) (void *fd);


void spp_read_handle(void * param);
void spp_wr_task_shut_down(void);
void spp_wr_task_start_up(spp_wr_task_cb_t p_cback, int fd);
void sendStatus(status_robot_t status);
#endif