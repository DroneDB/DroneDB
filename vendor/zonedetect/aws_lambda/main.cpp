#include <aws/lambda-runtime/runtime.h>
#include <string>
#include <ctime>
#include "json.hpp"
#include "../library/zonedetect.h"

using json = nlohmann::json;
using namespace aws::lambda_runtime;

ZoneDetect* zd;

std::string getTime(std::time_t param) {
    char tstr[100];
    if (std::strftime(tstr, sizeof(tstr), "%F %T %Z", std::localtime(&param))) {
        return std::string(tstr);
    }
    return "";
}

invocation_response zd_handler(invocation_request const& request) {
    try {
        auto body = json::parse(request.payload);

        if(body.count("queryStringParameters")) {
            auto param = body["queryStringParameters"];

            if(param.count("lat") && param.count("lon")) {
                float lat = std::stof(param["lat"].get<std::string>(), nullptr);
                float lon = std::stof(param["lon"].get<std::string>(), nullptr);

                json result;
                int blocked = 0;
                int compact = 0;

                if(param.count("obs")) {
                    auto obs = static_cast<std::time_t>(std::stol(param["obs"].get<std::string>(), nullptr));
                    result["Warning"] = "You are accessing this API on an unsupported endpoint. Please use http[s]://timezone.bertold.org/timezone instead. This endpoint will stop responding on " + getTime(obs);
                    if(time(NULL) >= obs) {
                        blocked = 1;
                    }
                }

                if(!blocked) {
                    if(param.count("c")) {
                        compact = std::stoi(param["c"].get<std::string>());
                    }

                    int simple = 0;
                    if(param.count("s")) {
                        simple = std::stoi(param["s"].get<std::string>());
                    }

                    if(!compact) {
                        result["Notice"] = ZDGetNotice(zd);
                    }

                    if(simple) {
                        auto sr = ZDHelperSimpleLookupString(zd, lat, lon);
                        if(sr) {
                            result["Result"] = sr;
                            free(sr);
                        }
                    } else {
                        float safezone = 0;
                        auto results = ZDLookup(zd, lat, lon, &safezone);

                        if(results) {
                            int index = 0;
                            while(results[index].lookupResult != ZD_LOOKUP_END) {
                                auto& zone = result["Zones"][index];
                                zone["Result"] = ZDLookupResultToString(results[index].lookupResult);
                                result["Safezone"] = safezone;

                                if(results[index].data) {
                                    for(unsigned int i = 0; i < results[index].numFields; i++) {
                                        if(results[index].fieldNames[i] && results[index].data[i]) {
                                            zone[results[index].fieldNames[i]] = results[index].data[i];
                                        }
                                    }

                                    if(zone.count("TimezoneId") && zone.count("TimezoneIdPrefix")) {
                                        zone["TimezoneId"] = zone["TimezoneIdPrefix"].get<std::string>() + zone["TimezoneId"].get<std::string>();
                                        zone.erase("TimezoneIdPrefix");
                                    }
                                }

                                index++;
                            }
                        }

                        ZDFreeResults(results);
                    }

                }

                json response;
                response["statusCode"] = 200;
                response["headers"]["Cache-Control"] = "max-age=86400";
                response["headers"]["Access-Control-Allow-Origin"] = "*";
                response["body"] = result.dump(compact?0:2);

                return invocation_response::success(response.dump(), "application/json");
            }
        }
    } catch(...) {}

    return invocation_response::failure("Error", "Error");
}

int main() {
    zd = ZDOpenDatabase("timezone21.bin");
    if(!zd) {
        return 1;
    }

    run_handler(zd_handler);

    ZDCloseDatabase(zd);
    return 0;
}
