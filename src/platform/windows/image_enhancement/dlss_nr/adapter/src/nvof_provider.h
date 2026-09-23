// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Foundation Sunshine contributors
#pragma once
#include <d3d11.h>
#include <memory>

// Experimental provider. Caller must drain the context before destruction.
class flow_provider {
public:
  static std::unique_ptr<flow_provider>
  create(ID3D11Device *, ID3D11DeviceContext *,
    ID3D11Texture2D *source, ID3D11Texture2D *dense_output, int quality);
  ~flow_provider();
  bool
  process(bool reset = false);

private:
  struct impl;
  explicit flow_provider(std::unique_ptr<impl>);
  std::unique_ptr<impl> p;
};
