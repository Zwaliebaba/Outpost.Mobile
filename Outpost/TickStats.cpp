#include "pch.h"
#include "TickStats.h"

#include <string>

namespace Outpost
{
bool WriteTickStatsFile(const TickStats& _stats)
{
  // Built as text and written whole, the way the save is: a torn stats file is worthless rather
  // than dangerous, but it costs nothing to write it the same way and it means the file is never
  // half a window (Neuron::BinaryFile::WriteFileAtomic).
  std::string text;
  text.reserve(512);
  const auto line = [&text](const char* _key, const std::string& _value)
  {
    text += _key;
    text += " = ";
    text += _value;
    text += "\n";
  };
  const auto number = [](double _value)
  {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f", _value);
    return std::string(buffer);
  };

  text += "# What the tick cost over the last window. Written by the composition root on the stats\n";
  text += "# cadence and reset each time, so this is one window and not a total since boot.\n";
  text += "# Milliseconds. Nothing reads this file yet (Design/Archive/TickTelemetry-work-order.md).\n";
  line("tick", std::to_string(_stats.tick));
  line("ticksInWindow", std::to_string(_stats.ticks));
  line("stepMeanMs", number(_stats.StepMeanMs()));
  line("stepWorstMs", number(_stats.stepWorstMs));
  line("publishMeanMs", number(_stats.PublishMeanMs()));
  line("publishWorstMs", number(_stats.publishWorstMs));
  line("ships", std::to_string(_stats.shipCount));
  line("subscribers", std::to_string(_stats.subscriberCount));
  // Zero until the first faucet and the first sink exist. Written anyway, so that the day one does
  // the file's shape does not change under whatever has started reading it.
  line("issued", std::to_string(_stats.issued));
  line("sunk", std::to_string(_stats.sunk));

  const Neuron::ByteBuffer bytes(text.begin(), text.end());
  return Neuron::BinaryFile::WriteFileAtomic(UNIVERSE_STATS_FILE, bytes);
}
} // namespace Outpost
