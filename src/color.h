// Treated-area color: the color recipe (temperature, channel cuts, saturation, brightness) that the
// overlay applies only where the guard is dimming, scaled by how hard it dims and by time of day.
// This class turns the params + sun schedule into a 3x3 matrix and a time amount for the shaders.
#pragma once
#include <cstdint>
#include <string>

class ParamStore;

class ColorPlan {
 public:
  // Call every frame (cheap; the schedule is re-evaluated every few seconds or on param changes).
  void update(const ParamStore& params, bool bypass);

  // Rows of the full-strength recipe: out_r = sum_c m[r*4 + c] * in_c (rows padded to float4).
  const float* matrix() const { return m_; }
  // [0] time-of-day amount (0..1), [1] enabled, [2..3] unused.
  const float* control() const { return ctl_; }

  // Human-readable summaries for the panel.
  const std::string& statusColor() const { return statusColor_; }
  const std::string& statusSchedule() const { return statusSchedule_; }

 private:
  void updateSchedule(const ParamStore& p);

  float m_[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  float ctl_[4] = {0, 0, 0, 0};
  double night_ = 0, late_ = 0;  // schedule blend weights
  unsigned long long lastScheduleMs_ = 0;
  uint64_t scheduleVersion_ = 0;
  std::string statusColor_, statusSchedule_;
};
