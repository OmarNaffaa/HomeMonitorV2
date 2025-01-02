#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <exception>
#include <map>
#include <chrono>
#include <format>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#define MAX_THINGSPEAK_REQUEST_SIZE   100

enum class ThingSpeakField
{
    Temperature = 1,
    Humidity
};

typedef std::map<std::string, std::string> thingSpeakEntry;

typedef struct
{
    std::string fieldName;
    int numDataPoints;

    int entryId[MAX_THINGSPEAK_REQUEST_SIZE];
    float xAxisData[MAX_THINGSPEAK_REQUEST_SIZE];
    float yAxisData[MAX_THINGSPEAK_REQUEST_SIZE];
    std::string timestamp[MAX_THINGSPEAK_REQUEST_SIZE];
} ThingSpeakFeedData_t;

class ThingSpeak
{
public:
    ThingSpeak() :
               objectName(""), thingSpeakChannel(""), thingSpeakKey("") {}
    ThingSpeak(std::string name, std::string id, std::string key) :
               objectName(name), thingSpeakChannel(id), thingSpeakKey(key) {}

    int GetFieldData();
    std::string const GetName();
    ThingSpeakFeedData_t const * const GetTemperature();
    ThingSpeakFeedData_t const * const GetHumidity();

private:
    // Member Variables
    std::string objectName;
	std::string thingSpeakKey;
	std::string thingSpeakChannel;

    ThingSpeakFeedData_t temperatureData;
    ThingSpeakFeedData_t humidityData;

    // Member Functions
    json GetChannelData(uint32_t numEntries);
	std::string BuildThingSpeakHttpGetUrl(uint32_t numRequests);
    std::string ConvertUtcDateTimeToPstDateTime(std::string utcDateTimeStr);
    int GetPstTimeOffset(void);
};
