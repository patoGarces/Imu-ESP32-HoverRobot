#include "driver/gpio.h"
#include "soc/gpio_periph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "string.h"
#include "stdio.h"
#include "math.h"
#include "esp_log.h"

#include "main.h"
#include "comms.h"
#include "PID.h"
#include "storage_flash.h"
#include "mpu6050_wrapper.h"

#ifdef HARDWARE_PROTOTYPE
    #include "stepper.h"
#endif

/* Incluyo componentes */
// #include "../components/BT_BLE/include/BT_BLE.h"
#include "../components/TCP_CLIENT/include/TCP_CLIENT.h"

#if defined(HARDWARE_S3)
    #include "../components/CAN_COMMS/include/CAN_MCB.h"
#endif

#define MAX_VELOCITY            1000.00

#if defined(HARDWARE_S3)
    #define MAX_ANGLE_JOYSTICK          4.0
    #define MAX_ANGLE_POS_CONTROL       15.0
    #define MAX_ROTATION_RATE_CONTROL   25.0
#else
    #define MAX_ANGLE_JOYSTICK      8.0
    #define MAX_ROTATION_RATE_CONTROL         100
#endif

extern QueueHandle_t mpu6050QueueHandler;                   // Recibo nuevos angulos obtenidos del MPU
QueueHandle_t motorControlQueueHandler;                     // Envio nuevos valores de salida para el control de motores
QueueHandle_t newPidParamsQueueHandler;                     // Recibo nuevos parametros relacionados al pid
QueueHandle_t newCommandQueueHandler;
QueueHandle_t receiveControlQueueHandler;
QueueHandle_t newMcbQueueHandler;

status_robot_t statusRobot;                            // Estructura que contiene todos los parametros de status a enviar a la app
output_motors_t speedMotors;
output_motors_t attitudeControlMotor;

struct {
    uint8_t attMode;
    float   setPointPosCms;
    float   setPointVel;
    float   setPointYaw;
    uint8_t contSafetyMaxSpeed;
} attitudeControlStat = {
    .attMode = ATT_MODE_ATTI,
    .setPointPosCms = 0.00,
    .setPointVel = 0.00,
    .setPointYaw = 0.00,
};

float pos2mts(int32_t steps) {
    return (steps/90.00) * 0.5310707511;                  // 90 steps por vuelta, distancia recorrida por vuelta: diam 17cm * pi = 53.10707 cms = 0.5310707511 mts
}

/*
  * Calculo error circular para el yaw, donde hay una discontinuidad entre -180 y 180, ya que en realidad ese salto no es tal.
*/

// ejemplo: setpoint = 179              // TODO: borrar
//         actualValue = -175

//         error = 179-(-175) =  354
//         error > 180 => error = error-360
//         return 354-360 = -6
float angularDistance(float setPoint,float actualValue) {

    float error = setPoint - actualValue;

    if(error > 180.00) {
        error -= 360.00;
        // ESP_LOGI("CircularError","Error: %f > 180.00",error);
    }
    else if(error < -180.00) {
        error += 360.00;
        // ESP_LOGI("CircularError","Error: %f < 180.00",error);
    }
    else { 
        // ESP_LOGI("CircularError","Error: %f en rango normal",error);
    }
    return error;

}

int16_t cutSpeedRange(int16_t speed) {
    if (speed > 1000) {
        return 1000;
    }
    else if (speed < -1000) {
        return -1000;
    }
    else {
        return speed;
    }
}

// uint32_t ipAddressToUin32(uint8_t ip1,uint8_t ip2,uint8_t ip3,uint8_t ip4) {
//     return (((uint32_t)ip1) << 24) + (((uint32_t)ip2) << 16) + (((uint32_t)ip3) << 8) + ip4; 
// }

void setStatusRobot(uint8_t newStatus) {
    const char *TAG = "StatusRobot";

    
    switch(newStatus) {
        case STATUS_ROBOT_STABILIZED:
            pidClearTerms(PID_ANGLE);
            statusRobot.speedL = 0;
            statusRobot.speedR = 0;
            speedMotors.motorL = 0;
            speedMotors.motorR = 0;

            attitudeControlStat.attMode = ATT_MODE_ATTI;
            attitudeControlMotor.motorL = 0;
            attitudeControlMotor.motorR = 0;
            speedMotors.enable = true;
            ESP_LOGI(TAG,"ROBOT STABILIZED");
        break;

        case STATUS_ROBOT_ARMED:
            speedMotors.enable = false;
            speedMotors.motorL = 0;
            speedMotors.motorR = 0;
            statusRobot.localConfig.pids[PID_ANGLE].setPoint = statusRobot.localConfig.centerAngle;         // Reseteo el setPoint de PID_ANGLE 
            pidSetSetPoint(PID_ANGLE,statusRobot.localConfig.pids[PID_ANGLE].setPoint);

            attitudeControlStat.attMode = ATT_MODE_ATTI;

            ESP_LOGI(TAG,"DISABLED -> ROBOT ARMED, safetyLimits: %f",statusRobot.localConfig.safetyLimits);
        break;

        case STATUS_ROBOT_ERROR:
            speedMotors.enable = false;
            speedMotors.motorL = 0;
            speedMotors.motorR = 0;
            attitudeControlStat.contSafetyMaxSpeed = 0;
            ESP_LOGI(TAG,"ROBOT ERROR: SAFETY MAX MOTOR");
        break;

        default:
            ESP_LOGE(TAG,"Unknown state");
        break;
    }

    statusRobot.statusCode = newStatus;
}

static void imuControlHandler(void *pvParameters) {
    vector_queue_t newAngles;
    float safetyLimitProm[5];
    uint8_t safetyLimitPromIndex = 0;
    float angleReference = 0.00;

    const char *TAG = "ImuControlHandler";

    while(1) {
        if(xQueueReceive(mpu6050QueueHandler,&newAngles,pdMS_TO_TICKS(10))) {
        
            statusRobot.roll = newAngles.roll;
            statusRobot.pitch = newAngles.pitch;
            statusRobot.yaw = newAngles.yaw * -1.00;
            statusRobot.tempImu = (uint16_t)newAngles.temp * 10;

            angleReference = newAngles.pitch;

            // printf("pitch: %f\troll: %f\n",newAngles.pitch,newAngles.roll);
            
            int16_t outputPidMotors = (uint16_t)(pidCalculate(PID_ANGLE,angleReference) * MAX_VELOCITY); 

            speedMotors.motorL = cutSpeedRange(outputPidMotors + attitudeControlMotor.motorL);
            speedMotors.motorR = cutSpeedRange(outputPidMotors + attitudeControlMotor.motorR);

            safetyLimitProm[safetyLimitPromIndex++] = angleReference;
            if (safetyLimitPromIndex > 2) {
                safetyLimitPromIndex = 0;
            }
            float angleSafetyLimit = (safetyLimitProm[0] + safetyLimitProm[1] + safetyLimitProm[2]) / 3;

            if (pidGetEnable(PID_ANGLE)) { 
                if ((angleSafetyLimit < (statusRobot.localConfig.centerAngle-statusRobot.localConfig.safetyLimits)) ||
                    (angleSafetyLimit > (statusRobot.localConfig.centerAngle+statusRobot.localConfig.safetyLimits))) { 
                    pidSetDisable(PID_ANGLE);
                    setStatusRobot(STATUS_ROBOT_ARMED);
                }
            }
            else { 
                if ((angleReference > (statusRobot.localConfig.centerAngle - 1)) && 
                    (angleReference < (statusRobot.localConfig.centerAngle + 1))) { 
                    
                    statusRobot.localConfig.pids[PID_ANGLE].setPoint = statusRobot.localConfig.centerAngle;         // Reseteo el setPoint de PID_ANGLE 
                    pidSetSetPoint(PID_ANGLE,statusRobot.localConfig.pids[PID_ANGLE].setPoint);
                    setStatusRobot(STATUS_ROBOT_STABILIZED);

                    pidSetEnable(PID_ANGLE);  
                }
            }

            if (abs(speedMotors.motorR) == 1000 || abs(speedMotors.motorL) == 1000) {
                attitudeControlStat.contSafetyMaxSpeed++;
                if (attitudeControlStat.contSafetyMaxSpeed > 10) { 
                    pidSetDisable(PID_ANGLE);
                    setStatusRobot(STATUS_ROBOT_ERROR);
                }
            }
            else {
                attitudeControlStat.contSafetyMaxSpeed = 0;
            }
            
            statusRobot.speedL = speedMotors.motorL;
            statusRobot.speedR = speedMotors.motorR;
            
            // ESP_LOGE("IMU_CONTROL_HANDLER", "Pitch: %f\tRoll: %f\tEnablePid: %d\tEnableMotor:%d\n",newAngles.pitch,newAngles.roll,pidGetEnable(),speedMotors.enable);
        }
    }
}

static void attitudeControl(void *pvParameters){
    float outputPosControl = 0.00;
    uint8_t disableManualControlYaw = false;

    while(true) {

        #ifdef HARDWARE_S3
        if (statusRobot.statusCode == STATUS_ROBOT_STABILIZED) {

            if (!statusRobot.dirControl.joyAxisX) {     // Yaw control
                if (!disableManualControlYaw) { 
                    attitudeControlStat.setPointYaw = statusRobot.yaw;
                    statusRobot.localConfig.pids[PID_YAW].setPoint = attitudeControlStat.setPointYaw;
                    // pidSetSetPoint(PID_YAW, attitudeControlStat.setPointYaw / 1.8);

                    pidSetSetPoint(PID_YAW, 0);

                    pidSetEnable(PID_YAW);
                    disableManualControlYaw = true;
                }

                float angularDist = angularDistance(attitudeControlStat.setPointYaw,statusRobot.yaw);
                float absDesiredYaw = angularDist + statusRobot.yaw;
                
                statusRobot.outputYawControl = pidCalculate(PID_YAW,angularDist / 1.8);

                ESP_LOGI("circular error","error circular: %f,\t setPoint: %f,\tActualYaw: %f\toutput: %f\t desired: %f",angularDist,attitudeControlStat.setPointYaw,statusRobot.yaw,statusRobot.outputYawControl,absDesiredYaw);

                attitudeControlMotor.motorR = statusRobot.outputYawControl * MAX_ROTATION_RATE_CONTROL;
                attitudeControlMotor.motorL = attitudeControlMotor.motorR * -1;
            }
            else {
                disableManualControlYaw = false;
                pidSetDisable(PID_YAW);
                // Yaw manual control
                attitudeControlMotor.motorR = (statusRobot.dirControl.joyAxisX / 100.00) * MAX_ROTATION_RATE_CONTROL;
                attitudeControlMotor.motorL = attitudeControlMotor.motorR * -1;
            }
            
            // #ifdef ENABLE_POS_CONTROL
                if (!statusRobot.dirControl.joyAxisY) {     // Pos control

                    statusRobot.distanceInCms = ((statusRobot.posInMetersL + statusRobot.posInMetersR) / 2) * 100.00;

                    if (attitudeControlStat.attMode != ATT_MODE_POS_CONTROL) {
                        attitudeControlStat.setPointPosCms = statusRobot.distanceInCms;
                        statusRobot.localConfig.pids[PID_POS].setPoint = attitudeControlStat.setPointPosCms;
                        pidSetSetPoint(PID_POS,attitudeControlStat.setPointPosCms);
                        pidSetEnable(PID_POS);
                        outputPosControl = 0;
                        pidClearTerms(PID_POS);
                        attitudeControlStat.attMode = ATT_MODE_POS_CONTROL;

                        ESP_LOGI("AttitudeControl","Enable POS_CONTROL");
                    }
                    else {
                        outputPosControl = pidCalculate(PID_POS,statusRobot.distanceInCms) * MAX_ANGLE_POS_CONTROL; 
                    }
                }
                else {
                    attitudeControlStat.attMode = ATT_MODE_ATTI;            // TODO: deberia switchear aca a modo control de velocidad
                    pidSetDisable(PID_POS);
                    outputPosControl = (statusRobot.dirControl.joyAxisY / 100.00) * MAX_ANGLE_JOYSTICK;
                }
            // #endif
        }
        else {
            if (disableManualControlYaw) {
                disableManualControlYaw = false;
            }
        }
        #endif

        statusRobot.localConfig.pids[PID_ANGLE].setPoint = outputPosControl + statusRobot.localConfig.centerAngle; // TODO: probar NO contemplar el center angle en position control
        pidSetSetPoint(PID_ANGLE,statusRobot.localConfig.pids[PID_ANGLE].setPoint);     // La salida del control de posicion alimenta al PID de angulo

        // PID POS
        // printf(">inPos:%f\n>spPos:%f\n>spPos2:%f\n>outPos:%f\n",statusRobot.distanceInCms,attitudeControlStat.setPointPosCms,pidGetSetPoint(PID_POS)*100,outputPosControl);
        // PID ANGLE
        // printf(">inAngle:%f\n>spAngle:%f\n>outAngle:%d\n",statusRobot.pitch,statusRobot.localConfig.pids[PID_ANGLE].setPoint,statusRobot.speedL);

        // printf(">Mot:%d\n>measMot:%d\n",statusRobot.speedL,statusRobot.speedMeasL);

        // printf("distance: %f\tsetPoint: %f\n",distanceInCms,attitudeControlStat.setPointPosCms);

        // printf(">inYaw:%f\n>spYaw:%f\n>outYaw:%d\n",statusRobot.yaw/1.8,150/1.8,attitudeControlMotor.motorR);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


static void commsManager(void *pvParameters) {
    uint8_t lastStateIsConnected = false;
    pid_settings_comms_t    newPidSettings;
    command_app_raw_t       newCommand;
    control_app_raw_t       newControl;
    rx_motor_control_board_t receiveMcb;

    uint8_t toggle = false;
    const char *TAG = "commsManager";

    while(true) {

        if (xQueueReceive(receiveControlQueueHandler,&newControl,0)) {
            statusRobot.dirControl.joyAxisX = newControl.axisX;
            statusRobot.dirControl.joyAxisY = newControl.axisY;

            if (newControl.compassYaw <= 360) {
                int16_t yawAngle = newControl.compassYaw;
                if (newControl.compassYaw > 180) { 
                    yawAngle -= 360;
                }
                statusRobot.dirControl.compassYaw = yawAngle;    
                pidSetSetPoint(PID_YAW, yawAngle/1.8);
            }
        }
        
        if(xQueueReceive(newPidParamsQueueHandler,&newPidSettings,0)) {
            pidSetConstants(newPidSettings.indexPid,newPidSettings.kp,newPidSettings.ki,newPidSettings.kd);
            if (newPidSettings.indexPid == PID_ANGLE) {
                pidSetSetPoint(PID_ANGLE,newPidSettings.centerAngle);
                statusRobot.localConfig.pids[newPidSettings.indexPid].setPoint = newPidSettings.centerAngle;      
            }           
            statusRobot.localConfig.pids[newPidSettings.indexPid].kp = newPidSettings.kp;
            statusRobot.localConfig.pids[newPidSettings.indexPid].ki = newPidSettings.ki;
            statusRobot.localConfig.pids[newPidSettings.indexPid].kd = newPidSettings.kd;
                    
            printf("\nNuevos parametros %d:\n\tP: %f\n\tI: %f\n\tD: %f,\n\tcenter: %f\n\tsafety limits: %f\n\n",newPidSettings.indexPid,newPidSettings.kp,newPidSettings.ki,newPidSettings.kd,newPidSettings.centerAngle,newPidSettings.safetyLimits);              
        }

        if(xQueueReceive(newCommandQueueHandler,&newCommand,0)) {

            switch (newCommand.command) {
                case COMMAND_CALIBRATE_IMU:
                    ESP_LOGI(TAG,"Calibrando IMU...");
                    mpu6050_recalibrate();
                break;
                case COMMAND_SAVE_LOCAL_CONFIG:
                    ESP_LOGI(TAG,"Guardando parametros...");
                    storageLocalConfig(statusRobot.localConfig);
                    sendLocalConfig(statusRobot.localConfig);
                break;

                case COMMAND_MOVE_FORWARD:
                    ESP_LOGI(TAG,"Move forward command, distance: %f",newCommand.value / PRECISION_DECIMALS_COMMS);
                    attitudeControlStat.setPointPosCms = statusRobot.distanceInCms + newCommand.value;
                    statusRobot.localConfig.pids[PID_POS].setPoint = attitudeControlStat.setPointPosCms;
                    pidSetSetPoint(PID_POS,attitudeControlStat.setPointPosCms);
                break;

                case COMMAND_MOVE_BACKWARD:
                    ESP_LOGI(TAG,"Move backward command, distance: %f",newCommand.value / PRECISION_DECIMALS_COMMS);
                    attitudeControlStat.setPointPosCms = statusRobot.distanceInCms - newCommand.value;
                    statusRobot.localConfig.pids[PID_POS].setPoint = attitudeControlStat.setPointPosCms;
                    pidSetSetPoint(PID_POS,attitudeControlStat.setPointPosCms);
                break;
            }            
        }
        
        if(xQueueReceive(newMcbQueueHandler,&receiveMcb,0)) {
            statusRobot.batVoltage = receiveMcb.batVoltage;
            statusRobot.tempImu = receiveMcb.boardTemp;
            statusRobot.speedMeasR = receiveMcb.speedR_meas;
            statusRobot.speedMeasL = receiveMcb.speedL_meas;
            statusRobot.posInMetersR = pos2mts(receiveMcb.posR);
            statusRobot.posInMetersL = pos2mts(receiveMcb.posL * -1);

            // float distance = statusRobot.posInMetersL * 100.00;
            // printf(">posLf:%f\n>posL:%ld\n",statusRobot.posInMetersL,receiveMcb.posL * -1);
            // statusRobot.setPoint; 
        }

        if (isTcpClientConnected()) {

            if (!lastStateIsConnected) {
                sendLocalConfig(statusRobot.localConfig);
            }

            robot_dynamic_data_t newData = {
                .batVoltage = statusRobot.batVoltage,
                .imuTemp = statusRobot.tempImu,
                .speedR = statusRobot.speedR,
                .speedL = statusRobot.speedL,
                .pitch =  statusRobot.pitch * PRECISION_DECIMALS_COMMS,
                .roll = statusRobot.roll * PRECISION_DECIMALS_COMMS,
                .yaw = statusRobot.yaw * PRECISION_DECIMALS_COMMS,
                .posInMeters = ((statusRobot.posInMetersL + statusRobot.posInMetersR) / 2) * PRECISION_DECIMALS_COMMS,
                .outputYawControl = statusRobot.outputYawControl * PRECISION_DECIMALS_COMMS,
                .setPointAngle = statusRobot.localConfig.pids[PID_ANGLE].setPoint * PRECISION_DECIMALS_COMMS,
                .setPointPos = statusRobot.localConfig.pids[PID_POS].setPoint,                                  // No lo multiplico, para mandarlo en mts
                .setPointYaw = statusRobot.localConfig.pids[PID_YAW].setPoint * PRECISION_DECIMALS_COMMS,
                .centerAngle = statusRobot.localConfig.centerAngle * PRECISION_DECIMALS_COMMS,
                .statusCode = statusRobot.statusCode
            };
            sendDynamicData(newData);
        }

        xQueueSend(motorControlQueueHandler,&speedMotors,0);

        gpio_set_level(PIN_OSCILO, toggle);
        toggle = !toggle;

        lastStateIsConnected = isTcpClientConnected();

        // testHardwareVibration();
        // printf(">angle:%f\n>outputMotor:%f\n",angleReference/10.0,statusRobot.speedL/100.0);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void testHardwareVibration(void) {
    uint8_t flagInc=true;
    int16_t testMotor=0;

    while(true) {
        printf(">angle:%f\n>outputMotor:%f\n",statusRobot.pitch/10.0,statusRobot.speedL/100.0);

        if(flagInc){
            testMotor++;
            if(testMotor>99){
                flagInc=false;
            }
        }
        else {
            testMotor--;
            if(testMotor < -99){
                flagInc=true;
                // testMotor = 0;
            }
        }

        statusRobot.speedL = testMotor*10;
        statusRobot.speedR = testMotor*10;
        speedMotors.motorL = testMotor*3;
        speedMotors.motorR = testMotor*3;
        xQueueSend(motorControlQueueHandler,&speedMotors,pdMS_TO_TICKS(1));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ledHandler(void *pvParameters) {
    uint16_t delay = 1000;
    while(1) {
        if (isTcpClientConnected()) {
            delay = 500;
        }
        else {
            delay = 1000;
        }
        gpio_set_level(PIN_LED,1);
        vTaskDelay(pdMS_TO_TICKS(delay));
        gpio_set_level(PIN_LED,0);
        vTaskDelay(pdMS_TO_TICKS(delay));
        // testHardwareVibration();
    }
}

static void senderDebug(void *pvParameters) {

    while(true) {
        // printf(">inPos:%f\n>spPos:%f\n",statusRobot.distanceInCms,attitudeControlStat.setPointPosCms);
        // printf(">inAngle:%f\n>spAngle:%f\n>Mot:%d\n>measMot:%d\n",statusRobot.pitch,statusRobot.setPointAngle,statusRobot.speedL,statusRobot.speedMeasL);

        // printf(">Mot:%d\n>measMot:%d\n",statusRobot.speedL,statusRobot.speedMeasL);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main() {
    const char *TAG = "app_main";
    gpio_set_direction(PIN_LED , GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 1);

    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_OSCILO], PIN_FUNC_GPIO);
    gpio_set_direction(PIN_OSCILO , GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_OSCILO, 1);

    setStatusRobot(STATUS_ROBOT_INIT);

    receiveControlQueueHandler = xQueueCreate(1, sizeof(control_app_raw_t));
    newPidParamsQueueHandler = xQueueCreate(1,sizeof(pid_settings_comms_t));
    newCommandQueueHandler = xQueueCreate(1, sizeof(command_app_raw_t));
    motorControlQueueHandler = xQueueCreate(1,sizeof(output_motors_t));
    mpu6050QueueHandler = xQueueCreate(1,sizeof(vector_queue_t));
    newMcbQueueHandler = xQueueCreate(1,sizeof(rx_motor_control_board_t));

    storageInit();
    statusRobot.localConfig = getFromStorageLocalConfig();

    #ifdef HARDWARE_S3

        statusRobot.localConfig.pids[PID_ANGLE].kp = 0.57;
        statusRobot.localConfig.pids[PID_ANGLE].ki = 0.13;
        statusRobot.localConfig.pids[PID_ANGLE].kd = 1.16;

        statusRobot.localConfig.pids[PID_POS].kp = 0.5;//0.28;
        statusRobot.localConfig.pids[PID_POS].ki = 0.03;//0.03;
        statusRobot.localConfig.pids[PID_POS].kd = 1.20;//0.8;

        statusRobot.localConfig.pids[PID_YAW].kp = 2.00;
        statusRobot.localConfig.pids[PID_YAW].ki = 0.3;
        statusRobot.localConfig.pids[PID_YAW].kd = 0.00;

        statusRobot.localConfig.centerAngle = 2.5;
        statusRobot.localConfig.safetyLimits = 45;
    #endif

    statusRobot.localConfig.pids[PID_ANGLE].setPoint = statusRobot.localConfig.centerAngle;

    ESP_LOGI(TAG, "\n------------------- local config -------------------"); 
    ESP_LOGI(TAG, "safetyLimits: %f\tcenterAngle: %f",statusRobot.localConfig.safetyLimits,statusRobot.localConfig.centerAngle);
    
    for (uint8_t i=0;i<CANT_PIDS;i++) {
        ESP_LOGI(TAG,"PID %d Params: kp: %f\tki: %f\tkd: %f\tsetPoint: %f",i,statusRobot.localConfig.pids[i].kp,statusRobot.localConfig.pids[i].ki,statusRobot.localConfig.pids[i].kd,statusRobot.localConfig.pids[i].setPoint);
    }
    ESP_LOGI(TAG, "\n------------------- local config -------------------\n"); 
    
    mpu6050_init_t configMpu = {
        .intGpio = GPIO_MPU_INT,
        .sclGpio = GPIO_MPU_SCL,
        .sdaGpio = GPIO_MPU_SDA,
        .priorityTask = MPU_HANDLER_PRIORITY,
        .core = IMU_HANDLER_CORE
    };
    mpu6050_initialize(&configMpu);

    pid_init_t pidConfig;
    pidConfig.sampleTimeInMs = PERIOD_IMU_MS;
    memcpy(pidConfig.pids,statusRobot.localConfig.pids,sizeof(pidConfig.pids));
    pidInit(pidConfig);

    #if defined(HARDWARE_S3)
        config_init_mcb_t configMcb = {
            .numUart = UART_PORT_CAN,
            .txPin = GPIO_CAN_TX,
            .rxPin = GPIO_CAN_RX,
            .queue = newMcbQueueHandler,
            .core = 0
        };
        mcbInit(&configMcb);
    #endif

    #ifdef HARDWARE_PROTOTYPE
        stepper_config_t configMotors = {
            .gpio_mot_l_step = GPIO_MOT_L_STEP,
            .gpio_mot_l_dir = GPIO_MOT_L_DIR,
            .gpio_mot_r_step = GPIO_MOT_R_STEP,
            .gpio_mot_r_dir = GPIO_MOT_R_DIR,
            .gpio_mot_enable = GPIO_MOT_ENABLE,
            .gpio_mot_microstepper = GPIO_MOT_MICRO_STEP
        };
        motorsInit(configMotors);
        setMicroSteps(true);
    #endif

    // esp_log_level_set("wifi", ESP_LOG_WARN);
    // esp_log_level_set("wifi_init", ESP_LOG_WARN);
    initTcpClient("");

    setStatusRobot(STATUS_ROBOT_ARMED);
    xTaskCreatePinnedToCore(imuControlHandler,"Imu Control",4096,NULL,IMU_HANDLER_PRIORITY,NULL,IMU_HANDLER_CORE);
    xTaskCreatePinnedToCore(attitudeControl,"attitude control",4096,NULL,4,NULL,IMU_HANDLER_CORE);
    xTaskCreatePinnedToCore(commsManager,"communication manager",4096,NULL,COMM_HANDLER_PRIORITY,NULL,IMU_HANDLER_CORE);
    xTaskCreatePinnedToCore(ledHandler,"Led handler",2048,NULL,2,NULL,IMU_HANDLER_CORE);
    
    // xTaskCreatePinnedToCore(senderDebug,"SenderDebug handler",45200,NULL,2,NULL,IMU_HANDLER_CORE);
}