// JPEG encoding via WIC (for the web UI preview).
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

bool encodeJpeg(const std::vector<uint8_t>& rgba, int w, int h, float quality, std::vector<uint8_t>& out) {
  static thread_local ComPtr<IWICImagingFactory> factory;
  if (!factory && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&factory))))
    return false;

  std::vector<uint8_t> bgr((size_t)w * h * 3);
  for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
    bgr[i * 3 + 0] = rgba[i * 4 + 2];
    bgr[i * 3 + 1] = rgba[i * 4 + 1];
    bgr[i * 3 + 2] = rgba[i * 4 + 0];
  }

  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
  PROPBAG2 opt{};
  opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_R4;
  v.fltVal = quality;
  props->Write(1, &opt, &v);
  if (FAILED(frame->Initialize(props.Get()))) return false;
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  frame->SetPixelFormat(&fmt);
  if (fmt != GUID_WICPixelFormat24bppBGR) return false;
  if (FAILED(frame->WritePixels(h, w * 3, (UINT)bgr.size(), bgr.data()))) return false;
  if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

  HGLOBAL hg = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &hg))) return false;
  STATSTG st{};
  stream->Stat(&st, STATFLAG_NONAME);
  size_t size = (size_t)st.cbSize.QuadPart;
  void* p = GlobalLock(hg);
  if (!p) return false;
  out.assign((uint8_t*)p, (uint8_t*)p + size);
  GlobalUnlock(hg);
  return true;
}
