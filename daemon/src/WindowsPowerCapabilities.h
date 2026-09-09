#pragma once
#ifdef _WIN32

#include <optional>
#include <string>
#include <vector>

struct WindowsPowerCapabilities {

    bool queried = false;

    bool s1 = false;
    bool s2 = false;
    bool s3 = false;
    bool s4 = false;
    bool hiberFilePresent = false;
    bool modernStandby = false;

    std::optional<bool> fastStartup;

    std::string summary() const;

    std::vector<std::string> details() const;
};

WindowsPowerCapabilities queryPowerCapabilities();

#endif
