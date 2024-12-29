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

typedef std::map<std::string, std::string> thingSpeakEntry;

typedef struct
{
    std::string fieldName;
    std::string timestamp;
    int entryId;
    float xAxisDataPoint;
    float yAxisDataPoint;
} ThingSpeakFeedEntry_t;

enum class ThingSpeakFieldNum
{
    Field1 = 1,
    Field2,
    Field3,
    Field4,
    Field5,
    Field6,
    Field7,
    Field8
};

class ThingSpeak
{
public:
    ThingSpeak(std::string id, std::string key) :
               thingSpeakChannel(id), thingSpeakKey(key) {}

    int GetFieldData(ThingSpeakFieldNum const fieldNum,
                     uint32_t const numDataPoints,
                     std::vector<ThingSpeakFeedEntry_t>& data);

private:
	std::string thingSpeakKey;
	std::string thingSpeakChannel;

    json GetChannelData(uint32_t numEntries);
	std::string BuildThingSpeakHttpGetUrl(uint32_t numRequests);
    std::string ConvertUtcDateTimeToPstDateTime(std::string utcDateTimeStr);
    int GetPstTimeOffset(void);
};
