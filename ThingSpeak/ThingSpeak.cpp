#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <assert.h>

#include "ThingSpeak.h"

#define DEBUG_THINGSPEAK false

#define THINGSPEAK_LOWEST_FIELD_NUMBER    1
#define THINGSPEAK_HIGHEST_FIELD_NUMBER   8

enum class HttpStatusCode
{
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503
};

/**
 * @brief Get/Update ThingSpeak object with latest ThingSpeak data
 * 
 * @return int - Negative value if data could not be fetched. 0 otherwise
 */
int ThingSpeak::GetFieldData()
{
    int i;

    json thingSpeakData = GetChannelData(MAX_THINGSPEAK_REQUEST_SIZE);
    if (thingSpeakData == NULL)
    {
        return -1;
    }

    // New data fetched; Update internal structures
    std::string temperature = "field1";
    std::string humidity = "field2";

    temperatureData.numDataPoints = 0;
    humidityData.numDataPoints = 0;

    for (auto& feed : thingSpeakData["feeds"])
    {
        // Update temperature data
        if (feed[temperature] == nullptr)
        {
            #if (DEBUG_THINGSPEAK)
            std::cout << "Skipping a temperature data point" << std::endl;
            #endif
            continue;
        }
        i = temperatureData.numDataPoints;

        temperatureData.fieldName = thingSpeakData["channel"][temperature];
        temperatureData.entryId[i] = feed["entry_id"];
        temperatureData.timestamp[i] = feed["created_at"];
        temperatureData.xAxisData[i] = i;
        temperatureData.yAxisData[i] = std::stof(static_cast<std::string>(feed[temperature]));

        temperatureData.numDataPoints++;

        // Update humidity data
        if (feed[humidity] == nullptr)
        {
            #if (DEBUG_THINGSPEAK)
            std::cout << "Skipping a humidity data point" << std::endl;
            #endif
            continue;
        }
        i = humidityData.numDataPoints;

        humidityData.fieldName = thingSpeakData["channel"][humidity];
        humidityData.entryId[i] = feed["entry_id"];
        humidityData.timestamp[i] = feed["created_at"];
        humidityData.xAxisData[i] = i;
        humidityData.yAxisData[i] = std::stof(static_cast<std::string>(feed[humidity]));

        humidityData.numDataPoints++;
    }

    return 0;
}

/**
 * @brief Returns name assigned to object
 * 
 * @return std::string const - Name of object
 */
std::string const ThingSpeak::GetName()
{
    return objectName;
}

/**
 * @brief Get current temperature data from this object
 * 
 * @return ThingSpeakFeedData_t const* const - Array of temperature data
 */
ThingSpeakFeedData_t const * const ThingSpeak::GetTemperature()
{
    return &temperatureData;
}

/**
 * @brief Get current humidity data from this object
 * 
 * @return ThingSpeakFeedData_t const* const - Array of humidity data
 */
ThingSpeakFeedData_t const * const ThingSpeak::GetHumidity()
{
    return &humidityData;
}

/**
 * @brief Perform an HTTP GET call to ThingSpeak endpoint to obtain
 *        data in JSON format
 * 
 * @param numEntries - The number of entries to obtain from ThingSpeak
 * 
 * @return json - JSON object containing response from ThingSpeak
 *                NULL if data could not be obtained
 */
json ThingSpeak::GetChannelData(uint32_t numEntries)
{
    std::string thingSpeakUrl = BuildThingSpeakHttpGetUrl(numEntries);

    cpr::Response result = cpr::Get(cpr::Url{thingSpeakUrl});

    json thingSpeakData = NULL;

    if (result.status_code == static_cast<long>(HttpStatusCode::OK))
    {
        std::cout << "\nGot successful response from " << thingSpeakUrl << std::endl;
        
        thingSpeakData = json::parse(result.text);

        for (auto& feed : thingSpeakData["feeds"])
        {
            feed["created_at"] = ConvertUtcDateTimeToPstDateTime(feed["created_at"]);

            #if (DEBUG_THINGSPEAK)
            std::cout << "\nFeed Data:\n" << feed << std::endl;
            #endif
        }
    }
    else
    {
        std::cerr << "[ERROR] Couldn't GET from " << thingSpeakUrl << ".\n"
                  << "        Return code: " << result.status_code << std::endl;
    }

    return thingSpeakData;
}

/**
 * @brief Create URL used to perform HTTP get request to ThingSpeak
 * 
 * @param numEntries - The number of entries to obtain from ThingSpeak
 * 
 * @return std::string - a string representing the URL to query
 */
std::string ThingSpeak::BuildThingSpeakHttpGetUrl(uint32_t numEntries)
{
    std::string url = "https://api.thingspeak.com/channels/";
    url += thingSpeakChannel;
    url += "/feeds.json?api_key=";
    url += thingSpeakKey;
    url += "&results=";
    url += std::to_string(numEntries);

    return url;
}

/**
 * @brief Functionality to convert the date and time provided by
 *        ThingSpeak (UTC) to its corresponding date and time PST
 * 
 * @param utcDateTimeStr - UTC Date/Time string (e.g. "2024-12-24T07:10:39Z")
 * 
 * @return std::string - PST Date/Time string
 */
std::string ThingSpeak::ConvertUtcDateTimeToPstDateTime(std::string utcDateTimeStr)
{
    // Parse the UTC time string to std::chrono::system_clock::time_point
    std::istringstream ss(utcDateTimeStr);
    std::chrono::sys_time<std::chrono::seconds> utcTimePoint;
    ss >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", utcTimePoint);

    // Define the PST offset and perform conversion
    auto pstOffset = std::chrono::hours(GetPstTimeOffset());
    auto pstTimePoint = utcTimePoint + pstOffset;

    // Convert time_point back to std::tm
    std::time_t pstTime = std::chrono::system_clock::to_time_t(pstTimePoint);
    std::tm* pstTm = std::gmtime(&pstTime);

    // Convert timestamp to string
    std::ostringstream oss;
    oss << std::put_time(pstTm, "%Y-%m-%d %H:%M:%S");
    std::string pstDateTimeStr = oss.str();

    return pstDateTimeStr;
}

/**
 * @brief Compute PST time offset based on Daylight savings
 * 
 * @return int - Offset, in hours, to convert UTC time to PST
 */
int ThingSpeak::GetPstTimeOffset(void)
{
    int pstOffset = -8;

    auto now = std::chrono::system_clock::now();
    auto local_time = std::chrono::zoned_time{std::chrono::current_zone(), now};

    bool daylightSavings = (local_time.get_info().save != std::chrono::seconds{0});
    if (daylightSavings)
    {
        pstOffset += 1;
    }

    return pstOffset;
}
