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

class ThingSpeak
{
public:
    ThingSpeak(std::string id, std::string key) :
               thingSpeakChannel(id), thingSpeakKey(key) {}

    json GetChannelData(uint32_t numEntries);

private:
	std::string thingSpeakKey;
	std::string thingSpeakChannel;

	std::string BuildThingSpeakHttpGetUrl(uint32_t numRequests);
    std::string ConvertUtcDateTimeToPstDateTime(std::string utcDateTimeStr);
    int GetPstTimeOffset(void);
};
