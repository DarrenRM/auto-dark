// Shader compilation, hot reload, and reflection of the `Params` cbuffer layout.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "params.h"

using Microsoft::WRL::ComPtr;

struct ParamVar {
  std::string name;
  uint32_t offset = 0;
  uint32_t size = 0;
  int type = 0;  // D3D_SHADER_VARIABLE_TYPE
};

struct ShaderSet {
  ComPtr<ID3D11ComputeShader> luma, gain, temporal, preview, stats;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  std::vector<ParamVar> paramVars;
  uint32_t paramSize = 16;

  // Fills the Params cbuffer bytes from the store. Names missing from the store read as 0.
  std::vector<uint8_t> packParams(const ParamStore& store) const;
};

class ShaderManager {
 public:
  explicit ShaderManager(std::string root) : root_(std::move(root)) {}

  // Compiles everything; on success replaces current(). On failure keeps the old set.
  bool compile(ID3D11Device* dev, std::string& err);
  // True when any shader source or params.json changed since the last call.
  bool sourcesChanged(bool* paramsJsonChanged);

  std::shared_ptr<ShaderSet> current() const { return current_; }

 private:
  std::string root_;
  std::shared_ptr<ShaderSet> current_;
  std::map<std::string, std::filesystem::file_time_type> stamps_;
  bool initialized_ = false;
};
