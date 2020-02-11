#include "EQPool.h"
#include "AtomicGuard.h"
#include <thread>
#include "absl/algorithm/container.h"
#include "SIMDHelpers.h"
using namespace std::chrono_literals;

sfz::EQHolder::EQHolder(const MidiState& state)
:midiState(state)
{

}

void sfz::EQHolder::reset()
{
    eq.clear();
}

void sfz::EQHolder::setup(const EQDescription& description, unsigned numChannels, uint8_t velocity)
{
    reset();
    eq.setChannels(numChannels);
    this->description = &description;
    const auto normalizedVelocity = normalizeVelocity(velocity);

    baseBandwidth = description.bandwidth;
    baseGain = Default::eqGainRange.clamp(description.gain + normalizedVelocity * description.vel2gain);
    baseFrequency = Default::eqFrequencyRange.clamp(description.frequency + normalizedVelocity * description.vel2frequency);
}

void sfz::EQHolder::process(const float** inputs, float** outputs, unsigned numFrames)
{
    if (description == nullptr) {
        for (unsigned channelIdx = 0; channelIdx < eq.channels(); channelIdx++)
            copy<float>({ inputs[channelIdx], numFrames }, { outputs[channelIdx], numFrames });
        return;
    }

    // TODO: Once the midistate envelopes are done, add modulation in there!
    // For now we take the last value
    lastFrequency = baseFrequency;
    for (auto& mod: description->frequencyCC) {
        lastFrequency += midiState.getCCValue(mod.first) * mod.second;
    }
    lastFrequency = Default::eqFrequencyRange.clamp(lastFrequency);

    lastBandwidth = baseBandwidth;
    for (auto& mod: description->bandwidthCC) {
        lastBandwidth += midiState.getCCValue(mod.first) * mod.second;
    }
    lastBandwidth = Default::eqBandwidthRange.clamp(lastBandwidth);

    lastGain = baseGain;
    for (auto& mod: description->gainCC) {
        lastGain += midiState.getCCValue(mod.first) * mod.second;
    }
    lastGain = Default::eqGainRange.clamp(lastGain);

    eq.process(inputs, outputs, lastFrequency, lastBandwidth, lastGain, numFrames);
}
float sfz::EQHolder::getLastFrequency() const
{
    return lastFrequency;
}
float sfz::EQHolder::getLastBandwidth() const
{
    return lastBandwidth;
}
float sfz::EQHolder::getLastGain() const
{
    return lastGain;
}
void sfz::EQHolder::setSampleRate(float sampleRate)
{
    eq.init(static_cast<double>(sampleRate));
}

sfz::EQPool::EQPool(const MidiState& state, int numEQs)
: midiState(state)
{
    setnumEQs(numEQs);
}

sfz::EQHolderPtr sfz::EQPool::getEQ(const EQDescription& description, unsigned numChannels, uint8_t velocity)
{
    AtomicGuard guard { givingOutEQs };
    if (!canGiveOutEQs)
        return {};

    auto eq = absl::c_find_if(eqs, [](const EQHolderPtr& holder) {
        return holder.use_count() == 1;
    });

    if (eq == eqs.end())
        return {};

    (**eq).setup(description, numChannels, velocity);
    return *eq;
}

size_t sfz::EQPool::getActiveEQs() const
{
    return absl::c_count_if(eqs, [](const EQHolderPtr& holder) {
        return holder.use_count() > 1;
    });
}

size_t sfz::EQPool::setnumEQs(size_t numEQs)
{
    AtomicDisabler disabler { canGiveOutEQs };

    while(givingOutEQs)
        std::this_thread::sleep_for(1ms);

    auto eqIterator = eqs.begin();
    auto eqSentinel = eqs.rbegin();
    while (eqIterator < eqSentinel.base()) {
        if (eqIterator->use_count() == 1) {
            std::iter_swap(eqIterator, eqSentinel);
            ++eqSentinel;
        } else {
            ++eqIterator;
        }
    }

    eqs.resize(std::distance(eqs.begin(), eqSentinel.base()));
    for (size_t i = eqs.size(); i < numEQs; ++i) {
        eqs.emplace_back(std::make_shared<EQHolder>(midiState));
        eqs.back()->setSampleRate(sampleRate);
    }

    return eqs.size();
}
void sfz::EQPool::setSampleRate(float sampleRate)
{
    for (auto& eq: eqs)
        eq->setSampleRate(sampleRate);
}