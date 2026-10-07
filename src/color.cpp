#include "color.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "params.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// Approximate sRGB tint of a black body (Tanner Helland's fit), each channel 0..1.
void kelvinToRgb(double k, double rgb[3]) {
  double t = std::clamp(k, 1000.0, 40000.0) / 100.0;
  double r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
  double g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60, -0.0755148492);
  double b = t >= 66 ? 255 : (t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307);
  rgb[0] = std::clamp(r, 0.0, 255.0) / 255.0;
  rgb[1] = std::clamp(g, 0.0, 255.0) / 255.0;
  rgb[2] = std::clamp(b, 0.0, 255.0) / 255.0;
}

// Sunrise/sunset in local minutes after midnight for today (NOAA approximation).
// Returns false during polar day/night.
bool sunTimes(double latDeg, double lonDeg, double& riseMin, double& setMin) {
  SYSTEMTIME utc, local;
  GetSystemTime(&utc);
  GetLocalTime(&local);
  FILETIME fu, fl;
  SystemTimeToFileTime(&utc, &fu);
  SystemTimeToFileTime(&local, &fl);
  double offsetMin = ((double)(*(int64_t*)&fl) - (double)(*(int64_t*)&fu)) / 600000000.0;

  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int y = local.wYear;
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  int doy = cum[local.wMonth - 1] + local.wDay + (leap && local.wMonth > 2 ? 1 : 0);
  double gamma = 2 * kPi / (leap ? 366 : 365) * (doy - 1);
  double eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma) -
                            0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
  double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma) - 0.006758 * cos(2 * gamma) +
                0.000907 * sin(2 * gamma) - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);
  double lat = latDeg * kPi / 180;
  double c = cos(90.833 * kPi / 180) / (cos(lat) * cos(decl)) - tan(lat) * tan(decl);
  if (c < -1 || c > 1) return false;
  double ha = acos(c) * 180 / kPi;
  riseMin = fmod(720 - 4 * (lonDeg + ha) - eqtime + offsetMin + 2880, 1440);
  setMin = fmod(720 - 4 * (lonDeg - ha) - eqtime + offsetMin + 2880, 1440);
  return true;
}

double smooth01(double u) {
  u = std::clamp(u, 0.0, 1.0);
  return u * u * (3 - 2 * u);
}

// 1 inside the circular interval [start, end] (minutes, may wrap midnight), with smooth edges
// `trans` minutes wide centred on each boundary.
double inInterval(double t, double start, double end, double trans) {
  if (end < start) end += 1440;
  trans = std::max(trans, 1.0);
  double best = 0;
  for (double shift : {-1440.0, 0.0, 1440.0}) {
    double x = t + shift;
    double rise = smooth01((x - start) / trans + 0.5);
    double fall = 1 - smooth01((x - end) / trans + 0.5);
    best = std::max(best, std::min(rise, fall));
  }
  return best;
}

std::string clock(double minutes) {
  int m = (int)std::lround(fmod(minutes + 1440, 1440));
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", (m / 60) % 24, m % 60);
  return buf;
}

}  // namespace

// f.lux-style phases: day -> evening around sunset -> late at bedtime -> day again at the later of
// wake time and sunrise. Stores smooth blend weights for the evening and late phases.
void ColorPlan::updateSchedule(const ParamStore& p) {
  SYSTEMTIME lt;
  GetLocalTime(&lt);
  double now = lt.wHour * 60 + lt.wMinute + lt.wSecond / 60.0;
  bool simulated = p.get("simTimeEnabled", 0) >= 0.5;
  if (simulated) now = std::fmod(p.get("simTime", 12), 24.0) * 60;
  double trans = p.get("transitionMin", 60);
  double wake = p.get("wakeTime", 6) * 60, bed = p.get("bedTime", 22) * 60;
  double rise = 6 * 60, set = 18 * 60;
  bool sun = sunTimes(p.get("latitude", 0), p.get("longitude", 0), rise, set);
  night_ = inInterval(now, set, std::max(rise, wake), trans);
  late_ = inInterval(now, bed, wake, trans);
  const char* phase = late_ > 0.5 ? "late" : night_ > 0.5 ? "evening" : "day";
  char buf[160];
  snprintf(buf, sizeof buf, "%s %s: %s · sunset %s · bedtime %s · sunrise %s%s", simulated ? "SIMULATED" : "now",
           clock(now).c_str(), phase, clock(set).c_str(), clock(bed).c_str(), clock(rise).c_str(),
           sun ? "" : " (no sunrise/sunset today)");
  statusSchedule_ = buf;
}

void ColorPlan::update(const ParamStore& p, bool bypass) {
  auto nowMs = GetTickCount64();
  if (nowMs - lastScheduleMs_ > 5000 || p.version() != scheduleVersion_) {
    lastScheduleMs_ = nowMs;
    scheduleVersion_ = p.version();
    updateSchedule(p);
  }

  // Full-strength recipe: saturation around Rec.709 luma, then per-channel gains from the
  // temperature (relative to 6500 K), channel cuts and brightness. Applied to encoded values,
  // like f.lux's gamma ramps.
  double ref[3], tint[3], gain[3];
  kelvinToRgb(6500, ref);
  kelvinToRgb(p.get("temperature", 6500), tint);
  for (int c = 0; c < 3; c++) gain[c] = std::min(1.0, tint[c] / ref[c]) * p.get("colorDim", 1);
  gain[1] *= 1 - p.get("greenCut", 0);
  gain[2] *= 1 - p.get("blueCut", 0);
  double s = p.get("saturation", 1);
  const double w[3] = {0.2126, 0.7152, 0.0722};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) m_[r * 4 + c] = (float)(gain[r] * ((1 - s) * w[c] + (r == c ? s : 0)));

  // How much of the recipe applies at this time of day.
  double amount = 1;
  bool sched = p.get("colorSchedule", 0) >= 0.5;
  if (sched) {
    amount = p.get("amountDay", 0);
    amount += (p.get("amountEvening", 0.6) - amount) * night_;
    amount += (p.get("amountLate", 1) - amount) * late_;
  }
  bool enabled = p.get("colorEnabled", 1) >= 0.5 && !bypass;
  ctl_[0] = (float)amount;
  ctl_[1] = enabled ? 1.0f : 0.0f;

  char buf[160];
  if (!enabled) snprintf(buf, sizeof buf, bypass ? "bypassed" : "off");
  else if (sched)
    snprintf(buf, sizeof buf, "scheduled: %.0f%% of the recipe right now (full at %.0f%% dimming)", amount * 100,
             p.get("followFullAt", 0.6) * 100);
  else
    snprintf(buf, sizeof buf, "ALWAYS: 100%% of the recipe, time of day ignored%s",
             p.get("simTimeEnabled", 0) >= 0.5 ? " (simulated time has no effect; set When to Sun schedule)" : "");
  statusColor_ = buf;
}
