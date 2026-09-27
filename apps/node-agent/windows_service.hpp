#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <string>
#include <atomic>
namespace asterion::node {
inline SERVICE_STATUS_HANDLE scm_handle = nullptr;
inline SERVICE_STATUS scm_status{};
inline std::function<int()> scm_run;
inline std::function<void()> scm_stop;
inline std::wstring scm_name;
inline void publish(DWORD state) {
    scm_status.dwServiceType=SERVICE_WIN32_OWN_PROCESS; scm_status.dwCurrentState=state;
    scm_status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    SetServiceStatus(scm_handle,&scm_status);
}
inline void WINAPI control(DWORD code) { if(code==SERVICE_CONTROL_STOP||code==SERVICE_CONTROL_SHUTDOWN) scm_stop(); }
inline void WINAPI entry(DWORD,wchar_t**) {
    scm_handle=RegisterServiceCtrlHandlerW(scm_name.c_str(),control); if(!scm_handle) return;
    publish(SERVICE_RUNNING); const auto result=scm_run(); scm_status.dwWin32ExitCode=result?ERROR_SERVICE_SPECIFIC_ERROR:NO_ERROR; scm_status.dwServiceSpecificExitCode=static_cast<DWORD>(result); publish(SERVICE_STOPPED);
}
inline int run_service(const std::string& name,std::function<int()> run,std::function<void()> stop) {
    scm_name.assign(name.begin(),name.end()); scm_run=std::move(run); scm_stop=std::move(stop);
    SERVICE_TABLE_ENTRYW table[]{{scm_name.data(),entry},{nullptr,nullptr}};
    return StartServiceCtrlDispatcherW(table)?(scm_status.dwWin32ExitCode?1:0):1;
}
}
#endif
