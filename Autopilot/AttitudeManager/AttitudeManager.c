/* 
 * File:   AttitudeManager.c
 * Author: Mitch
 *
 * Created on June 15, 2013, 3:40 PM
 */
 
//Include Header Files
#include "delay.h"
#include "VN100.h"
#include "InputCapture.h"
#include "OutputCompare.h"
#include "net.h"
#include "PWM.h"
#include "AttitudeManager.h"
#include "commands.h"
#include "cameraManager.h"
#include "StartupErrorCodes.h"
#include "main.h"
#include "InterchipDMA.h"

#include "DisplayQuad.h"


extern PMData pmData;
extern AMData amData;
extern char DMADataAvailable;

long int lastTime = 0;
long int heartbeatTimer = 0;
long int UHFSafetyTimer = 0;
long int gpsTimer = 0;

float* velocityComponents;

// Setpoints (From radio transmitter or autopilot)
int sp_PitchRate = 0;
int sp_ThrottleRate = MIN_PWM;
int sp_YawRate = 0;
int sp_RollRate = 0;

int tail_OutputR;   //what the rudder used to be
int tail_OutputL;


int sp_ComputedPitchRate = 0;
//int sp_ComputedThrottleRate = 0;
int sp_ComputedRollRate = 0;
int sp_ComputedYawRate = 0;

char currentGain = 0;

int sp_PitchAngle = 0;
//float sp_YawAngle = 0;
int sp_RollAngle = 0;

//Heading Variables
int sp_Heading = 0;
int sp_HeadingRate = 0;

//Altitude Variables
int sp_Altitude = 0;
float sp_GroundSpeed = 0;

//GPS Data
int gps_Heading = 0;
float gps_GroundSpeed = 0; //NOTE: NEEDS TO BE IN METERS/SECOND. CALCULATIONS DEPEND ON THESE UNITS. GPS RETURNS KM/H.
float gps_Time = 0;
long double gps_Longitude = 0;
long double gps_Latitude = 0;
float gps_Altitude = 0;
char gps_Satellites = 0;
char gps_PositionFix = 0;
char waypointIndex = 0;
char waypointChecksum = 0;
char waypointCount = 0;
char batteryLevel = 0;


// System outputs (get from IMU)
float imuData[3];
float imu_RollRate = 0;
float imu_PitchRate = 0;
float imu_YawRate = 0;

//IMU integration outputs
float imu_RollAngle = 0;
float imu_PitchAngle = 0;
float imu_YawAngle = 0;

int rollTrim = 0;
int pitchTrim = 0;
int yawTrim = 0;

//Input Signals (Input Capture Values)
//Input
int input_Roll = 0;
int input_Pitch = 0;
int input_Throttle = 0;
int input_Yaw = 0;
int sp_Value = 0; //0=Roll, 1= Pitch, 2=Yaw
int sp_Type = 0; //0 = Saved Value, 1 = Edit Mode
int sp_Switch = 0;
int sp_UHFSwitch = 0;

//PID Global Variable Storage Values
int rollPID, pitchPID, throttlePID, yawPID;

float scaleFactor = 20; //Change this

char displayGain = 0;
int controlLevel = 0;
int lastCommandSentCode = 0;

int headingCounter = 0;
char altitudeTrigger = 0;

float refRotationMatrix[9];
float lastAltitude = 0;
long int lastAltitudeTime = 0;

char lastNumSatellites = 0;

unsigned int cameraCounter = 0;

void attitudeInit() {
    //Initialize Timer
    initTimer4();

    //Initialize Interchip communication
    TRISFbits.TRISF3 = 0;
    LATFbits.LATF3 = 1;

    TRISDbits.TRISD14 = 0;
    LATDbits.LATD14 = 0;

    amData.checkbyteDMA = generateAMDataDMAChecksum();

    //Initialize Interchip Interrupts for Use in DMA Reset
    //Set opposite Input / Output Configuration on the PathManager
    TRISAbits.TRISA12 = 0;  //Init RA12 as Output (0), (1) is Input
    INTERCOM_1 = 0;    //Set RA12 to Output a Value of 0
    TRISAbits.TRISA13 = 0;  //Init RA13 as Output (0), (1) is Input
    INTERCOM_2 = 0;    //Set RA13 to Output a Value of 0

    TRISBbits.TRISB4 = 1;   //Init RB4 as Input (1), (0) is Output
    TRISBbits.TRISB5 = 1;   //Init RB5 as Input (1), (0) is Output
    TRISAbits.TRISA3 = 0;
    PORTAbits.RA3 = 1;

    init_SPI1();
    init_DMA0();
    init_DMA1();


    /* Initialize Input Capture and Output Compare Modules */
#if DEBUG
    initPWM(0b10011111, 0b11111111);
    debug("INITIALIZATION - ATTITUDE MANAGER");
#else
    initPWM(0b10011111, 0b11111111);
#endif
    /* Initialize IMU with correct orientation matrix and filter settings */
    //In order: Angular Walk, Angular Rate x 3, Magnetometer x 3, Acceleration x 3
    float filterVariance[10] = {1e-9, 1e-9, 1e-9, 1e-9, 1, 1, 1, 1e-3, 1e-3, 1e-3}; //  float filterVariance[10] = {1e-10, 1e-6, 1e-6, 1e-6, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2};
    VN100_initSPI();
    //IMU position matrix
    // offset = {roll, pitch, yaw}
    float cal_roll = 0.0;
    float cal_pitch = 0.0;
    float cal_yaw = 0.0;
    float offset[3] = {cal_roll,cal_pitch,cal_yaw};
    setVNOrientationMatrix((float*)&offset);
    VN100_SPI_SetFiltMeasVar(0, (float*)&filterVariance);
    initialization();
}


char checkDMA(){
    //Transfer data from PATHMANAGER CHIP
    lastNumSatellites = gps_Satellites; //get the last number of satellites
    DMADataAvailable = 0;

    if (generatePMDataDMAChecksum() == pmData.checkbyteDMA) {
        //Check if this data is new and requires action or if it is old and redundant
        if (gps_Altitude == pmData.altitude && gps_Heading == pmData.heading && gps_GroundSpeed == pmData.speed && gps_Latitude == pmData.latitude && gps_Longitude == pmData.longitude){
            return FALSE;
        }
        gps_Time = pmData.time;
        gps_Heading = pmData.heading;
        gps_GroundSpeed = pmData.speed * 1000.0/3600.0; //Convert from km/h to m/s
        gps_Longitude = pmData.longitude;
        gps_Latitude = pmData.latitude;
        gps_Altitude = pmData.altitude;
        gps_Satellites = pmData.satellites;
        gps_PositionFix = pmData.positionFix;
        if (controlLevel & ALTITUDE_CONTROL_SOURCE)
            sp_Altitude = pmData.sp_Altitude;
        if (controlLevel & HEADING_CONTROL_SOURCE){
            if (gps_PositionFix){
                sp_Heading = pmData.sp_Heading;
            }
        }
        waypointIndex = pmData.targetWaypoint;
        batteryLevel = pmData.batteryLevel;
        waypointCount = pmData.waypointCount;
    }
    return FALSE;
}

float getAltitude(){
    return gps_Altitude;
}
int getHeading(){
    return gps_Heading;
}
long double getLongitude(){
    return gps_Longitude;
}
long double getLatitude(){
    return gps_Latitude;
}
float getRoll(){
    return imu_RollAngle;
}
float getPitch(){
    return imu_PitchAngle;
}
float getYaw(){
    return imu_YawAngle;
}
float getRollRate(){
    return imu_RollRate;
}
float getPitchRate(){
    return imu_PitchRate;
}
float getYawRate(){
    return imu_YawRate;
}
int getAltitudeSetpoint(){
    return sp_Altitude;
}
int getHeadingSetpoint(){
    return sp_Heading;
}
int getRollAngleSetpoint(){
    return sp_RollAngle;
}
int getPitchAngleSetpoint(){
    return sp_PitchAngle;
}
int getPitchRateSetpoint(){
    return sp_PitchRate;
}
int getRollRateSetpoint(){
    return sp_RollRate;
}
int getYawRateSetpoint(){
    return sp_YawRate;
}

void setPitchAngleSetpoint(int setpoint){
    sp_PitchAngle = setpoint;
}
void setRollAngleSetpoint(int setpoint){
    sp_RollAngle = setpoint;
}
void setPitchRateSetpoint(int setpoint){
    sp_PitchRate = setpoint;
}
void setRollRateSetpoint(int setpoint){
    sp_RollRate = setpoint;
}
void setYawRateSetpoint(int setpoint){
    sp_YawRate = setpoint;
}

void inputCapture(){
//    int* channelIn;
//    channelIn = getPWMArray();
//    inputMixing(channelIn, &input_Roll, &input_Pitch, &input_Throttle, &input_Yaw);
//
//    // Switches and Knobs
//    sp_UHFSwitch = channelIn[4];
////        sp_Type = channelIn[5];
////        sp_Value = channelIn[6];
//    sp_Switch = channelIn[7];
//
//    //Controller Input Interpretation Code
//    if (sp_Switch > MIN_PWM && sp_Switch < MIN_PWM + 50) {
//        unfreezeIntegral();
//    } else {
//        freezeIntegral();
//    }
}

void imuCommunication(){
    /*****************************************************************************
     *****************************************************************************
                                IMU COMMUNICATION
     *****************************************************************************
     *****************************************************************************/
    VN100_SPI_GetRates(0, (float*) &imuData);

    //TODO: This is a reminder for me to figure out a more elegant way to fix improper derivative control (based on configuration of the sensor), adding this negative is a temporary fix. Some kind of calibration command or something.
    //DO NOT ADD NEGATIVES IN THE STATEMENTS BELOW. IT IS A GOOD WAY TO ROYALLY SCREW YOURSELF OVER LATER.
    //Outputs in order: Roll,Pitch,Yaw
    imu_RollRate = (imuData[IMU_ROLL_RATE]);
    imu_PitchRate = imuData[IMU_PITCH_RATE];
    imu_YawRate = imuData[IMU_YAW_RATE];
    VN100_SPI_GetYPR(0, &imuData[YAW], &imuData[PITCH], &imuData[ROLL]);
    imu_YawAngle = imuData[YAW];
    imu_PitchAngle = imuData[PITCH];
    imu_RollAngle = (imuData[ROLL]);

    // Rate - Radians, Angle - Degrees
    char x[30];
    sprintf(&x, "IMU Roll Rate: %f", imu_RollRate);
    debug(&x);
    sprintf(&x, "IMU Pitch Rate: %f", imu_PitchRate);
    debug(&x);
    sprintf(&x, "IMU Pitch Angle: %f", imu_PitchAngle);
    debug(&x);
    sprintf(&x, "IMU Roll Angle: %f", imu_RollAngle);
    debug(&x);
}

int altitudeControl(int setpoint, int sensorAltitude){
    //Altitude
    if (controlLevel & ALTITUDE_CONTROL){
        sp_PitchAngle = controlSignalAltitude(setpoint, sensorAltitude);
        if (sp_PitchAngle > MAX_PITCH_ANGLE)
            sp_PitchAngle = MAX_PITCH_ANGLE;
        if (sp_PitchAngle < -MAX_PITCH_ANGLE)
            sp_PitchAngle = -MAX_PITCH_ANGLE;
    }
    return sp_PitchAngle;
}

int throttleControl(int setpoint, int sensor){
    //Throttle
    if ((THROTTLE_CONTROL_SOURCE & controlLevel) >> 4 >= 1){
        throttlePID = sp_ThrottleRate + controlSignalThrottle(setpoint, sensor);
    }
    else
        throttlePID = sp_ThrottleRate;
        
    return throttlePID;
}

//Equivalent to "Yaw Angle Control"
int headingControl(int setpoint, int sensor){
    //Heading
    if (controlLevel & HEADING_CONTROL){
        //Estimation of Roll angle based on heading:

        while (setpoint > 360)
            setpoint -=360;
        while (setpoint < 0)
            setpoint +=360;
        // -(maxHeadingRate)/180.0,
            sp_HeadingRate = controlSignalHeading(setpoint, sensor);//gps_Satellites>=4?gps_Heading:(int)imu_YawAngle); //changed to monitor satellites, since we know these are good values while PositionFix might be corrupt...
            //Approximating Roll angle from Heading
            sp_RollAngle = sp_HeadingRate;      //TODO: HOW IS HEADING HANDLED DIFFERENTLY BETWEEN QUADS AND PLANES

        if (sp_RollAngle > MAX_ROLL_ANGLE)
            sp_RollAngle = MAX_ROLL_ANGLE;
        if (sp_RollAngle < -MAX_ROLL_ANGLE)
            sp_RollAngle = -MAX_ROLL_ANGLE;
    }
    return sp_RollAngle;
}


    // If we are getting input from the controller convert sp_xxxxRate to an sp_xxxxAngle in degrees
//    if ((controlLevel & ROLL_CONTROL_SOURCE) == 0 && (controlLevel & HEADING_CONTROL_ON) == 0)
//        sp_RollAngle = (int)((sp_RollRate / ((float)SP_RANGE / MAX_ROLL_ANGLE) ));
//    if ((controlLevel & PITCH_CONTROL_SOURCE) == 0 && (controlLevel & ALTITUDE_CONTROL_ON) == 0)
//        sp_PitchAngle = (int)(sp_PitchRate / ((float)SP_RANGE / MAX_PITCH_ANGLE));

int rollAngleControl(int setpoint, int sensor){
    //Roll Angle
    if (controlLevel & ROLL_CONTROL_TYPE || controlLevel & HEADING_CONTROL){
        sp_ComputedRollRate = controlSignalAngles(setpoint, sensor, ROLL, -(SP_RANGE) / (MAX_ROLL_ANGLE));
    }
    else{
        sp_ComputedRollRate = -sp_RollRate;
    }
    return sp_ComputedRollRate;
}

int pitchAngleControl(int setpoint, int sensor){
    //Pitch Angle
    if (controlLevel & PITCH_CONTROL_TYPE || controlLevel & ALTITUDE_CONTROL){
        sp_ComputedPitchRate = controlSignalAngles(setpoint, sensor, PITCH, -(SP_RANGE) / (MAX_PITCH_ANGLE)); //Removed negative
    }
    else{
        sp_ComputedPitchRate = -sp_PitchRate;
    }
    return sp_ComputedPitchRate;
}

int coordinatedTurn(float pitchRate, int rollAngle){
    //Feed forward Term when turning
    if (controlLevel & ALTITUDE_CONTROL){
        pitchRate -= abs((int)(scaleFactor * rollAngle)); //Linear Function
    }
    return pitchRate;
}

int rollRateControl(int setpoint, int sensor){
    rollPID = controlSignal(setpoint, sensor, ROLL);
    return rollPID;
}
int pitchRateControl(int setpoint, int sensor){
    pitchPID = controlSignal(setpoint, sensor, PITCH);
    return pitchPID;
}
int yawRateControl(int setpoint, int sensor){
    yawPID = controlSignal(setpoint, sensor, YAW);
    return yawPID;
}

char getControlPermission(int controlMask, int expectedValue){
    return (controlMask & controlLevel) == expectedValue;
}


void readDatalink(void){
  
    struct command* cmd = popCommand();
    //TODO: Add rudimentary input validation
    if ( cmd ) {
        if (lastCommandSentCode/100 == cmd->cmd){
            lastCommandSentCode++;
        }
        else{
            lastCommandSentCode = cmd->cmd * 100;
        }
        switch (cmd->cmd) {
            case DEBUG_TEST:             // Debugging command, writes to debug UART
#if DEBUG
                debug("Foo");
                debug( (char*) cmd->data);
#endif
                break;
            case SET_PITCH_KD_GAIN:
                setGain(PITCH, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KD_GAIN:
                setGain(ROLL, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_YAW_KD_GAIN:
                setGain(YAW, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_PITCH_KP_GAIN:
                setGain(PITCH, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KP_GAIN:
                setGain(ROLL, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_YAW_KP_GAIN:
                setGain(YAW, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_PITCH_KI_GAIN:
                setGain(PITCH, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KI_GAIN:
                setGain(ROLL, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_YAW_KI_GAIN:
                setGain(YAW, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KD_GAIN:
                setGain(HEADING, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KP_GAIN:   
                setGain(HEADING, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KI_GAIN:
                setGain(HEADING, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KD_GAIN:
                setGain(ALTITUDE, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KP_GAIN:
                setGain(ALTITUDE, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KI_GAIN:
                setGain(ALTITUDE, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KD_GAIN:
                setGain(THROTTLE, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KP_GAIN:
                setGain(THROTTLE, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KI_GAIN:
                setGain(THROTTLE, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_PATH_GAIN:
                amData.pathGain = *(float*)(&cmd->data);
                amData.command = PM_SET_PATH_GAIN;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_ORBIT_GAIN:
                amData.orbitGain = *(float*)(&cmd->data);
                amData.command = PM_SET_ORBIT_GAIN;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SHOW_GAIN:
                displayGain = *(char*)(&cmd->data);
                break;
            case SET_PITCH_RATE:
                sp_PitchRate = *(int*)(&cmd->data);
                break;
            case SET_ROLL_RATE:
                sp_RollRate = *(int*)(&cmd->data);
                break;
            case SET_YAW_RATE:
                sp_YawRate = *(int*)(&cmd->data);
                break;
            case SET_PITCH_ANGLE:
                setPitchAngleSetpoint(*(int*)(&cmd->data));
                break;
            case SET_ROLL_ANGLE:
                setRollAngleSetpoint(*(int*)(&cmd->data));
                break;
            case SET_YAW_ANGLE:
//                sp_YawAngle = *(int*)(&cmd->data);
                break;
            case SET_ALTITUDE:
                sp_Altitude = *(int*)(&cmd->data);
                break;
            case SET_HEADING:
                sp_Heading = *(int*)(&cmd->data);
                break;
            case SET_THROTTLE:
                sp_ThrottleRate = (int)(((long int)(*(int*)(&cmd->data))) * MAX_PWM * 2 / 100) - MAX_PWM;
                break;
            case SET_AUTONOMOUS_LEVEL:
                controlLevel = *(int*)(&cmd->data);
                break;
            case SET_ANGULAR_WALK_VARIANCE:
                setAngularWalkVariance(*(float*)(&cmd->data));
                break;
            case SET_GYRO_VARIANCE:
                setGyroVariance(*(float*)(&cmd->data));
                break;
            case SET_MAGNETIC_VARIANCE:
                setMagneticVariance(*(float*)(&cmd->data));
                break;
            case SET_ACCEL_VARIANCE:
                setAccelVariance(*(float*)(&cmd->data));
                break;
            case SET_SCALE_FACTOR:
                scaleFactor = *(float*)(&cmd->data);
                break;
            case CALIBRATE_ALTIMETER:
                amData.calibrationHeight = *(float*)(&cmd->data);
                amData.command = PM_CALIBRATE_ALTIMETER;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case CLEAR_WAYPOINTS:
                amData.waypoint.id = (*(char *)(&cmd->data)); //Dummy Data
                amData.command = PM_CLEAR_WAYPOINTS;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case REMOVE_WAYPOINT:
                amData.waypoint.id = (*(char *)(&cmd->data));
                amData.command = PM_REMOVE_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_TARGET_WAYPOINT:
                amData.waypoint.id = *(char *)(&cmd->data);
                amData.command = PM_SET_TARGET_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case RETURN_HOME:
                amData.command = PM_RETURN_HOME;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case CANCEL_RETURN_HOME:
                amData.command = PM_CANCEL_RETURN_HOME;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SEND_HEARTBEAT:
                    heartbeatTimer = getTime();
                break;
            case TRIGGER_CAMERA:
                triggerCamera(*(unsigned int*)(&cmd->data));
                break;
            case SET_TRIGGER_DISTANCE:
                setTriggerDistance(*(float*)(&cmd->data));
                break;
            case SET_GIMBLE_OFFSET:
                setGimbalOffset(*(unsigned int*)(&cmd->data));
                break;
            case KILL_PLANE:
                if (*(int*)(&cmd->data) == 1234)
//                    killingPlane = 1;
                break;
            case UNKILL_PLANE:
                if (*(int*)(&cmd->data) == 1234)
//                    killingPlane = 0;
                break;
            case LOCK_GOPRO:
                    lockGoPro(*(int*)(&cmd->data));
                break;
            case ARM_VEHICLE:
                if (*(int*)(&cmd->data) == 1234)
                    startArm();
                break;

            case DEARM_VEHICLE:
                if (*(int*)(&cmd->data) == 1234)
                    stopArm();
                break;

            case NEW_WAYPOINT:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.id = (*(WaypointWrapper*)(&cmd->data)).id;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.waypoint.radius = (*(WaypointWrapper*)(&cmd->data)).radius;
                amData.command = PM_NEW_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case INSERT_WAYPOINT:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.waypoint.radius = (*(WaypointWrapper*)(&cmd->data)).radius;
                amData.waypoint.nextId = (*(WaypointWrapper*)(&cmd->data)).nextId;
                amData.waypoint.previousId = (*(WaypointWrapper*)(&cmd->data)).previousId;
                amData.command = PM_INSERT_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_RETURN_HOME_COORDINATES:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.command = PM_SET_RETURN_HOME_COORDINATES;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case TARE_IMU:
                adjustVNOrientationMatrix((float*)(&cmd->data));
                break;
            case SET_IMU:
                setVNOrientationMatrix((float*)(&cmd->data));
                break;
            default:
                break;
        }
        destroyCommand( cmd );
    }
 
}
int writeDatalink(){
     
    struct telem_block* statusData = createTelemetryBlock();//getDebugTelemetryBlock();

    statusData->lat = gps_Latitude;
    statusData->lon = gps_Longitude;
    statusData->millis = gps_Time;
    statusData->pitch = imu_PitchAngle;
    statusData->roll = imu_RollAngle;
    statusData->yaw = imu_YawAngle;
    statusData->pitchRate = imu_PitchRate;
    statusData->rollRate = imu_RollRate;
    statusData->yawRate = imu_YawRate;
    statusData->pitch_gain = getGain(displayGain, GAIN_KD);
    statusData->roll_gain = getGain(displayGain, GAIN_KP);
    statusData->yaw_gain = getGain(displayGain, GAIN_KI);
    statusData->groundSpeed = gps_GroundSpeed;
    statusData->altitude = gps_Altitude;
    statusData->heading = gps_Heading;
    statusData->pitchSetpoint = sp_PitchAngle;
    statusData->rollSetpoint = sp_RollAngle;
    statusData->headingSetpoint = sp_Heading;
    statusData->throttleSetpoint = (int)(((long int)(sp_ThrottleRate + MAX_PWM) * 100) / MAX_PWM/2);
    statusData->altitudeSetpoint = sp_Altitude;
    statusData->cPitchSetpoint = sp_PitchRate;
    statusData->cRollSetpoint = sp_RollRate;
    statusData->cYawSetpoint = sp_YawRate;
    statusData->lastCommandSent = lastCommandSentCode;
    statusData->errorCodes = getErrorCodes() + ((sp_UHFSwitch < -429)<< 11);
    statusData->cameraStatus = cameraCounter;
    statusData->waypointIndex = waypointIndex;
    statusData->editing_gain = displayGain + ((sp_Switch > 380) << 4);
    statusData->gpsStatus = gps_Satellites + (gps_PositionFix << 4);
    statusData->batteryLevel = batteryLevel;
    statusData->waypointCount = waypointCount;


    if (BLOCKING_MODE) {
        sendTelemetryBlock(statusData);
        destroyTelemetryBlock(statusData);
    } else {
        return pushOutboundTelemetryQueue(statusData);
    }
         
    return 0;

}

void adjustVNOrientationMatrix(float* adjustment){

    adjustment[0] = deg2rad(adjustment[0]);
    adjustment[1] = deg2rad(adjustment[1]);
    adjustment[2] = deg2rad(adjustment[2]);

    float matrix[9];
    VN100_SPI_GetRefFrameRot(0, (float*)&matrix);
    
    float refRotationMatrix[9] = {cos(adjustment[1]) * cos(adjustment[2]), -cos(adjustment[1]) * sin(adjustment[2]), sin(adjustment[1]),
        sin(deg2rad(adjustment[0])) * sin(adjustment[1]) * cos(adjustment[2]) + sin(adjustment[2]) * cos(adjustment[0]), -sin(adjustment[0]) * sin(adjustment[1]) * sin(adjustment[2]) + cos(adjustment[2]) * cos(adjustment[0]), -sin(adjustment[0]) * cos(adjustment[1]),
        -cos(deg2rad(adjustment[0])) * sin(adjustment[1]) * cos(adjustment[2]) + sin(adjustment[2]) * sin(adjustment[0]), cos(adjustment[0]) * sin(adjustment[1]) * sin(adjustment[2]) + cos(adjustment[2]) * sin(adjustment[0]), cos(adjustment[0]) * cos(adjustment[1])};

    int i = 0;
    for (i = 0; i < 9; i++){
        refRotationMatrix[i] += matrix[i];
    }

    VN100_SPI_SetRefFrameRot(0, (float*)&refRotationMatrix);
    VN100_SPI_WriteSettings(0);
    VN100_SPI_Reset(0);

}

void setVNOrientationMatrix(float* angleOffset){
    //angleOffset[0] = x, angleOffset[1] = y, angleOffset[2] = z
    angleOffset[0] = deg2rad(angleOffset[0]);
    angleOffset[1] = deg2rad(angleOffset[1]);
    angleOffset[2] = deg2rad(angleOffset[2]);

    refRotationMatrix[0] = cos(angleOffset[1]) * cos(angleOffset[2]);
    refRotationMatrix[1] = -cos(angleOffset[1]) * sin(angleOffset[2]);
    refRotationMatrix[2] = sin(angleOffset[1]);

    refRotationMatrix[3] = sin(angleOffset[0]) * sin(angleOffset[1]) * cos(angleOffset[2]) + sin(angleOffset[2]) * cos(angleOffset[0]);
    refRotationMatrix[4] = -sin(angleOffset[0]) * sin(angleOffset[1]) * sin(angleOffset[2]) + cos(angleOffset[2]) * cos(angleOffset[0]);
    refRotationMatrix[5] = -sin(angleOffset[0]) * cos(angleOffset[1]);

    refRotationMatrix[6] = -cos(angleOffset[0]) * sin(angleOffset[1]) * cos(angleOffset[2]) + sin(angleOffset[2]) * sin(angleOffset[0]);
    refRotationMatrix[7] = cos(angleOffset[0]) * sin(angleOffset[1]) * sin(angleOffset[2]) + cos(angleOffset[2]) * sin(angleOffset[0]);
    refRotationMatrix[8] = cos(angleOffset[0]) * cos(angleOffset[1]);
    VN100_SPI_SetRefFrameRot(0, (float*)&refRotationMatrix);
    VN100_SPI_WriteSettings(0);
    VN100_SPI_Reset(0);
}
void setAngularWalkVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[0] = variance;
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setGyroVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[1] = variance; //X -Can be split up later if needed
    previousVariance[2] = variance; //Y
    previousVariance[3] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setMagneticVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[4] = variance; //X -Can be split up later if needed
    previousVariance[5] = variance; //Y
    previousVariance[6] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setAccelVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[7] = variance; //X -Can be split up later if needed
    previousVariance[8] = variance; //Y
    previousVariance[9] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

char generateAMDataDMAChecksum(void){
    return 0xAB;
}

char generateAMDataChecksum(AMData* data){
    char checksum = 0;
    int i = 0;
    //Two checksums and padding = 3 bytes
    for (i = 0; i < sizeof(AMData) - 3; i++){
        checksum += ((char*)data)[i];
    }
    return checksum;
}
