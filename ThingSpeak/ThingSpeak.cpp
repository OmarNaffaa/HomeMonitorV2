#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <assert.h>

#include "ThingSpeak.h"

#define DEBUG_THINGSPEAK false

#define THINGSPEAK_LOWEST_FIELD_NUMBER    1
#define THINGSPEAK_HIGHEST_FIELD_NUMBER   8

enum class HttpStatusCode {
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
 * @brief Get ThingSpeak data for a specified field
 * 
 * @param fieldNum      - [Input] Field number to obtain data for
 * @param fieldName     - [Output] Name of field defined in ThingSpeak
 * @param numDataPoints - [Input] Number of data points to request
 * @param xAxisData     - [Output] Data to plot on x-axis (entry_id)
 * @param yAxisData     - [Output] Data to plot on y-axis (field#)
 * 
 * @return int - Size of data obtained.
 *               Negative value if no data / field does not exist.
 */
int ThingSpeak::GetFieldData(uint8_t const fieldNum,
                             std::string& fieldName,
                             uint32_t const numDataPoints,
                             std::vector<int>& xAxisData,
                             std::vector<float>& yAxisData)
{
    if ((fieldNum < THINGSPEAK_LOWEST_FIELD_NUMBER)
        || (fieldNum > THINGSPEAK_HIGHEST_FIELD_NUMBER))
    {
        std::cerr << "Field number " << fieldNum << " is not supported" << std::endl;
        return -1;
    }

    json thingSpeakData = GetChannelData(numDataPoints);

    std::string fieldId = "field" + std::to_string(fieldNum);
    try
    {
        fieldName = thingSpeakData["channel"][fieldId];
    }
    catch(const std::out_of_range& e)
    {
        std::cerr << e.what() << '\n';
        return -1;
    }

    for (auto& feed : thingSpeakData["feeds"])
    {
        // TODO: Get timestamps to display when hovering over data points
        xAxisData.push_back(feed["entry_id"]);
        try
        {
            std::string fieldValue = feed[fieldId];
            yAxisData.push_back(std::stof(fieldValue));
        }
        catch(const std::exception& e)
        {
            std::cerr << "Error on entry_id = " << feed["entry_id"]
                      << ". Value = " << feed[fieldId];
            std::cerr << e.what() << '\n';

            // Ensure only entries with valid data are kept
            xAxisData.pop_back();
        }
    }

    assert(xAxisData.size() == yAxisData.size());
    return xAxisData.size();
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
    std::string url = "https://api.thingSpeak.com/channels/";
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
