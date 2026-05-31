#pragma once

#include <Arduino.h>

static const size_t NUMSENSORS = 4;

extern float voltage[NUMSENSORS];
extern int temperature[NUMSENSORS];
extern float pressurePSI[NUMSENSORS];
extern float pressureBAR[NUMSENSORS];
extern bool updated[NUMSENSORS];
extern unsigned long lastupdate[NUMSENSORS];

void startTpms();
void checkTpms();
