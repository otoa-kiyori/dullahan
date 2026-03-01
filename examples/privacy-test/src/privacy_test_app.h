/**
    @brief  Dullahan Privacy Test application

            Renders a comprehensive fingerprint-probe test page inside a Dullahan
            CEF instance.  An ImGui panel on the left shows per-test PASS/FAIL
            results captured from structured console.log messages emitted by the
            HTML page.  The browser render fills the right side of the window.

            Launch flags:
              --privacy-on   (default) initialises Dullahan with protect_privacy = true
              --privacy-off            initialises Dullahan with protect_privacy = false

    @author Callum Prentice / Otoa Kiyori - 2025
*/

#pragma once

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <glad/glad.h>
#if defined(WIN32)
#  undef APIENTRY
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3.h>
#  include <GLFW/glfw3native.h>
#  include <commctrl.h>
#else
#  define GLFW_INCLUDE_NONE
#  include <GLFW/glfw3.h>
#endif

#include <string>
#include <vector>
#include <mutex>

class dullahan;

// One row in the results table — populated when the HTML page sends
// a structured console.log message: PRIV|Category|Key|Expected|Actual|PASS/FAIL
struct TestResult
{
    std::string category;
    std::string key;
    std::string label;
    std::string expected;
    std::string actual;
    bool        pass    = false;
    bool        pending = true;  // true until the async resolve arrives
};

class PrivacyTestApp
{
public:
    PrivacyTestApp();

    bool init(bool privacyEnabled);
    bool run();
    void shutdown();

    // GLFW static → instance trampolines
    void resizeCallback(int w, int h);
    void mouseButtonCallback(int button, int action, int mods);
    void mouseMoveCallback(double xpos, double ypos);
    void mouseScrollCallback(double xoffset, double yoffset);

private:
    // ── Dullahan ────────────────────────────────────────────────────────────
    dullahan*   mDullahan     = nullptr;
    bool        mPrivacyOn    = true;
    std::string mTestPageUrl;
    int         mBrowserW     = 1024;
    int         mBrowserH     = 768;

    // ── GLFW / OpenGL ───────────────────────────────────────────────────────
    GLFWwindow* mWindow       = nullptr;
    GLuint      mTextureId    = 0;
    bool        mShouldClose  = false;

    // ── Test results (written from CEF callback, read from main thread) ─────
    std::vector<TestResult> mResults;
    std::mutex              mResultsMtx;
    int                     mPassCount = 0;
    int                     mFailCount = 0;
    int                     mTotalCount = 0;

    // ── ImGui layout ────────────────────────────────────────────────────────
    static constexpr int kLeftPanelW  = 390;
    static constexpr int kWindowW     = 1600;
    static constexpr int kWindowH     = 900;

    // Browser panel rect — set each frame so mouse callbacks can use it
    ImVec2      mBrowserRectMin = { 0, 0 };
    ImVec2      mBrowserRectMax = { 0, 0 };

    // ── Dullahan callbacks ──────────────────────────────────────────────────
    void onPageChanged(const unsigned char* pixels, int x, int y, int w, int h);
    void onConsoleMessage(const std::string& msg, const std::string& src, int line);
    void onRequestExit();

    // ── Helpers ─────────────────────────────────────────────────────────────
    void        initGL();
    void        initImGui();
    void        updateUI();
    void        drawTestPanel();
    void        relaunch(bool newPrivacyState);
    std::string buildTestPageUrl();
    std::string executableDir();
    void        recountResults();

    // ── GLFW static trampolines ─────────────────────────────────────────────
    static void resizeCB(GLFWwindow* w, int width, int height)
    { static_cast<PrivacyTestApp*>(glfwGetWindowUserPointer(w))->resizeCallback(width, height); }
    static void mouseButtonCB(GLFWwindow* w, int btn, int act, int mods)
    { static_cast<PrivacyTestApp*>(glfwGetWindowUserPointer(w))->mouseButtonCallback(btn, act, mods); }
    static void mouseMoveCB(GLFWwindow* w, double x, double y)
    { static_cast<PrivacyTestApp*>(glfwGetWindowUserPointer(w))->mouseMoveCallback(x, y); }
    static void mouseScrollCB(GLFWwindow* w, double dx, double dy)
    { static_cast<PrivacyTestApp*>(glfwGetWindowUserPointer(w))->mouseScrollCallback(dx, dy); }

#if defined(WIN32)
    static LRESULT CALLBACK keySubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR uIdSubclass,
                                            DWORD_PTR dwRefData);
#endif
};
