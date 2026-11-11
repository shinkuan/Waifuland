#include "LAppIPC.hpp"
#include "LAppDelegate.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppModel.hpp"
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppJson.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <poll.h>

using namespace Csm;

namespace {
    LAppIPC* s_instance = NULL;
}

float LAppIPC::_externalMouthY = 0.0f;
bool LAppIPC::_hasExternalMouthY = false;
float LAppIPC::_externalLookX = 0.0f;
float LAppIPC::_externalLookY = 0.0f;
bool LAppIPC::_hasExternalLook = false;
double LAppIPC::_externalMouthUntil = 0.0;
double LAppIPC::_externalLookUntil = 0.0;

static double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool LAppIPC::HasExternalMouthY()
{
    if (_hasExternalMouthY && _externalMouthUntil > 0.0 && NowSeconds() >= _externalMouthUntil)
        _hasExternalMouthY = false;
    return _hasExternalMouthY;
}

bool LAppIPC::HasExternalLook()
{
    if (_hasExternalLook && _externalLookUntil > 0.0 && NowSeconds() >= _externalLookUntil)
        _hasExternalLook = false;
    return _hasExternalLook;
}

void LAppIPC::ClearOverrides()
{
    _hasExternalMouthY = false;
    _hasExternalLook = false;
}

LAppIPC* LAppIPC::GetInstance()
{
    if (s_instance == NULL)
    {
        s_instance = new LAppIPC();
    }
    return s_instance;
}

void LAppIPC::ReleaseInstance()
{
    if (s_instance != NULL)
    {
        delete s_instance;
        s_instance = NULL;
    }
}

LAppIPC::LAppIPC()
    : _serverFd(-1)
{
}

LAppIPC::~LAppIPC()
{
    Release();
}

bool LAppIPC::Initialize()
{
    // Determine socket path
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (runtimeDir && strlen(runtimeDir) > 0)
    {
        _socketPath = std::string(runtimeDir) + "/waifuland.sock";
    }
    else
    {
        _socketPath = "/tmp/waifuland.sock";
    }

    // Remove stale socket
    unlink(_socketPath.c_str());

    _serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_serverFd < 0)
    {
        LAppPal::PrintLogLn("[IPC] Failed to create socket: %s", strerror(errno));
        return false;
    }

    // Set non-blocking
    int flags = fcntl(_serverFd, F_GETFL, 0);
    fcntl(_serverFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LAppPal::PrintLogLn("[IPC] Failed to bind socket: %s", strerror(errno));
        close(_serverFd);
        _serverFd = -1;
        return false;
    }

    if (listen(_serverFd, 4) < 0)
    {
        LAppPal::PrintLogLn("[IPC] Failed to listen: %s", strerror(errno));
        close(_serverFd);
        _serverFd = -1;
        unlink(_socketPath.c_str());
        return false;
    }

    LAppPal::PrintLogLn("[IPC] Listening on %s", _socketPath.c_str());
    return true;
}

void LAppIPC::Release()
{
    for (size_t i = 0; i < _clients.size(); i++)
    {
        close(_clients[i]);
    }
    _clients.clear();
    _clientBuffers.clear();

    if (_serverFd >= 0)
    {
        close(_serverFd);
        _serverFd = -1;
    }

    if (!_socketPath.empty())
    {
        unlink(_socketPath.c_str());
        _socketPath.clear();
    }
}

std::string& LAppIPC::GetClientBuffer(int fd)
{
    for (size_t i = 0; i < _clientBuffers.size(); i++)
    {
        if (_clientBuffers[i].fd == fd) return _clientBuffers[i].buf;
    }
    ClientBuffer cb;
    cb.fd = fd;
    _clientBuffers.push_back(cb);
    return _clientBuffers.back().buf;
}

void LAppIPC::RemoveClientBuffer(int fd)
{
    for (size_t i = 0; i < _clientBuffers.size(); i++)
    {
        if (_clientBuffers[i].fd == fd)
        {
            _clientBuffers.erase(_clientBuffers.begin() + i);
            return;
        }
    }
}

void LAppIPC::Poll()
{
    if (_serverFd < 0) return;

    // Accept new connections
    while (true)
    {
        int clientFd = accept(_serverFd, NULL, NULL);
        if (clientFd < 0) break;

        int flags = fcntl(clientFd, F_GETFL, 0);
        fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
        _clients.push_back(clientFd);
    }

    // Process existing clients
    std::vector<int> toRemove;
    for (size_t i = 0; i < _clients.size(); i++)
    {
        int fd = _clients[i];
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0)
        {
            buf[n] = '\0';
            std::string& clientBuf = GetClientBuffer(fd);
            clientBuf += buf;

            // Process complete lines
            size_t pos;
            while ((pos = clientBuf.find('\n')) != std::string::npos)
            {
                std::string line = clientBuf.substr(0, pos);
                clientBuf.erase(0, pos + 1);

                if (!line.empty() && line != "\r")
                {
                    std::string response = ProcessLine(line);
                    response += "\n";
                    WriteAll(fd, response);
                }
            }
        }
        else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            close(fd);
            RemoveClientBuffer(fd);
            toRemove.push_back(fd);
        }
    }

    // Remove disconnected clients
    for (size_t i = 0; i < toRemove.size(); i++)
    {
        _clients.erase(std::remove(_clients.begin(), _clients.end(), toRemove[i]), _clients.end());
    }
}

// ─── Output ───────────────────────────────────────────────────────────────────

void LAppIPC::WriteAll(int fd, const std::string& data)
{
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Large responses can exceed the socket buffer; wait briefly.
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, 1000) > 0) continue;
        }
        if (n < 0 && errno == EINTR) continue;
        LAppPal::PrintLogLn("[IPC] Failed to write response (%zu/%zu bytes)", off, data.size());
        return;
    }
}

// ─── Request helpers ─────────────────────────────────────────────────────────

namespace {

std::string Error(const std::string& message)
{
    return JsonObjectBuilder().Add("ok", "false").Add("error", JsonQuote(message)).Str();
}

const std::string kOk = "{\"ok\":true}";

// String argument; numbers are accepted and formatted (e.g. a model named "2").
bool ArgString(const JsonValue& req, const char* key, std::string& out)
{
    const JsonValue* v = req.Find(key);
    if (!v) return false;
    if (v->IsString()) { out = v->AsString(); return true; }
    if (v->IsNumber()) { out = JsonNumber(v->AsNumber()); return true; }
    return false;
}

// Number argument; numeric strings are accepted.
bool ArgNumber(const JsonValue& req, const char* key, double& out)
{
    const JsonValue* v = req.Find(key);
    if (!v) return false;
    if (v->IsNumber()) { out = v->AsNumber(); return true; }
    if (v->IsString() && !v->AsString().empty())
    {
        char* end = NULL;
        double d = strtod(v->AsString().c_str(), &end);
        if (end && *end == '\0') { out = d; return true; }
    }
    return false;
}

double ArgNumberOr(const JsonValue& req, const char* key, double def)
{
    double v;
    return ArgNumber(req, key, v) ? v : def;
}

// Boolean argument; accepts true/false, 1/0, "true"/"false".
bool ArgBool(const JsonValue& req, const char* key, bool& out)
{
    const JsonValue* v = req.Find(key);
    if (!v) return false;
    if (v->IsBool()) { out = v->AsBool(); return true; }
    if (v->IsNumber()) { out = v->AsNumber() != 0.0; return true; }
    if (v->IsString())
    {
        if (v->AsString() == "true" || v->AsString() == "1") { out = true; return true; }
        if (v->AsString() == "false" || v->AsString() == "0") { out = false; return true; }
    }
    return false;
}

bool ArgFlag(const JsonValue& req, const char* key)
{
    bool b = false;
    return ArgBool(req, key, b) && b;
}

// Array of strings, or a single string, or a comma-separated string.
bool ArgStringList(const JsonValue& req, const char* key, std::vector<std::string>& out)
{
    const JsonValue* v = req.Find(key);
    if (!v) return false;
    out.clear();
    if (v->IsArray())
    {
        for (size_t i = 0; i < v->Items().size(); i++)
        {
            if (!v->Items()[i].IsString()) return false;
            out.push_back(v->Items()[i].AsString());
        }
        return true;
    }
    if (v->IsString())
    {
        std::string s = v->AsString();
        size_t start = 0;
        while (start <= s.size())
        {
            size_t comma = s.find(',', start);
            if (comma == std::string::npos) comma = s.size();
            std::string item = s.substr(start, comma - start);
            if (!item.empty()) out.push_back(item);
            start = comma + 1;
        }
        return true;
    }
    return false;
}

std::string ToLower(std::string s)
{
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    return s;
}

// Keys that configure a command rather than name a pose/parameter value.
bool IsReservedKey(const std::string& key)
{
    return key == "command" || key == "duration" || key == "fade" || key == "blend" ||
           key == "pose" || key == "params" || key == "id";
}

std::string CurrentModelName(LAppLive2DManager* mgr)
{
    csmVector<csmString> dirs = mgr->GetModelDir();
    csmInt32 idx = mgr->GetSceneIndex();
    if (idx >= 0 && idx < (csmInt32)dirs.GetSize()) return dirs[idx].GetRawString();
    return "";
}

std::string MotionListJson(LAppModel* model)
{
    std::vector<LAppModel::MotionInfo> motions = model->GetMotionList();
    std::string out = "[";
    for (size_t i = 0; i < motions.size(); i++)
    {
        if (i > 0) out += ",";
        out += JsonObjectBuilder()
            .Add("group", JsonQuote(motions[i].group))
            .Add("index", JsonNumber(motions[i].index))
            .Add("file", JsonQuote(motions[i].file))
            .Add("duration", JsonNumber(motions[i].duration))
            .Add("fade_in", JsonNumber(motions[i].fadeIn))
            .Add("fade_out", JsonNumber(motions[i].fadeOut))
            .Str();
    }
    return out + "]";
}

std::string MotionStateJson(LAppModel* model)
{
    LAppModel::MotionState st = model->GetMotionState();
    return JsonObjectBuilder()
        .Add("group", st.group.empty() ? "null" : JsonQuote(st.group))
        .Add("index", st.index < 0 ? "null" : JsonNumber(st.index))
        .Add("playing", JsonBool(st.playing))
        .Str();
}

std::string OverridesJson(LAppModel* model)
{
    std::vector<LAppModel::OverrideInfo> ov = model->GetParameterOverrides();
    std::string out = "[";
    for (size_t i = 0; i < ov.size(); i++)
    {
        if (i > 0) out += ",";
        out += JsonObjectBuilder()
            .Add("id", JsonQuote(ov[i].id))
            .Add("value", JsonNumber(ov[i].value))
            .Add("blend", JsonQuote(ov[i].blend))
            .Add("remaining", ov[i].remaining < 0 ? "null" : JsonNumber(ov[i].remaining))
            .Add("releasing", JsonBool(ov[i].releasing))
            .Str();
    }
    return out + "]";
}

bool ParseBlend(const std::string& s, LAppModel::OverrideBlend& out)
{
    if (s.empty() || s == "overwrite" || s == "set") { out = LAppModel::OverrideOverwrite; return true; }
    if (s == "add") { out = LAppModel::OverrideAdd; return true; }
    if (s == "multiply") { out = LAppModel::OverrideMultiply; return true; }
    return false;
}

// One-line help per command, returned by list_commands.
struct CommandHelp { const char* name; const char* args; const char* description; };
const CommandHelp kCommands[] = {
    { "list_commands", "", "List all commands" },
    { "describe_model", "", "Summary of the current model: expressions, motions, pose controls, parameter groups, current state" },
    { "get_status", "", "Window/model status plus current expression, toggles, motion, overrides, look and mouth state" },
    { "get_available_models", "", "List model names" },
    { "get_current_model", "", "Current model name and index" },
    { "set_model", "name | index", "Switch model" },
    { "next_model", "", "Switch to the next model" },
    { "prev_model", "", "Switch to the previous model" },
    { "get_expressions", "detail?", "List expressions; `active` = toggled ones, `current` = set_expression one; detail=true adds parameters" },
    { "set_expression", "id, duration?", "Set the main expression (replaces the previous one); duration seconds, -1 = never revert, default = config emotion_timeout" },
    { "toggle_expression", "id, active?, duration?", "Stackable expression; flips state, or sets it when active is given; duration = seconds before it turns off" },
    { "clear_expressions", "", "Turn off all expressions" },
    { "get_motions", "", "List motions (group, index, file, duration)" },
    { "do_motion", "group?, index?, priority?", "Play a motion; no index = random in group; no group = random tap motion; priority 1 idle, 2 normal (default), 3 force" },
    { "stop_motion", "", "Stop motions (idle resumes)" },
    { "get_parameters", "filter?, ids?", "List parameters with display name, group, range, default, current value, physics info" },
    { "set_parameters", "params{id:value}, duration?, fade?, blend?", "Temporarily drive parameters; blend overwrite|add|multiply; duration -1 = until released; fade seconds (default 0.3)" },
    { "release_parameters", "ids?, fade?", "Release parameter overrides (all if ids omitted)" },
    { "set_pose", "pose{alias:value} or alias keys, duration?, fade?", "Model-independent pose controls (head_x, eye_open, mouth_open, ...); see describe_model for ranges" },
    { "release_pose", "keys?, fade?", "Release pose aliases (all overrides if keys omitted)" },
    { "set_look", "x, y, duration? | reset", "Override gaze direction (-1..1, + = viewer's right / up); reset returns control to the cursor" },
    { "set_mouth_y", "value, duration? | reset", "Override lip-sync mouth opening (0..1)" },
    { "reset_model", "", "Stop motions, clear expressions/overrides/look/mouth, reset all parameters to defaults" },
    { "set_model_zoom", "value", "Model scale 0.1..10" },
    { "set_model_position", "x?, y?", "Model offset" },
    { "get_model_position", "", "Model offset and scale" },
    { "toggle_hidden", "", "Toggle window visibility" },
    { "switch_skin", "", "Cycle skins (models whose motion groups are outfits)" },
    { "batch", "commands[]", "Run several commands in the same frame; returns results[]" },
};

} // namespace

// ─── Command dispatch ─────────────────────────────────────────────────────────

std::string LAppIPC::ProcessLine(const std::string& line)
{
    JsonValue req;
    std::string error;
    if (!JsonValue::Parse(line, req, error)) return Error("invalid JSON: " + error);
    if (!req.IsObject()) return Error("request must be a JSON object");

    std::string command;
    if (ArgString(req, "command", command) && command == "batch")
    {
        const JsonValue* cmds = req.Find("commands");
        if (!cmds || !cmds->IsArray()) return Error("batch needs 'commands' array");
        std::string results = "[";
        bool allOk = true;
        for (size_t i = 0; i < cmds->Items().size(); i++)
        {
            const JsonValue& sub = cmds->Items()[i];
            std::string subCommand;
            std::string r;
            if (!sub.IsObject()) r = Error("command must be an object");
            else if (ArgString(sub, "command", subCommand) && subCommand == "batch") r = Error("nested batch not allowed");
            else r = ProcessCommand(sub);
            if (r.compare(0, 11, "{\"ok\":false") == 0) allOk = false;
            if (i > 0) results += ",";
            results += r;
        }
        results += "]";
        return JsonObjectBuilder().Add("ok", JsonBool(allOk)).Add("results", results).Str();
    }
    return ProcessCommand(req);
}

std::string LAppIPC::ProcessCommand(const JsonValue& req)
{
    std::string command;
    if (!ArgString(req, "command", command) || command.empty())
    {
        return Error("missing 'command' field");
    }

    LAppDelegate* app = LAppDelegate::GetInstance();
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();

    // ── Commands that do not need a model ──

    if (command == "list_commands")
    {
        std::string out = "[";
        for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++)
        {
            if (i > 0) out += ",";
            out += JsonObjectBuilder()
                .Add("command", JsonQuote(kCommands[i].name))
                .Add("args", JsonQuote(kCommands[i].args))
                .Add("description", JsonQuote(kCommands[i].description))
                .Str();
        }
        return JsonObjectBuilder().Add("ok", "true").Add("commands", out + "]").Str();
    }

    if (command == "get_available_models")
    {
        csmVector<csmString> dirs = mgr->GetModelDir();
        std::vector<std::string> names;
        for (csmUint32 i = 0; i < dirs.GetSize(); i++) names.push_back(dirs[i].GetRawString());
        return JsonObjectBuilder().Add("ok", "true").Add("models", JsonStringArray(names)).Str();
    }

    if (command == "get_current_model")
    {
        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("model", JsonQuote(CurrentModelName(mgr)))
            .Add("index", JsonNumber(mgr->GetSceneIndex()))
            .Str();
    }

    if (command == "set_model")
    {
        std::string name;
        int index = (int)ArgNumberOr(req, "index", -1);
        if (ArgString(req, "name", name) && !name.empty())
        {
            index = -1;
            csmVector<csmString> dirs = mgr->GetModelDir();
            for (csmInt32 i = 0; i < (csmInt32)dirs.GetSize(); i++)
            {
                if (name == dirs[i].GetRawString()) { index = i; break; }
            }
            if (index < 0) return Error("model not found: " + name);
        }
        if (index < 0 || index >= mgr->GetModelDirSize()) return Error("invalid index");
        mgr->ChangeScene(index);
        return kOk;
    }

    if (command == "next_model")
    {
        mgr->NextScene();
        return kOk;
    }

    if (command == "prev_model")
    {
        csmInt32 size = mgr->GetModelDirSize();
        if (size > 0) mgr->ChangeScene((mgr->GetSceneIndex() - 1 + size) % size);
        return kOk;
    }

    if (command == "set_model_zoom")
    {
        double zoom;
        if (!ArgNumber(req, "value", zoom) || zoom < 0.1 || zoom > 10.0) return Error("value must be 0.1..10.0");
        app->_modelScale = (float)zoom;
        return kOk;
    }

    if (command == "set_model_position")
    {
        app->_modelX = (float)ArgNumberOr(req, "x", app->_modelX);
        app->_modelY = (float)ArgNumberOr(req, "y", app->_modelY);
        return kOk;
    }

    if (command == "get_model_position")
    {
        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("x", JsonNumber(app->_modelX))
            .Add("y", JsonNumber(app->_modelY))
            .Add("zoom", JsonNumber(app->_modelScale))
            .Str();
    }

    if (command == "toggle_hidden")
    {
        app->ToggleHidden();
        return JsonObjectBuilder().Add("ok", "true").Add("hidden", JsonBool(app->_isHidden)).Str();
    }

    if (command == "switch_skin")
    {
        mgr->SwitchSkin();
        return kOk;
    }

    if (command == "set_look")
    {
        if (ArgFlag(req, "reset"))
        {
            _hasExternalLook = false;
            return kOk;
        }
        double x, y;
        if (!ArgNumber(req, "x", x) || !ArgNumber(req, "y", y)) return Error("need x and y (-1..1), or reset");
        double duration = ArgNumberOr(req, "duration", -1.0);
        _externalLookX = (float)std::max(-1.0, std::min(1.0, x));
        _externalLookY = (float)std::max(-1.0, std::min(1.0, y));
        _externalLookUntil = duration > 0.0 ? NowSeconds() + duration : 0.0;
        _hasExternalLook = true;
        return kOk;
    }

    if (command == "set_mouth_y")
    {
        if (ArgFlag(req, "reset"))
        {
            _hasExternalMouthY = false;
            return kOk;
        }
        double value;
        if (!ArgNumber(req, "value", value)) return Error("need value (0..1), or reset");
        double duration = ArgNumberOr(req, "duration", -1.0);
        _externalMouthY = (float)std::max(0.0, std::min(1.0, value));
        _externalMouthUntil = duration > 0.0 ? NowSeconds() + duration : 0.0;
        _hasExternalMouthY = true;
        return kOk;
    }

    // ── Commands that need a model ──

    LAppModel* model = mgr->GetModel(0);

    if (command == "get_status")
    {
        JsonObjectBuilder b;
        b.Add("ok", "true")
         .Add("model", JsonQuote(CurrentModelName(mgr)))
         .Add("model_index", JsonNumber(mgr->GetSceneIndex()))
         .Add("model_count", JsonNumber(mgr->GetModelDirSize()))
         .Add("hidden", JsonBool(app->_isHidden))
         .Add("zoom", JsonNumber(app->_modelScale))
         .Add("x", JsonNumber(app->_modelX))
         .Add("y", JsonNumber(app->_modelY));
        if (model)
        {
            b.Add("expression", model->GetCurrentExpression().empty() ? "null" : JsonQuote(model->GetCurrentExpression()))
             .Add("toggled_expressions", JsonStringArray(model->GetActiveToggledExpressions()))
             .Add("motion", MotionStateJson(model))
             .Add("overrides", OverridesJson(model));
        }
        b.Add("look", HasExternalLook()
                ? JsonObjectBuilder().Add("x", JsonNumber(_externalLookX)).Add("y", JsonNumber(_externalLookY))
                      .Add("remaining", _externalLookUntil > 0.0 ? JsonNumber(_externalLookUntil - NowSeconds()) : "null").Str()
                : "null");
        b.Add("mouth_y", HasExternalMouthY()
                ? JsonObjectBuilder().Add("value", JsonNumber(_externalMouthY))
                      .Add("remaining", _externalMouthUntil > 0.0 ? JsonNumber(_externalMouthUntil - NowSeconds()) : "null").Str()
                : "null");
        return b.Str();
    }

    if (!model) return Error("no model loaded");

    if (command == "describe_model")
    {
        std::vector<LAppModel::ParameterInfo> params = model->GetParameterInfos();
        std::vector<std::string> groups;
        for (size_t i = 0; i < params.size(); i++)
        {
            if (!params[i].group.empty() && std::find(groups.begin(), groups.end(), params[i].group) == groups.end())
                groups.push_back(params[i].group);
        }

        std::string pose = "{";
        std::vector<LAppModel::PoseAliasInfo> aliases = model->GetPoseAliases();
        for (size_t i = 0; i < aliases.size(); i++)
        {
            if (i > 0) pose += ",";
            pose += JsonQuote(aliases[i].name) + ":" + JsonObjectBuilder()
                .Add("range", JsonQuote(aliases[i].range))
                .Add("description", JsonQuote(aliases[i].description))
                .Add("params", JsonStringArray(aliases[i].params))
                .Add("supported", JsonBool(!aliases[i].params.empty()))
                .Str();
        }
        pose += "}";

        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("model", JsonQuote(CurrentModelName(mgr)))
            .Add("expressions", JsonStringArray(model->GetExpressionIds()))
            .Add("motions", MotionListJson(model))
            .Add("pose", pose)
            .Add("parameter_count", JsonNumber((double)params.size()))
            .Add("parameter_groups", JsonStringArray(groups))
            .Add("hit_areas", JsonStringArray(model->GetHitAreaNames()))
            .Add("look_origin", JsonQuote(model->HasLookOrigin() ? "eyes" : "model center"))
            .Add("state", JsonObjectBuilder()
                .Add("expression", model->GetCurrentExpression().empty() ? "null" : JsonQuote(model->GetCurrentExpression()))
                .Add("toggled_expressions", JsonStringArray(model->GetActiveToggledExpressions()))
                .Add("motion", MotionStateJson(model))
                .Add("overrides", OverridesJson(model))
                .Str())
            .Str();
    }

    if (command == "get_expressions")
    {
        std::vector<std::string> exprs = model->GetExpressionIds();
        std::string list;
        if (ArgFlag(req, "detail"))
        {
            list = "[";
            for (size_t i = 0; i < exprs.size(); i++)
            {
                if (i > 0) list += ",";
                std::vector<LAppModel::ExpressionParamInfo> ps = model->GetExpressionParams(exprs[i]);
                std::string pl = "[";
                for (size_t p = 0; p < ps.size(); p++)
                {
                    if (p > 0) pl += ",";
                    pl += JsonObjectBuilder()
                        .Add("id", JsonQuote(ps[p].id))
                        .Add("name", JsonQuote(ps[p].name))
                        .Add("value", JsonNumber(ps[p].value))
                        .Add("blend", JsonQuote(ps[p].blend))
                        .Str();
                }
                list += JsonObjectBuilder().Add("name", JsonQuote(exprs[i])).Add("params", pl + "]").Str();
            }
            list += "]";
        }
        else
        {
            list = JsonStringArray(exprs);
        }
        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("expressions", list)
            .Add("active", JsonStringArray(model->GetActiveToggledExpressions()))
            .Add("current", model->GetCurrentExpression().empty() ? "null" : JsonQuote(model->GetCurrentExpression()))
            .Str();
    }

    if (command == "set_expression")
    {
        std::string id;
        if (!ArgString(req, "id", id) || id.empty()) return Error("missing 'id'");
        double duration = ArgNumberOr(req, "duration", -2.0);
        if (!model->SetExpression(id.c_str(), (float)duration)) return Error("unknown expression: " + id);
        return kOk;
    }

    if (command == "toggle_expression")
    {
        std::string id;
        if (!ArgString(req, "id", id) || id.empty()) return Error("missing 'id'");
        bool active;
        double duration = ArgNumberOr(req, "duration", -1.0);
        if (ArgBool(req, "active", active) || duration > 0.0)
        {
            if (duration > 0.0 && !req.Find("active")) active = true;
            if (!model->SetToggledExpression(id.c_str(), active, (float)duration)) return Error("unknown expression: " + id);
        }
        else
        {
            csmBool nowActive = false;
            if (!model->ToggleExpression(id.c_str(), nowActive)) return Error("unknown expression: " + id);
            active = nowActive;
        }
        return JsonObjectBuilder().Add("ok", "true").Add("active", JsonBool(active)).Str();
    }

    if (command == "clear_expressions")
    {
        model->ClearExpressions();
        return kOk;
    }

    if (command == "get_motions")
    {
        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("motions", MotionListJson(model))
            .Add("state", MotionStateJson(model))
            .Str();
    }

    if (command == "do_motion")
    {
        std::string group;
        int priority = (int)ArgNumberOr(req, "priority", LAppDefine::PriorityNormal);
        if (priority < LAppDefine::PriorityIdle || priority > LAppDefine::PriorityForce) return Error("priority must be 1..3");

        CubismMotionQueueEntryHandle handle;
        if (!ArgString(req, "group", group) || group.empty())
        {
            handle = model->StartRandomMotion(LAppDefine::MotionGroupTapBody, priority);
            if (handle == InvalidMotionQueueEntryHandleValue && !model->HasMotion(LAppDefine::MotionGroupTapBody, 0))
            {
                return Error("model has no TapBody motions; pass a group (see get_motions)");
            }
        }
        else
        {
            double index;
            if (ArgNumber(req, "index", index))
            {
                if (!model->HasMotion(group, (int)index)) return Error("unknown motion: " + group + " #" + JsonNumber(index));
                handle = model->StartMotion(group.c_str(), (int)index, priority);
            }
            else
            {
                if (!model->HasMotion(group, 0)) return Error("unknown motion group: " + group);
                handle = model->StartRandomMotion(group.c_str(), priority);
            }
        }
        if (handle == InvalidMotionQueueEntryHandleValue)
        {
            return Error("motion not started: a motion with equal or higher priority is playing (use priority 3 to force)");
        }
        return JsonObjectBuilder().Add("ok", "true").Add("motion", MotionStateJson(model)).Str();
    }

    if (command == "stop_motion")
    {
        model->StopMotions();
        return kOk;
    }

    if (command == "get_parameters")
    {
        std::string filter;
        ArgString(req, "filter", filter);
        filter = ToLower(filter);
        std::vector<std::string> ids;
        bool byIds = ArgStringList(req, "ids", ids);

        std::vector<LAppModel::ParameterInfo> params = model->GetParameterInfos();
        std::string out = "[";
        int n = 0;
        for (size_t i = 0; i < params.size(); i++)
        {
            const LAppModel::ParameterInfo& p = params[i];
            if (byIds && std::find(ids.begin(), ids.end(), p.id) == ids.end()) continue;
            if (!filter.empty() &&
                ToLower(p.id).find(filter) == std::string::npos &&
                ToLower(p.name).find(filter) == std::string::npos &&
                ToLower(p.group).find(filter) == std::string::npos) continue;
            if (n++ > 0) out += ",";
            JsonObjectBuilder b;
            b.Add("id", JsonQuote(p.id))
             .Add("name", JsonQuote(p.name))
             .Add("group", JsonQuote(p.group))
             .Add("min", JsonNumber(p.min))
             .Add("max", JsonNumber(p.max))
             .Add("default", JsonNumber(p.def))
             .Add("value", JsonNumber(p.value));
            if (p.physicsOutput) b.Add("physics_output", "true");
            if (!p.physicsSource.empty()) b.Add("physics_source", JsonQuote(p.physicsSource));
            out += b.Str();
        }
        out += "]";
        return JsonObjectBuilder().Add("ok", "true").Add("count", JsonNumber(n)).Add("parameters", out).Str();
    }

    if (command == "set_parameters")
    {
        const JsonValue* params = req.Find("params");
        if (!params || !params->IsObject() || params->Keys().empty()) return Error("need 'params' object {id: value}");
        std::string blendName;
        ArgString(req, "blend", blendName);
        LAppModel::OverrideBlend blend;
        if (!ParseBlend(blendName, blend)) return Error("blend must be overwrite, add or multiply");
        double duration = ArgNumberOr(req, "duration", -1.0);
        double fade = ArgNumberOr(req, "fade", 0.3);

        // Validate everything first so a request is applied all-or-nothing.
        for (size_t i = 0; i < params->Keys().size(); i++)
        {
            if (!params->Items()[i].IsNumber()) return Error("value for " + params->Keys()[i] + " must be a number");
            if (!model->HasParameter(params->Keys()[i])) return Error("unknown parameter: " + params->Keys()[i]);
        }
        std::string err;
        for (size_t i = 0; i < params->Keys().size(); i++)
        {
            model->SetParameterOverride(params->Keys()[i], (float)params->Items()[i].AsNumber(), blend,
                                        (float)duration, (float)fade, err);
        }
        return JsonObjectBuilder().Add("ok", "true").Add("applied", JsonStringArray(params->Keys())).Str();
    }

    if (command == "release_parameters")
    {
        std::vector<std::string> ids;
        ArgStringList(req, "ids", ids);
        double fade = ArgNumberOr(req, "fade", -1.0);
        int n = model->ReleaseParameterOverrides(ids, (float)fade);
        return JsonObjectBuilder().Add("ok", "true").Add("released", JsonNumber(n)).Str();
    }

    if (command == "set_pose")
    {
        // Values come from "pose": {...} or from alias keys at the top level.
        std::vector<std::string> keys;
        std::vector<double> values;
        const JsonValue* pose = req.Find("pose");
        const JsonValue& src = (pose && pose->IsObject()) ? *pose : req;
        for (size_t i = 0; i < src.Keys().size(); i++)
        {
            const std::string& k = src.Keys()[i];
            if (&src == &req && IsReservedKey(k)) continue;
            if (!LAppModel::IsPoseAlias(k)) return Error("unknown pose key: " + k + " (see describe_model)");
            double v;
            const JsonValue& item = src.Items()[i];
            if (item.IsNumber()) v = item.AsNumber();
            else if (!(item.IsString() && ArgNumber(src, k.c_str(), v))) return Error("value for " + k + " must be a number");
            keys.push_back(k);
            values.push_back(v);
        }
        if (keys.empty()) return Error("no pose values given");

        double duration = ArgNumberOr(req, "duration", -1.0);
        double fade = ArgNumberOr(req, "fade", 0.3);
        std::string applied = "{";
        std::vector<std::string> unsupported;
        int n = 0;
        std::string err;
        for (size_t i = 0; i < keys.size(); i++)
        {
            std::vector<LAppModel::PoseTarget> targets;
            model->ResolvePose(keys[i], (float)values[i], targets);
            if (targets.empty()) { unsupported.push_back(keys[i]); continue; }
            std::string t = "{";
            for (size_t j = 0; j < targets.size(); j++)
            {
                model->SetParameterOverride(targets[j].param, targets[j].value, LAppModel::OverrideOverwrite,
                                            (float)duration, (float)fade, err);
                if (j > 0) t += ",";
                t += JsonQuote(targets[j].param) + ":" + JsonNumber(targets[j].value);
            }
            if (n++ > 0) applied += ",";
            applied += JsonQuote(keys[i]) + ":" + t + "}";
        }
        applied += "}";
        return JsonObjectBuilder()
            .Add("ok", "true")
            .Add("applied", applied)
            .Add("unsupported", JsonStringArray(unsupported))
            .Str();
    }

    if (command == "release_pose")
    {
        std::vector<std::string> keys;
        double fade = ArgNumberOr(req, "fade", -1.0);
        if (!ArgStringList(req, "keys", keys) || keys.empty())
        {
            int n = model->ReleaseParameterOverrides(std::vector<std::string>(), (float)fade);
            return JsonObjectBuilder().Add("ok", "true").Add("released", JsonNumber(n)).Str();
        }
        std::vector<std::string> ids;
        for (size_t i = 0; i < keys.size(); i++)
        {
            if (!LAppModel::IsPoseAlias(keys[i])) return Error("unknown pose key: " + keys[i]);
            std::vector<LAppModel::PoseTarget> targets;
            model->ResolvePose(keys[i], 0.0f, targets);
            for (size_t j = 0; j < targets.size(); j++) ids.push_back(targets[j].param);
        }
        int n = ids.empty() ? 0 : model->ReleaseParameterOverrides(ids, (float)fade);
        return JsonObjectBuilder().Add("ok", "true").Add("released", JsonNumber(n)).Str();
    }

    if (command == "reset_model")
    {
        model->ResetState();
        ClearOverrides();
        return kOk;
    }

    return Error("unknown command: " + command + " (see list_commands)");
}
