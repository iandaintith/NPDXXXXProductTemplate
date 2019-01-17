#include "app/framework/include/af.h"
#include "app/framework/plugin-soc/connection-manager/connection-manager.h"
#include "app/builder/AuroraComponents/Silabs/Include/mcuAuroraEFR32MG13.h"
#include "app/builder/AuroraComponents/Silabs/Include/clusterUtils.h"
#include "app/builder/AuroraComponents/Silabs/Include/porDetect.h"
#include "app/builder/AuroraComponents/Silabs/Include/resetCause.h"
#include "app/builder/AuroraComponents/Common/Include/appDebug.h"

#ifndef TRACE_APP
#define TRACE_APP true
#endif

#ifndef TRACE_ZIGBEE
#define TRACE_ZIGBEE true
#endif

#ifndef TRACE_UI
#define TRACE_UI true
#endif

#define POR_DETECT_INITIAL_DELAY_MS    500
#define POR_DETECT_WINDOW_MS           1000
#define POR_DETECT_THRESHOLD           6
#define START_UP_DELAY_MS              0
#define MCU_COMMAND_INTERVAL_MS        100
#define UI_ACTIVE_TIME_MS              5000
#define LIGHT_ENDPOINT                 1
#define HIVE_SUPPORT                   true

// Globals
bool g_traceEnabled = true;
EmberEventControl eventIdentifyControl;

// Forward Declarations
static void AppOnStartup();
static void AppReset();
static void AppLeaveNetwork();
static void AppOnRxFrame(u8 *frame);
static void ZigbeeProcess();
static void HiveProcess();

static bool s_uiActive = false;                    // TRUE if UI is active indicating to user
static u32 s_uiActiveStartTimeMs = 0;              // The time the UI activated
static bool s_zigbeeChangePending = false;         // Set to TRUE when the Zigbee domain has changed, and we need to update the MCU
static bool s_indicateJoiningSequence = false;     // Set TRUE when we want any subsequent Joining to be indicated, FALSE otherwise
static bool s_indicateJoinSuccess = false;         // Set TRUE when we want any subsequent Join success to be indicated, FALSE otherwise
static bool s_startupComplete = false;             // TRUE when AppStartup() completed

///////////////////////////////////////////////////////////////////////////////
// GLOBAL FUNCTIONS
///////////////////////////////////////////////////////////////////////////////

// Called once of initialisation
void emberAfMainInitCallback(void)
{
   TRACE(TRACE_APP, "*******************************\r\n");
   TRACE(TRACE_APP, "APP: NPD3920 Fixed White MPro\r\n");
   TRACE(TRACE_APP, "APP: Version: %x (%d)\r\n", EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION, EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION);
   TRACE(TRACE_APP, "*******************************\r\n");
#if NPD3700_HW
   GPIO_PinModeSet(STATUS_LED_PORT, STATUS_LED_PIN, gpioModePushPull, 0);
#endif
}

// Called once after HAL initialisation but before stack starts
bool emberAfMainStartCallback(int* returnCode,
                              int argc,
                              char** argv)
{
   MCUInit(COM_USART1);

   // The soonest we can put the light on, we do
   if(!ResetWasBootloader())
   {
      MCURequestLevelModeSingleColour(LIGHT_LEVEL_MAX_ZCL);
   }
   return false;
}

// Called periodically throughout run time
void emberAfMainTickCallback(void)
{
   static bool s_firstTick = true;
   static u32 appStartupMs = 0;

   if(s_firstTick)
   {
      AppOnStartup();
      appStartupMs = halCommonGetInt32uMillisecondTick();
      s_firstTick = false;
   }

   u32 nowMs = halCommonGetInt32uMillisecondTick();

   // Wait for a period of START_UP_DELAY_MS to avoid the light blip before the flash for joining
   if(elapsedTimeInt32u(appStartupMs, nowMs)> START_UP_DELAY_MS)
   {
      // if a UI indication is active
      if(s_uiActive)
      {
         // let's see if it should stop
         if(elapsedTimeInt32u(s_uiActiveStartTimeMs, nowMs)> UI_ACTIVE_TIME_MS)
         {
            s_uiActive = false;
            s_uiActiveStartTimeMs = 0;
            s_zigbeeChangePending = true;
            TRACE(TRACE_APP, "APP: UI Sequence Stopped, restoring light levels from Zigbee Cluster\r\n");
         }
      }
      else
      {
         // Process any response from MCU
         u8 *mcuRxFrame = MCUProcess();
         if(mcuRxFrame)
         {
            AppOnRxFrame(mcuRxFrame);
         }

         // Process any Zigbee domain changes
         ZigbeeProcess();
      }

      // Perform any Hive specific processes
      HiveProcess();
   }
}


// This callback is fired when the Connection Manager plugin is finished with
// the network search process. The result of the operation will be returned as
// the status parameter. Blink ONCE if join successful
void emberAfPluginConnectionManagerFinishedCallback(EmberStatus status)
{
   TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);

   //if success, blink STATUS LED once
   if (emberAfNetworkState() == EMBER_JOINED_NETWORK)
   {
      ClusterSetOTAClient(LIGHT_ENDPOINT, EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION,
             EMBER_AF_MANUFACTURER_CODE , EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_IMAGE_TYPE_ID);

      if(s_indicateJoinSuccess)
      {
         s_indicateJoinSuccess = false;
         TRACE(TRACE_ZIGBEE, "ZIGBEE: Join OK\r\n");
         s_uiActive = true;
         s_uiActiveStartTimeMs = halCommonGetInt32uMillisecondTick();
         MCURequestFlash(1);
         s_zigbeeChangePending = true;
      }
   }
   else
   {
      TRACE(TRACE_ZIGBEE, "ZIGBEE: Join Failed\r\n");
#pragma message(__LOC__"TODO What is the UI indication if the join fails?")
   }
}

 // Called by the Connection Manager Plugin when the device
 // is about to leave the network.  It is normally used to trigger a UI event to
 // notify the user of a network leave.
void emberAfPluginConnectionManagerLeaveNetworkCallback()
{
   TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);
   s_indicateJoiningSequence = true;    // The next join should be indicated to the user
   s_indicateJoinSuccess = true;
}

// Called by the Connection Manager Plugin when it starts
// to search for a new network.  It is normally used to trigger a UI event to
// notify the user that the device is currently searching for a network.
// Blink TWICE
void emberAfPluginConnectionManagerStartNetworkSearchCallback()
{
   TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);

   if(s_indicateJoiningSequence)
   {
      s_indicateJoiningSequence = false;
      s_uiActive = true;
      s_uiActiveStartTimeMs = halCommonGetInt32uMillisecondTick();
      MCURequestFlash(2);
      s_zigbeeChangePending = true;
   }
}

// Called when the On/Off cluster has changed
void emberAfOnOffClusterServerAttributeChangedCallback(uint8_t endpoint,
      EmberAfAttributeId attributeId)
{
   TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);

   if (ZCL_ON_OFF_ATTRIBUTE_ID == attributeId)
   {
      s_zigbeeChangePending = true;
   }
}

// Called when the Level control cluster has changed
void emberAfLevelControlClusterServerAttributeChangedCallback(int8u endpoint,
      EmberAfAttributeId attributeId)
{
   if (ZCL_CURRENT_LEVEL_ATTRIBUTE_ID == attributeId)
   {
      s_zigbeeChangePending = true;
   }
}

// Called when the endpoint should start identifying
void emberAfPluginIdentifyStartFeedbackCallback(u8 endpoint,
                                                u16 identifyTime)
{
   TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);

   if(s_startupComplete)
   {
      if(LIGHT_ENDPOINT == endpoint)
      {
         s_uiActive = true;
         emberEventControlSetDelayMS(eventIdentifyControl, 1000);
      }
   }
}

// Called when the endpoint should stop identifying
void emberAfPluginIdentifyStopFeedbackCallback(u8 endpoint)
{
   if(s_startupComplete)
   {
      TRACE(TRACE_ZIGBEE, "ZIGBEE: %s()\r\n", __FUNCTION__);

      if(LIGHT_ENDPOINT == endpoint)
      {
         MCURequestLevelModeSingleColour(LIGHT_LEVEL_OFF_ZCL);
         s_uiActive = false;
         s_zigbeeChangePending = true;
         emberEventControlSetInactive(eventIdentifyControl);
      }
   }
}

// Called every 1000ms during identify
void eventIdentifyCallback()
{
   MCURequestFlash(1);
   s_uiActive = true;
   s_uiActiveStartTimeMs = halCommonGetInt32uMillisecondTick();
   emberEventControlSetDelayMS(eventIdentifyControl, 1000);
}

// Called when a ZCL command is received
// we want to use this to preserve level and on/off state if this
// is a ZCL_UPGRADE_END_RESPONSE_COMMAND_ID
boolean emberAfPreCommandReceivedCallback(EmberAfClusterCommand* cmd)
{
   if(cmd->apsFrame->clusterId == ZCL_OTA_BOOTLOAD_CLUSTER_ID)
   {
      if(cmd->commandId == ZCL_UPGRADE_END_RESPONSE_COMMAND_ID)
      {
         u8 onOff = ClusterReadOnOff(LIGHT_ENDPOINT);
         u8 zclLevel = ClusterReadLevel(LIGHT_ENDPOINT);
         halCommonSetToken(TOKEN_APP_ONOFF_STATE, &onOff);
         halCommonSetToken(TOKEN_APP_LIGHT_LEVEL, &zclLevel);
      }
   }
   return false; // allow the stack to process
}

///////////////////////////////////////////////////////////////////////////////
// LOCAL FUNCTIONS
///////////////////////////////////////////////////////////////////////////////

// Process any Zigbee domain changes and update MCU as required
static void ZigbeeProcess()
{
   static u32 lastZigbeeProcess = 0;
   u32 nowMs = halCommonGetInt32uMillisecondTick();

   if((s_zigbeeChangePending) && (elapsedTimeInt32u(lastZigbeeProcess, nowMs) > MCU_COMMAND_INTERVAL_MS))
   {
      lastZigbeeProcess = nowMs;
      u8 onOff = ClusterReadOnOff(LIGHT_ENDPOINT);
      u8 zclLevel = ClusterReadLevel(LIGHT_ENDPOINT);
      TRACE(TRACE_APP, "APP: %s() Change Pending. Onoff=%d, level=%x\r\n", __FUNCTION__, onOff, zclLevel);
      MCURequestLevelModeSingleColour(onOff ? zclLevel : LIGHT_LEVEL_OFF_ZCL);
      s_zigbeeChangePending = false;
   }
}

// Specific to Hive. Reset join attempts so that if device is in
// pairing mode, it stays there forever
static void HiveProcess()
{
#if HIVE_SUPPORT
   emberAfPluginConnectionManagerResetJoinAttempts();
#endif
}

// Called once on application startup
static void AppOnStartup()
{
   ClusterSetOTAClient(LIGHT_ENDPOINT, EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION,
         EMBER_AF_MANUFACTURER_CODE , EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_IMAGE_TYPE_ID);

   if(ResetWasBootloader())
   {
      u8 onOff = 0;
      u8 zclLevel = LIGHT_LEVEL_MAX_ZCL;

      halCommonGetToken(&onOff, TOKEN_APP_ONOFF_STATE);
      halCommonGetToken(&zclLevel, TOKEN_APP_LIGHT_LEVEL);
      ClusterWriteOnOff(LIGHT_ENDPOINT, onOff);
      ClusterWriteLevel(LIGHT_ENDPOINT, zclLevel);
   }

   s_zigbeeChangePending = true;
   s_startupComplete = true;
}

// Application reset. Leave network and default clusters
static void AppReset()
{
   TRACE(TRACE_APP, "APP: %s()\r\n", __FUNCTION__);
   AppLeaveNetwork();
   emberAfPluginConnectionManagerFactoryReset();
   ClusterWriteOnOff(LIGHT_ENDPOINT, true);
   ClusterWriteLevel(LIGHT_ENDPOINT, LIGHT_LEVEL_MAX_ZCL);
   ClusterSetOTAClient(LIGHT_ENDPOINT, EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION,
         EMBER_AF_MANUFACTURER_CODE , EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_IMAGE_TYPE_ID);
   s_zigbeeChangePending = true;
}

// Application function to leave network
static void AppLeaveNetwork()
{
   TRACE(TRACE_APP, "APP: %s()\r\n", __FUNCTION__);
   emberLeaveNetwork();
   s_indicateJoiningSequence = true;
   s_indicateJoinSuccess = true;
}

// Application handler for a RX frame received form the MCU
static void AppOnRxFrame(u8 *frame)
{
   TRACE(TRACE_APP, "APP: %s()\r\n", __FUNCTION__);

   if(FRAME_MODE_NETWORK == frame[1])
   {
      TRACE(TRACE_APP, "APP: MCU Requests to leave network and reset\r\n");
      AppReset();
   }
   else
   if(FRAME_MODE_FIXTURE == frame[1])
   {
      TRACE(TRACE_APP, "APP: MCU Reports Fixture as %d\r\n", frame[2]);
   }
}

///////////////////////////////////////////////////////////////////////////////
// CLI TEST FUNCTIONS
///////////////////////////////////////////////////////////////////////////////

void cliLightOnOff(void)
{
   u8 onOff = (u8)emberUnsignedCommandArgument(0);
   ClusterWriteOnOff(LIGHT_ENDPOINT, onOff);
}

void cliLightLevel(void)
{
   u8 zclLevel = (u8)emberUnsignedCommandArgument(0);
   ClusterWriteLevel(LIGHT_ENDPOINT, zclLevel);
}
void cliFlash(void)
{
   u8 count = (u8)emberUnsignedCommandArgument(0);
   MCURequestFlash(count);
   s_uiActive = true;
   s_uiActiveStartTimeMs = halCommonGetInt32uMillisecondTick();
}

void cliType(void)
{
   MCURequestType();
}

void cliVersion(void)
{
   u16 version = EMBER_AF_PLUGIN_OTA_CLIENT_POLICY_FIRMWARE_VERSION;
   emberSerialPrintf(APP_SERIAL, "Application FW Version = %d\r\n", version);
}

void cliAppReset()
{
   AppReset();
}

EmberCommandEntry emberAfCustomCommands[] = {
   emberCommandEntryActionWithDetails("onoff", cliLightOnOff, "u", "Writes on/off cluster <0/1>", NULL),
   emberCommandEntryActionWithDetails("level", cliLightLevel, "u", "Writes to level Cluster  <level>", NULL),
   emberCommandEntryActionWithDetails("flash", cliFlash, "u", "Flash light <count>", NULL),
   emberCommandEntryActionWithDetails("type", cliType, "", "Request type of luminaire", NULL),
   emberCommandEntryActionWithDetails("version", cliVersion, "", "App FW Version", NULL),
   emberCommandEntryActionWithDetails("appreset", cliAppReset, "", "App Reset", NULL),
   emberCommandEntryTerminator()
};

