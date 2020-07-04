#include "MidiSyncTrackNode.h"
#include "MainController.h"
#include "MetronomeUtils.h"
#include "audio/core/AudioDriver.h"
#include <algorithm>

using audio::MidiSyncTrackNode;
using audio::SamplesBuffer;

MidiSyncTrackNode::MidiSyncTrackNode(MainController *controller) :
    samplesPerPulse(0),
    intervalPosition(0),
    currentPulse(0),
    lastPlayedPulse(-1),
    running(false),
    mainController(controller)
{
    resetInterval();
}

MidiSyncTrackNode::~MidiSyncTrackNode()
{
}

void MidiSyncTrackNode::setSamplesPerPulse(double samplesPerPulse)
{
    if (samplesPerPulse <= 0)
        qCritical() << "samples per pulse <= 0";

    this->samplesPerPulse = samplesPerPulse;
    resetInterval();
}

void MidiSyncTrackNode::resetInterval()
{
    intervalPosition = 0;
    lastPlayedPulse = -1;
}

void MidiSyncTrackNode::setIntervalPosition(long intervalPosition)
{
    if (samplesPerPulse <= 0)
        return;

    this->intervalPosition = intervalPosition;
    this->currentPulse = ((double)intervalPosition / samplesPerPulse);
}

void MidiSyncTrackNode::start()
{
    running = true;
}

void MidiSyncTrackNode::stop()
{
    running = false;
    mainController->stopMidiClock();
}

void MidiSyncTrackNode::processReplacing(const SamplesBuffer &in, SamplesBuffer &out,
                                          int SampleRate, std::vector<midi::MidiMessage> &midiBuffer)
{
    if (samplesPerPulse <= 0)
        return;

    if (currentPulse == 0 && currentPulse != lastPlayedPulse) {
        if (running) { mainController->startMidiClock(); }
        lastPlayedPulse = -1;
    }
    while (currentPulse - lastPlayedPulse >= 1) {
        mainController->sendMidiClockPulse();
        lastPlayedPulse++;
    }
    AudioNode::processReplacing(in, out, SampleRate, midiBuffer);
}
