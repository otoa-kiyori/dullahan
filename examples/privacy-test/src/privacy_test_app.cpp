/**
    @brief  Dullahan Privacy Test application — implementation
*/

#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <random>

#include "privacy_test_app.h"
#include "dullahan.h"

// ── Platform helpers ────────────────────────────────────────────────────────
#if defined(WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

static void glfwErrorCB(int err, const char* desc)
{
    std::cerr << "GLFW error " << err << ": " << desc << "\n";
}

// ── PrivacyTestApp ──────────────────────────────────────────────────────────

PrivacyTestApp::PrivacyTestApp() = default;

// ── init ────────────────────────────────────────────────────────────────────

bool PrivacyTestApp::init(bool privacyEnabled)
{
    mPrivacyOn = privacyEnabled;

    // ── GLFW ----------------------------------------------------------------
    glfwSetErrorCallback(glfwErrorCB);
    if (!glfwInit()) { return false; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_SAMPLES,    0);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED,  GLFW_TRUE);

    std::string title = std::string("Dullahan Privacy Test  [Privacy: ")
                        + (mPrivacyOn ? "ON]" : "OFF]");
    mWindow = glfwCreateWindow(kWindowW, kWindowH, title.c_str(), nullptr, nullptr);
    if (!mWindow) { glfwTerminate(); return false; }

    glfwSetWindowUserPointer(mWindow, this);
    glfwSetFramebufferSizeCallback(mWindow, resizeCB);
    glfwSetMouseButtonCallback(mWindow,    mouseButtonCB);
    glfwSetCursorPosCallback(mWindow,      mouseMoveCB);
    glfwSetScrollCallback(mWindow,         mouseScrollCB);

#if defined(WIN32)
    HWND hwnd = glfwGetWin32Window(mWindow);
    SetWindowSubclass(hwnd, keySubclassProc, 0x02, (DWORD_PTR)this);
#endif

    glfwMakeContextCurrent(mWindow);
    gladLoadGL();
    glfwSwapInterval(1);

    // ── OpenGL texture -------------------------------------------------------
    glGenTextures(1, &mTextureId);
    glBindTexture(GL_TEXTURE_2D, mTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // ── ImGui ----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    io.FontGlobalScale = 1.1f;
    ImGui_ImplGlfw_InitForOpenGL(mWindow, true);
    ImGui_ImplOpenGL2_Init();

    // ── Dullahan -------------------------------------------------------------
    mDullahan = new dullahan();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(100000, 999999);
    int rnd = dist(gen);

    std::filesystem::path cachePath =
        std::filesystem::absolute("./privacy-test-profile") / std::to_string(rnd);

    dullahan::dullahan_settings settings;
    settings.protect_privacy  = mPrivacyOn;
    settings.root_cache_path  = cachePath.string();
    settings.log_file         = (cachePath / "privacy-test-cef.log").string();
    settings.initial_width    = kWindowW - kLeftPanelW;
    settings.initial_height   = kWindowH;
    settings.disable_gpu      = false;
    settings.user_agent_substring = "DullahanPrivacyTest/1.0";
#if defined(__APPLE__)
    settings.use_mock_keychain = true;
#endif

    mBrowserW = settings.initial_width;
    mBrowserH = settings.initial_height;

    if (!mDullahan->init(settings)) { return false; }

    mDullahan->setOnPageChangedCallback(
        std::bind(&PrivacyTestApp::onPageChanged, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4,
            std::placeholders::_5));
    mDullahan->setOnConsoleMessageCallback(
        std::bind(&PrivacyTestApp::onConsoleMessage, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3));
    mDullahan->setOnRequestExitCallback(
        std::bind(&PrivacyTestApp::onRequestExit, this));

    mTestPageUrl = buildTestPageUrl();
    mDullahan->navigate(mTestPageUrl);

    return true;
}

// ── run ─────────────────────────────────────────────────────────────────────

bool PrivacyTestApp::run()
{
    while (!glfwWindowShouldClose(mWindow) && !mShouldClose)
    {
        if (mDullahan) { mDullahan->update(); }

        glClearColor(0.12f, 0.12f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        updateUI();

        glfwSwapBuffers(mWindow);
        glfwPollEvents();
    }
    return true;
}

// ── shutdown ─────────────────────────────────────────────────────────────────

void PrivacyTestApp::shutdown()
{
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (mTextureId) { glDeleteTextures(1, &mTextureId); }
    glfwDestroyWindow(mWindow);
    glfwTerminate();
}

// ── updateUI ────────────────────────────────────────────────────────────────

void PrivacyTestApp::updateUI()
{
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float menuH = ImGui::GetFrameHeight();

    // ── Menu bar ─────────────────────────────────────────────────────────────
    if (ImGui::BeginMainMenuBar())
    {
        // Privacy state indicator + relaunch toggle
        ImVec4 col = mPrivacyOn
            ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f)
            : ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(col, mPrivacyOn ? " PRIVACY: ON " : " PRIVACY: OFF ");

        if (ImGui::SmallButton(mPrivacyOn ? "Relaunch OFF" : "Relaunch ON"))
        {
            relaunch(!mPrivacyOn);
        }

        ImGui::Separator();
        if (ImGui::SmallButton("Refresh Tests"))
        {
            std::lock_guard<std::mutex> lk(mResultsMtx);
            mResults.clear();
            mPassCount = mFailCount = mTotalCount = 0;
            mDullahan->navigate(mTestPageUrl);
        }

        ImGui::Separator();
        if (ImGui::SmallButton("Dev Tools")) { mDullahan->showDevTools(); }

        ImGui::Separator();
        if (ImGui::SmallButton("Quit"))     { mDullahan->requestExit(); }

        ImGui::EndMainMenuBar();
    }

    // ── Full-screen container (below menu bar) ────────────────────────────────
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos (ImVec2(0, menuH));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - menuH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr, wf);
    ImGui::PopStyleVar();

    const float totalH = vp->Size.y - menuH;

    // ── Left panel: test results ─────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::BeginChild("##left", ImVec2((float)kLeftPanelW, totalH), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    drawTestPanel();

    ImGui::EndChild();

    // ── Right panel: browser texture ─────────────────────────────────────────
    ImGui::SameLine(0, 0);

    const float rightW = vp->Size.x - (float)kLeftPanelW;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##right", ImVec2(rightW, totalH), false);
    ImGui::PopStyleVar();

    // Display the CEF render texture; flip UVs so top is top
    // (OpenGL textures are stored bottom-up; dullahan provides top-down pixels)
    ImVec2 imgSize(rightW, totalH);
    ImGui::Image((ImTextureID)(uintptr_t)mTextureId, imgSize,
                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    // Store the image rect so mouse callbacks can compute browser coords
    mBrowserRectMin = ImGui::GetItemRectMin();
    mBrowserRectMax = ImGui::GetItemRectMax();

    // Forward mouse move while hovering (enables link hover, text selection)
    if (ImGui::IsItemHovered())
    {
        ImVec2 mp = ImGui::GetMousePos();
        float relX = (mp.x - mBrowserRectMin.x) / (mBrowserRectMax.x - mBrowserRectMin.x);
        float relY = (mp.y - mBrowserRectMin.y) / (mBrowserRectMax.y - mBrowserRectMin.y);
        mDullahan->mouseMove((int)(relX * mBrowserW), (int)(relY * mBrowserH));
    }

    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

// ── drawTestPanel ────────────────────────────────────────────────────────────

void PrivacyTestApp::drawTestPanel()
{
    std::lock_guard<std::mutex> lk(mResultsMtx);

    // Summary header
    {
        float pct = mTotalCount > 0 ? (float)mPassCount / (float)mTotalCount : 0.0f;
        std::ostringstream ss;
        ss << mPassCount << " / " << mTotalCount;

        ImVec4 scoreCol = (mFailCount == 0 && mTotalCount > 0)
            ? ImVec4(0.4f, 0.85f, 0.5f, 1.0f)
            : ImVec4(0.9f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(scoreCol, "%s", ss.str().c_str());
        ImGui::SameLine();

        int pending = mTotalCount - mPassCount - mFailCount;
        if (pending > 0)
        {
            std::string pendStr = std::to_string(pending) + " pending";
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1.0f), "%s", pendStr.c_str());
        }
        else if (mTotalCount > 0)
        {
            ImGui::TextColored(scoreCol, mFailCount == 0 ? "ALL PASS" : "FAIL");
        }

        ImGui::ProgressBar(pct, ImVec2(-1.0f, 6.0f), "");
        ImGui::Spacing();
    }

    // Results table
    const ImGuiTableFlags tfl =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY       | ImGuiTableFlags_SizingFixedFit;

    float tableH = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##results", 4, tfl, ImVec2(-1.0f, tableH)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableSetupColumn("Cat",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Test",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actual",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (const auto& r : mResults)
        {
            ImGui::TableNextRow();

            // Status indicator
            ImGui::TableSetColumnIndex(0);
            if (r.pending)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1.0f), "~");
            }
            else if (r.pass)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.0f), "\xE2\x9C\x93"); // utf8 check
            }
            else
            {
                ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "X");
            }

            // Category
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.77f, 1.0f), "%s", r.category.c_str());

            // Test label + expected tooltip
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.label.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Expected: %s", r.expected.c_str());
            }

            // Actual value
            ImGui::TableSetColumnIndex(3);
            ImVec4 actCol = r.pending
                ? ImVec4(0.9f, 0.75f, 0.2f, 1.0f)
                : r.pass
                    ? ImVec4(0.64f, 0.85f, 0.64f, 1.0f)
                    : ImVec4(0.95f, 0.6f, 0.6f, 1.0f);
            ImGui::TextColored(actCol, "%s", r.actual.c_str());
        }

        ImGui::EndTable();
    }
}

// ── Dullahan callbacks ───────────────────────────────────────────────────────

void PrivacyTestApp::onPageChanged(const unsigned char* pixels,
                                   int /*x*/, int /*y*/, int w, int h)
{
    if (w != mBrowserW || h != mBrowserH)
    {
        glDeleteTextures(1, &mTextureId);
        glGenTextures(1, &mTextureId);
        glBindTexture(GL_TEXTURE_2D, mTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        mBrowserW = w;
        mBrowserH = h;
    }
    glBindTexture(GL_TEXTURE_2D, mTextureId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 (GLsizei)w, (GLsizei)h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, pixels);
}

void PrivacyTestApp::onConsoleMessage(const std::string& msg,
                                      const std::string& /*src*/, int /*line*/)
{
    // Structured message format emitted by privacy_test.html:
    //   PRIV|Category|Key|Expected|Actual|PASS  or  FAIL
    if (msg.size() < 5 || msg.substr(0, 5) != "PRIV|") { return; }

    std::vector<std::string> parts;
    std::stringstream ss(msg);
    std::string tok;
    while (std::getline(ss, tok, '|')) { parts.push_back(tok); }
    if (parts.size() != 6) { return; }

    TestResult r;
    r.category = parts[1];
    r.key      = parts[2];
    r.label    = parts[2]; // key doubles as short label
    r.expected = parts[3];
    r.actual   = parts[4];
    r.pass     = (parts[5] == "PASS");
    r.pending  = false;

    std::lock_guard<std::mutex> lk(mResultsMtx);

    // Update existing row (async tests resolve over time)
    for (auto& existing : mResults)
    {
        if (existing.key == r.key)
        {
            bool wasPending = existing.pending;
            existing = r;
            if (wasPending) { mTotalCount++; } // pending slot now counts
            if (r.pass) mPassCount++; else mFailCount++;
            return;
        }
    }

    // New row
    mResults.push_back(r);
    mTotalCount++;
    if (r.pass) mPassCount++; else mFailCount++;
}

void PrivacyTestApp::onRequestExit()
{
    glfwSetWindowShouldClose(mWindow, GLFW_TRUE);
}

// ── Mouse / keyboard callbacks ───────────────────────────────────────────────

void PrivacyTestApp::resizeCallback(int w, int h)
{
    glViewport(0, 0, w, h);
}

void PrivacyTestApp::mouseButtonCallback(int button, int action, int /*mods*/)
{
    if (button != GLFW_MOUSE_BUTTON_LEFT) { return; }

    double xpos, ypos;
    glfwGetCursorPos(mWindow, &xpos, &ypos);

    // Only forward clicks that land on the browser panel
    if (mBrowserRectMax.x > 0
        && xpos >= mBrowserRectMin.x && xpos <= mBrowserRectMax.x
        && ypos >= mBrowserRectMin.y && ypos <= mBrowserRectMax.y)
    {
        float relX = (float)(xpos - mBrowserRectMin.x) / (mBrowserRectMax.x - mBrowserRectMin.x);
        float relY = (float)(ypos - mBrowserRectMin.y) / (mBrowserRectMax.y - mBrowserRectMin.y);
        int tx = (int)(relX * mBrowserW);
        int ty = (int)(relY * mBrowserH);
        mDullahan->mouseButton(
            dullahan::MB_MOUSE_BUTTON_LEFT,
            action == GLFW_PRESS ? dullahan::ME_MOUSE_DOWN : dullahan::ME_MOUSE_UP,
            tx, ty);
    }
}

void PrivacyTestApp::mouseMoveCallback(double /*xpos*/, double /*ypos*/)
{
    // Handled inside updateUI() via ImGui::IsItemHovered() for accuracy
}

void PrivacyTestApp::mouseScrollCallback(double /*xoffset*/, double yoffset)
{
    double xpos, ypos;
    glfwGetCursorPos(mWindow, &xpos, &ypos);

    if (mBrowserRectMax.x > 0
        && xpos >= mBrowserRectMin.x && xpos <= mBrowserRectMax.x
        && ypos >= mBrowserRectMin.y && ypos <= mBrowserRectMax.y)
    {
        float relX = (float)(xpos - mBrowserRectMin.x) / (mBrowserRectMax.x - mBrowserRectMin.x);
        float relY = (float)(ypos - mBrowserRectMin.y) / (mBrowserRectMax.y - mBrowserRectMin.y);
        int tx = (int)(relX * mBrowserW);
        int ty = (int)(relY * mBrowserH);
        mDullahan->mouseWheel(tx, ty, 0, (int)(yoffset * 40));
    }
}

#if defined(WIN32)
LRESULT CALLBACK PrivacyTestApp::keySubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    if (uMsg == WM_CHAR || uMsg == WM_KEYDOWN || uMsg == WM_KEYUP)
    {
        PrivacyTestApp* app = reinterpret_cast<PrivacyTestApp*>(dwRefData);
        app->mDullahan->nativeKeyboardEventWin(uMsg, (uint32_t)wParam, lParam);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}
#endif

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string PrivacyTestApp::executableDir()
{
#if defined(WIN32)
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().generic_string();
#elif defined(__APPLE__)
    char buf[1024] = {};
    uint32_t sz = (uint32_t)sizeof(buf);
    _NSGetExecutablePath(buf, &sz);
    return std::filesystem::path(buf).parent_path().generic_string();
#else
    return ".";
#endif
}

std::string PrivacyTestApp::buildTestPageUrl()
{
    std::string dir = executableDir();
    std::string path = dir + "/privacy_test.html";
#if defined(WIN32)
    return "file:///" + path;
#else
    return "file://" + path;
#endif
}

void PrivacyTestApp::relaunch(bool newPrivacy)
{
#if defined(WIN32)
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string arg = newPrivacy ? "--privacy-on" : "--privacy-off";
    ShellExecuteA(nullptr, "open", exePath, arg.c_str(), nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char exePath[1024] = {};
    uint32_t sz = (uint32_t)sizeof(exePath);
    _NSGetExecutablePath(exePath, &sz);
    std::string cmd = std::string("\"") + exePath + "\" "
                    + (newPrivacy ? "--privacy-on" : "--privacy-off") + " &";
    system(cmd.c_str());
#endif
    mDullahan->requestExit();
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    bool privacyOn = true; // default: test with privacy protection enabled
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--privacy-off") { privacyOn = false; }
        if (a == "--privacy-on")  { privacyOn = true;  }
    }

    PrivacyTestApp* app = new PrivacyTestApp();

    if (!app->init(privacyOn))
    {
        std::cerr << "Failed to initialise PrivacyTestApp\n";
        return 1;
    }

    app->run();
    app->shutdown();

    delete app;
    return 0;
}
