#include "AnimController.h"

#include <cmath>

namespace nsk::anim
{

float ctrlTime(float time, float start, float stop, float frequency, float phase,
               NifTimeController::Cycle cycle)
{
    // Port of NifSkope Controller::ctrlTime (glcontroller.cpp).
    time = frequency * time + phase;

    if (time >= start && time <= stop)
        return time;

    switch (cycle)
    {
    case NifTimeController::Cycle::Loop:
    {
        const float delta = stop - start;
        if (delta <= 0.0f)
            return start;
        const float x = (time - start) / delta;
        const float y = (x - std::floor(x)) * delta;
        return start + y;
    }
    case NifTimeController::Cycle::Reverse:
    {
        const float delta = stop - start;
        if (delta <= 0.0f)
            return start;
        const float x = (time - start) / delta;
        const float y = (x - std::floor(x)) * delta;
        if ((static_cast<int>(std::fabs(std::floor(x))) & 1) == 0)
            return start + y;
        return stop - y;
    }
    case NifTimeController::Cycle::Clamp:
    default:
        if (time < start)
            return start;
        if (time > stop)
            return stop;
        return time;
    }
}

namespace
{
    // Port of Controller::timeIndex: locate the key pair bracketing `time`,
    // starting the search from the cached index `i` (bidirectional), and
    // return the [0,1] fraction between them. Clamps at the ends. The
    // backward-search branch inverts x and swaps i/j ("Quadratic Bug Fix" in
    // NifSkope) so tangent-based interpolation stays oriented forward.
    template <typename T>
    bool timeIndex(float time, const std::vector<NifKey<T>>& keys, int& i, int& j, float& x)
    {
        const int count = static_cast<int>(keys.size());
        if (count == 0)
            return false;

        if (time <= keys.front().time)
        {
            i = j = 0;
            x = 0.0f;
            return true;
        }
        if (time >= keys.back().time)
        {
            i = j = count - 1;
            x = 0.0f;
            return true;
        }

        if (i < 0 || i >= count)
            i = 0;

        float tI = keys[static_cast<std::size_t>(i)].time;
        if (time > tI)
        {
            j = i + 1;
            float tJ;
            while (time >= (tJ = keys[static_cast<std::size_t>(j)].time))
            {
                i = j++;
                tI = tJ;
            }
            x = (time - tI) / (tJ - tI);
            return true;
        }
        if (time < tI)
        {
            j = i - 1;
            float tJ;
            while (time <= (tJ = keys[static_cast<std::size_t>(j)].time))
            {
                i = j--;
                tI = tJ;
            }
            x = (time - tI) / (tJ - tI);
            x = 1.0f - x;
            std::swap(i, j);
            return true;
        }

        j = i;
        x = 0.0f;
        return true;
    }

    // Port of glcontroller.cpp's interpolate<T>: linear by default (including
    // TBC, which NifSkope also lerps), cubic Hermite for Quadratic (t1 = the
    // BACKWARD tangent of the earlier key, t2 = the FORWARD tangent of the
    // later key - note the deliberate crossover), step for Const.
    template <typename T>
    bool interpolateGroup(const NifKeyGroup<T>& group, float time, T& value, int& last)
    {
        int next = 0;
        float x = 0.0f;
        if (!timeIndex(time, group.keys, last, next, x))
            return false;

        const NifKey<T>& k1 = group.keys[static_cast<std::size_t>(last)];
        const NifKey<T>& k2 = group.keys[static_cast<std::size_t>(next)];

        switch (group.keyType)
        {
        case NifKeyType::Quadratic:
        {
            const T& t1 = k1.backward;
            const T& t2 = k2.forward;
            const float x2 = x * x;
            const float x3 = x2 * x;
            // x(t) = (2t^3 - 3t^2 + 1)P1 + (-2t^3 + 3t^2)P2 + (t^3 - 2t^2 + t)T1 + (t^3 - t^2)T2
            value = k1.value * (2.0f * x3 - 3.0f * x2 + 1.0f) + k2.value * (-2.0f * x3 + 3.0f * x2)
                  + t1 * (x3 - 2.0f * x2 + x) + t2 * (x3 - x2);
            return true;
        }
        case NifKeyType::Const:
            value = (x < 0.5f) ? k1.value : k2.value;
            return true;
        default: // Linear, Tbc (lerped, matching NifSkope), anything unknown
            value = k1.value + (k2.value - k1.value) * x;
            return true;
        }
    }

    Quat slerp(const Quat& a, const Quat& b, float x)
    {
        // Standard slerp with a nlerp fallback for near-parallel inputs
        // (mirrors NifSkope's Quat::slerp).
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        dot = std::clamp(dot, -1.0f, 1.0f);
        const float angle = std::acos(dot);
        Quat out;
        if (std::fabs(angle) >= 5.0e-5f)
        {
            const float inv = 1.0f / std::sin(angle);
            const float wa = std::sin((1.0f - x) * angle) * inv;
            const float wb = std::sin(x * angle) * inv;
            for (int i = 0; i < 4; ++i)
                out[i] = a[i] * wa + b[i] * wb;
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                out[i] = a[i] + (b[i] - a[i]) * x;
        }
        out.normalize();
        return out;
    }

    // Single-axis rotation matrices matching NifSkope's Matrix::euler(x,0,0)
    // etc.; the XYZ channel composition below multiplies them Z * Y * X
    // exactly like glcontroller.cpp's interpolate<Matrix>.
    Matrix rotX(float a)
    {
        Matrix m;
        const float c = std::cos(a), s = std::sin(a);
        m(1, 1) = c; m(1, 2) = -s;
        m(2, 1) = s; m(2, 2) = c;
        return m;
    }
    Matrix rotY(float a)
    {
        Matrix m;
        const float c = std::cos(a), s = std::sin(a);
        m(0, 0) = c; m(0, 2) = s;
        m(2, 0) = -s; m(2, 2) = c;
        return m;
    }
    Matrix rotZ(float a)
    {
        Matrix m;
        const float c = std::cos(a), s = std::sin(a);
        m(0, 0) = c; m(0, 1) = -s;
        m(1, 0) = s; m(1, 1) = c;
        return m;
    }
}

bool sampleKeys(const NifKeyGroup<float>& group, float time, float& out, int& lastIndex)
{
    return interpolateGroup(group, time, out, lastIndex);
}

bool sampleKeys(const NifKeyGroup<Vector3>& group, float time, Vector3& out, int& lastIndex)
{
    return interpolateGroup(group, time, out, lastIndex);
}

bool sampleRotation(const NifTransformData& data, float time, Matrix& out, int lastIndex[3])
{
    if (data.rotationType == NifKeyType::XyzRotation)
    {
        bool any = false;
        float r[3] = { 0.0f, 0.0f, 0.0f };
        for (int axis = 0; axis < 3; ++axis)
            any |= sampleKeys(data.xyzRotations[axis], time, r[axis], lastIndex[axis]);
        if (!any)
            return false;
        out = rotZ(r[2]) * rotY(r[1]) * rotX(r[0]);
        return true;
    }

    if (data.quatKeys.empty())
        return false;

    int next = 0;
    float x = 0.0f;
    if (!timeIndex(time, data.quatKeys, lastIndex[0], next, x))
        return false;

    Quat v1 = data.quatKeys[static_cast<std::size_t>(lastIndex[0])].value;
    const Quat& v2 = data.quatKeys[static_cast<std::size_t>(next)].value;
    const float dot = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2] + v1[3] * v2[3];
    if (dot < 0.0f)
        for (int i = 0; i < 4; ++i)
            v1[i] = -v1[i]; // don't take the long path

    out.fromQuat(slerp(v1, v2, x));
    return true;
}

// --- AnimPlayer --------------------------------------------------------------

namespace
{
    int findNodeByName(const std::vector<NifSceneNode>& nodes, const std::string& name)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].name == name)
                return static_cast<int>(i);
        return -1;
    }
}

void AnimPlayer::bind(const NifDocument& doc)
{
    m_doc = &doc;
    m_standalone.clear();
    m_sequences.clear();
    m_activeSequence = -1;

    const auto& nodes = doc.nodes();
    const auto& controllers = doc.timeControllers();
    const auto& interpolators = doc.transformInterpolators();
    const auto& datas = doc.transformData();

    // Standalone controllers: walk each node's NiTimeController chain and take
    // the active, non-manager-driven NiTransformControllers (the "always
    // playing" kind, e.g. simple spinning props without a manager).
    for (std::size_t n = 0; n < nodes.size(); ++n)
    {
        std::int32_t ref = nodes[n].controllerRef;
        int guard = 0;
        while (ref != kNoRef && guard++ < 64)
        {
            const auto it = controllers.find(ref);
            if (it == controllers.end())
                break;
            const NifTimeController& ctrl = it->second;
            if (ctrl.typeName == "NiTransformController" && ctrl.active() && !ctrl.managerControlled())
            {
                const auto interpIt = interpolators.find(ctrl.interpolatorRef);
                if (interpIt != interpolators.end() &&
                    NifTransformInterpolator::validComponent(ctrl.startTime) && ctrl.stopTime > ctrl.startTime)
                {
                    Channel ch;
                    ch.nodeIndex = static_cast<int>(n);
                    ch.interp = &interpIt->second;
                    if (interpIt->second.dataRef != kNoRef)
                    {
                        const auto dataIt = datas.find(interpIt->second.dataRef);
                        if (dataIt != datas.end() && !dataIt->second.empty())
                            ch.data = &dataIt->second;
                    }
                    ch.start = ctrl.startTime;
                    ch.stop = ctrl.stopTime;
                    ch.frequency = ctrl.frequency;
                    ch.phase = ctrl.phase;
                    ch.cycle = ctrl.cycleType();
                    m_standalone.push_back(std::move(ch));
                }
            }
            ref = ctrl.nextControllerRef;
        }
    }

    // Manager sequences: each ControlledBlock binds an interpolator to a node
    // by name (ControllerManager::setSequence's logic). The sequence's own
    // time range / cycle / frequency scopes every channel in it.
    std::vector<std::pair<std::int32_t, const NifControllerSequence*>> ordered;
    for (const auto& [blockIdx, seq] : doc.controllerSequences())
        ordered.emplace_back(blockIdx, &seq);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [blockIdx, seqPtr] : ordered)
    {
        const NifControllerSequence& src = *seqPtr;
        Sequence seq;
        seq.name = src.name;
        seq.start = src.startTime;
        seq.stop = src.stopTime;
        const auto cycle = static_cast<NifTimeController::Cycle>(src.cycleType);
        for (const NifControlledBlock& cb : src.controlledBlocks)
        {
            if (cb.controllerType != "NiTransformController" || cb.interpolatorRef == kNoRef)
                continue; // property/particle controllers: out of scope
            const int nodeIndex = findNodeByName(nodes, cb.nodeName);
            const auto interpIt = interpolators.find(cb.interpolatorRef);
            if (nodeIndex < 0 || interpIt == interpolators.end())
                continue;
            Channel ch;
            ch.nodeIndex = nodeIndex;
            ch.interp = &interpIt->second;
            if (interpIt->second.dataRef != kNoRef)
            {
                const auto dataIt = datas.find(interpIt->second.dataRef);
                if (dataIt != datas.end() && !dataIt->second.empty())
                    ch.data = &dataIt->second;
            }
            ch.start = src.startTime;
            ch.stop = src.stopTime;
            ch.frequency = src.frequency;
            ch.phase = 0.0f;
            ch.cycle = cycle;
            seq.channels.push_back(std::move(ch));
        }
        if (!seq.channels.empty())
            m_sequences.push_back(std::move(seq));
    }

    if (!m_sequences.empty())
        m_activeSequence = 0; // sensible default: the first named sequence

    m_animLocals.assign(nodes.size(), Transform());
    m_worldCache.assign(nodes.size(), Transform());
    m_worldState.assign(nodes.size(), 0);
}

void AnimPlayer::selectSequence(int index)
{
    if (index >= static_cast<int>(m_sequences.size()))
        index = m_sequences.empty() ? -1 : static_cast<int>(m_sequences.size()) - 1;
    m_activeSequence = (index < 0) ? -1 : index;
}

float AnimPlayer::timeMin() const
{
    if (m_activeSequence >= 0)
        return m_sequences[static_cast<std::size_t>(m_activeSequence)].start;
    float t = 0.0f;
    bool any = false;
    for (const Channel& ch : m_standalone)
    {
        t = any ? (std::min)(t, ch.start) : ch.start;
        any = true;
    }
    return t;
}

float AnimPlayer::timeMax() const
{
    if (m_activeSequence >= 0)
        return m_sequences[static_cast<std::size_t>(m_activeSequence)].stop;
    float t = 0.0f;
    for (const Channel& ch : m_standalone)
        t = (std::max)(t, ch.stop);
    return t;
}

void AnimPlayer::evalChannel(Channel& ch, float time)
{
    const float ct = ctrlTime(time, ch.start, ch.stop, ch.frequency, ch.phase, ch.cycle);
    Transform& local = m_animLocals[static_cast<std::size_t>(ch.nodeIndex)];

    // Interpolator default pose first (only the valid components - see the
    // -FLT_MAX sentinel note on NifTransformInterpolator), then keyed channels
    // on top.
    if (ch.interp->translationValid())
        local.translation = ch.interp->translation;
    if (ch.interp->rotationValid())
        local.rotation.fromQuat(ch.interp->rotation);
    if (ch.interp->scaleValid())
        local.scale = ch.interp->scale;

    if (ch.data != nullptr)
    {
        Matrix rot;
        if (sampleRotation(*ch.data, ct, rot, ch.lastRot))
            local.rotation = rot;
        Vector3 trans;
        if (sampleKeys(ch.data->translations, ct, trans, ch.lastTrans))
            local.translation = trans;
        float scale = local.scale;
        if (sampleKeys(ch.data->scales, ct, scale, ch.lastScale))
            local.scale = scale;
    }
}

void AnimPlayer::update(float time, std::vector<RenderMesh>& meshes)
{
    if (m_doc == nullptr)
        return;
    const auto& nodes = m_doc->nodes();
    if (nodes.empty())
        return;

    // Start from the bind pose each frame; animated channels overwrite their
    // nodes. (Node counts are tiny - tens - so a full copy beats bookkeeping.)
    for (std::size_t i = 0; i < nodes.size(); ++i)
        m_animLocals[i] = nodes[i].localTransform;

    for (Channel& ch : m_standalone)
        evalChannel(ch, time);
    if (m_activeSequence >= 0)
        for (Channel& ch : m_sequences[static_cast<std::size_t>(m_activeSequence)].channels)
            evalChannel(ch, time);

    // Recompose world transforms from the animated locals (iterative memoized
    // parent-chain walk, same guards as SceneBuilder's computeWorldTransform).
    std::fill(m_worldState.begin(), m_worldState.end(), std::int8_t(0));
    const auto worldOf = [&](int index, const auto& self) -> Transform
    {
        if (m_worldState[static_cast<std::size_t>(index)] == 2)
            return m_worldCache[static_cast<std::size_t>(index)];
        const NifSceneNode& node = nodes[static_cast<std::size_t>(index)];
        Transform world = m_animLocals[static_cast<std::size_t>(index)];
        if (node.parentIndex != kNoRef && m_worldState[static_cast<std::size_t>(node.parentIndex)] != 1)
        {
            m_worldState[static_cast<std::size_t>(index)] = 1;
            world = self(node.parentIndex, self) * m_animLocals[static_cast<std::size_t>(index)];
        }
        m_worldCache[static_cast<std::size_t>(index)] = world;
        m_worldState[static_cast<std::size_t>(index)] = 2;
        return world;
    };
    for (std::size_t i = 0; i < nodes.size(); ++i)
        (void)worldOf(static_cast<int>(i), worldOf);

    const Matrix4 axis = SceneBuilder::axisCorrection();
    for (RenderMesh& mesh : meshes)
    {
        if (mesh.sourceNodeIndex < 0 ||
            static_cast<std::size_t>(mesh.sourceNodeIndex) >= nodes.size() ||
            mesh.ownedGeometry) // skinned: pose baked into vertices (Phase 4)
            continue;
        mesh.worldTransform = axis * m_worldCache[static_cast<std::size_t>(mesh.sourceNodeIndex)].toMatrix4();
    }
}

} // namespace nsk::anim
