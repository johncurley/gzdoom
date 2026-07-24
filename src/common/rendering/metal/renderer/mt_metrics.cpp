#include "mt_metrics.h"

#include <algorithm>

namespace {
constexpr std::array<MtMetricInfo, (size_t)MtMetric::Count> MetricInfos = {{
    {"AO", "ComputeAOCPUms", "ComputeCPU"},
    {"Bloom", "ComputeBloomCPUms", "ComputeCPU"},
    {"AO", "PPAOCPUms", "PPCPU"},
    {"Bloom", "PPBloomCPUms", "PPCPU"},
    {"Frame", "FrameGPUms", "GPU"},
    {"TexUp", "TextureUploadCPUms", "Upload"},
}};

size_t MetricIndex(MtMetric metric) {
  auto index = (size_t)metric;
  return index < (size_t)MtMetric::Count ? index : (size_t)MtMetric::Count;
}
}

void MtMetricFrame::Reset() {
  values.fill(0.0f);
}

void MtMetricFrame::Record(MtMetric metric, float durationMs) {
  auto index = MetricIndex(metric);
  if (index >= values.size())
    return;

  values[index] += durationMs;
}

float MtMetricFrame::Get(MtMetric metric) const {
  auto index = MetricIndex(metric);
  if (index >= values.size())
    return 0.0f;

  return values[index];
}

MtMetricHistory::MtMetricHistory(size_t maxFrames) : maxFrames(maxFrames) {
  frames.reserve(maxFrames);
}

void MtMetricHistory::RecordFrame(const MtMetricFrame &frame) {
  frames.push_back(frame);
  if (frames.size() > maxFrames)
    frames.erase(frames.begin());
}

MtMetricStats MtMetricHistory::GetStats(MtMetric metric) const {
  MtMetricStats stats;
  if (frames.empty())
    return stats;

  stats.current = frames.back().Get(metric);

  float total = 0.0f;
  for (const auto &frame : frames) {
    const float value = frame.Get(metric);
    if (value <= 0.0f)
      continue;

    total += value;
    if (stats.samples == 0) {
      stats.minimum = value;
      stats.maximum = value;
    } else {
      stats.minimum = std::min(stats.minimum, value);
      stats.maximum = std::max(stats.maximum, value);
    }
    stats.samples++;
  }

  if (stats.samples > 0)
    stats.average = total / (float)stats.samples;
  return stats;
}

void MtMetricHistory::Clear() {
  frames.clear();
}

const MtMetricInfo &MtMetricHistory::GetInfo(MtMetric metric) {
  auto index = MetricIndex(metric);
  if (index >= MetricInfos.size())
    return MetricInfos[0];

  return MetricInfos[index];
}
