#include "LAppConfig.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <cstdio>

static LAppConfig s_config;

LAppConfig& LAppConfig::GetInstance()
{
    return s_config;
}

std::string LAppConfig::GetDefaultConfigPath()
{
    const char* configHome = getenv("XDG_CONFIG_HOME");
    if (configHome && strlen(configHome) > 0)
    {
        return std::string(configHome) + "/waifuland/config.json";
    }
    const char* homeDir = getenv("HOME");
    if (!homeDir || strlen(homeDir) == 0)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }
    return std::string(homeDir) + "/.config/waifuland/config.json";
}

std::string LAppConfig::ExpandTilde(const std::string& path)
{
    if (path.empty() || path[0] != '~') return path;

    const char* homeDir = getenv("HOME");
    if (!homeDir || strlen(homeDir) == 0)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }
    return std::string(homeDir) + path.substr(1);
}

std::string LAppConfig::Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string LAppConfig::ParseStringValue(const std::string& s, const std::string& key)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return "";

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";

    pos = s.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos) return "";

    return s.substr(pos + 1, end - pos - 1);
}

float LAppConfig::ParseFloatValue(const std::string& s, const std::string& key, float defaultVal)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return defaultVal;

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;

    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;

    try {
        return std::stof(s.substr(pos));
    } catch (...) {
        return defaultVal;
    }
}

int LAppConfig::ParseIntValue(const std::string& s, const std::string& key, int defaultVal)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return defaultVal;

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;

    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;

    try {
        return std::stoi(s.substr(pos));
    } catch (...) {
        return defaultVal;
    }
}

std::vector<std::string> LAppConfig::ParseStringArray(const std::string& s, const std::string& key)
{
    std::vector<std::string> result;
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return result;

    pos = s.find('[', pos + pattern.size());
    if (pos == std::string::npos) return result;

    size_t end = s.find(']', pos);
    if (end == std::string::npos) return result;

    std::string arrayContent = s.substr(pos + 1, end - pos - 1);

    size_t cur = 0;
    while (cur < arrayContent.size())
    {
        size_t qStart = arrayContent.find('"', cur);
        if (qStart == std::string::npos) break;

        size_t qEnd = arrayContent.find('"', qStart + 1);
        if (qEnd == std::string::npos) break;

        result.push_back(arrayContent.substr(qStart + 1, qEnd - qStart - 1));
        cur = qEnd + 1;
    }

    return result;
}

std::string LAppConfig::StripComments(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    bool inString = false;

    for (size_t i = 0; i < s.size(); i++)
    {
        if (inString)
        {
            result += s[i];
            if (s[i] == '\\' && i + 1 < s.size())
            {
                result += s[++i];
            }
            else if (s[i] == '"')
            {
                inString = false;
            }
        }
        else if (s[i] == '"')
        {
            inString = true;
            result += s[i];
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            // Skip until end of line
            while (i < s.size() && s[i] != '\n') i++;
            if (i < s.size()) result += '\n';
        }
        else
        {
            result += s[i];
        }
    }

    return result;
}

bool LAppConfig::LoadFromFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = StripComments(ss.str());

    additionalModelDirs = ParseStringArray(content, "additional_model_dirs");
    for (size_t i = 0; i < additionalModelDirs.size(); i++)
    {
        additionalModelDirs[i] = ExpandTilde(additionalModelDirs[i]);
    }
    defaultModel = ParseStringValue(content, "default_model");
    emotionTimeout = ParseFloatValue(content, "emotion_timeout", 5.0f);
    modelScale = ParseFloatValue(content, "model_scale", 1.0f);
    modelX = ParseFloatValue(content, "model_x", 0.0f);
    modelY = ParseFloatValue(content, "model_y", 0.0f);
    windowWidth = ParseIntValue(content, "window_width", 1900);
    windowHeight = ParseIntValue(content, "window_height", 1000);

    return true;
}
