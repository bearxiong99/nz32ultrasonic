/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    (C)2013 Semtech
Description: LoRaMac classA device implementation
License: Revised BSD License, see LICENSE.TXT file include in the project
Maintainer: Miguel Luis and Gregory Cristian
*/
//#include <string.h>
//#include <math.h>
#include "mbed.h"
#include "board.h"
#include "radio.h"
#include "LoRaMac.h"
#include "Comissioning.h"
#include "bcd.h"

#include "ME007-ULS-V1.h"
#include "WakeUp.h"

Serial pc(USBTX, USBRX);

/*!
 * Join requests trials duty cycle.
 */
#define OVER_THE_AIR_ACTIVATION_DUTYCYCLE           6000000 // 6 [s] value in us

#define APP_TX_DUTYCYCLE                            5000000 // us
#define APP_TX_DUTYCYCLE_RND                        1000000 // us

#define LORAWAN_DEFAULT_DATARATE                    DR_0

#define LORAWAN_CONFIRMED_MSG_ON                    true

/*!
 * LoRaWAN Adaptive Data Rate
 *
 * \remark Please note that when ADR is enabled the end-device should be static
 */
#define LORAWAN_ADR_ON                              1

#if defined( USE_BAND_868 )

#define LORAWAN_DUTYCYCLE_ON                        false  // Not used for NZ865

 /*!
 *  NB - EU Channels to support NZ865 (KotahiNet Frequencies)
 *  are setup in LoRaMAC.cpp
 *
 */

#endif

#define LORAWAN_APP_PORT                            15

 /*!
 *  Application data size is constrained by maximum for default data rate e.g. D0
 */
#define LORAWAN_APP_DATA_SIZE                       30


#if( OVER_THE_AIR_ACTIVATION != 0 )
static uint8_t DevEui[] = LORAWAN_DEVICE_EUI;
static uint8_t AppEui[] = LORAWAN_APPLICATION_EUI;
static uint8_t AppKey[] = LORAWAN_APPLICATION_KEY;
#else
static uint8_t NwkSKey[] = LORAWAN_NWKSKEY;
static uint8_t AppSKey[] = LORAWAN_APPSKEY;
/*!
 * Device address
 */
static uint32_t DevAddr = LORAWAN_DEVICE_ADDRESS;
#endif

/*!
 * Application port
 */
static uint8_t AppPort = LORAWAN_APP_PORT;

/*!
 * User application data size
 */
static uint8_t AppDataSize = LORAWAN_APP_DATA_SIZE;
/*!
 * User application data buffer size
 */
#define LORAWAN_APP_DATA_MAX_SIZE                           64

/*!
 * User application data
 */
static uint8_t AppData[LORAWAN_APP_DATA_MAX_SIZE];

/*!
 * Indicates if the node is sending confirmed or unconfirmed messages
 */
static uint8_t IsTxConfirmed = LORAWAN_CONFIRMED_MSG_ON;

/*!
 * Defines the application data transmission duty cycle
 */
static uint32_t TxDutyCycleTime;

/*!
 * Timer to handle the application data transmission duty cycle
 */
static TimerEvent_t TxNextPacketTimer;

/**
*   Sensor Support
*/

#define DEEPSLEEP_SECONDS   20

UltraSonic sensor (PA_1, PA_3); // Trigger, Receive

/*!
 * Timer to handle ultrasonic sensor failures
 */
static TimerEvent_t ultrasonicTimer;

int noxt = 0;
//Timeout txmessage;



/** Ultrasonic Sensor states */
enum sensorstates {INIT=0, TRIGGERED, SENDING, SENT, FAULT};
/**
* INIT - before sensor is first triggered
* TRIGGERED - Sensor is collecting readings, sets distanceAvailable to true when available
* SENT - Range and temperature readings have been sent
*/

sensorstates SensorState = INIT;
static void txSensor(void);
static void txSensorError(void);

/************************************************************************/

/*!
 * Specifies the state of the application LED
 */
//static bool AppLedStateOn = false;

static TimerEvent_t Led1Timer;

/*!
 * Indicates if a new packet can be sent
 */
static bool NextTx = true;

/*!
 * Device states
 */
static enum eDevicState
{
    DEVICE_STATE_INIT,
    DEVICE_STATE_JOIN,
    DEVICE_STATE_SEND,
    DEVICE_STATE_CYCLE,
    DEVICE_STATE_SLEEP
}DeviceState;

/*!
 * LoRaWAN compliance tests support data
 */
struct ComplianceTest_s
{
    bool Running;
    uint8_t State;
    bool IsTxConfirmed;
    uint8_t AppPort;
    uint8_t AppDataSize;
    uint8_t *AppDataBuffer;
    uint16_t DownLinkCounter;
    bool LinkCheck;
    uint8_t DemodMargin;
    uint8_t NbGateways;
}ComplianceTest;

/*!
 * \brief   Prepares the payload of the frame
 */
static void PrepareTxFrame( uint8_t port )
{
    switch( port )
    {
    case 15:
        {
              pc.printf("PrepareTxFrame\r\n");
              pc.printf("SensorState = %d, distanceAvailable = ",SensorState);
              if (sensor.distanceAvailable)
                     pc.printf("true\r\n");
              else
                     pc.printf("false\r\n");
            if (SensorState == FAULT) {
                txSensorError();
                SensorState = INIT; // Try Again
                break;
            }
            
            if (/*(SensorState == SENDING) && */(sensor.distanceAvailable)) { /* Ultrasonic reading is ready to send */
                txSensor(); // Prepare sensor data
                SensorState = SENT;
            }
            else {
                memcpy(AppData, "Unexpected Msg",14);
                AppDataSize = 14;
            }    
        }
        break;
        case 22:
        {
            memcpy(AppData, "Goodbye",7);
            AppDataSize = 7;
        }
        break;
    case 224:
        if( ComplianceTest.LinkCheck == true )
        {
            ComplianceTest.LinkCheck = false;
            AppDataSize = 3;
            AppData[0] = 5;
            AppData[1] = ComplianceTest.DemodMargin;
            AppData[2] = ComplianceTest.NbGateways;
            ComplianceTest.State = 1;
        }
        else
        {
            switch( ComplianceTest.State )
            {
            case 4:
                ComplianceTest.State = 1;
                break;
            case 1:
                AppDataSize = 2;
                AppData[0] = ComplianceTest.DownLinkCounter >> 8;
                AppData[1] = ComplianceTest.DownLinkCounter;
                break;
            }
        }
        break;
    default:
        break;
    }
}

/*!
 * \brief   Prepares the payload of the frame
 *
 * \retval  [0: frame could be send, 1: error]
 */
static bool SendFrame( void )
{
      pc.printf("SendFrame\r\n");
    McpsReq_t mcpsReq;
    LoRaMacTxInfo_t txInfo;
    
    if( LoRaMacQueryTxPossible( AppDataSize, &txInfo ) != LORAMAC_STATUS_OK )  // Unable to send frame 
    {
          pc.printf("SendFrame - Unable to Send\r\n");
        // Send empty frame in order to flush MAC commands
        mcpsReq.Type = MCPS_UNCONFIRMED;
        mcpsReq.Req.Unconfirmed.fBuffer = NULL;
        mcpsReq.Req.Unconfirmed.fBufferSize = 0;
        mcpsReq.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
    }
    else  // Frame is ok to send
    {
        if( IsTxConfirmed == false )  // Configured at start of main.cpp
        {
              pc.printf("SendFrame - Unconfirmed\r\n");
            mcpsReq.Type = MCPS_UNCONFIRMED;
            mcpsReq.Req.Unconfirmed.fPort = AppPort;
            mcpsReq.Req.Unconfirmed.fBuffer = AppData;
            mcpsReq.Req.Unconfirmed.fBufferSize = AppDataSize;
            mcpsReq.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
        }
        else
        {
              pc.printf("SendFrame - Confirmed\r\n");
            mcpsReq.Type = MCPS_CONFIRMED;
            mcpsReq.Req.Confirmed.fPort = AppPort;
            mcpsReq.Req.Confirmed.fBuffer = AppData;
            mcpsReq.Req.Confirmed.fBufferSize = AppDataSize;
            mcpsReq.Req.Confirmed.NbTrials = 8; // number of retries in case of failure to confirm
            mcpsReq.Req.Confirmed.Datarate = LORAWAN_DEFAULT_DATARATE;
        }
    }

    /*  Possible return values of LoRaMacMcpsRequest are:
     *          \ref LORAMAC_STATUS_OK,
     *          \ref LORAMAC_STATUS_BUSY,
     *          \ref LORAMAC_STATUS_SERVICE_UNKNOWN,
     *          \ref LORAMAC_STATUS_PARAMETER_INVALID,
     *          \ref LORAMAC_STATUS_NO_NETWORK_JOINED,
     *          \ref LORAMAC_STATUS_LENGTH_ERROR,
     *          \ref LORAMAC_STATUS_DEVICE_OFF.
     */
      pc.printf("SendFrame  LoRaMacMcpsRequest Status = %d\r\n", LoRaMacMcpsRequest( &mcpsReq ) );
    if( LoRaMacMcpsRequest( &mcpsReq ) == LORAMAC_STATUS_OK ) 
    {
        return false; // NextTx is set to this, so don't resend
    }
    return true;  // NextTx is set to this, so will resend if true
}

/*!
 * \brief  Function executed on TxNextPacket Timeout event
 *  If network not joined then changes state to DEVICE_STATE_JOIN
 *  else changes state to DEVICE_STATE_SEND
 */
static void OnTxNextPacketTimerEvent( void )
{
    pc.printf("OnTxNextPacketTimerEvent\r\n");
    MibRequestConfirm_t mibReq;
    LoRaMacStatus_t status;
    TimerStop( &TxNextPacketTimer );
    mibReq.Type = MIB_NETWORK_JOINED;
    status = LoRaMacMibGetRequestConfirm( &mibReq ); // Check the MAC status
    if( status == LORAMAC_STATUS_OK )
    {
        if( mibReq.Param.IsNetworkJoined == true )
        {
            DeviceState = DEVICE_STATE_SEND;
            pc.printf("Set device state to send\r\n");
            // NextTx = true;
        }
        else
        {
            DeviceState = DEVICE_STATE_JOIN;
            pc.printf("Set device state to join\r\n");
        }
        NextTx = true; // Must be set to true to either send message or join
    }
}

/*!
 * \brief  Function executed on ultrasonic sensor timeout event
 *  If range and temperature readings not available in reasonable time
 *  sends sensor data with range and temperature in error 
 */
static void OnUltrasonicTimeout ( void ) {

    pc.printf("OnUltrasonicTimeout - Sensor FAULT\r\n");
    SensorState = FAULT;
    NextTx = true;
    TimerSetValue( &TxNextPacketTimer, 100 ); // Schedule immediate transmission
    TimerStart( &TxNextPacketTimer );

}

/*!
 * \brief Function executed on Led 1 Timeout event
 */
static void OnLed1TimerEvent( void )
{
    TimerStop( &Led1Timer );
    // Switch LED 1 OFF
    // Led1State = false;
}

/*!
 * \brief   MCPS-Confirm event function
 *
 * \param   [IN] mcpsConfirm - Pointer to the confirm structure,
 *               containing confirm attributes.
 */
static void McpsConfirm( McpsConfirm_t *mcpsConfirm )
{
    if( mcpsConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
    {
        switch( mcpsConfirm->McpsRequest )
        {
            case MCPS_UNCONFIRMED:
            {
                // Check Datarate
                // Check TxPower
                break;
            }
            case MCPS_CONFIRMED:
            {
                // Check Datarate
                // Check TxPower
                // Check AckReceived
                // Check NbTrials
                break;
            }
            case MCPS_PROPRIETARY:
            {
                break;
            }
            default:
                break;
        }
        // Switch LED 1 ON
       // GpioWrite( &Led1, 0 );
        TimerStart( &Led1Timer );
    }
    NextTx = true;
}

/*!
 * \brief   MCPS-Indication event function
 *
 * \param   [IN] mcpsIndication - Pointer to the indication structure,
 *               containing indication attributes.
 */
static void McpsIndication( McpsIndication_t *mcpsIndication )
{
    if( mcpsIndication->Status != LORAMAC_EVENT_INFO_STATUS_OK )
    {
        return;
    }
    switch( mcpsIndication->McpsIndication )
    {
        case MCPS_UNCONFIRMED:
        {
            break;
        }
        case MCPS_CONFIRMED:
        {
            break;
        }
        case MCPS_PROPRIETARY:
        {
            break;
        }
        case MCPS_MULTICAST:
        {
            break;
        }
        default:
            break;
    }
    // Check Multicast
    // Check Port
    // Check Datarate
    // Check FramePending
    // Check Buffer
    // Check BufferSize
    // Check Rssi
    // Check Snr
    // Check RxSlot
    if( ComplianceTest.Running == true )
    {
        ComplianceTest.DownLinkCounter++;
    }
    if( mcpsIndication->RxData == true )
    {
        switch( mcpsIndication->Port )
        {
        case 1: // The application LED can be controlled on port 1 or 2
        case 2:
            if( mcpsIndication->BufferSize == 1 )
            {
                //AppLedStateOn = mcpsIndication->Buffer[0] & 0x01;
                //GpioWrite( &Led3, ( ( AppLedStateOn & 0x01 ) != 0 ) ? 0 : 1 );
            }
            break;
        case 22:
            if( mcpsIndication->BufferSize > 1 )
            {
                if (memcmp ( mcpsIndication->Buffer, "on", 2 )== 0)
                {
                    //switch on Valve

                }
                else
                    if (memcmp ( mcpsIndication->Buffer, "off", 3 )== 0)
                    {
                        //switch off Valve

                    }
                    // std::string msg(mcpsIndication->Buffer, mcpsIndication->Buffer+mcpsIndication->BufferSize); // convert uint8_t array to string
            }
            break;
        case 224:
            if( ComplianceTest.Running == false )
            {
                // Check compliance test enable command (i)
                if( ( mcpsIndication->BufferSize == 4 ) &&
                    ( mcpsIndication->Buffer[0] == 0x01 ) &&
                    ( mcpsIndication->Buffer[1] == 0x01 ) &&
                    ( mcpsIndication->Buffer[2] == 0x01 ) &&
                    ( mcpsIndication->Buffer[3] == 0x01 ) )
                {
                    IsTxConfirmed = false;
                    AppPort = 224;
                    AppDataSize = 2;
                    ComplianceTest.DownLinkCounter = 0;
                    ComplianceTest.LinkCheck = false;
                    ComplianceTest.DemodMargin = 0;
                    ComplianceTest.NbGateways = 0;
                    ComplianceTest.Running = true;
                    ComplianceTest.State = 1;
                    
                    MibRequestConfirm_t mibReq;
                    mibReq.Type = MIB_ADR;
                    mibReq.Param.AdrEnable = true;
                    LoRaMacMibSetRequestConfirm( &mibReq );
                    //GpsStop( );
                }
            }
            else
            {
                ComplianceTest.State = mcpsIndication->Buffer[0];
                switch( ComplianceTest.State )
                {
                case 0: // Check compliance test disable command (ii)
                    IsTxConfirmed = LORAWAN_CONFIRMED_MSG_ON;
                    AppPort = LORAWAN_APP_PORT;
                    AppDataSize = LORAWAN_APP_DATA_SIZE;
                    ComplianceTest.DownLinkCounter = 0;
                    ComplianceTest.Running = false;
                    
                    MibRequestConfirm_t mibReq;
                    mibReq.Type = MIB_ADR;
                    mibReq.Param.AdrEnable = LORAWAN_ADR_ON;
                    LoRaMacMibSetRequestConfirm( &mibReq );
                    //GpsStart( );
                    break;
                case 1: // (iii, iv)
                    AppDataSize = 2;
                    break;
                case 2: // Enable confirmed messages (v)
                    IsTxConfirmed = true;
                    ComplianceTest.State = 1;
                    break;
                case 3:  // Disable confirmed messages (vi)
                    IsTxConfirmed = false;
                    ComplianceTest.State = 1;
                    break;
                case 4: // (vii)
                    AppDataSize = mcpsIndication->BufferSize;
                    AppData[0] = 4;
                    for( uint8_t i = 1; i < AppDataSize; i++ )
                    {
                        AppData[i] = mcpsIndication->Buffer[i] + 1;
                    }
                    break;
                case 5: // (viii)
                    {
                        MlmeReq_t mlmeReq;
                        mlmeReq.Type = MLME_LINK_CHECK;
                        LoRaMacMlmeRequest( &mlmeReq );
                    }
                    break;
                case 6: // (ix)
                    {
                        MlmeReq_t mlmeReq;
                        mlmeReq.Type = MLME_JOIN;
                        mlmeReq.Req.Join.DevEui = DevEui;
                        mlmeReq.Req.Join.AppEui = AppEui;
                        mlmeReq.Req.Join.AppKey = AppKey;
                        LoRaMacMlmeRequest( &mlmeReq );
                        DeviceState = DEVICE_STATE_SLEEP;
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
    // Switch LED 2 ON for each received downlink
    //GpioWrite( &Led2, 0 );

}


/*!
 * \brief   MLME-Confirm event function
 *
 * \param   [IN] mlmeConfirm - Pointer to the confirm structure,
 *               containing confirm attributes.
 */
static void MlmeConfirm( MlmeConfirm_t *mlmeConfirm )
{
    if( mlmeConfirm->Status == LORAMAC_EVENT_INFO_STATUS_OK )
    {
        switch( mlmeConfirm->MlmeRequest )
        {
            case MLME_JOIN:
            {
                // Status is OK, node has joined the network
                pc.printf("MLME_JOIN\r\n");
                DeviceState = DEVICE_STATE_SEND;
                NextTx = true;
                break;
            }
            case MLME_LINK_CHECK:
            {
                // Check DemodMargin
                // Check NbGateways
                pc.printf("MLME_LINK_CHECK\r\n");
                break;
            }
            default: {
                pc.printf("MLME_NOT_JOIN\r\n");
                break;
            }
        }
    }
    NextTx = true;
}

/*!
 * \brief   Prepare Ultrasonic range and temperature data for transmission
 */
static void txSensorError(void) {
    
    //static MbedJSONValue jsonobj;
    uint8_t volts = 32;
    char buf[5];
    sprintf(buf, "%.1f", (float) 99.9); // format float temperature value
    pc.printf("range = %d centimeters, temperature = %.1f\r\n",999, buf);
  //  jsonobj["rng"] = 999;
 //   jsonobj["tmp"] = buf;
 //   jsonobj["vlt"] = volts;
  //  std::string strng = jsonobj.serialize();
    // Prepare application data for transmission
 //   memcpy(AppData, strng.c_str(), strng.length());
  //  AppDataSize = strng.length();
}

uint8_t int2bcd_2(uint8_t input)
{
    uint8_t high = 0;
    
    while (input >= 10)  {               // Count tens
        high++;
        input -= 10;
    }
    return  (high << 4) | input;        // Add ones and return answer
}


static void txSensor(void) {
    
    float temperature;
    uint16_t range = sensor.getDistance(&temperature);
    uint8_t volts = 33;
    #define PAYLOAD_SIZE 5
    pc.printf("range = %d centimeters, temperature = %.1f\r\n",range, temperature);

    /* Format payload data to packed binary coded decimal*/
    formatPayload(AppData, range, temperature, volts);

        printf("payload = ");
    for (int i = 0; i<PAYLOAD_SIZE; i++)
        printf("%x",AppData[i]);
    printf("\r\n");
   // memcpy(AppData, buf, PAYLOAD_SIZE);
    AppDataSize = PAYLOAD_SIZE;
}

void LowPowerRestore(void) {

    GPIO_InitTypeDef GPIO_InitStruct;
    
    // Mode input, pulldown, medium speed
     GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
     GPIO_InitStruct.Pull = GPIO_PULLDOWN;
     GPIO_InitStruct.Speed = GPIO_SPEED_MEDIUM; 
        
     GPIO_InitStruct.Pin = (GPIO_PIN_10); // PA10 dio3
     HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
     
     DigitalInOut(PA_10, PIN_INPUT, PullDown, 0);

     GPIO_InitStruct.Pin = (GPIO_PIN_0 | GPIO_PIN_1); // PB0 dio0, PB1 dio1
     HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
     
     DigitalInOut(PB_0, PIN_INPUT, PullDown, 0);
     DigitalInOut(PB_1, PIN_INPUT, PullDown, 0);
     
     GPIO_InitStruct.Pin = (GPIO_PIN_6); // PC6 dio2
     HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
     
     DigitalInOut(PC_6, PIN_INPUT, PullDown, 0);
     
     // Digitalin
     GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
     GPIO_InitStruct.Pull = GPIO_PULLDOWN; // default mode
     
}

void LowPowerConfiguration(void)
{
        // Enable GPIO clocks
        RCC->AHBENR |= (RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN | RCC_AHBENR_GPIOCEN |
                            RCC_AHBENR_GPIODEN | RCC_AHBENR_GPIOEEN | RCC_AHBENR_GPIOHEN);
                            
         GPIO_InitTypeDef GPIO_InitStruct;
         // All other ports are analog input mode
         GPIO_InitStruct.Pin = GPIO_PIN_All;
         GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
         GPIO_InitStruct.Pull = GPIO_NOPULL;
         GPIO_InitStruct.Speed = GPIO_SPEED_VERY_LOW;
         // HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
         // HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
         // HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
         HAL_GPIO_Init(GPIOD, &GPIO_InitStruct); // Not used
         HAL_GPIO_Init(GPIOE, &GPIO_InitStruct); // Not used
         HAL_GPIO_Init(GPIOH, &GPIO_InitStruct); // Not used
       
         /* Set unused Port A pins to analogue input, low speed, no pull */
         GPIO_InitStruct.Pin = (GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 |
                                   GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15 );
         HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
         
         /* Set unused Port B pins to analogue input, low speed, no pull */
         GPIO_InitStruct.Pin = (GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | 
                                    GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15 );
         HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
         
         /* Set unused Port C pins to analogue input, low speed, no pull */
         GPIO_InitStruct.Pin = (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_7 | GPIO_PIN_9 |
                                   GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14 | GPIO_PIN_15 );
         HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    
         // Disable GPIO clocks
         // RCC->AHBENR &= ~(RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |RCC_AHBENR_GPIOCEN |
         //           RCC_AHBENR_GPIODEN | RCC_AHBENR_GPIOHEN);
                    
         RCC->AHBENR &= ~(RCC_AHBENR_GPIODEN | RCC_AHBENR_GPIOEEN | RCC_AHBENR_GPIOHEN);
}

Timeout secondTimeout;
bool expired = false;
 
void guardCallback(void) {
    expired = true;
}
 

int main( void )
{
    // LowPowerConfiguration();
    pc.baud(115200);
    // print banner
    pc.printf("\r\n============== DEBUG STARTED ==============\r\n");
    
    /** User button triggers the ultrasonic sensor */ 
    //PinDetect button(PC_13,PullUp);
    //button.attach_asserted(&sensor, &UltraSonic::triggerSample); // callback routine to trigger usonic
    //button.setSampleFrequency();

    LoRaMacPrimitives_t LoRaMacPrimitives;
    LoRaMacCallback_t LoRaMacCallbacks;
    MibRequestConfirm_t mibReq;

    //BoardInitMcu( );
    //BoardInitPeriph( );
    BoardInit( );

    DeviceState = DEVICE_STATE_INIT;

    while( 1 )
    {
        wait(0.5);

        switch( DeviceState )
        {
            case DEVICE_STATE_INIT:
            {
                  pc.printf("DEVICE_STATE_INIT\r\n");

                LoRaMacPrimitives.MacMcpsConfirm = McpsConfirm;
                LoRaMacPrimitives.MacMcpsIndication = McpsIndication;
                LoRaMacPrimitives.MacMlmeConfirm = MlmeConfirm;
                LoRaMacCallbacks.GetBatteryLevel = BoardGetBatteryLevel;
                LoRaMacInitialization( &LoRaMacPrimitives, &LoRaMacCallbacks );

                TimerInit( &TxNextPacketTimer, OnTxNextPacketTimerEvent );
                TimerInit( &ultrasonicTimer, OnUltrasonicTimeout );

                TimerInit( &Led1Timer, OnLed1TimerEvent );
                TimerSetValue( &Led1Timer, 50000 );

                mibReq.Type = MIB_ADR;
                mibReq.Param.AdrEnable = LORAWAN_ADR_ON;
                LoRaMacMibSetRequestConfirm( &mibReq ); // MAC information base service to set attributes of LoRaMac layer

                mibReq.Type = MIB_PUBLIC_NETWORK;
                mibReq.Param.EnablePublicNetwork = LORAWAN_PUBLIC_NETWORK;
                LoRaMacMibSetRequestConfirm( &mibReq );
                
                sensor.distanceAvailable = false; // Ensure initialised to false after power up

                DeviceState = DEVICE_STATE_JOIN;
                break;
            }
            case DEVICE_STATE_JOIN:
            {
                  pc.printf("DEVICE_STATE_JOIN\r\n");

#if( OVER_THE_AIR_ACTIVATION != 0 )
                MlmeReq_t mlmeReq;

                // Initialize LoRaMac device unique ID
                //BoardGetUniqueId( DevEui );
                mlmeReq.Type = MLME_JOIN; // MAC management service types are MLME_JOIN or MLME_LINK_CHECK

                mlmeReq.Req.Join.DevEui = DevEui;
                mlmeReq.Req.Join.AppEui = AppEui;
                mlmeReq.Req.Join.AppKey = AppKey;

                if( NextTx == true )
                {
                    LoRaMacMlmeRequest( &mlmeReq );  // Sends a join request
                }
                // Schedule next packet transmission
                //TxDutyCycleTime = OVER_THE_AIR_ACTIVATION_DUTYCYCLE;
                //DeviceState = DEVICE_STATE_CYCLE;

                DeviceState = DEVICE_STATE_CYCLE;
#else
                // Choose a random device address if not already defined in Comissioning.h
                if( DevAddr == 0 )
                {
                    // Random seed initialization
                    srand1( BoardGetRandomSeed( ) );
                    // Choose a random device address
                    DevAddr = randr( 0, 0x01FFFFFF );
                }
                mibReq.Type = MIB_NET_ID;
                mibReq.Param.NetID = LORAWAN_NETWORK_ID;
                LoRaMacMibSetRequestConfirm( &mibReq );

                mibReq.Type = MIB_DEV_ADDR;
                mibReq.Param.DevAddr = DevAddr;
                LoRaMacMibSetRequestConfirm( &mibReq );

                mibReq.Type = MIB_NWK_SKEY;
                mibReq.Param.NwkSKey = NwkSKey;
                LoRaMacMibSetRequestConfirm( &mibReq );

                mibReq.Type = MIB_APP_SKEY;
                mibReq.Param.AppSKey = AppSKey;
                LoRaMacMibSetRequestConfirm( &mibReq );

                mibReq.Type = MIB_NETWORK_JOINED;
                mibReq.Param.IsNetworkJoined = true;
                LoRaMacMibSetRequestConfirm( &mibReq );

                DeviceState = DEVICE_STATE_SEND;
#endif
                break;
            }
            case DEVICE_STATE_SEND:
            {
                  pc.printf("DEVICE_STATE_SEND\r\n");

                if( NextTx == true )
                {
                    pc.printf("Sending\r\n");
                    PrepareTxFrame( AppPort );
                    NextTx = SendFrame( );
                }
            }   
            case DEVICE_STATE_CYCLE:
            {
                 pc.printf("DEVICE_STATE_CYCLE\r\n");
          
                 pc.printf("SensorState = %d, distanceAvailable = ",SensorState);
                if (sensor.distanceAvailable)
                      pc.printf("true\r\n");
                else
                      pc.printf("false\r\n");

                pc.printf("LoRaMacState = %d\r\n",GetMacStatus());      
                      
                if ((SensorState == SENT) || (SensorState == INIT)) { /** Range and Temperature readings have been sent so we can sleep */
                    TimerStop( &TxNextPacketTimer ); 
                    if (GetMacStatus() == 0) { // Ensure MAC state and Valve power are idle before sleeping
                        pc.printf("DeepSleep ..zzz.\r\n");
                        WakeUp::set(DEEPSLEEP_SECONDS); // Set RTC alarm to wake up from deep sleep
                        Radio.DisableRadioIRQs(); // Disable radio IRQs to prevent them from waking the MCU
                        Radio.Sleep();
                        // Disable GPIO clocks
                       // RCC->AHBENR &= ~(RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |RCC_AHBENR_GPIOCEN |
                       // RCC_AHBENR_GPIODEN | RCC_AHBENR_GPIOHEN);
                        //wait(15); /** Must wait until the radio fully shuts down before sleeping */
                        deepsleep(); // Deep sleep until wake up alarm from RTC    
                        // Enable required GPIO clocks
                         //DigitalInOut(PB_0, PIN_INPUT, PullDown, 0);
                        // DigitalInOut(PB_1, PIN_INPUT, PullDown, 0);
                       // RCC->AHBENR &= (RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |RCC_AHBENR_GPIOCEN);                  
                        pc.printf("..Wake!\r\n");
                        Radio.EnableRadioIRQs(); // Re-enable the radio IRQs
                        SensorState = TRIGGERED;
                        /* Trigger ultrasonic sensor and start failure timer */
                        sensor.triggerSample(); // Get Ultrasonic reading
                        TimerSetValue( &ultrasonicTimer, 3000000 ); // 3s timeout
                        TimerStart( &ultrasonicTimer );
                        
                        DeviceState = DEVICE_STATE_SLEEP; // Cycle in sleep until sensor readings ready
                    }
                    else { // Cycle around until MAC state is idle
                        DeviceState = DEVICE_STATE_CYCLE;
                    }
                }
                else {
                    // Error shouldn't get here
                      pc.printf("Error State!!\r\n");
                    //TimerSetValue( &TxNextPacketTimer, TxDutyCycleTime );
                    //TimerStart( &TxNextPacketTimer );
                    SensorState = INIT;
                    DeviceState = DEVICE_STATE_SLEEP;
                }        

                break;
            }
            case DEVICE_STATE_SLEEP:
            {
                 pc.printf("DEVICE_STATE_SLEEP\r\n");

                // Sleep until sensor ready
                
                secondTimeout.attach(guardCallback, 3.0f); // 3 second guard on the while loop to ensure that it doesn't get stuck
                while(((SensorState != TRIGGERED) || (sensor.distanceAvailable == false)) && (SensorState != FAULT) && (!expired)) sleep();
                
                /** Send range and temperature as soon as they are available */
                if ((SensorState == TRIGGERED) && (sensor.distanceAvailable)) {
                    pc.printf("Xmit Reading..!\r\n");
                    TimerStop( &ultrasonicTimer ); // stop the sensor failure timer
                    SensorState = SENDING;
                    TimerSetValue( &TxNextPacketTimer, 100 ); // Schedule immediate transmission
                    TimerStart( &TxNextPacketTimer );
                }
        
                break;
            }
            default:
            {
                DeviceState = DEVICE_STATE_INIT;
                break;
            }
        }
    }
}



