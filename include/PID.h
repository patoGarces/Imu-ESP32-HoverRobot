#ifndef __PID_H__
#define __PID_H__

#include <stdio.h>
#include "stdbool.h"
#include "esp_err.h"

#define CANT_PIDS	3

typedef struct {
	float kp;
    float ki;
    float kd;
	float initSetPoint;
	float sampleTimeInMs;
} pid_init_t;

typedef struct {
	uint8_t enablePID;
	float lastInput;
	float iTerm;
	float kp;											//parametro P
	float ki;											//parametro I
	float kd;											//parametro D
	float setPoint;										//setPoint del PID, normalizado: [-1.00;1.00]
	float sampleTimeInSec;
} pid_control_t;

esp_err_t pidInit(pid_init_t initParams[CANT_PIDS]);

void pidSetEnable(uint8_t numPid);

void pidSetDisable(uint8_t numPid);

bool pidGetEnable(uint8_t numPid);
/*
 * Esta funcion debe ser llamada cada un periodo fijo definido en pidControl1.sampleTime
 * @param input entra en rango [-100;100]
 * @return resultado del PID normalizado [-1.00;1.00]
 */
float pidCalculate(uint8_t numPid,float input);

/*
 * Funcion para asignar un nuevo setPoint
 */
void pidSetSetPoint(uint8_t numPid,float value);

/*
 * Funcion para obtener el setPoint actual
 */
float pidGetSetPoint(uint8_t numPid);

/*
 * 	Funcion para cargar los parametros al filtro PID
 */
void pidSetConstants(uint8_t numPid,float KP, float KI, float KD);


#endif
