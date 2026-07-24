#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class MtMetric : uint8_t {
  ComputeAO,
  ComputeBloom,
  PPAO,
  PPBloom,
  FrameGPU, // Real GPU execution time for the whole frame's command buffer,
            // via MTLCommandBuffer::GPUStartTime()/GPUEndTime(). Reported
            // asynchronously from a completion handler, typically 1-2
            // frames behind due to pipelining -- see
            // MtDebugManager::RecordGPUFrameTimeAsync.
  TextureUploadCPU, // CPU-side cost (staging memcpy + blit encode + commit)
            // of the "world texture" upload path in mt_texture.cpp -- the
            // one that submits its own separate command buffer via
            // GetBlitCommandBuffer(), competing with the main per-frame
            // render command buffer for render-thread CPU time and GPU
            // queue submission overhead. See MtDebugManager::RecordTextureUpload.
  Count
};

struct MtMetricInfo {
  const char *label;
  const char *csvName;
  const char *group;
};

class MtMetricFrame {
public:
  void Reset();
  void Record(MtMetric metric, float durationMs);
  float Get(MtMetric metric) const;
  bool Has(MtMetric metric) const { return Get(metric) > 0.0f; }

private:
  std::array<float, (size_t)MtMetric::Count> values = {};
};

struct MtMetricStats {
  float current = 0.0f;
  float average = 0.0f;
  float minimum = 0.0f;
  float maximum = 0.0f;
  size_t samples = 0;
};

class MtMetricHistory {
public:
  explicit MtMetricHistory(size_t maxFrames = 120);

  void RecordFrame(const MtMetricFrame &frame);
  MtMetricStats GetStats(MtMetric metric) const;
  void Clear();

  static const MtMetricInfo &GetInfo(MtMetric metric);

private:
  size_t maxFrames = 120;
  std::vector<MtMetricFrame> frames;
};
