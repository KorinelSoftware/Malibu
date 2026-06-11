// tests/test_electron.cpp
// Tasks 33/35: an Electron-style app running entirely on Malibu — main process
// on MalibuJS, renderer on MalibuView — with two-way IPC (send / on / invoke /
// handle) and webContents.send, all without Node or Chromium.

#include <gtest/gtest.h>
#include "malibu/electron/electron_runtime.h"

#include <optional>
#include <string>

using malibu::electron::ElectronRuntime;

namespace {
std::string narrow(const std::u16string& s) {
    std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r;
}
// NOTE: scripts avoid destructuring (`const {a} = ...`) — not yet supported by
// MalibuJS (a known parser gap; real Electron apps need it added next).
const char* kRenderer = R"HTML(
<body>
  <h1 id="title">Renderer on Malibu</h1>
  <script>
    var ipcRenderer = require('electron').ipcRenderer;
    globalThis.received = '';
    ipcRenderer.on('greeting', function(e, payload) { globalThis.received = payload.text; });
    ipcRenderer.send('note', 'from renderer');
    ipcRenderer.invoke('add', 2, 3).then(function(sum) { globalThis.sum = sum; });
  </script>
</body>
)HTML";

const char* kMain = R"JS(
var electron = require('electron');
var app = electron.app;
var BrowserWindow = electron.BrowserWindow;
var ipcMain = electron.ipcMain;
ipcMain.handle('add', function(e, a, b) { return a + b; });
ipcMain.on('note', function(e, msg) { globalThis.note = msg; });
app.whenReady().then(function() {
  var win = new BrowserWindow({ width: 800, height: 600 });
  win.loadFile('index.html');
  globalThis.windowCreated = true;
  win.webContents.send('greeting', { text: 'hello renderer' });
});
)JS";
}  // namespace

TEST(Electron, FullAppWithTwoWayIpc) {
    auto loader = [](const std::string& path) -> std::optional<std::string> {
        if (path == "index.html") return std::string(kRenderer);
        return std::nullopt;
    };
    ElectronRuntime rt(loader);
    ASSERT_TRUE(rt.run_main(kMain));

    auto main_eval = [&](const char* src) -> std::string {
        auto r = rt.main_engine().evaluate(src, "probe");
        return r.ok ? narrow(rt.main_engine().interpreter().json_stringify(r.value)) : std::string("ERR");
    };

    // The app became ready and created a window.
    EXPECT_EQ(main_eval("globalThis.windowCreated"), "true");
    ASSERT_EQ(rt.window_count(), 1u);

    // renderer → main (ipcRenderer.send → ipcMain.on)
    EXPECT_EQ(main_eval("globalThis.note"), "\"from renderer\"");

    // renderer → main → renderer (ipcRenderer.invoke → ipcMain.handle → resolve)
    EXPECT_EQ(rt.window(0)->eval_js("globalThis.sum"), "5");

    // main → renderer (webContents.send → ipcRenderer.on)
    EXPECT_EQ(rt.window(0)->eval_js("globalThis.received"), "\"hello renderer\"");

    // The renderer is a real MalibuView with a live DOM.
    EXPECT_EQ(rt.window(0)->eval_js("document.querySelector('#title').textContent"),
              "\"Renderer on Malibu\"");
}

TEST(Electron, MainScriptErrorReported) {
    ElectronRuntime rt;
    EXPECT_FALSE(rt.run_main("this is not valid javascript {{{"));
}

TEST(Electron, AppQuit) {
    ElectronRuntime rt;
    ASSERT_TRUE(rt.run_main(
        "var app = require('electron').app; app.whenReady().then(function(){ app.quit(); });"));
    EXPECT_TRUE(rt.quit_requested());
}

TEST(Electron, RendererRendersToPixels) {
    // The Electron renderer is a full MalibuView — it produces real pixels.
    auto loader = [](const std::string&) -> std::optional<std::string> {
        return std::string("<body style='margin:0'>"
                           "<div style='background-color:#00aa44; width:50px; height:50px'></div></body>");
    };
    ElectronRuntime rt(loader);
    ASSERT_TRUE(rt.run_main(
        "var electron = require('electron');"
        "electron.app.whenReady().then(function(){ new electron.BrowserWindow().loadFile('index.html'); });"));
    ASSERT_EQ(rt.window_count(), 1u);
    auto fb = rt.window(0)->render(80, 80);
    EXPECT_EQ(fb.at(20, 20), (malibu::render::Color{0x00, 0xaa, 0x44, 255}));
}
