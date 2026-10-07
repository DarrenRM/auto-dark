#include "params.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "log.h"

namespace fs = std::filesystem;

std::string readFile(const std::string& path, bool* ok) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (ok) *ok = bool(f);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
  std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << data;
  return bool(f);
}

ParamStore::ParamStore(std::string root) : root_(std::move(root)) {}

void ParamStore::init(const std::string& profileOverride) {
  std::lock_guard lk(mu_);
  std::string err;
  if (!loadSchemaLocked(err)) {
    logf("params: %s", err.c_str());
    schema_ = json::Object{{"groups", json::Array{}}, {"params", json::Array{}}};
  }
  applyDefaultsLocked();
  fs::create_directories(fs::u8path(root_ + "/profiles"));

  std::string name = profileOverride;
  if (name.empty()) {
    name = readFile(root_ + "/profiles/.active");
    while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();
  }
  if (name.empty() || !validName(name)) name = "default";
  if (!fs::exists(fs::u8path(profilePath(name)))) {
    if (name != "default") logf("params: profile '%s' not found, using default", name.c_str());
    name = "default";
  }
  if (fs::exists(fs::u8path(profilePath(name)))) {
    if (!loadProfileLocked(name, err)) logf("params: %s", err.c_str());
  } else {
    // First run: write the schema defaults as the default profile.
    active_ = "default";
    json::Object file{{"name", "default"}, {"params", json::Value(values_).clone()}};
    writeFile(profilePath("default"), json::dump(json::Value(file), true));
  }
  writeActiveMarkerLocked();
  logf("params: profile '%s'", active_.c_str());
}

bool ParamStore::loadSchemaLocked(std::string& err) {
  bool ok = false;
  std::string text = readFile(root_ + "/shaders/params.json", &ok);
  if (!ok) { err = "cannot read shaders/params.json"; return false; }
  try {
    json::Value v = json::parse(text);
    if (!v["params"].isArr()) { err = "params.json: missing 'params' array"; return false; }
    schema_ = v;
    schemaVersion_++;
    return true;
  } catch (std::exception& e) {
    err = std::string("params.json: ") + e.what();
    return false;
  }
}

bool ParamStore::reloadSchema(std::string& err) {
  std::lock_guard lk(mu_);
  if (!loadSchemaLocked(err)) return false;
  applyDefaultsLocked();
  // Re-validate existing values against the (possibly changed) ranges.
  json::Object old = values_;
  for (auto& [k, v] : old) setOneLocked(k, v.clone());
  version_++;
  return true;
}

void ParamStore::applyDefaultsLocked() {
  for (auto& p : schema_["params"].arr()) {
    const std::string& id = p["id"].str();
    if (id.empty() || values_.count(id)) continue;
    values_[id] = p["default"].clone();
  }
}

const json::Value* ParamStore::paramDefLocked(const std::string& id) const {
  for (auto& p : schema_["params"].arr())
    if (p["id"].str() == id) return &p;
  return nullptr;
}

bool ParamStore::setOneLocked(const std::string& id, const json::Value& v) {
  const json::Value* def = paramDefLocked(id);
  if (!def) return false;
  const std::string& type = (*def)["type"].str();
  if (type == "curve") {
    if (!v.isArr()) return false;
    double xMin = (*def)["xMin"].num(-7), xMax = (*def)["xMax"].num(0);
    double yMin = (*def)["yMin"].num(0.001), yMax = (*def)["yMax"].num(1);
    std::vector<std::pair<double, double>> pts;
    for (auto& e : v.arr()) {
      if (!e.isArr() || e.arr().size() < 2) continue;
      pts.push_back({std::clamp(e.arr()[0].num(), xMin, xMax), std::clamp(e.arr()[1].num(), yMin, yMax)});
    }
    if (pts.size() < 2) return false;
    std::sort(pts.begin(), pts.end());
    json::Array out;
    for (auto& [x, y] : pts) out.push_back(json::Array{x, y});
    values_[id] = json::Value(std::move(out));
    return true;
  }
  if (!v.isNum() && v.type != json::Value::Type::Bool) return false;
  double x = v.num();
  if (type == "enum") {
    int n = (int)(*def)["options"].arr().size();
    x = std::clamp((double)(int)std::lround(x), 0.0, (double)std::max(0, n - 1));
  } else {
    if ((*def).has("min")) x = std::max(x, (*def)["min"].num());
    if ((*def).has("max")) x = std::min(x, (*def)["max"].num());
    if (type == "int") x = std::round(x);
  }
  values_[id] = json::Value(x);
  return true;
}

json::Value ParamStore::schema() const {
  std::lock_guard lk(mu_);
  return schema_.clone();
}

int ParamStore::schemaVersion() const {
  std::lock_guard lk(mu_);
  return schemaVersion_;
}

json::Value ParamStore::values() const {
  std::lock_guard lk(mu_);
  return json::Value(values_).clone();
}

bool ParamStore::set(const json::Value& partial) {
  std::lock_guard lk(mu_);
  bool any = false;
  for (auto& [k, v] : partial.obj()) any |= setOneLocked(k, v);
  if (any) { dirty_ = true; version_++; }
  return any;
}

double ParamStore::get(const std::string& id, double def) const {
  std::lock_guard lk(mu_);
  auto it = values_.find(id);
  return it == values_.end() ? def : it->second.num(def);
}

std::vector<std::pair<double, double>> ParamStore::curve() const {
  std::lock_guard lk(mu_);
  std::vector<std::pair<double, double>> pts;
  auto it = values_.find("curve");
  if (it != values_.end())
    for (auto& e : it->second.arr())
      if (e.arr().size() >= 2) pts.push_back({e.arr()[0].num(), e.arr()[1].num()});
  std::sort(pts.begin(), pts.end());
  return pts;
}

double ParamStore::evalCurve(const std::vector<std::pair<double, double>>& pts, double x) {
  if (pts.empty()) return 1.0;
  if (x <= pts.front().first) return pts.front().second;
  if (x >= pts.back().first) return pts.back().second;
  for (size_t i = 1; i < pts.size(); i++) {
    auto [x0, y0] = pts[i - 1];
    auto [x1, y1] = pts[i];
    if (x <= x1) {
      double t = x1 > x0 ? (x - x0) / (x1 - x0) : 1.0;
      double ly = std::log10(std::max(y0, 1e-6)) * (1 - t) + std::log10(std::max(y1, 1e-6)) * t;
      return std::pow(10.0, ly);
    }
  }
  return pts.back().second;
}

uint64_t ParamStore::version() const {
  std::lock_guard lk(mu_);
  return version_;
}

bool ParamStore::validName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (unsigned char c : name)
    if (!(isalnum(c) || c == ' ' || c == '-' || c == '_')) return false;
  return name.front() != ' ' && name.back() != ' ';
}

std::string ParamStore::profilePath(const std::string& name) const {
  return root_ + "/profiles/" + name + ".json";
}

std::vector<std::string> ParamStore::listProfiles() const {
  std::lock_guard lk(mu_);
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::u8path(root_ + "/profiles"), ec)) {
    if (e.path().extension() != ".json") continue;
    std::string stem = e.path().stem().u8string();
    if (validName(stem)) out.push_back(stem);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool ParamStore::loadProfileLocked(const std::string& name, std::string& err) {
  if (!validName(name)) { err = "invalid profile name"; return false; }
  bool ok = false;
  std::string text = readFile(profilePath(name), &ok);
  if (!ok) { err = "profile not found: " + name; return false; }
  json::Value v;
  try {
    v = json::parse(text);
  } catch (std::exception& e) {
    err = "profile " + name + ": " + e.what();
    return false;
  }
  json::Object keep;  // transient params survive profile loads
  for (auto& [k, val] : values_) {
    const json::Value* def = paramDefLocked(k);
    if (def && (*def)["transient"].num() != 0) keep[k] = val;
  }
  values_ = keep;
  applyDefaultsLocked();
  for (auto& [k, val] : v["params"].obj()) {
    const json::Value* def = paramDefLocked(k);
    if (!(def && (*def)["transient"].num() != 0)) setOneLocked(k, val);
  }
  active_ = name;
  dirty_ = false;
  version_++;
  return true;
}

bool ParamStore::loadProfile(const std::string& name, std::string& err) {
  std::lock_guard lk(mu_);
  if (!loadProfileLocked(name, err)) return false;
  writeActiveMarkerLocked();
  return true;
}

bool ParamStore::saveProfile(const std::string& name, std::string& err) {
  std::lock_guard lk(mu_);
  if (!validName(name)) { err = "invalid profile name (letters, digits, space, - and _ only)"; return false; }
  json::Object saved;
  for (auto& [k, v] : values_) {
    const json::Value* def = paramDefLocked(k);
    if (!(def && (*def)["transient"].num() != 0)) saved[k] = v.clone();
  }
  json::Object file{{"name", name}, {"params", json::Value(std::move(saved))}};
  if (!writeFile(profilePath(name), json::dump(json::Value(file), true))) {
    err = "cannot write profile file";
    return false;
  }
  active_ = name;
  dirty_ = false;
  version_++;
  writeActiveMarkerLocked();
  return true;
}

bool ParamStore::deleteProfile(const std::string& name, std::string& err) {
  std::lock_guard lk(mu_);
  if (!validName(name)) { err = "invalid profile name"; return false; }
  std::error_code ec;
  if (!fs::remove(fs::u8path(profilePath(name)), ec)) { err = "profile not found"; return false; }
  if (name == active_) { dirty_ = true; version_++; }
  return true;
}

bool ParamStore::cycleProfile() {
  auto names = listProfiles();
  if (names.empty()) return false;
  std::string cur = activeProfile();
  auto it = std::upper_bound(names.begin(), names.end(), cur);
  std::string next = it == names.end() ? names.front() : *it;
  std::string err;
  if (!loadProfile(next, err)) { logf("params: %s", err.c_str()); return false; }
  logf("params: profile '%s'", next.c_str());
  return true;
}

std::string ParamStore::activeProfile() const {
  std::lock_guard lk(mu_);
  return active_;
}

bool ParamStore::dirty() const {
  std::lock_guard lk(mu_);
  return dirty_;
}

void ParamStore::cycleEnum(const std::string& id) {
  std::lock_guard lk(mu_);
  const json::Value* def = paramDefLocked(id);
  if (!def) return;
  int n = (int)(*def)["options"].arr().size();
  if (n <= 0) return;
  int cur = (int)values_[id].num();
  values_[id] = json::Value((cur + 1) % n);
  dirty_ = true;
  version_++;
}

void ParamStore::writeActiveMarkerLocked() {
  writeFile(root_ + "/profiles/.active", active_);
}
