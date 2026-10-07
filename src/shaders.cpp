#include "shaders.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& s) { return fs::u8path(s).wstring(); }

bool compileOne(const std::string& path, const char* entry, const char* target, ComPtr<ID3DBlob>& out,
                std::string& err) {
  ComPtr<ID3DBlob> errors;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  HRESULT hr = D3DCompileFromFile(widen(path).c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                  flags, 0, &out, &errors);
  if (FAILED(hr)) {
    err = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize())
                 : "failed to compile " + path + " (hr=" + std::to_string(hr) + ")";
    while (!err.empty() && (err.back() == '\n' || err.back() == '\0')) err.pop_back();
    return false;
  }
  return true;
}

// Merges the Params cbuffer variables of one shader into `vars`.
void reflectParams(ID3DBlob* blob, std::vector<ParamVar>& vars, uint32_t& size) {
  ComPtr<ID3D11ShaderReflection> refl;
  if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&refl)))) return;
  ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("Params");
  D3D11_SHADER_BUFFER_DESC cbDesc{};
  if (FAILED(cb->GetDesc(&cbDesc))) return;  // shader doesn't use Params
  size = std::max(size, (uint32_t)cbDesc.Size);
  for (UINT i = 0; i < cbDesc.Variables; i++) {
    ID3D11ShaderReflectionVariable* v = cb->GetVariableByIndex(i);
    D3D11_SHADER_VARIABLE_DESC vd{};
    D3D11_SHADER_TYPE_DESC td{};
    if (FAILED(v->GetDesc(&vd)) || FAILED(v->GetType()->GetDesc(&td))) continue;
    bool known = std::any_of(vars.begin(), vars.end(), [&](const ParamVar& p) { return p.name == vd.Name; });
    if (!known) vars.push_back({vd.Name, vd.StartOffset, vd.Size, (int)td.Type});
  }
}

}  // namespace

std::vector<uint8_t> ShaderSet::packParams(const ParamStore& store) const {
  std::vector<uint8_t> bytes((paramSize + 15) / 16 * 16, 0);
  for (auto& v : paramVars) {
    if (v.size < 4 || v.offset + 4 > bytes.size()) continue;
    double x = store.get(v.name, 0);
    if (v.type == D3D_SVT_FLOAT) {
      float f = (float)x;
      memcpy(&bytes[v.offset], &f, 4);
    } else if (v.type == D3D_SVT_INT) {
      int32_t i = (int32_t)x;
      memcpy(&bytes[v.offset], &i, 4);
    } else {  // uint / bool
      uint32_t u = (uint32_t)std::max(0.0, x);
      memcpy(&bytes[v.offset], &u, 4);
    }
  }
  return bytes;
}

bool ShaderManager::compile(ID3D11Device* dev, std::string& err) {
  auto set = std::make_shared<ShaderSet>();
  set->paramSize = 16;
  std::string dir = root_ + "/shaders/";

  struct CsJob { const char* file; ComPtr<ID3D11ComputeShader>* out; };
  CsJob cs[] = {{"luma.hlsl", &set->luma}, {"gain.hlsl", &set->gain}, {"temporal.hlsl", &set->temporal},
                {"preview.hlsl", &set->preview}, {"stats.hlsl", &set->stats}};
  for (auto& job : cs) {
    ComPtr<ID3DBlob> blob;
    if (!compileOne(dir + job.file, "CSMain", "cs_5_0", blob, err)) return false;
    if (FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, job.out->GetAddressOf()))) {
      err = std::string("CreateComputeShader failed: ") + job.file;
      return false;
    }
    reflectParams(blob.Get(), set->paramVars, set->paramSize);
  }
  ComPtr<ID3DBlob> vsBlob, psBlob;
  if (!compileOne(dir + "apply.hlsl", "VSMain", "vs_5_0", vsBlob, err)) return false;
  if (!compileOne(dir + "apply.hlsl", "PSMain", "ps_5_0", psBlob, err)) return false;
  if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &set->vs)) ||
      FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &set->ps))) {
    err = "CreateVertex/PixelShader failed";
    return false;
  }
  reflectParams(psBlob.Get(), set->paramVars, set->paramSize);

  current_ = set;
  err.clear();
  return true;
}

bool ShaderManager::sourcesChanged(bool* paramsJsonChanged) {
  bool changed = false;
  if (paramsJsonChanged) *paramsJsonChanged = false;
  std::error_code ec;
  bool initial = !initialized_;
  initialized_ = true;
  for (auto& e : fs::directory_iterator(fs::u8path(root_ + "/shaders"), ec)) {
    auto ext = e.path().extension();
    if (ext != ".hlsl" && ext != ".hlsli" && ext != ".json") continue;
    std::string key = e.path().filename().u8string();
    auto t = fs::last_write_time(e.path(), ec);
    if (ec) continue;
    auto it = stamps_.find(key);
    if (it == stamps_.end() || it->second != t) {
      stamps_[key] = t;
      if (!initial) {
        changed = true;
        if (ext == ".json" && paramsJsonChanged) *paramsJsonChanged = true;
      }
    }
  }
  return changed;
}
