// Parameter store: schema (shaders/params.json), live values, and profiles (profiles/*.json).
// Shared between the render thread and the HTTP thread; every public method locks.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "json.h"

class ParamStore {
 public:
  explicit ParamStore(std::string root);

  // Loads the schema and the last active profile (or "default").
  void init(const std::string& profileOverride);

  // Re-reads params.json. Keeps current values, fills new params with defaults.
  bool reloadSchema(std::string& err);

  json::Value schema() const;
  int schemaVersion() const;

  json::Value values() const;  // deep copy of current values
  // Partial update {id: value}. Unknown ids are ignored. Returns false if nothing applied.
  bool set(const json::Value& partial);
  double get(const std::string& id, double def = 0) const;
  // Budget curve points (x = log10 area, y = budget), sorted by x.
  std::vector<std::pair<double, double>> curve() const;
  // Evaluates the curve, interpolating linearly in (log10 area, log10 budget).
  static double evalCurve(const std::vector<std::pair<double, double>>& pts, double logArea);

  // Increments on every value change (used to detect "something changed" in the render loop).
  uint64_t version() const;

  std::vector<std::string> listProfiles() const;
  bool loadProfile(const std::string& name, std::string& err);
  bool saveProfile(const std::string& name, std::string& err);
  bool deleteProfile(const std::string& name, std::string& err);
  bool cycleProfile();  // loads the next profile alphabetically
  std::string activeProfile() const;
  bool dirty() const;

  // Cycles an enum param through its options (used by the debug-view hotkey).
  void cycleEnum(const std::string& id);

 private:
  bool loadSchemaLocked(std::string& err);
  void applyDefaultsLocked();
  bool setOneLocked(const std::string& id, const json::Value& v);
  const json::Value* paramDefLocked(const std::string& id) const;
  std::string profilePath(const std::string& name) const;
  bool loadProfileLocked(const std::string& name, std::string& err);
  void writeActiveMarkerLocked();
  static bool validName(const std::string& name);

  mutable std::mutex mu_;
  std::string root_;
  json::Value schema_;
  json::Object values_;
  std::string active_ = "default";
  bool dirty_ = false;
  int schemaVersion_ = 0;
  uint64_t version_ = 1;
};

std::string readFile(const std::string& path, bool* ok = nullptr);
bool writeFile(const std::string& path, const std::string& data);
