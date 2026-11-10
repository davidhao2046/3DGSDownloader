#include "ui/text/text.hpp"
#include <algorithm>
#include <imm.h>
#include <iostream>
#include <stdexcept>

using namespace gsdl::text;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    Editor edit;
    edit.begin(L"原目录");
    edit.select_all();
    edit.insert(L"中文 路径");
    require(edit.draft() == L"中文 路径", "Unicode insertion");
    require(!edit.commit([](auto) { return false; }), "invalid rejection");
    require(edit.active() && edit.invalid() && edit.draft() == L"中文 路径",
            "invalid draft retained");
    edit.cancel();
    require(edit.draft() == L"原目录" && !edit.active(), "Escape baseline");
    edit.begin(L"a");
    edit.insert(L"\U0001F600");
    require(edit.caret() == 3, "UTF16 surrogate pair");
    edit.erase();
    require(edit.draft() == L"a", "backspace whole surrogate pair");
    edit.undo();
    require(edit.draft() == L"a\U0001F600", "undo draft");
    edit.redo();
    require(edit.draft() == L"a", "redo draft");
    edit.begin(L"abcdef");
    edit.home(false);
    edit.move(1);
    edit.move(1, true);
    edit.move(1, true);
    edit.start_composition();
    edit.update_composition(L"中文", 1);
    require(edit.display_text() == L"a中文def" && edit.draft() == L"abcdef",
            "composition does not edit draft");
    require(!edit.commit(), "composition cannot submit");
    edit.result_composition(L"中文");
    require(edit.draft() == L"a中文def",
            "composition result replaces selection once");
    edit.end_composition();
    require(edit.commit(), "composition result can commit");
    edit.begin(L"1.2345678901234567");
    require(edit.commit() && edit.draft() == L"1.2345678901234567",
            "unedited exact text preserved");
    edit.begin(L"x");
    edit.insert(L"\n");
    edit.insert(std::wstring(1, static_cast<wchar_t>(0xd800)));
    require(edit.draft() == L"x", "invalid multiline/surrogate input rejected");
    Rasterizer raster;
    const auto m = raster.measure(L"中文路径 Folder", 14);
    require(m.width > 40 && m.height > 10 && m.baseline > 0, "CJK metrics");
    const auto cx = raster.caret_x(L"中文路径", 14, 2);
    require(cx > 0, "caret measurement");
    require(raster.hit_test(L"中文路径", 14, cx) == 2, "hit-test roundtrip");
    for (float scale : {1.f, 1.5f, 2.f}) {
      std::vector<std::uint8_t> pixels(400 * 100 * 4, 0);
      for (std::size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;
      raster.draw(pixels, 400, 100, L"中文路径 Folder", 5, 5, 14,
                  {218, 218, 218, 255}, scale, Clip{0, 0, 400, 100});
      std::size_t lit{};
      for (std::size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i])
          ++lit;
      require(lit > 40, "real CJK glyph raster is nonempty");
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"3DGSDownloader.TextTest";
    require(RegisterClassW(&wc) != 0, "test window class");
    const auto hwnd =
        CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 20, 20,
                        nullptr, nullptr, wc.hInstance, nullptr);
    require(hwnd != nullptr, "hidden input host");
    {
      ImeBinding binding(hwnd);
      require(binding.set_text_focus(true), "restore system input context");
      const auto original = ImmGetContext(hwnd);
      require(original != nullptr, "text has input context");
      ImmReleaseContext(hwnd, original);
      require(!binding.set_text_focus(false, true),
              "do not detach mid-composition");
      require(binding.set_text_focus(false), "detach non-text input");
      require(ImmGetContext(hwnd) == nullptr, "no non-text IME target");
      require(binding.set_text_focus(true), "reattach text input");
      const auto restored = ImmGetContext(hwnd);
      require(restored == original, "same borrowed context restored");
      ImmReleaseContext(hwnd, restored);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::cout
        << "text policy, IME state, CJK layout/raster and DPI probes passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
