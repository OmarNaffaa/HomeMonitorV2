#include <cpr/cpr.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "ThingSpeak.h"

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

void ThingSpeak::GetChannelData(uint32_t numEntries)
{
    std::string thingspeakUrl = BuildThingspeakUrl(numEntries);

    cpr::Response result = cpr::Get(cpr::Url{thingspeakUrl});

    if (result.status_code == static_cast<long>(HttpStatusCode::OK))
    {
        std::cout << "\nGot successful response from " << thingspeakUrl << std::endl;
        
        json thingspeakData = json::parse(result.text);

        auto feeds = thingspeakData["feeds"];
        
        for (auto& feed : feeds)
        {
            std::cout << "Feed Data:\n\n" << feed << std::endl;
        }

        // Json::Value jsonData;
        // Json::CharReaderBuilder jsonReader;
        // string errs;

        // if (Json::parseFromStream(jsonReader, httpData, &jsonData, &errs))
        // {
        //     const Json::Value jsonValues = jsonData["feeds"];

        //     for (int i = 0; i < jsonValues.size(); i++)
        //     {
        //         // iterate and store thingspeak values to field vector
        //         currResult.insert(pair<string, string>("created_at", jsonValues[i][jsonValues[i].getMemberNames()[0]].asString()));
        //         currResult.insert(pair<string, string>("entry_id", jsonValues[i][jsonValues[i].getMemberNames()[1]].asString()));
        //         currResult.insert(pair<string, string>("field1", jsonValues[i][jsonValues[i].getMemberNames()[2]].asString())); 
        //         currResult.insert(pair<string, string>("field2", jsonValues[i][jsonValues[i].getMemberNames()[3]].asString()));

        //         fieldResults.push_back(currResult);

        //         currResult.clear();
        //     }
        // }
        // else
        // {
        //     cout << "Could not parse HTTP data as JSON" << endl;
        //     cout << "HTTP data was: " << httpData.str() << std::endl;
        // }
    }
    else
    {
        std::cout << "[ERROR] Couldn't GET from " << thingspeakUrl << ".\n"
                  << "        Return code: " << result.status_code << std::endl;
    }
}

void ThingSpeak::printData()
{
    // for (auto& myMap : fieldResults)
    // {
    //     for (auto it = myMap.cbegin(); it != myMap.cend(); ++it)
    //     {
    //         cout << it->first << " " << it->second << "\n";
    //     }

    //     cout << endl;
    // }
}

std::string ThingSpeak::GetMostRecentTemp(int fieldNum)
{
    std::string mostRecentPoint;

    // if (fieldNum == 1)
    // {
    //     for (auto& mapItem : fieldResults)
    //     {
    //         for (auto it = mapItem.cbegin(); it != mapItem.cend(); ++it)
    //         {
    //             if (it->first == "field1" && it->second != "") mostRecentPoint = it->second;
    //         }
    //     }
    // }
    // else if (fieldNum == 2)
    // {
    //     for (auto& mapItem : fieldResults)
    //     {
    //         for (auto it = mapItem.cbegin(); it != mapItem.cend(); ++it)
    //         {
    //             if (it->first == "field2" && it->second != "") mostRecentPoint = it->second;
    //         }
    //     }
    // }
    // else
    // {
    //     mostRecentPoint = "Invalid Field";
    // }

    return mostRecentPoint;
}

std::string ThingSpeak::GetMostRecentTimestamp()
{
    std::string mostRecentTime;

    // for (auto& mapItem : fieldResults)
    // {
    //     for (auto it = mapItem.cbegin(); it != mapItem.cend(); ++it)
    //     {
    //         if (it->first == "created_at") mostRecentTime = it->second;
    //     }
    // }

    return mostRecentTime;
}

std::vector<thingspeakEntry> ThingSpeak::GetFieldResults()
{
    std::vector<thingspeakEntry> temp;
    return temp;
}

std::string ThingSpeak::BuildThingspeakUrl(uint32_t numRequests)
{
    std::string url = "https://api.thingspeak.com/channels/";
    url += thingspeakChannel;
    url += "/feeds.json?api_key=";
    url += thingspeakKey;
    url += "&results=";
    url += std::to_string(numRequests);

    return url;
}
