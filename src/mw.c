#include "board.h"
#include "mw.h"

// March  2012     V2.0

#define CHECKBOXITEMS 11
#define PIDITEMS 8

int16_t debug1, debug2, debug3, debug4;
uint8_t buzzerState = 0;
uint32_t currentTime = 0;
uint32_t previousTime = 0;
uint16_t cycleTime = 0;         // this is the number in micro second to achieve a full loop, it can differ a little and is taken into account in the PID loop
uint8_t GPSModeHome = 0;        // if GPS RTH is activated
uint8_t GPSModeHold = 0;        // if GPS PH is activated
uint8_t headFreeMode = 0;       // if head free mode is a activated
uint8_t passThruMode = 0;       // if passthrough mode is activated
int16_t headFreeModeHold;
int16_t annex650_overrun_count = 0;
uint8_t armed = 0;
uint8_t vbat;                   // battery voltage in 0.1V steps

volatile int16_t failsafeCnt = 0;
int16_t failsafeEvents = 0;
int16_t rcData[8] = { 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500 };              // interval [1000;2000]
int16_t rcCommand[4];           // interval [1000;2000] for THROTTLE and [-500;+500] for ROLL/PITCH/YAW 
int16_t lookupRX[7];            // lookup table for expo & RC rate
rcReadRawDataPtr rcReadRawFunc = NULL; // receive data from default (pwm/ppm) or additional (spek/sbus/?? receiver drivers)

uint8_t dynP8[3], dynI8[3], dynD8[3];
uint8_t rcOptions[CHECKBOXITEMS];
uint8_t okToArm = 0;
uint8_t accMode = 0;            // if level mode is a activated
uint8_t magMode = 0;            // if compass heading hold is a activated
uint8_t baroMode = 0;           // if altitude hold is activated

int16_t axisPID[3];

// **********************
// GPS
// **********************
int32_t GPS_latitude, GPS_longitude;
int32_t GPS_latitude_home, GPS_longitude_home;
int32_t GPS_latitude_hold, GPS_longitude_hold;
uint8_t GPS_fix, GPS_fix_home = 0;
uint8_t GPS_numSat;
uint16_t GPS_distanceToHome;    // in meters
int16_t GPS_directionToHome = 0;        // in degrees
uint16_t GPS_distanceToHome, GPS_distanceToHold;        // distance to home or hold point in meters
int16_t GPS_directionToHome, GPS_directionToHold;       // direction to home or hol point in degrees
int16_t GPS_heading;                     // gps heading in degrees
uint16_t GPS_altitude, GPS_speed;       // altitude in 0.1m and speed in 0.1m/s - Added by Mis
uint8_t GPS_update = 0;         // it's a binary toogle to distinct a GPS position update
int16_t GPS_angle[2];           // it's the angles that must be applied for GPS correction

//Automatic ACC Offset Calibration
// **********************
uint16_t InflightcalibratingA = 0;
int16_t AccInflightCalibrationArmed;
uint16_t AccInflightCalibrationMeasurementDone = 0;
uint16_t AccInflightCalibrationSavetoEEProm = 0;
uint16_t AccInflightCalibrationActive = 0;

// Battery monitoring stuff
uint8_t batteryCellCount = 3;   // cell count
uint16_t batteryWarningVoltage; // annoying buzzer after this one, battery ready to be dead

void blinkLED(uint8_t num, uint8_t wait, uint8_t repeat)
{
    uint8_t i, r;

    for (r = 0; r < repeat; r++) {
        for (i = 0; i < num; i++) {
            LED0_TOGGLE;        // switch LEDPIN state
            BEEP_ON;
            delay(wait);
            BEEP_OFF;
        }
        delay(60);
    }
}

// this code is executed at each loop and won't interfere with control loop if it lasts less than 650 microseconds
void annexCode(void)
{
    static uint32_t buzzerTime, calibratedAccTime;
    static uint8_t vbatTimer = 0;
    static uint8_t buzzerFreq;  //delay between buzzer ring
    uint8_t axis, prop1, prop2;
    static uint8_t ind = 0;
    uint16_t vbatRaw = 0;
    static uint16_t vbatRawArray[8];
    uint8_t i;

    // PITCH & ROLL only dynamic PID adjustemnt,  depending on throttle value
    if (rcData[THROTTLE] < 1500) {
        prop2 = 100;
    } else if (rcData[THROTTLE] < 2000) {
        prop2 = 100 - (uint16_t) cfg.dynThrPID * (rcData[THROTTLE] - 1500) / 500;
    } else {
        prop2 = 100 - cfg.dynThrPID;
    }

    for (axis = 0; axis < 3; axis++) {
        uint16_t tmp = min(abs(rcData[axis] - cfg.midrc), 500);
        if (axis != 2) {        // ROLL & PITCH
            uint16_t tmp2;
            if (cfg.deadband) {
                if (tmp > cfg.deadband) {
                    tmp -= cfg.deadband;
                } else {
                    tmp = 0;
                }
            }
            tmp2 = tmp / 100;
            rcCommand[axis] = lookupRX[tmp2] + (tmp - tmp2 * 100) * (lookupRX[tmp2 + 1] - lookupRX[tmp2]) / 100;
            prop1 = 100 - (uint16_t) cfg.rollPitchRate * tmp / 500;
            prop1 = (uint16_t)prop1 * prop2 / 100;
        } else {                // YAW
            if (cfg.yawdeadband) {
                if (tmp > cfg.yawdeadband) {
                    tmp -= cfg.yawdeadband;
                } else {
                    tmp = 0;
                }
            }
            rcCommand[axis] = tmp;
            prop1 = 100 - (uint16_t)cfg.yawRate * tmp / 500;
        }
        dynP8[axis] = (uint16_t)cfg.P8[axis] * prop1 / 100;
        dynD8[axis] = (uint16_t)cfg.D8[axis] * prop1 / 100;
        if (rcData[axis] < cfg.midrc)
            rcCommand[axis] = -rcCommand[axis];
    }
    rcCommand[THROTTLE] = cfg.minthrottle + (int32_t)(cfg.maxthrottle - cfg.minthrottle) * (rcData[THROTTLE] - cfg.mincheck) / (2000 - cfg.mincheck);

    if (headFreeMode) {
        float radDiff = (heading - headFreeModeHold) * M_PI / 180.0f;
        float cosDiff = cosf(radDiff);
        float sinDiff = sinf(radDiff);
        int16_t rcCommand_PITCH = rcCommand[PITCH] * cosDiff + rcCommand[ROLL] * sinDiff;
        rcCommand[ROLL] = rcCommand[ROLL] * cosDiff - rcCommand[PITCH] * sinDiff;
        rcCommand[PITCH] = rcCommand_PITCH;
    }

    if (feature(FEATURE_VBAT)) {
        if (!(++vbatTimer % VBATFREQ)) {
            vbatRawArray[(ind++) % 8] = adcGetBattery();
            for (i = 0; i < 8; i++)
                vbatRaw += vbatRawArray[i];
            vbat = batteryAdcToVoltage(vbatRaw / 8);
        }
        if (rcOptions[BOXBEEPERON]) {   // unconditional beeper on via AUXn switch 
            buzzerFreq = 7;
        } else if ((vbat > batteryWarningVoltage) || (vbat < cfg.vbatmincellvoltage)) {      //VBAT ok, buzzer off
            buzzerFreq = 0;
            buzzerState = 0;
            BEEP_OFF;
        } else
            buzzerFreq = 4;     // low battery
        if (buzzerFreq) {
            if (buzzerState && (currentTime > buzzerTime + 250000)) {
                buzzerState = 0;
                BEEP_OFF;
                buzzerTime = currentTime;
            } else if (!buzzerState && (currentTime > (buzzerTime + (2000000 >> buzzerFreq)))) {
                buzzerState = 1;
                BEEP_ON;
                buzzerTime = currentTime;
            }
        }
    }

    if ((calibratingA > 0 && sensors(SENSOR_ACC)) || (calibratingG > 0)) {      // Calibration phasis
        LED0_TOGGLE;
    } else {
        if (calibratedACC == 1) {
            LED0_OFF;
        }
        if (armed) {
            LED0_ON;
        }
    }

#ifdef LEDRING
    if (feature(FEATURE_LED_RING)) {
        static uint32_t LEDTime;
        if (currentTime > LEDTime) {
            LEDTime = currentTime + 50000;
            ledringState();
        }
    }
#endif

    if (currentTime > calibratedAccTime) {
        if (smallAngle25 == 0) {
            calibratedACC = 0;  //the multi uses ACC and is not calibrated or is too much inclinated
            LED0_TOGGLE;
            calibratedAccTime = currentTime + 500000;
        } else
            calibratedACC = 1;
    }

    serialCom();

    if (sensors(SENSOR_GPS)) {
        static uint32_t GPSLEDTime;
        if (currentTime > GPSLEDTime && (GPS_fix_home == 1)) {
            GPSLEDTime = currentTime + 150000;
            LED1_TOGGLE;
        }
    }
}

uint16_t pwmReadRawRC(uint8_t chan)
{
    uint16_t data;

    failsafeCnt = 0;
    data = pwmRead(cfg.rcmap[chan]);
    if (data < 750 || data > 2250)
        data = cfg.midrc;

    return data;
}

void computeRC(void)
{
    static int16_t rcData4Values[8][4], rcDataMean[8];
    static uint8_t rc4ValuesIndex = 0;
    uint8_t chan, a;

    rc4ValuesIndex++;
    for (chan = 0; chan < 8; chan++) {
        rcData4Values[chan][rc4ValuesIndex % 4] = rcReadRawFunc(chan);
        rcDataMean[chan] = 0;
        for (a = 0; a < 4; a++)
            rcDataMean[chan] += rcData4Values[chan][a];

        rcDataMean[chan] = (rcDataMean[chan] + 2) / 4;
        if (rcDataMean[chan] < rcData[chan] - 3)
            rcData[chan] = rcDataMean[chan] + 2;
        if (rcDataMean[chan] > rcData[chan] + 3)
            rcData[chan] = rcDataMean[chan] - 2;
    }
}

void loop(void)
{
    static uint8_t rcDelayCommand;      // this indicates the number of time (multiple of RC measurement at 50Hz) the sticks must be maintained to run or switch off motors
    uint8_t axis, i;
    int16_t error, errorAngle;
    int16_t delta, deltaSum;
    int16_t PTerm, ITerm, DTerm;
    static int16_t lastGyro[3] = { 0, 0, 0 };
    static int16_t delta1[3], delta2[3];
    static int16_t errorGyroI[3] = { 0, 0, 0 };
    static int16_t errorAngleI[2] = { 0, 0 };
    static uint32_t rcTime = 0;
    static int16_t initialThrottleHold;

    // this will return false if spektrum is disabled. shrug.
    if (spektrumFrameComplete())
        computeRC();

    if (currentTime > rcTime) { // 50Hz
        rcTime = currentTime + 20000;
        // TODO clean this up. computeRC should handle this check
        if (!feature(FEATURE_SPEKTRUM))
            computeRC();
        // Failsafe routine - added by MIS
#if defined(FAILSAFE)
        if (failsafeCnt > (5 * FAILSAVE_DELAY) && armed == 1) { // Stabilize, and set Throttle to specified level
            for (i = 0; i < 3; i++)
                rcData[i] = MIDRC;      // after specified guard time after RC signal is lost (in 0.1sec)
            rcData[THROTTLE] = FAILSAVE_THR0TTLE;
            if (failsafeCnt > 5 * (FAILSAVE_DELAY + FAILSAVE_OFF_DELAY)) {      // Turn OFF motors after specified Time (in 0.1sec)
                armed = 0;      //This will prevent the copter to automatically rearm if failsafe shuts it down and prevents
                okToArm = 0;    //to restart accidentely by just reconnect to the tx - you will have to switch off first to rearm
            }
            failsafeEvents++;
        }
        failsafeCnt++;
#endif
        // end of failsave routine - next change is made with RcOptions setting
        if (rcData[THROTTLE] < cfg.mincheck) {
            errorGyroI[ROLL] = 0;
            errorGyroI[PITCH] = 0;
            errorGyroI[YAW] = 0;
            errorAngleI[ROLL] = 0;
            errorAngleI[PITCH] = 0;
            rcDelayCommand++;
            if (rcData[YAW] < cfg.mincheck && rcData[PITCH] < cfg.mincheck && armed == 0) {
                if (rcDelayCommand == 20)
                    calibratingG = 400;
            } else if (feature(FEATURE_INFLIGHT_ACC_CAL) && (armed == 0 && rcData[YAW] < cfg.mincheck && rcData[PITCH] > cfg.maxcheck && rcData[ROLL] > cfg.maxcheck)) {
                if (rcDelayCommand == 20) {
                    if (AccInflightCalibrationMeasurementDone) {        // trigger saving into eeprom after landing
                        AccInflightCalibrationMeasurementDone = 0;
                        AccInflightCalibrationSavetoEEProm = 1;
                    } else {
                        AccInflightCalibrationArmed = !AccInflightCalibrationArmed;
                        if (AccInflightCalibrationArmed) {
                            blinkLED(10, 1, 2);
                        } else {
                            blinkLED(10, 10, 3);
                        }
                    }
                }
            } else if ((cfg.activate1[BOXARM] > 0) || (cfg.activate2[BOXARM] > 0)) {
                if (rcOptions[BOXARM] && okToArm) {
                    armed = 1;
                    headFreeModeHold = heading;
                } else if (armed)
                    armed = 0;
                rcDelayCommand = 0;
            } else if ((rcData[YAW] < cfg.mincheck || rcData[ROLL] < cfg.mincheck) && armed == 1) {
                if (rcDelayCommand == 20)
                    armed = 0;  // rcDelayCommand = 20 => 20x20ms = 0.4s = time to wait for a specific RC command to be acknowledged
            } else if ((rcData[YAW] > cfg.maxcheck || rcData[ROLL] > cfg.maxcheck) && rcData[PITCH] < cfg.maxcheck && armed == 0 && calibratingG == 0 && calibratedACC == 1) {
                if (rcDelayCommand == 20) {
                    armed = 1;
                    headFreeModeHold = heading;
                }
            } else
                rcDelayCommand = 0;
        } else if (rcData[THROTTLE] > cfg.maxcheck && armed == 0) {
            if (rcData[YAW] < cfg.mincheck && rcData[PITCH] < cfg.mincheck) {   // throttle=max, yaw=left, pitch=min
                if (rcDelayCommand == 20)
                    calibratingA = 400;
                rcDelayCommand++;
            } else if (rcData[YAW] > cfg.maxcheck && rcData[PITCH] < cfg.mincheck) {    // throttle=max, yaw=right, pitch=min
                if (rcDelayCommand == 20)
                    calibratingM = 1;   // MAG calibration request
                rcDelayCommand++;
            } else if (rcData[PITCH] > cfg.maxcheck) {
                cfg.accTrim[PITCH] += 2;
                writeParams();
#ifdef LEDRING
                if (feature(FEATURE_LED_RING))
                    ledringBlink();
#endif
            } else if (rcData[PITCH] < cfg.mincheck) {
                cfg.accTrim[PITCH] -= 2;
                writeParams();
#ifdef LEDRING
                if (feature(FEATURE_LED_RING))
                    ledringBlink();
#endif
            } else if (rcData[ROLL] > cfg.maxcheck) {
                cfg.accTrim[ROLL] += 2;
                writeParams();
#ifdef LEDRING
                if (feature(FEATURE_LED_RING))
                    ledringBlink();
#endif
            } else if (rcData[ROLL] < cfg.mincheck) {
                cfg.accTrim[ROLL] -= 2;
                writeParams();
#ifdef LEDRING
                if (feature(FEATURE_LED_RING))
                    ledringBlink();
#endif
            } else {
                rcDelayCommand = 0;
            }
        }
#ifdef LOG_VALUES
        if (cycleTime > cycleTimeMax)
            cycleTimeMax = cycleTime;   // remember highscore
        if (cycleTime < cycleTimeMin)
            cycleTimeMin = cycleTime;   // remember lowscore
#endif

        if (feature(FEATURE_INFLIGHT_ACC_CAL)) {
            if (AccInflightCalibrationArmed && armed == 1 && rcData[THROTTLE] > cfg.mincheck && !rcOptions[BOXARM]) {   // Copter is airborne and you are turning it off via boxarm : start measurement
                InflightcalibratingA = 50;
                AccInflightCalibrationArmed = 0;
            }
            if (rcOptions[BOXPASSTHRU]) {       // Use the Passthru Option to activate : Passthru = TRUE Meausrement started, Land and passtrhu = 0 measurement stored
                if (!AccInflightCalibrationArmed) {
                    AccInflightCalibrationArmed = 1;
                    InflightcalibratingA = 50;
                }
            } else if (AccInflightCalibrationMeasurementDone && armed == 0) {
                AccInflightCalibrationArmed = 0;
                AccInflightCalibrationMeasurementDone = 0;
                AccInflightCalibrationSavetoEEProm = 1;
            }
        }

        for (i = 0; i < CHECKBOXITEMS; i++) {
            rcOptions[i] = (((rcData[AUX1] < 1300) | (1300 < rcData[AUX1] && rcData[AUX1] < 1700) << 1 | (rcData[AUX1] > 1700) << 2 | (rcData[AUX2] < 1300) << 3 | (1300 < rcData[AUX2] && rcData[AUX2] < 1700) << 4 | (rcData[AUX2] > 1700) << 5) & cfg.activate1[i])
                || (((rcData[AUX3] < 1300) | (1300 < rcData[AUX3] && rcData[AUX3] < 1700) << 1 | (rcData[AUX3] > 1700) << 2 | (rcData[AUX4] < 1300) << 3 | (1300 < rcData[AUX4] && rcData[AUX4] < 1700) << 4 | (rcData[AUX4] > 1700) << 5) & cfg.activate2[i]);
        }

        // note: if FAILSAFE is disable, failsafeCnt > 5*FAILSAVE_DELAY is always false
        if ((rcOptions[BOXACC] || (failsafeCnt > 5 * FAILSAVE_DELAY)) && (sensors(SENSOR_ACC))) {
            // bumpless transfer to Level mode
            if (!accMode) {
                errorAngleI[ROLL] = 0;
                errorAngleI[PITCH] = 0;
                accMode = 1;
            }
        } else
            accMode = 0;        // modified by MIS for failsave support

        if ((rcOptions[BOXARM]) == 0)
            okToArm = 1;
        if (accMode == 1) {
            LED1_ON;
        } else {
            LED1_OFF;
        }

#ifdef BARO
        if (sensors(SENSOR_BARO)) {
            if (rcOptions[BOXBARO]) {
                if (baroMode == 0) {
                    baroMode = 1;
                    AltHold = EstAlt;
                    initialThrottleHold = rcCommand[THROTTLE];
                    errorAltitudeI = 0;
                    BaroPID = 0;
                }
            } else
                baroMode = 0;
        }
#endif

#ifdef  MAG
        if (sensors(SENSOR_MAG)) {
            if (rcOptions[BOXMAG]) {
                if (magMode == 0) {
                    magMode = 1;
                    magHold = heading;
                }
            } else
                magMode = 0;
            if (rcOptions[BOXHEADFREE]) {
                if (headFreeMode == 0) {
                    headFreeMode = 1;
                }
            } else
                headFreeMode = 0;
        }
#endif

        if (sensors(SENSOR_GPS)) {
            if (rcOptions[BOXGPSHOME]) {
                GPSModeHome = 1;
            } else
                GPSModeHome = 0;
            if (rcOptions[BOXGPSHOLD]) {
                if (GPSModeHold == 0) {
                    GPSModeHold = 1;
                    GPS_latitude_hold = GPS_latitude;
                    GPS_longitude_hold = GPS_longitude;
                }
            } else {
                GPSModeHold = 0;
            }
        }

        if (rcOptions[BOXPASSTHRU]) {
            passThruMode = 1;
        } else
            passThruMode = 0;
    } else {                    // not in rc loop
        static int8_t taskOrder = 0;    // never call all function in the same loop, to avoid high delay spikes
        switch (taskOrder) {
        case 0:
            taskOrder++;
#ifdef MAG
            if (sensors(SENSOR_MAG)){
                Mag_getADC();                
            } else if (sensors(SENSOR_GPS) && cfg.mixerConfiguration == MULTITYPE_FLYING_WING) {
                heading = GPS_heading;
            }
#endif
            break;
        case 1:
            taskOrder++;
#ifdef BARO
            if (sensors(SENSOR_BARO))
                Baro_update();
#endif
            break;
        case 2:
            taskOrder++;
#ifdef BARO
            if (sensors(SENSOR_BARO))
                getEstimatedAltitude();
#endif
            break;
        case 3:
            taskOrder++;
#if 0                           // GPS - not used as we read gps data in interrupt mode
            GPS_NewData();
#endif
            break;
        default:
            taskOrder = 0;
            break;
        }
    }

    computeIMU();
    // Measure loop rate just afer reading the sensors
    currentTime = micros();
    cycleTime = currentTime - previousTime;
    previousTime = currentTime;

#ifdef MPU6050_DMP
    mpu6050DmpLoop();
#endif

#ifdef MAG
    if (sensors(SENSOR_MAG)) {
        if (abs(rcCommand[YAW]) < 70 && magMode) {
            int16_t dif = heading - magHold;
            if (dif <= -180)
                dif += 360;
            if (dif >= +180)
                dif -= 360;
            if (smallAngle25)
                rcCommand[YAW] -= dif * cfg.P8[PIDMAG] / 30;    // 18 deg
        } else
            magHold = heading;
    }
#endif

#ifdef BARO
    if (sensors(SENSOR_BARO)) {
        if (baroMode) {
            if (abs(rcCommand[THROTTLE] - initialThrottleHold) > 20) {
                baroMode = 0;   // so that a new althold reference is defined
            }
            rcCommand[THROTTLE] = initialThrottleHold + BaroPID;
        }
    }
#endif

    if (sensors(SENSOR_GPS)) {
        uint16_t GPS_dist = 0;
        int16_t GPS_dir = 0;

        if ((GPSModeHome == 0 && GPSModeHold == 0) || (GPS_fix_home == 0)) {
            GPS_angle[ROLL] = 0;
            GPS_angle[PITCH] = 0;
        } else {
            float radDiff;
            if (GPSModeHome == 1) {
                GPS_dist = GPS_distanceToHome;
                GPS_dir = GPS_directionToHome;
            }
            if (GPSModeHold == 1) {
                GPS_dist = GPS_distanceToHold;
                GPS_dir = GPS_directionToHold;
            }
            radDiff = (GPS_dir - heading) * M_PI / 180.0f;
            GPS_angle[ROLL] = constrain(cfg.P8[PIDGPS] * sinf(radDiff) * GPS_dist / 10, -cfg.D8[PIDGPS] * 10, +cfg.D8[PIDGPS] * 10);     // with P=5.0, a distance of 1 meter = 0.5deg inclination
            GPS_angle[PITCH] = constrain(cfg.P8[PIDGPS] * cosf(radDiff) * GPS_dist / 10, -cfg.D8[PIDGPS] * 10, +cfg.D8[PIDGPS] * 10);    // max inclination = D deg
        }
    }

    // **** PITCH & ROLL & YAW PID ****    
    for (axis = 0; axis < 3; axis++) {
        if (accMode == 1 && axis < 2) { // LEVEL MODE
            // 50 degrees max inclination
            errorAngle = constrain(2 * rcCommand[axis] - GPS_angle[axis], -500, +500) - angle[axis] + cfg.accTrim[axis];        //16 bits is ok here
#ifdef LEVEL_PDF
            PTerm = -(int32_t)angle[axis] * cfg.P8[PIDLEVEL] / 100;
#else
            PTerm = (int32_t)errorAngle * cfg.P8[PIDLEVEL] / 100;       //32 bits is needed for calculation: errorAngle*P8[PIDLEVEL] could exceed 32768   16 bits is ok for result
#endif
            PTerm = constrain(PTerm, -cfg.D8[PIDLEVEL] * 5, +cfg.D8[PIDLEVEL] * 5);

            errorAngleI[axis] = constrain(errorAngleI[axis] + errorAngle, -10000, +10000);      // WindUp     // 16 bits is ok here
            ITerm = ((int32_t)errorAngleI[axis] * cfg.I8[PIDLEVEL]) >> 12;     // 32 bits is needed for calculation:10000*I8 could exceed 32768   16 bits is ok for result
        } else {                // ACRO MODE or YAW axis
            error = (int32_t)rcCommand[axis] * 10 * 8 / cfg.P8[axis];  //32 bits is needed for calculation: 500*5*10*8 = 200000   16 bits is ok for result if P8>2 (P>0.2)
            error -= gyroData[axis];

            PTerm = rcCommand[axis];

            errorGyroI[axis] = constrain(errorGyroI[axis] + error, -16000, +16000);     // WindUp // 16 bits is ok here
            if (abs(gyroData[axis]) > 640)
                errorGyroI[axis] = 0;
            ITerm = (errorGyroI[axis] / 125 * cfg.I8[axis]) >> 6;       // 16 bits is ok here 16000/125 = 128 ; 128*250 = 32000
        }
        PTerm -= (int32_t)gyroData[axis] * dynP8[axis] / 10 / 8;       // 32 bits is needed for calculation

        delta = gyroData[axis] - lastGyro[axis];        //16 bits is ok here, the dif between 2 consecutive gyro reads is limited to 800
        lastGyro[axis] = gyroData[axis];
        deltaSum = delta1[axis] + delta2[axis] + delta;
        delta2[axis] = delta1[axis];
        delta1[axis] = delta;

        DTerm = ((int32_t)deltaSum * dynD8[axis]) >> 5;        //32 bits is needed for calculation

        axisPID[axis] = PTerm + ITerm - DTerm;
    }

    mixTable();
    writeServos();
    writeMotors();
}
