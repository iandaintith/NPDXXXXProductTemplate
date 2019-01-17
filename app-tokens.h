#define CREATOR_APP_POR_COUNT      (0x000A)
#define CREATOR_APP_ONOFF_STATE    (0x000B)
#define CREATOR_APP_LIGHT_LEVEL    (0x000C)

#ifdef DEFINETYPES
// Include or define any typedef for tokens here
#endif //DEFINETYPES

#ifdef DEFINETOKENS
DEFINE_COUNTER_TOKEN(APP_POR_COUNT, int8_t, 0)
DEFINE_BASIC_TOKEN(APP_ONOFF_STATE, int8_t, 0)
DEFINE_BASIC_TOKEN(APP_LIGHT_LEVEL, int8_t, 0)
#endif

