/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include <dirent.h>
#include <cstring>
#include <stdlib.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <cfloat>
#include <Utils/CubismJson.hpp>
#include <Math/CubismMath.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/ICubismUpdater.hpp>
#include <CubismModelSettingJson.hpp>
#include <Motion/CubismMotion.hpp>
#include <Physics/CubismPhysics.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Utils/CubismString.hpp>
#include <Id/CubismIdManager.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include "LAppDefine.hpp"
#include "LAppConfig.hpp"
#include "LAppPal.hpp"
#include "LAppIPC.hpp"
#include "LAppTextureManager.hpp"
#include "LAppDelegate.hpp"
#include <Motion/CubismMotionJson.hpp>
#include "Motion/CubismBreathUpdater.hpp"
#include "Motion/CubismLookUpdater.hpp"
#include "Motion/CubismExpressionUpdater.hpp"
#include "Motion/CubismEyeBlinkUpdater.hpp"
#include "Motion/CubismLipSyncUpdater.hpp"
#include "Motion/CubismPhysicsUpdater.hpp"
#include "Motion/CubismPoseUpdater.hpp"

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;
using namespace LAppDefine;


// Stackable expression layers applied right after the regular expression
// manager and before physics, so toggles feeding physics inputs still work.
class ExpressionToggleUpdater : public ICubismUpdater
{
public:
    ExpressionToggleUpdater() : ICubismUpdater(CubismUpdateOrder_Expression + 1) {}

    // Returns the new state.
    csmBool Toggle(const std::string& name, CubismExpressionMotion* expression)
    {
        for (size_t i = 0; i < _layers.size(); ++i)
        {
            if (_layers[i].name == name)
            {
                Set(name, expression, !_layers[i].active, -1.0f);
                return _layers[i].active;
            }
        }
        Set(name, expression, true, -1.0f);
        return true;
    }

    // duration < 0: stays in the requested state
    void Set(const std::string& name, CubismExpressionMotion* expression, csmBool active, csmFloat32 duration)
    {
        for (size_t i = 0; i < _layers.size(); ++i)
        {
            if (_layers[i].name == name)
            {
                _layers[i].active = active;
                _layers[i].remaining = active ? duration : -1.0f;
                return;
            }
        }
        if (!active) return;

        Layer layer;
        layer.name = name;
        csmVector<CubismExpressionMotion::ExpressionParameter> params = expression->GetExpressionParameters();
        for (csmUint32 i = 0; i < params.GetSize(); ++i) layer.params.push_back(params[i]);
        layer.fadeIn = expression->GetFadeInTime();
        layer.fadeOut = expression->GetFadeOutTime();
        layer.weight = 0.0f;
        layer.active = true;
        layer.remaining = duration;
        _layers.push_back(layer);
    }

    void Clear()
    {
        for (size_t i = 0; i < _layers.size(); ++i) _layers[i].active = false;
    }

    std::vector<std::string> GetActive() const
    {
        std::vector<std::string> result;
        for (size_t i = 0; i < _layers.size(); ++i)
        {
            if (_layers[i].active) result.push_back(_layers[i].name);
        }
        return result;
    }

    void OnLateUpdate(CubismModel* model, csmFloat32 deltaTimeSeconds) override
    {
        for (size_t i = 0; i < _layers.size();)
        {
            Layer& layer = _layers[i];
            if (layer.active && layer.remaining >= 0.0f)
            {
                layer.remaining -= deltaTimeSeconds;
                if (layer.remaining <= 0.0f) layer.active = false;
            }
            const csmFloat32 fade = layer.active ? layer.fadeIn : layer.fadeOut;
            const csmFloat32 step = (fade > 0.0f) ? deltaTimeSeconds / fade : 1.0f;
            layer.weight = layer.active ? std::min(1.0f, layer.weight + step)
                                        : std::max(0.0f, layer.weight - step);
            if (!layer.active && layer.weight <= 0.0f)
            {
                _layers.erase(_layers.begin() + i);
                continue;
            }

            const csmFloat32 w = CubismMath::GetEasingSine(layer.weight);
            for (size_t p = 0; p < layer.params.size(); ++p)
            {
                const CubismExpressionMotion::ExpressionParameter& param = layer.params[p];
                switch (param.BlendType)
                {
                case CubismExpressionMotion::Additive:
                    model->AddParameterValue(param.ParameterId, param.Value, w);
                    break;
                case CubismExpressionMotion::Multiply:
                    model->MultiplyParameterValue(param.ParameterId, param.Value, w);
                    break;
                case CubismExpressionMotion::Overwrite:
                    model->SetParameterValue(param.ParameterId, param.Value, w);
                    break;
                }
            }
            ++i;
        }
    }

private:
    struct Layer
    {
        std::string name;
        std::vector<CubismExpressionMotion::ExpressionParameter> params;
        csmFloat32 fadeIn;
        csmFloat32 fadeOut;
        csmFloat32 weight;
        csmBool active;
        csmFloat32 remaining; ///< seconds until it switches off, < 0 = never
    };
    std::vector<Layer> _layers;
};

// Temporary per-parameter overrides set over IPC (gaze, pose, mouth, any
// parameter). Applied before physics so physics inputs react, and re-applied
// after the rest of the update for parameters something else overwrote
// later in the frame (physics outputs, lip sync).
class ParameterOverrideUpdater : public ICubismUpdater
{
public:
    struct Entry
    {
        CubismIdHandle id;
        std::string name;
        int blend;            // LAppModel::OverrideBlend
        csmFloat32 from, to;  // value transition
        csmFloat32 transition, transitionTime;
        csmFloat32 weight;    // 0..1 blend-in progress
        csmFloat32 fade;
        csmFloat32 remaining; // < 0 = until released
        csmBool releasing;
        csmFloat32 appliedValue, appliedWeight, written;
    };

    ParameterOverrideUpdater() : ICubismUpdater(CubismUpdateOrder_Physics - 50) {}

    void Set(CubismIdHandle id, const std::string& name, csmFloat32 value, int blend, csmFloat32 duration, csmFloat32 fade)
    {
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            Entry& e = _entries[i];
            if (e.id != id) continue;
            e.from = (e.blend == blend) ? CurrentValue(e) : value;
            e.to = value;
            e.transition = fade;
            e.transitionTime = 0.0f;
            e.blend = blend;
            e.fade = fade;
            e.remaining = duration;
            e.releasing = false;
            return;
        }
        Entry e;
        e.id = id;
        e.name = name;
        e.blend = blend;
        e.from = e.to = value;
        e.transition = e.transitionTime = 0.0f;
        e.weight = 0.0f;
        e.fade = fade;
        e.remaining = duration;
        e.releasing = false;
        e.appliedValue = value;
        e.appliedWeight = 0.0f;
        e.written = 0.0f;
        _entries.push_back(e);
    }

    int Release(const std::vector<CubismIdHandle>& ids, csmFloat32 fade)
    {
        int count = 0;
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            Entry& e = _entries[i];
            if (!ids.empty() && std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
            if (fade >= 0.0f) e.fade = fade;
            if (!e.releasing) count++;
            e.releasing = true;
        }
        return count;
    }

    void Clear() { _entries.clear(); }

    const std::vector<Entry>& Entries() const { return _entries; }

    static csmFloat32 CurrentValue(const Entry& e)
    {
        if (e.transition <= 0.0f || e.transitionTime >= e.transition) return e.to;
        const csmFloat32 t = CubismMath::GetEasingSine(e.transitionTime / e.transition);
        return e.from + (e.to - e.from) * t;
    }

    void OnLateUpdate(CubismModel* model, csmFloat32 deltaTimeSeconds) override
    {
        for (size_t i = 0; i < _entries.size();)
        {
            Entry& e = _entries[i];
            if (!e.releasing && e.remaining >= 0.0f)
            {
                e.remaining -= deltaTimeSeconds;
                if (e.remaining <= 0.0f) e.releasing = true;
            }
            const csmFloat32 step = (e.fade > 0.0f) ? deltaTimeSeconds / e.fade : 1.0f;
            e.weight = e.releasing ? e.weight - step : std::min(1.0f, e.weight + step);
            if (e.releasing && e.weight <= 0.0f)
            {
                _entries.erase(_entries.begin() + i);
                continue;
            }
            if (e.transitionTime < e.transition) e.transitionTime += deltaTimeSeconds;

            e.appliedValue = CurrentValue(e);
            e.appliedWeight = CubismMath::GetEasingSine(e.weight);
            Apply(model, e);
            e.written = model->GetParameterValue(e.id);
            ++i;
        }
    }

    // Called after all other updates of the frame.
    void ApplyLate(CubismModel* model)
    {
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            Entry& e = _entries[i];
            if (e.blend != LAppModel::OverrideOverwrite) continue;
            if (std::fabs(model->GetParameterValue(e.id) - e.written) > 1e-5f) Apply(model, e);
        }
    }

private:
    static void Apply(CubismModel* model, const Entry& e)
    {
        switch (e.blend)
        {
        case LAppModel::OverrideAdd:
            model->AddParameterValue(e.id, e.appliedValue, e.appliedWeight);
            break;
        case LAppModel::OverrideMultiply:
            model->MultiplyParameterValue(e.id, e.appliedValue, e.appliedWeight);
            break;
        default:
            model->SetParameterValue(e.id, e.appliedValue, e.appliedWeight);
            break;
        }
    }

    std::vector<Entry> _entries;
};

namespace {
typedef std::map<CubismIdHandle, CubismIdHandle> PhysicsSourceMap;

// Map each physics output parameter that is fed by exactly one input parameter
// to that input. Some models (typically made for VTube Studio) drive the
// standard head/body angle parameters through physics from custom inputs
// (e.g. AngleX -> ParamAngleX), so writing the standard parameter directly has
// no visible effect: physics overwrites it every frame.
PhysicsSourceMap BuildPhysicsSourceMap(const csmByte* buffer, csmSizeInt size, std::set<CubismIdHandle>* allOutputs = NULL)
{
    PhysicsSourceMap result;
    std::map<CubismIdHandle, int> outputCount;
    Utils::CubismJson* json = Utils::CubismJson::Create(buffer, size);
    if (json == NULL) return result;

    Utils::Value& settings = json->GetRoot()["PhysicsSettings"];
    for (csmInt32 i = 0; i < settings.GetSize(); ++i)
    {
        Utils::Value& inputs = settings[i]["Input"];
        Utils::Value& outputs = settings[i]["Output"];
        for (csmInt32 o = 0; o < outputs.GetSize(); ++o)
        {
            CubismIdHandle dst = CubismFramework::GetIdManager()->GetId(outputs[o]["Destination"]["Id"].GetRawString());
            outputCount[dst]++;
            if (allOutputs) allOutputs->insert(dst);
            if (inputs.GetSize() == 1)
            {
                result[dst] = CubismFramework::GetIdManager()->GetId(inputs[0]["Source"]["Id"].GetRawString());
            }
        }
    }
    Utils::CubismJson::Delete(json);

    // Ambiguous if several physics settings write the same parameter.
    for (std::map<CubismIdHandle, int>::const_iterator it = outputCount.begin(); it != outputCount.end(); ++it)
    {
        if (it->second > 1) result.erase(it->first);
    }
    return result;
}

csmFloat32 ParameterMaxAbs(CubismModel* model, CubismIdHandle id)
{
    const csmInt32 index = model->GetParameterIndex(id);
    if (index < 0 || index >= model->GetParameterCount()) return 0.0f;
    return std::max(std::fabs(model->GetParameterMinimumValue(index)),
                    std::fabs(model->GetParameterMaximumValue(index)));
}

// If `id` is overwritten by physics from a single input, return that input and
// the factor converting a value on `id` to the input's range. Otherwise return
// `id` unchanged with scale 1.
CubismIdHandle ResolvePhysicsSource(const PhysicsSourceMap& sources, CubismModel* model,
                                    CubismIdHandle id, csmFloat32& scale)
{
    scale = 1.0f;
    PhysicsSourceMap::const_iterator it = sources.find(id);
    if (it == sources.end()) return id;

    const csmFloat32 dstRange = ParameterMaxAbs(model, id);
    const csmFloat32 srcRange = ParameterMaxAbs(model, it->second);
    if (dstRange <= 0.0f || srcRange <= 0.0f) return id;

    scale = srcRange / dstRange;
    return it->second;
}
}

const csmFloat32 LAppModel::ExpressionTimeoutSeconds = 5.0f;

LAppModel::LAppModel()
    : LAppModel_Common()
    , _modelSetting(NULL)
    , _userTimeSeconds(0.0f)
    , _motionUpdated(false)
    , _currentSkinIndex(0)
    , _lastExpressionTime(-1.0f)
    , _nextExpressionIndex(0)
    , _expressionToggles(NULL)
    , _hasLookOrigin(false)
    , _currentExpressionTimeout(-1.0f)
    , _lastMotionIndex(-1)
    , _parameterOverrides(NULL)
    , _lookOriginX(0.0f)
    , _lookOriginY(0.0f)
{
    if (DebugLogEnable)
    {
        _debugMode = true;
    }

    _idParamAngleX = CubismFramework::GetIdManager()->GetId(ParamAngleX);
    _idParamAngleY = CubismFramework::GetIdManager()->GetId(ParamAngleY);
    _idParamAngleZ = CubismFramework::GetIdManager()->GetId(ParamAngleZ);
    _idParamBodyAngleX = CubismFramework::GetIdManager()->GetId(ParamBodyAngleX);
    _idParamBodyAngleY = CubismFramework::GetIdManager()->GetId(ParamBodyAngleY);
    _idParamBodyAngleZ = CubismFramework::GetIdManager()->GetId(ParamBodyAngleZ);
    _idParamEyeBallX = CubismFramework::GetIdManager()->GetId(ParamEyeBallX);
    _idParamEyeBallY = CubismFramework::GetIdManager()->GetId(ParamEyeBallY);
}

LAppModel::~LAppModel()
{
    _renderBuffer.DestroyRenderTarget();

    ReleaseMotions();
    ReleaseExpressions();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        ReleaseMotionGroup(group);
    }
    delete(_modelSetting);
}

void LAppModel::LoadAssets(const csmChar* dir, const csmChar* fileName)
{
    _modelHomeDir = dir;

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]load model setting: %s", fileName);
    }

    csmSizeInt size;
    const csmString path = csmString(dir) + fileName;

    csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
    ICubismModelSetting* setting = new CubismModelSettingJson(buffer, size);
    DeleteBuffer(buffer, path.GetRawString());

    SetupModel(setting);

    if (_model == NULL)
    {
        LAppPal::PrintLogLn("Failed to LoadAssets().");
        return;
    }

    CreateRendererForModel();

    SetupTextures();
}


void LAppModel::SetupModel(ICubismModelSetting* setting)
{
    _updating = true;
    _initialized = false;

    _modelSetting = setting;

    csmByte* buffer;
    csmSizeInt size;

    //Cubism Model
    if (strcmp(_modelSetting->GetModelFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetModelFileName();
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]create model: %s", setting->GetModelFileName());
        }

        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadModel(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());
    }

    //Expression
    {
        csmBool hasExpression = false;
        
        // 1. Load from model3.json
        if (_modelSetting->GetExpressionCount() > 0)
        {
            const csmInt32 count = _modelSetting->GetExpressionCount();
            for (csmInt32 i = 0; i < count; i++)
            {
                csmString name = _modelSetting->GetExpressionName(i);
                csmString path = _modelSetting->GetExpressionFileName(i);
                path = _modelHomeDir + path;

                buffer = CreateBuffer(path.GetRawString(), &size);
                ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());

                if (motion)
                {
                    if (_expressions[name] != NULL)
                    {
                        ACubismMotion::Delete(_expressions[name]);
                        _expressions[name] = NULL;
                    }
                    _expressions[name] = motion;
                    hasExpression = true;
                }
                DeleteBuffer(buffer, path.GetRawString());
            }
        }
        
        // 2. Auto-discover standalone .exp3.json files in the model folder and Expressions/ subfolder
        const csmChar* exprSearchDirs[] = { "", "Expressions/" };
        for (int d = 0; d < 2; d++) {
            csmString searchDir = _modelHomeDir + exprSearchDirs[d];
            DIR* pDir = opendir(searchDir.GetRawString());
            if (pDir != NULL) {
                struct dirent* ent;
                while ((ent = readdir(pDir)) != NULL) {
                    csmString fileName = ent->d_name;
                    if (strstr(fileName.GetRawString(), ".exp3.json")) {
                        csmString name = fileName;
                        if (_expressions[name] == NULL) { // Not already loaded
                            csmString fullPath = searchDir + fileName;
                            buffer = CreateBuffer(fullPath.GetRawString(), &size);
                            if (buffer) {
                                ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());
                                if (motion) {
                                    _expressions[name] = motion;
                                    hasExpression = true;
                                    LAppPal::PrintLogLn("[APP] Auto-loaded standalone expression: %s", name.GetRawString());
                                }
                                DeleteBuffer(buffer, fullPath.GetRawString());
                            }
                        }
                    }
                }
                closedir(pDir);
            }
        }

        if (hasExpression)
        {
            CubismExpressionUpdater* expression = CSM_NEW CubismExpressionUpdater(*_expressionManager);
            _updateScheduler.AddUpdatableList(expression);

            _expressionToggles = CSM_NEW ExpressionToggleUpdater();
            _updateScheduler.AddUpdatableList(_expressionToggles);
        }
    }

    //Physics
    PhysicsSourceMap physicsSources;
    if (strcmp(_modelSetting->GetPhysicsFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPhysicsFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadPhysics(buffer, size);
        if (buffer)
        {
            _physicsOutputs.clear();
            physicsSources = BuildPhysicsSourceMap(buffer, size, &_physicsOutputs);
            _physicsSources = physicsSources;
        }
        DeleteBuffer(buffer, path.GetRawString());

        if (_physics != nullptr)
        {
            CubismPhysicsUpdater* physics = CSM_NEW CubismPhysicsUpdater(*_physics);
            _updateScheduler.AddUpdatableList(physics);
        }
    }

    //Pose
    if (strcmp(_modelSetting->GetPoseFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPoseFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadPose(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());

        if (_pose != nullptr)
        {
            CubismPoseUpdater* pose = CSM_NEW CubismPoseUpdater(*_pose);
            _updateScheduler.AddUpdatableList(pose);
        }
    }

    //EyeBlink
    {
        if (_modelSetting->GetEyeBlinkParameterCount() > 0)
        {
            _eyeBlink = CubismEyeBlink::Create(_modelSetting);

            CubismEyeBlinkUpdater* eyeBlink = CSM_NEW CubismEyeBlinkUpdater(_motionUpdated, *_eyeBlink);
            _updateScheduler.AddUpdatableList(eyeBlink);
        }
    }

    //Breath
    {
        _breath = CubismBreath::Create();

        csmVector<CubismBreath::BreathParameterData> breathParameters;

        const CubismIdHandle breathIds[] = { _idParamAngleX, _idParamAngleY, _idParamAngleZ, _idParamBodyAngleX };
        const csmFloat32 breathPeaks[] = { 15.0f, 8.0f, 10.0f, 4.0f };
        const csmFloat32 breathCycles[] = { 6.5345f, 3.5345f, 5.5345f, 15.5345f };
        for (csmInt32 i = 0; i < 4; ++i)
        {
            csmFloat32 scale;
            CubismIdHandle id = ResolvePhysicsSource(physicsSources, _model, breathIds[i], scale);
            breathParameters.PushBack(CubismBreath::BreathParameterData(id, 0.0f, breathPeaks[i] * scale, breathCycles[i], 0.5f));
        }
        breathParameters.PushBack(CubismBreath::BreathParameterData(CubismFramework::GetIdManager()->GetId(ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));

        _breath->SetParameters(breathParameters);

        CubismBreathUpdater* breath = CSM_NEW CubismBreathUpdater(*_breath);
        _updateScheduler.AddUpdatableList(breath);
    }

    //UserData
    if (strcmp(_modelSetting->GetUserDataFile(), "") != 0)
    {
        csmString path = _modelSetting->GetUserDataFile();
        path = _modelHomeDir + path;
        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadUserData(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());
    }

    // EyeBlinkIds
    {
        csmInt32 eyeBlinkIdCount = _modelSetting->GetEyeBlinkParameterCount();
        for (csmInt32 i = 0; i < eyeBlinkIdCount; ++i)
        {
            _eyeBlinkIds.PushBack(_modelSetting->GetEyeBlinkParameterId(i));
        }
    }

    // LipSyncIds
    {
        csmInt32 lipSyncIdCount = _modelSetting->GetLipSyncParameterCount();
        for (csmInt32 i = 0; i < lipSyncIdCount; ++i)
        {
            _lipSyncIds.PushBack(_modelSetting->GetLipSyncParameterId(i));
        }

        CubismLipSyncUpdater* lipSync = CSM_NEW CubismLipSyncUpdater(_lipSyncIds, _wavFileHandler);
        _updateScheduler.AddUpdatableList(lipSync);
    }

    // Look
    {
        _look = CubismLook::Create();

        csmVector<CubismLook::LookParameterData> lookParameters;

        struct { CubismIdHandle id; csmFloat32 fx, fy, fxy; } looks[] = {
            { _idParamAngleX, 30.0f, 0.0f, 0.0f },
            { _idParamAngleY, 0.0f, 30.0f, 0.0f },
            { _idParamAngleZ, 0.0f, 0.0f, -30.0f },
            { _idParamBodyAngleX, 10.0f, 0.0f, 0.0f },
            { _idParamBodyAngleY, 0.0f, 10.0f, 0.0f },
            { _idParamBodyAngleZ, 0.0f, 0.0f, -10.0f },
            { _idParamEyeBallX, 1.0f, 0.0f, 0.0f },
            { _idParamEyeBallY, 0.0f, 1.0f, 0.0f },
        };
        for (size_t i = 0; i < sizeof(looks) / sizeof(looks[0]); ++i)
        {
            csmFloat32 scale;
            CubismIdHandle id = ResolvePhysicsSource(physicsSources, _model, looks[i].id, scale);
            if (id != looks[i].id)
            {
                LAppPal::PrintLogLn("[APP]look/breath: %s is driven by physics, using input %s (x%.2f)",
                                    looks[i].id->GetString().GetRawString(), id->GetString().GetRawString(), scale);
            }
            lookParameters.PushBack(CubismLook::LookParameterData(id, looks[i].fx * scale, looks[i].fy * scale, looks[i].fxy * scale));
        }

        _look->SetParameters(lookParameters);

        CubismLookUpdater* look = CSM_NEW CubismLookUpdater(*_look, *_dragManager);
        _updateScheduler.AddUpdatableList(look);
    }

    // Parameter overrides (IPC pose / parameter control)
    _parameterOverrides = CSM_NEW ParameterOverrideUpdater();
    _updateScheduler.AddUpdatableList(_parameterOverrides);

    _updateScheduler.SortUpdatableList();

    LoadDisplayInfo();

    if (_modelSetting == NULL || _modelMatrix == NULL)
    {
        LAppPal::PrintLogLn("Failed to SetupModel().");
        return;
    }

    //Layout
    csmMap<csmString, csmFloat32> layout;
    _modelSetting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);

    ComputeLookOrigin();

    _model->SaveParameters();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        PreloadMotionGroup(group);
    }

    // Auto-discover extra .motion3.json
    DIR* pDirM = opendir(_modelHomeDir.GetRawString());
    if (pDirM != NULL) {
        struct dirent* entM;
        while ((entM = readdir(pDirM)) != NULL) {
            csmString fileName = entM->d_name;
            if (strstr(fileName.GetRawString(), ".motion3.json") && !strstr(fileName.GetRawString(), "loop") && !strstr(fileName.GetRawString(), "idle")) {
                // very rough check: try not to double load if already in some group, but hard to know.
                // let's just load it.
                csmString fullPath = _modelHomeDir + fileName;
                csmSizeInt sizeM;
                csmByte* buffM = CreateBuffer(fullPath.GetRawString(), &sizeM);
                if (buffM) {
                    ACubismMotion* motion = LoadMotion(buffM, sizeM, fileName.GetRawString());
                    if (motion) {
                        _autoMotions.PushBack(motion);
                        LAppPal::PrintLogLn("[APP] Auto-loaded standalone motion: %s", fileName.GetRawString());
                    }
                    DeleteBuffer(buffM, fullPath.GetRawString());
                }
            }
        }
        closedir(pDirM);
    }

    _motionManager->StopAllMotions();

    // Collect all parameter IDs used across skin motion groups
    CollectSkinParams();

    _updating = false;
    _initialized = true;
}

void LAppModel::CollectSkinParams()
{
    _allSkinParamIds.Clear();
    _allSkinParamDefaults.Clear();

    csmInt32 groupCount = _modelSetting->GetMotionGroupCount();
    if (groupCount <= 1) return; // No skin switching needed

    // Collect all unique parameter IDs animated by any motion group
    for (csmInt32 g = 0; g < groupCount; g++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(g);
        csmInt32 motionCount = _modelSetting->GetMotionCount(group);
        for (csmInt32 m = 0; m < motionCount; m++)
        {
            csmString path = _modelSetting->GetMotionFileName(group, m);
            path = _modelHomeDir + path;

            csmSizeInt size;
            csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
            if (!buffer) continue;

            CubismMotionJson motionJson(buffer, size);
            csmInt32 curveCount = motionJson.GetMotionCurveCount();
            for (csmInt32 c = 0; c < curveCount; c++)
            {
                const csmChar* target = motionJson.GetMotionCurveTarget(c);
                if (strcmp(target, "Parameter") != 0) continue;

                CubismIdHandle paramId = motionJson.GetMotionCurveId(c);

                // Check if already collected
                bool found = false;
                for (csmInt32 k = 0; k < _allSkinParamIds.GetSize(); k++)
                {
                    if (_allSkinParamIds[k] == paramId)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    _allSkinParamIds.PushBack(paramId);
                    // Store the model's default value for this parameter
                    csmInt32 paramIndex = _model->GetParameterIndex(paramId);
                    csmFloat32 defaultVal = _model->GetParameterDefaultValue(paramIndex);
                    _allSkinParamDefaults.PushBack(defaultVal);
                    LAppPal::PrintLogLn("[APP] Collected skin param: %s (default=%.2f)",
                        paramId->GetString().GetRawString(), defaultVal);
                }
            }

            DeleteBuffer(buffer, path.GetRawString());
        }
    }

    LAppPal::PrintLogLn("[APP] Total skin params collected: %d", _allSkinParamIds.GetSize());
}

void LAppModel::PreloadMotionGroup(const csmChar* group)
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);

    for (csmInt32 i = 0; i < count; i++)
    {
        //ex) idle_0
        csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, i);
        csmString path = _modelSetting->GetMotionFileName(group, i);
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]load motion: %s => [%s_%d] ", path.GetRawString(), group, i);
        }

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        CubismMotion* tmpMotion = static_cast<CubismMotion*>(LoadMotion(buffer, size, name.GetRawString(), NULL, NULL, _modelSetting, group, i));

        if (tmpMotion)
        {
            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);

            if (_motions[name] != NULL)
            {
                ACubismMotion::Delete(_motions[name]);
            }
            _motions[name] = tmpMotion;
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
}

void LAppModel::ReleaseMotionGroup(const csmChar* group) const
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);
    for (csmInt32 i = 0; i < count; i++)
    {
        csmString voice = _modelSetting->GetMotionSoundFileName(group, i);
        if (strcmp(voice.GetRawString(), "") != 0)
        {
            csmString path = voice;
            path = _modelHomeDir + path;
        }
    }
}

/**
* @brief すべてのモーションデータの解放
*
* すべてのモーションデータを解放する。
*/
void LAppModel::ReleaseMotions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _motions.Begin(); iter != _motions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _motions.Clear();
    for (csmUint32 i=0; i<_autoMotions.GetSize(); ++i) { ACubismMotion::Delete(_autoMotions[i]); }
    _autoMotions.Clear();
}

/**
* @brief すべての表情データの解放
*
* すべての表情データを解放する。
*/
void LAppModel::ReleaseExpressions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _expressions.Begin(); iter != _expressions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _expressions.Clear();
}

void LAppModel::Update()
{
    const csmFloat32 deltaTimeSeconds = LAppPal::GetDeltaTime();
    _userTimeSeconds += deltaTimeSeconds;

    // モーションによるパラメータ更新の有無
    _motionUpdated = false;

    //-----------------------------------------------------------------
    _model->LoadParameters(); // 前回セーブされた状態をロード

    if (_motionManager->IsFinished())
    {
        // モーションの再生がない場合、待機モーションの中からランダムで再生する
        StartRandomMotion(MotionGroupIdle, PriorityIdle);
    }
    else
    {
        _motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds); // モーションを更新
    }
    _model->SaveParameters(); // 状態を保存
    //-----------------------------------------------------------------

    // 不透明度
    _opacity = _model->GetModelOpacity();

    _updateScheduler.OnLateUpdate(_model, deltaTimeSeconds);

    // External mouth Y override (IPC lipsync)
    if (LAppIPC::HasExternalMouthY())
    {
        csmFloat32 mouthValue = LAppIPC::GetExternalMouthY();
        for (csmUint32 i = 0; i < _lipSyncIds.GetSize(); ++i)
        {
            _model->SetParameterValue(_lipSyncIds[i], mouthValue);
        }
    }

    // External look-at override (IPC head tracking)
    if (LAppIPC::HasExternalLook())
    {
        _dragManager->Set(LAppIPC::GetExternalLookX(), LAppIPC::GetExternalLookY());
    }

    // Re-apply parameter overrides that physics / lip sync overwrote
    if (_parameterOverrides) _parameterOverrides->ApplyLate(_model);

    // Expression timeout: revert to default after emotion_timeout seconds
    // emotion_timeout < 0 means expressions never revert
    {
        float timeout = _currentExpressionTimeout;
        if (timeout >= 0.0f && _lastExpressionTime >= 0.0f &&
            (_userTimeSeconds - _lastExpressionTime) > timeout)
        {
            _expressionManager->StopAllMotions();
            _lastExpressionTime = -1.0f;
            _currentExpression.clear();
            if (_debugMode) LAppPal::PrintLogLn("[APP] Expression timed out, reverted to default");
        }
    }

    _model->Update();

}

CubismMotionQueueEntryHandle LAppModel::StartMotion(const csmChar* group, csmInt32 no, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler, ACubismMotion::BeganMotionCallback onBeganMotionHandler)
{
    if (no < 0 || no >= _modelSetting->GetMotionCount(group))
    {
        if (_debugMode) LAppPal::PrintLogLn("[APP]motion not found: [%s_%d]", group, no);
        return InvalidMotionQueueEntryHandleValue;
    }

    if (priority == PriorityForce)
    {
        _motionManager->SetReservePriority(priority);
    }
    else if (!_motionManager->ReserveMotion(priority))
    {
        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]can't start motion.");
        }
        return InvalidMotionQueueEntryHandleValue;
    }

    const csmString fileName = _modelSetting->GetMotionFileName(group, no);

    //ex) idle_0
    csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, no);
    CubismMotion* motion = static_cast<CubismMotion*>(_motions[name.GetRawString()]);
    csmBool autoDelete = false;

    if (motion == NULL)
    {
        csmString path = fileName;
        path = _modelHomeDir + path;

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        motion = static_cast<CubismMotion*>(LoadMotion(buffer, size, NULL, onFinishedMotionHandler, onBeganMotionHandler, _modelSetting, group, no));

        if (motion)
        {
            motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
            autoDelete = true; // 終了時にメモリから削除
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
    else
    {
        motion->SetBeganMotionHandler(onBeganMotionHandler);
        motion->SetFinishedMotionHandler(onFinishedMotionHandler);
    }

    //voice
    csmString voice = _modelSetting->GetMotionSoundFileName(group, no);
    if (strcmp(voice.GetRawString(), "") != 0)
    {
        csmString path = voice;
        path = _modelHomeDir + path;
        _wavFileHandler.Start(path);
    }

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]start motion: [%s_%d]", group, no);
    }
    CubismMotionQueueEntryHandle handle = _motionManager->StartMotionPriority(motion, autoDelete, priority);
    if (handle != InvalidMotionQueueEntryHandleValue)
    {
        _lastMotionGroup = group;
        _lastMotionIndex = no;
    }
    return handle;
}

CubismMotionQueueEntryHandle LAppModel::StartRandomMotion(const csmChar* group, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler, ACubismMotion::BeganMotionCallback onBeganMotionHandler)
{
    if (_modelSetting->GetMotionCount(group) == 0)
    {
        if (_autoMotions.GetSize() > 0)
        {
            csmInt32 no = rand() % _autoMotions.GetSize();
            ACubismMotion* motion = _autoMotions[no];
            motion->SetFinishedMotionHandler(onFinishedMotionHandler);
            motion->SetBeganMotionHandler(onBeganMotionHandler);
            return _motionManager->StartMotionPriority(motion, false, priority);
        }
        return InvalidMotionQueueEntryHandleValue;
    }

    csmInt32 no = rand() % _modelSetting->GetMotionCount(group);

    return StartMotion(group, no, priority, onFinishedMotionHandler, onBeganMotionHandler);
}

void LAppModel::DoDraw()
{
    if (_model == NULL)
    {
        return;
    }

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->DrawModel();
}

void LAppModel::Draw(CubismMatrix44& matrix)
{
    if (_model == NULL)
    {
        return;
    }

    matrix.MultiplyByMatrix(_modelMatrix);

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->SetMvpMatrix(&matrix);

    DoDraw();
}

csmBool LAppModel::HitTest(const csmChar* hitAreaName, csmFloat32 x, csmFloat32 y)
{
    // 透明時は当たり判定なし。
    if (_opacity < 1)
    {
        return false;
    }
    const csmInt32 count = _modelSetting->GetHitAreasCount();
    for (csmInt32 i = 0; i < count; i++)
    {
        if (strcmp(_modelSetting->GetHitAreaName(i), hitAreaName) == 0)
        {
            const CubismIdHandle drawID = _modelSetting->GetHitAreaId(i);
            return IsHit(drawID, x, y);
        }
    }
    
    if (count == 0) {
        // Fallback for models without configured HitAreas (like 薇薇安)
        // Check generic top/bottom zones
        if (strcmp(hitAreaName, HitAreaNameHead) == 0) {
            return y > 0.0f; // upper half of logical space
        } else if (strcmp(hitAreaName, HitAreaNameBody) == 0) {
            return y <= 0.0f; // lower half
        }
    }
    
    return false; // 存在しない場合はfalse
}

csmBool LAppModel::SetExpression(const csmChar* expressionID, csmFloat32 duration)
{
    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]expression: [%s]", expressionID);
    }

    // csmMap::operator[] inserts a NULL entry for unknown keys, so check first.
    ACubismMotion* motion = _expressions.IsExist(expressionID) ? _expressions[expressionID] : NULL;
    if (motion == NULL)
    {
        if (_debugMode) LAppPal::PrintLogLn("[APP]expression[%s] is null ", expressionID);
        return false;
    }

    _expressionManager->StartMotion(motion, false);
    _lastExpressionTime = _userTimeSeconds;
    // duration: -2 = use emotion_timeout from config, < 0 = never revert
    _currentExpressionTimeout = (duration <= -2.0f) ? LAppConfig::GetInstance().emotionTimeout : duration;
    _currentExpression = expressionID;
    return true;
}

csmBool LAppModel::ToggleExpression(const csmChar* expressionID, csmBool& nowActive)
{
    nowActive = false;
    ACubismMotion* motion = _expressions.IsExist(expressionID) ? _expressions[expressionID] : NULL;
    if (motion == NULL || _expressionToggles == NULL) return false;

    nowActive = _expressionToggles->Toggle(expressionID, static_cast<CubismExpressionMotion*>(motion));
    if (_debugMode) LAppPal::PrintLogLn("[APP]toggle expression: [%s] -> %s", expressionID, nowActive ? "on" : "off");
    return true;
}

csmBool LAppModel::SetToggledExpression(const csmChar* expressionID, csmBool active, csmFloat32 duration)
{
    ACubismMotion* motion = _expressions.IsExist(expressionID) ? _expressions[expressionID] : NULL;
    if (motion == NULL || _expressionToggles == NULL) return false;
    _expressionToggles->Set(expressionID, static_cast<CubismExpressionMotion*>(motion), active, duration);
    return true;
}

void LAppModel::ClearExpressions()
{
    if (_expressionToggles) _expressionToggles->Clear();
    if (_expressionManager) _expressionManager->StopAllMotions();
    _lastExpressionTime = -1.0f;
    _currentExpression.clear();
}

std::vector<std::string> LAppModel::GetActiveToggledExpressions() const
{
    if (_expressionToggles == NULL) return std::vector<std::string>();
    return _expressionToggles->GetActive();
}

void LAppModel::SetRandomExpression()
{
    if (_expressions.GetSize() == 0)
    {
        return;
    }

    csmInt32 no = _nextExpressionIndex % _expressions.GetSize();
    _nextExpressionIndex = (no + 1) % _expressions.GetSize();
    csmMap<csmString, ACubismMotion*>::const_iterator map_ite;
    csmInt32 i = 0;
    for (map_ite = _expressions.Begin(); map_ite != _expressions.End(); map_ite++)
    {
        if (i == no)
        {
            csmString name = (*map_ite).First;
            SetExpression(name.GetRawString());
            return;
        }
        i++;
    }
}

void LAppModel::ReloadRenderer()
{
    DeleteRenderer();

    CreateRendererForModel();

    SetupTextures();
}

namespace {
// Number of distinct mask sets, matching how CubismClippingManager groups
// objects into clipping contexts (same mask IDs regardless of order).
csmInt32 CountClippingContexts(csmInt32 objectCount, const csmInt32* maskCounts, const csmInt32** masks)
{
    std::vector<std::vector<csmInt32> > contexts;
    for (csmInt32 i = 0; i < objectCount; ++i)
    {
        if (maskCounts[i] <= 0) continue;
        std::vector<csmInt32> ids(masks[i], masks[i] + maskCounts[i]);
        std::sort(ids.begin(), ids.end());
        if (std::find(contexts.begin(), contexts.end(), ids) == contexts.end())
        {
            contexts.push_back(ids);
        }
    }
    return static_cast<csmInt32>(contexts.size());
}
}

void LAppModel::ComputeLookOrigin()
{
    _hasLookOrigin = false;
    const csmInt32 drawableCount = _model->GetDrawableCount();

    // 1. Drawables moved by the eyeball parameter are the pupils/irises.
    const csmInt32 paramIndex = _model->GetParameterIndex(_idParamEyeBallX);
    if (paramIndex >= 0 && paramIndex < _model->GetParameterCount())
    {
        const csmFloat32 original = _model->GetParameterValue(paramIndex);

        _model->Update();
        std::vector<std::vector<csmFloat32> > base(drawableCount);
        std::vector<bool> visible(drawableCount);
        for (csmInt32 i = 0; i < drawableCount; ++i)
        {
            const csmInt32 n = _model->GetDrawableVertexCount(i) * 2;
            const csmFloat32* v = _model->GetDrawableVertices(i);
            base[i].assign(v, v + n);
            visible[i] = _model->GetDrawableDynamicFlagIsVisible(i) && _model->GetDrawableOpacity(i) > 0.0f;
        }

        _model->SetParameterValue(paramIndex, _model->GetParameterMaximumValue(paramIndex));
        _model->Update();

        const csmFloat32 eps = _model->GetCanvasHeight() * 0.001f;
        csmFloat32 minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        csmInt32 moved = 0;
        for (csmInt32 i = 0; i < drawableCount; ++i)
        {
            if (!visible[i]) continue;
            const csmFloat32* v = _model->GetDrawableVertices(i);
            bool changed = false;
            for (size_t k = 0; k < base[i].size(); ++k)
            {
                if (std::fabs(v[k] - base[i][k]) > eps) { changed = true; break; }
            }
            if (!changed) continue;
            ++moved;
            for (size_t k = 0; k + 1 < base[i].size(); k += 2)
            {
                minX = std::min(minX, base[i][k]);     maxX = std::max(maxX, base[i][k]);
                minY = std::min(minY, base[i][k + 1]); maxY = std::max(maxY, base[i][k + 1]);
            }
        }

        _model->SetParameterValue(paramIndex, original);
        _model->Update();

        if (moved > 0)
        {
            _lookOriginX = (minX + maxX) * 0.5f;
            _lookOriginY = (minY + maxY) * 0.5f;
            _hasLookOrigin = true;
            if (_debugMode)
            {
                LAppPal::PrintLogLn("[APP]look origin from %d eye drawables: (%.3f, %.3f), bounds x[%.3f, %.3f] y[%.3f, %.3f]",
                                    moved, _lookOriginX, _lookOriginY, minX, maxX, minY, maxY);
            }
            return;
        }
    }

    // 2. Center of the "Head" hit area.
    for (csmInt32 i = 0; i < _modelSetting->GetHitAreasCount(); ++i)
    {
        if (strcmp(_modelSetting->GetHitAreaName(i), HitAreaNameHead) != 0) continue;
        const csmInt32 drawable = _model->GetDrawableIndex(_modelSetting->GetHitAreaId(i));
        if (drawable < 0 || drawable >= drawableCount) break;
        const csmInt32 n = _model->GetDrawableVertexCount(drawable);
        const csmFloat32* v = _model->GetDrawableVertices(drawable);
        if (n <= 0) break;
        csmFloat32 minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (csmInt32 k = 0; k < n; ++k)
        {
            minX = std::min(minX, v[k * 2]);     maxX = std::max(maxX, v[k * 2]);
            minY = std::min(minY, v[k * 2 + 1]); maxY = std::max(maxY, v[k * 2 + 1]);
        }
        _lookOriginX = (minX + maxX) * 0.5f;
        _lookOriginY = (minY + maxY) * 0.5f;
        _hasLookOrigin = true;
        if (_debugMode) LAppPal::PrintLogLn("[APP]look origin from Head hit area: (%.3f, %.3f)", _lookOriginX, _lookOriginY);
        return;
    }

    if (_debugMode) LAppPal::PrintLogLn("[APP]look origin: not found, using model center");
}

csmBool LAppModel::GetLookOrigin(csmFloat32& x, csmFloat32& y)
{
    if (!_hasLookOrigin || _modelMatrix == NULL) return false;
    x = _modelMatrix->TransformX(_lookOriginX);
    y = _modelMatrix->TransformY(_lookOriginY);
    return true;
}

void LAppModel::CreateRendererForModel()
{
    // Per-texture mask limits from CubismClippingManager.hpp:
    // 36 with a single render texture, 32 per texture when using several.
    const csmInt32 maxMasksSingle = 36;
    const csmInt32 maxMasksPerTexture = 32;
    const csmFloat32 maskBufferSize = 2048.0f;

    csmInt32 clipCount = CountClippingContexts(_model->GetDrawableCount(),
                                               _model->GetDrawableMaskCounts(),
                                               _model->GetDrawableMasks());
    csmInt32 offscreenClipCount = CountClippingContexts(_model->GetOffscreenCount(),
                                                        _model->GetOffscreenMaskCounts(),
                                                        _model->GetOffscreenMasks());
    if (offscreenClipCount > clipCount) clipCount = offscreenClipCount;

    csmInt32 maskBufferCount = 1;
    if (clipCount > maxMasksSingle)
    {
        maskBufferCount = (clipCount + maxMasksPerTexture - 1) / maxMasksPerTexture;
    }

    CreateRenderer(LAppDelegate::GetInstance()->GetWindowWidth(),
                   LAppDelegate::GetInstance()->GetWindowHeight(),
                   maskBufferCount);

    Rendering::CubismRenderer_OpenGLES2* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    renderer->SetDrawableClippingMaskBufferSize(maskBufferSize, maskBufferSize);
    renderer->SetOffscreenClippingMaskBufferSize(maskBufferSize, maskBufferSize);

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]clipping contexts: %d, mask render textures: %d", clipCount, maskBufferCount);
    }
}

void LAppModel::SwitchSkin()
{
    csmInt32 groupCount = _modelSetting->GetMotionGroupCount();
    if (groupCount == 0) {
        if (_expressions.GetSize() > 0) {
            LAppPal::PrintLogLn("[APP] No motion groups, cycling expression instead");
            SetRandomExpression();
        } else {
            LAppPal::PrintLogLn("[APP] No motion groups or expressions available for skin switching");
        }
        return;
    }

    // Reset all skin parameters to their defaults before switching
    for (csmInt32 i = 0; i < _allSkinParamIds.GetSize(); i++)
    {
        _model->SetParameterValue(_allSkinParamIds[i], _allSkinParamDefaults[i]);
    }
    _model->SaveParameters();

    _currentSkinIndex = (_currentSkinIndex + 1) % groupCount;
    const csmChar* group = _modelSetting->GetMotionGroupName(_currentSkinIndex);
    LAppPal::PrintLogLn("[APP] Switching skin to group: %s (index %d)", group, _currentSkinIndex);
    StartMotion(group, 0, PriorityForce);
}

void LAppModel::SetupTextures()
{
    for (csmInt32 modelTextureNumber = 0; modelTextureNumber < _modelSetting->GetTextureCount(); modelTextureNumber++)
    {
        if (strcmp(_modelSetting->GetTextureFileName(modelTextureNumber), "") == 0) continue;

        csmString texturePath = _modelSetting->GetTextureFileName(modelTextureNumber);
        texturePath = _modelHomeDir + texturePath;

        LAppTextureManager::TextureInfo* texture = LAppDelegate::GetInstance()->GetTextureManager()->CreateTextureFromPngFile(texturePath.GetRawString());
        const csmInt32 glTextueNumber = texture->id;

        GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->BindTexture(modelTextureNumber, glTextueNumber);
    }

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(false);
}

void LAppModel::MotionEventFired(const csmString& eventValue)
{
    CubismLogInfo("%s is fired on LAppModel!!", eventValue.GetRawString());
}

Csm::Rendering::CubismRenderTarget_OpenGLES2& LAppModel::GetRenderBuffer()
{
    return _renderBuffer;
}

std::vector<std::string> LAppModel::GetExpressionIds() const
{
    std::vector<std::string> result;
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _expressions.Begin();
         iter != _expressions.End(); ++iter)
    {
        result.push_back(std::string(iter->First.GetRawString()));
    }
    return result;
}

// ─── Agent control API ──────────────────────────────────────────────────────

namespace {
enum PoseMapping
{
    PoseSigned,   // -1..1 around the default value
    PoseUnsigned, // 0..1 from minimum to maximum
    PoseOpen,     // 0 = minimum, 1 = default, 2 = maximum (eye openness)
};

struct PoseAliasDef
{
    const char* name;
    const char* params[2];
    PoseMapping mapping;
    bool followPhysics; // drive the physics input if the parameter is a physics output
    const char* description;
};

const PoseAliasDef kPoseAliases[] = {
    { "head_x", { "ParamAngleX", NULL }, PoseSigned, true, "head turn left/right (+ = model's left, viewer's right)" },
    { "head_y", { "ParamAngleY", NULL }, PoseSigned, true, "head tilt up/down (+ = up)" },
    { "head_z", { "ParamAngleZ", NULL }, PoseSigned, true, "head roll (+ = top of head tilts toward viewer's right)" },
    { "body_x", { "ParamBodyAngleX", NULL }, PoseSigned, true, "body turn left/right" },
    { "body_y", { "ParamBodyAngleY", NULL }, PoseSigned, true, "body lean up/down" },
    { "body_z", { "ParamBodyAngleZ", NULL }, PoseSigned, true, "body roll" },
    { "eye_x", { "ParamEyeBallX", NULL }, PoseSigned, false, "eyeballs left/right (+ = viewer's right)" },
    { "eye_y", { "ParamEyeBallY", NULL }, PoseSigned, false, "eyeballs up/down (+ = up)" },
    { "eye_open", { "ParamEyeLOpen", "ParamEyeROpen" }, PoseOpen, false, "both eyes: 0 closed, 1 normal, 2 wide" },
    { "eye_open_l", { "ParamEyeLOpen", NULL }, PoseOpen, false, "left eye: 0 closed, 1 normal, 2 wide" },
    { "eye_open_r", { "ParamEyeROpen", NULL }, PoseOpen, false, "right eye: 0 closed, 1 normal, 2 wide" },
    { "eye_smile", { "ParamEyeLSmile", "ParamEyeRSmile" }, PoseUnsigned, false, "smiling eyes" },
    { "brow_y", { "ParamBrowLY", "ParamBrowRY" }, PoseSigned, false, "eyebrows up/down" },
    { "brow_angle", { "ParamBrowLAngle", "ParamBrowRAngle" }, PoseSigned, false, "eyebrow angle (- = angry, + = worried)" },
    { "brow_form", { "ParamBrowLForm", "ParamBrowRForm" }, PoseSigned, false, "eyebrow shape" },
    { "mouth_open", { "ParamMouthOpenY", NULL }, PoseUnsigned, false, "mouth opening" },
    { "mouth_form", { "ParamMouthForm", NULL }, PoseSigned, false, "mouth shape (- = frown, + = smile)" },
    { "cheek", { "ParamCheek", NULL }, PoseUnsigned, false, "blush" },
    { "arm_l", { "ParamArmLA", NULL }, PoseSigned, false, "left arm (standard ParamArmLA)" },
    { "arm_r", { "ParamArmRA", NULL }, PoseSigned, false, "right arm (standard ParamArmRA)" },
    { "arm_l_b", { "ParamArmLB", NULL }, PoseSigned, false, "left arm, second set (ParamArmLB)" },
    { "arm_r_b", { "ParamArmRB", NULL }, PoseSigned, false, "right arm, second set (ParamArmRB)" },
    { "hand_l", { "ParamHandL", NULL }, PoseSigned, false, "left hand" },
    { "hand_r", { "ParamHandR", NULL }, PoseSigned, false, "right hand" },
    { "shoulder_y", { "ParamShoulderY", NULL }, PoseSigned, false, "shoulders up/down" },
};

const PoseAliasDef* FindPoseAlias(const std::string& name)
{
    for (size_t i = 0; i < sizeof(kPoseAliases) / sizeof(kPoseAliases[0]); ++i)
    {
        if (name == kPoseAliases[i].name) return &kPoseAliases[i];
    }
    return NULL;
}

const char* PoseRange(PoseMapping m)
{
    switch (m)
    {
    case PoseSigned: return "-1..1";
    case PoseUnsigned: return "0..1";
    default: return "0..2";
    }
}

bool ModelHasParameter(CubismModel* model, CubismIdHandle id)
{
    const csmInt32 index = model->GetParameterIndex(id);
    return index >= 0 && index < model->GetParameterCount();
}
}

void LAppModel::LoadDisplayInfo()
{
    _parameterNames.clear();
    _parameterGroups.clear();
    const csmChar* file = _modelSetting->GetDisplayInfoFileName();
    if (file == NULL || strcmp(file, "") == 0) return;

    csmString path = _modelHomeDir + file;
    csmSizeInt size;
    csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
    if (buffer == NULL) return;

    Utils::CubismJson* json = Utils::CubismJson::Create(buffer, size);
    if (json)
    {
        std::map<std::string, std::string> groupNames;
        Utils::Value& groups = json->GetRoot()["ParameterGroups"];
        for (csmInt32 i = 0; !groups.IsNull() && i < groups.GetSize(); ++i)
        {
            if (groups[i]["Id"].IsNull() || groups[i]["Name"].IsNull()) continue;
            groupNames[groups[i]["Id"].GetRawString()] = groups[i]["Name"].GetRawString();
        }
        Utils::Value& params = json->GetRoot()["Parameters"];
        for (csmInt32 i = 0; !params.IsNull() && i < params.GetSize(); ++i)
        {
            if (params[i]["Id"].IsNull()) continue;
            const std::string id = params[i]["Id"].GetRawString();
            if (!params[i]["Name"].IsNull()) _parameterNames[id] = params[i]["Name"].GetRawString();
            if (!params[i]["GroupId"].IsNull())
            {
                std::map<std::string, std::string>::const_iterator g = groupNames.find(params[i]["GroupId"].GetRawString());
                if (g != groupNames.end()) _parameterGroups[id] = g->second;
            }
        }
        Utils::CubismJson::Delete(json);
    }
    DeleteBuffer(buffer, path.GetRawString());
}

std::vector<LAppModel::ParameterInfo> LAppModel::GetParameterInfos() const
{
    std::vector<ParameterInfo> result;
    if (_model == NULL) return result;
    const csmInt32 count = _model->GetParameterCount();
    for (csmInt32 i = 0; i < count; ++i)
    {
        CubismIdHandle id = _model->GetParameterId(i);
        ParameterInfo info;
        info.id = id->GetString().GetRawString();
        std::map<std::string, std::string>::const_iterator n = _parameterNames.find(info.id);
        info.name = (n != _parameterNames.end()) ? n->second : "";
        std::map<std::string, std::string>::const_iterator g = _parameterGroups.find(info.id);
        info.group = (g != _parameterGroups.end()) ? g->second : "";
        info.min = _model->GetParameterMinimumValue(i);
        info.max = _model->GetParameterMaximumValue(i);
        info.def = _model->GetParameterDefaultValue(i);
        info.value = _model->GetParameterValue(i);
        info.physicsOutput = _physicsOutputs.count(id) > 0;
        std::map<CubismIdHandle, CubismIdHandle>::const_iterator src = _physicsSources.find(id);
        info.physicsSource = (src != _physicsSources.end()) ? src->second->GetString().GetRawString() : "";
        result.push_back(info);
    }
    return result;
}

csmBool LAppModel::HasParameter(const std::string& id) const
{
    return _model != NULL && ModelHasParameter(_model, CubismFramework::GetIdManager()->GetId(id.c_str()));
}

csmBool LAppModel::SetParameterOverride(const std::string& id, float value, OverrideBlend blend,
                                        float duration, float fade, std::string& error)
{
    if (_parameterOverrides == NULL) { error = "model not ready"; return false; }
    CubismIdHandle handle = CubismFramework::GetIdManager()->GetId(id.c_str());
    if (!ModelHasParameter(_model, handle)) { error = "unknown parameter: " + id; return false; }
    _parameterOverrides->Set(handle, id, value, blend, duration, fade < 0.0f ? 0.0f : fade);
    return true;
}

int LAppModel::ReleaseParameterOverrides(const std::vector<std::string>& ids, float fade)
{
    if (_parameterOverrides == NULL) return 0;
    std::vector<CubismIdHandle> handles;
    for (size_t i = 0; i < ids.size(); ++i) handles.push_back(CubismFramework::GetIdManager()->GetId(ids[i].c_str()));
    if (!ids.empty() && handles.empty()) return 0;
    return _parameterOverrides->Release(handles, fade);
}

std::vector<LAppModel::OverrideInfo> LAppModel::GetParameterOverrides() const
{
    std::vector<OverrideInfo> result;
    if (_parameterOverrides == NULL) return result;
    const std::vector<ParameterOverrideUpdater::Entry>& entries = _parameterOverrides->Entries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        OverrideInfo info;
        info.id = entries[i].name;
        info.value = entries[i].to;
        info.blend = entries[i].blend == OverrideAdd ? "add" : (entries[i].blend == OverrideMultiply ? "multiply" : "overwrite");
        info.remaining = entries[i].remaining;
        info.releasing = entries[i].releasing;
        result.push_back(info);
    }
    return result;
}

csmBool LAppModel::IsPoseAlias(const std::string& name)
{
    return FindPoseAlias(name) != NULL;
}

csmBool LAppModel::ResolvePose(const std::string& alias, float v, std::vector<PoseTarget>& out) const
{
    out.clear();
    const PoseAliasDef* def = FindPoseAlias(alias);
    if (def == NULL || _model == NULL) return false;

    for (int p = 0; p < 2 && def->params[p] != NULL; ++p)
    {
        CubismIdHandle id = CubismFramework::GetIdManager()->GetId(def->params[p]);
        if (!ModelHasParameter(_model, id)) continue;
        if (def->followPhysics)
        {
            std::map<CubismIdHandle, CubismIdHandle>::const_iterator src = _physicsSources.find(id);
            if (src != _physicsSources.end() && ModelHasParameter(_model, src->second)) id = src->second;
        }
        const csmInt32 index = _model->GetParameterIndex(id);
        const float mn = _model->GetParameterMinimumValue(index);
        const float mx = _model->GetParameterMaximumValue(index);
        const float df = _model->GetParameterDefaultValue(index);
        float value;
        switch (def->mapping)
        {
        case PoseSigned:
        {
            const float c = std::max(-1.0f, std::min(1.0f, v));
            value = (c >= 0.0f) ? df + c * (mx - df) : df + c * (df - mn);
            break;
        }
        case PoseUnsigned:
        {
            const float c = std::max(0.0f, std::min(1.0f, v));
            value = mn + c * (mx - mn);
            break;
        }
        default: // PoseOpen
        {
            const float c = std::max(0.0f, std::min(2.0f, v));
            value = (c <= 1.0f) ? mn + c * (df - mn) : df + (c - 1.0f) * (mx - df);
            break;
        }
        }
        PoseTarget t;
        t.param = id->GetString().GetRawString();
        t.value = value;
        out.push_back(t);
    }
    return true;
}

std::vector<LAppModel::PoseAliasInfo> LAppModel::GetPoseAliases() const
{
    std::vector<PoseAliasInfo> result;
    for (size_t i = 0; i < sizeof(kPoseAliases) / sizeof(kPoseAliases[0]); ++i)
    {
        PoseAliasInfo info;
        info.name = kPoseAliases[i].name;
        info.range = PoseRange(kPoseAliases[i].mapping);
        info.description = kPoseAliases[i].description;
        std::vector<PoseTarget> targets;
        ResolvePose(info.name, 0.0f, targets);
        for (size_t t = 0; t < targets.size(); ++t) info.params.push_back(targets[t].param);
        result.push_back(info);
    }
    return result;
}

LAppModel::MotionState LAppModel::GetMotionState() const
{
    MotionState state;
    state.group = _lastMotionGroup;
    state.index = _lastMotionIndex;
    state.playing = _motionManager != NULL && !_motionManager->IsFinished();
    return state;
}

csmBool LAppModel::HasMotion(const std::string& group, int index) const
{
    return _modelSetting != NULL && index >= 0 && index < _modelSetting->GetMotionCount(group.c_str());
}

void LAppModel::StopMotions()
{
    if (_motionManager == NULL) return;
    _motionManager->StopAllMotions();
    // StopAllMotions leaves the stopped motion's priority as "current", which
    // would block the idle motion (and anything below it) forever. An update
    // with an empty queue resets it without touching parameters.
    _motionManager->UpdateMotion(_model, 0.0f);
    _motionManager->SetReservePriority(0);
}

std::vector<LAppModel::ExpressionParamInfo> LAppModel::GetExpressionParams(const std::string& expressionID) const
{
    std::vector<ExpressionParamInfo> result;
    if (!_expressions.IsExist(expressionID.c_str())) return result;
    // csmMap's const operator[] does not compile; IsExist guarantees no insertion.
    ACubismMotion* motion = const_cast<csmMap<csmString, ACubismMotion*>&>(_expressions)[expressionID.c_str()];
    if (motion == NULL) return result;
    csmVector<CubismExpressionMotion::ExpressionParameter> params = static_cast<CubismExpressionMotion*>(motion)->GetExpressionParameters();
    for (csmUint32 i = 0; i < params.GetSize(); ++i)
    {
        ExpressionParamInfo info;
        info.id = params[i].ParameterId->GetString().GetRawString();
        std::map<std::string, std::string>::const_iterator n = _parameterNames.find(info.id);
        info.name = (n != _parameterNames.end()) ? n->second : "";
        info.value = params[i].Value;
        info.blend = params[i].BlendType == CubismExpressionMotion::Additive ? "add"
                   : (params[i].BlendType == CubismExpressionMotion::Multiply ? "multiply" : "overwrite");
        result.push_back(info);
    }
    return result;
}

std::vector<std::string> LAppModel::GetHitAreaNames() const
{
    std::vector<std::string> result;
    if (_modelSetting == NULL) return result;
    for (csmInt32 i = 0; i < _modelSetting->GetHitAreasCount(); ++i) result.push_back(_modelSetting->GetHitAreaName(i));
    return result;
}

void LAppModel::ResetState()
{
    StopMotions();
    if (_expressionToggles) _expressionToggles->Clear();
    if (_expressionManager) _expressionManager->StopAllMotions();
    _lastExpressionTime = -1.0f;
    _currentExpression.clear();
    if (_parameterOverrides) _parameterOverrides->Clear();

    // Motions leave their last values in the saved state (e.g. held poses);
    // put every parameter back to its default.
    for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i)
    {
        _model->SetParameterValue(i, _model->GetParameterDefaultValue(i));
    }
    _model->SaveParameters();
}

std::vector<LAppModel::MotionInfo> LAppModel::GetMotionList() const
{
    std::vector<MotionInfo> result;
    if (!_modelSetting) return result;

    for (csmInt32 g = 0; g < _modelSetting->GetMotionGroupCount(); g++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(g);
        csmInt32 count = _modelSetting->GetMotionCount(group);
        for (csmInt32 i = 0; i < count; i++)
        {
            MotionInfo info;
            info.group = group;
            info.index = i;
            info.file = _modelSetting->GetMotionFileName(group, i);
            info.duration = -1.0f;
            info.fadeIn = info.fadeOut = -1.0f;
            csmString key = Utils::CubismString::GetFormatedString("%s_%d", group, i);
            // csmMap's const operator[] does not compile; IsExist guarantees no insertion.
            csmMap<csmString, ACubismMotion*>& motions = const_cast<csmMap<csmString, ACubismMotion*>&>(_motions);
            if (motions.IsExist(key) && motions[key] != NULL)
            {
                ACubismMotion* motion = motions[key];
                info.duration = motion->GetLoopDuration();
                info.fadeIn = motion->GetFadeInTime();
                info.fadeOut = motion->GetFadeOutTime();
            }
            result.push_back(info);
        }
    }
    return result;
}
