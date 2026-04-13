#pragma once

#include <string>
#include <vector>

struct LAppConfig
{
    std::vector<std::string> additionalModelDirs;
    std::string defaultModel;
    float emotionTimeout = 5.0f;
    float modelScale = 1.0f;
    float modelX = 0.0f;
    float modelY = 0.0f;
    int windowWidth = 1900;
    int windowHeight = 1000;

    static LAppConfig& GetInstance();

    bool LoadFromFile(const std::string& path);

    static std::string GetDefaultConfigPath();

    static std::string ExpandTilde(const std::string& path);

private:
    static std::string Trim(const std::string& s);
    static std::string ParseStringValue(const std::string& s, const std::string& key);
    static float ParseFloatValue(const std::string& s, const std::string& key, float defaultVal);
    static int ParseIntValue(const std::string& s, const std::string& key, int defaultVal);
    static std::vector<std::string> ParseStringArray(const std::string& s, const std::string& key);
    static std::string StripComments(const std::string& s);
};
