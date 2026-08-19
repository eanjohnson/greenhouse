// Greenhouse Host shell -- minimal CoreApplication (no XAML) UWP entry point.
// On launch it runs the P0 spike sequence (GREENHOUSE.md §10 / Appendix H.3 #4)
// -- the same VirtualAlloc(RW) -> write x64 stub -> VirtualProtect(RX) -> call
// sequence already validated off-console under Wine -- and writes the result
// to LocalFolder so it can be pulled back over WDP without a live debugger
// attached. This is the one question Wine cannot answer: whether the Xbox
// Dev Mode UWP app-container sandbox permits executable-memory allocation.
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.applicationmodel.activation.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.storage.h>
#include <winrt/windows.ui.core.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::Storage;

static void WriteDiagWin32(const std::string& log);  // defined below

// x64 machine code for: mov eax, 42 ; ret
static const uint8_t kStub[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };

static std::string RunSpike()
{
    void* mem = VirtualAlloc(nullptr, sizeof(kStub), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem)
        return "FAIL VirtualAlloc error=" + std::to_string(GetLastError());

    memcpy(mem, kStub, sizeof(kStub));

    DWORD oldProtect = 0;
    if (!VirtualProtect(mem, sizeof(kStub), PAGE_EXECUTE_READ, &oldProtect))
    {
        std::string err = "FAIL VirtualProtect error=" + std::to_string(GetLastError());
        VirtualFree(mem, 0, MEM_RELEASE);
        return err;
    }

    using StubFn = int(*)(void);
    StubFn fn = reinterpret_cast<StubFn>(mem);
    // If DEP/W^X in the app-container blocks execution, this call faults with
    // 0xC0000005 and never returns -- the persisted "about_to_call" marker
    // (plus a fresh crash dump) is then the linchpin answer: NO.
    WriteDiagWin32("STAGE=about_to_call_exec_mem");
    int result = fn();

    VirtualFree(mem, 0, MEM_RELEASE);

    if (result == 42)
        return "PASS result=42";
    return "FAIL unexpected result=" + std::to_string(result);
}

// Plain-Win32 file write -- no WinRT, so it works even when the WinRT
// apartment / activation is exactly what is failing. Writes into the app
// container's temp dir (GetTempPathW), which WDP exposes for retrieval.
static void WriteDiagWin32(const std::string& log)
{
    // Append to the app's LocalState by absolute path (known from WDP). This
    // folder is app-container-writable AND retrievable over WDP, unlike the
    // GetTempPathW location which WDP does not expose.
    static const wchar_t* kPath =
        L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
        L"Greenhouse.P0Spike_mgw46vk3ppz9w\\LocalState\\gh_diag.txt";
    HANDLE h = CreateFileW(kPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string line = log + "\r\n";
        DWORD written = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(h);
    }
}

static void WriteResult(const std::string& log)
{
    // Always persist via plain Win32 first -- guaranteed path.
    WriteDiagWin32(log);
    try
    {
        auto folder = ApplicationData::Current().LocalFolder();
        std::wstring path(folder.Path());
        path += L"\\p0_spike_result.txt";
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        f << log << std::endl;
    }
    catch (...)
    {
        // ApplicationData unavailable -- Win32 copy above still has it.
    }
}

struct App : implements<App, IFrameworkView, IFrameworkViewSource>
{
    bool m_exit = false;

    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void SetWindow(CoreWindow const& window)
    {
        window.Closed([&](auto&&, auto&&) { m_exit = true; });
    }

    void Run()
    {
        // Spike already ran in main(); just satisfy the view lifecycle and
        // exit so WDP process-exit polling sees a clean shutdown.
        CoreApplication::Exit();
    }
};

static std::string HresultToHex(winrt::hresult h)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)(int32_t)h);
    return buf;
}

int main()
{
    WriteDiagWin32("STAGE=entered_main");

    // ---- THE LINCHPIN ----
    // Pure Win32, no WinRT/apartment dependency. Answers the one question
    // Wine cannot: does the Xbox Dev Mode app-container permit allocating,
    // marking executable, and calling dynamically-generated code? Runs first
    // so the answer is captured independent of any CoreApplication quirks.
    {
        std::string spikeResult = RunSpike();
        WriteResult(spikeResult);               // -> gh_diag.txt + LocalFolder
        WriteDiagWin32("SPIKE_RESULT=" + spikeResult);
    }

    // ---- Activation handshake (secondary) ----
    // CoreApplication requires its first access to come from the MTA; join it
    // explicitly (cppwinrt's implicit-MTA fallback is not sufficient on Xbox).
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WriteDiagWin32("STAGE=mta_initialized");
    }
    catch (winrt::hresult_error const& e)
    {
        WriteDiagWin32(std::string("STAGE=mta_init_failed code=") +
                       HresultToHex(e.code()) + " " + winrt::to_string(e.message()));
    }
    try
    {
        CoreApplication::Run(App());
        WriteDiagWin32("STAGE=run_returned_ok");
    }
    catch (winrt::hresult_error const& e)
    {
        std::string msg = "STAGE=hresult_error code=";
        msg += HresultToHex(e.code());
        msg += " msg=";
        msg += winrt::to_string(e.message());
        WriteDiagWin32(msg);
    }
    catch (std::exception const& e)
    {
        WriteDiagWin32(std::string("STAGE=std_exception what=") + e.what());
    }
    catch (...)
    {
        WriteDiagWin32("STAGE=unknown_exception");
    }
    return 0;
}
