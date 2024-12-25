#include <iostream>
#include <string>
#include <vector>
#include <exception>
#include <map>

typedef std::map<std::string, std::string> thingspeakEntry;

class ThingSpeak
{
public:
    ThingSpeak(std::string id, std::string key) :
               thingspeakChannel(id), thingspeakKey(key) {}

    // Functions
    void printData();
    void GetChannelData(uint32_t numEntries);
    std::string GetMostRecentTemp(int fieldNum);
    std::string GetMostRecentTimestamp();
    std::vector<thingspeakEntry> GetFieldResults();

private:
	std::string thingspeakKey;
	std::string thingspeakChannel;

	std::string BuildThingspeakUrl(uint32_t numRequests);
};
