#include "ControlPipe.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wmf::host {
namespace {
struct Event {
    HANDLE handle{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    ~Event() { if (handle) CloseHandle(handle); }
};
Result WaitIo(HANDLE pipe,HANDLE stop,OVERLAPPED& operation,std::uint64_t until,DWORD& transferred) noexcept {
    const auto now = GetTickCount64();
    const auto timeout = now >= until ? 0 : static_cast<DWORD>((std::min)(until-now,static_cast<std::uint64_t>(MAXDWORD-1)));
    HANDLE waits[]{stop,operation.hEvent}; const auto waited = WaitForMultipleObjects(2,waits,FALSE,timeout);
    if (waited == WAIT_OBJECT_0+1) return GetOverlappedResult(pipe,&operation,&transferred,FALSE) ? Result{} : Result{Status::NativeFailure,GetLastError()};
    const auto error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : waited == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError();
    CancelIoEx(pipe,&operation); GetOverlappedResult(pipe,&operation,&transferred,TRUE);
    return {Status::NativeFailure,error};
}
Result Transfer(HANDLE pipe,HANDLE stop,void* data,DWORD bytes,bool write,std::uint64_t until) noexcept {
    Event event; if (!event.handle) return {Status::NativeFailure,GetLastError()};
    DWORD done{};
    while (done < bytes) {
        if (WaitForSingleObject(stop,0) == WAIT_OBJECT_0) return {Status::NativeFailure,ERROR_OPERATION_ABORTED};
        OVERLAPPED operation{}; operation.hEvent = event.handle; ResetEvent(event.handle); DWORD count{};
        const auto position = static_cast<std::byte*>(data)+done;
        const auto immediate = write ? WriteFile(pipe,position,bytes-done,&count,&operation) : ReadFile(pipe,position,bytes-done,&count,&operation);
        if (!immediate) {
            const auto error = GetLastError(); if (error != ERROR_IO_PENDING) return {Status::NativeFailure,error};
            const auto completed = WaitIo(pipe,stop,operation,until,count); if (!completed) return completed;
        } else if (!GetOverlappedResult(pipe,&operation,&count,FALSE)) return {Status::NativeFailure,GetLastError()};
        if (!count) return {Status::NativeFailure,ERROR_BROKEN_PIPE}; done += count;
        if (done < bytes && GetTickCount64() >= until) return {Status::NativeFailure,ERROR_TIMEOUT};
    }
    return {};
}
}
Result ReadControlFrame(HANDLE pipe,HANDLE stop,std::string& text,DWORD timeout) noexcept {
    try {
        const auto until = GetTickCount64()+timeout; std::uint32_t bytes{};
        auto result = Transfer(pipe,stop,&bytes,sizeof(bytes),false,until); if (!result) return result;
        if (!bytes || bytes > MaximumControlBytes) return {Status::OutOfRange,ERROR_BAD_LENGTH};
        std::string next(bytes,'\0'); result = Transfer(pipe,stop,next.data(),bytes,false,until);
        if (result) text = std::move(next); return result;
    } catch (...) { return {Status::AllocationFailure}; }
}
Result WriteControlFrame(HANDLE pipe,HANDLE stop,std::string_view text,DWORD timeout) noexcept {
    if (text.empty() || text.size() > MaximumControlBytes) return {Status::OutOfRange,ERROR_BAD_LENGTH};
    const auto until = GetTickCount64()+timeout; auto bytes = static_cast<std::uint32_t>(text.size());
    auto result = Transfer(pipe,stop,&bytes,sizeof(bytes),true,until); if (!result) return result;
    return Transfer(pipe,stop,const_cast<char*>(text.data()),bytes,true,until);
}
ControlPipe::ControlPipe(ProductEnvironment environment,Handler handler,Disconnected disconnected)
    : environment_(std::move(environment)),handler_(std::move(handler)),disconnected_(std::move(disconnected)) {
    if (!handler_) throw std::invalid_argument("Control pipe requires a request handler");
    if (environment_.simulation && environment_.scope.find(L".simulation.") == std::wstring::npos)
        throw std::invalid_argument("Simulation requires its own pipe namespace");
}
ControlPipe::~ControlPipe() {
    Stop(); Join(); for (auto& slot : slots_) if (slot.pipe != INVALID_HANDLE_VALUE) CloseHandle(slot.pipe);
    if (stop_) CloseHandle(stop_);
}
Result ControlPipe::Start() {
    if (started_) return {Status::InvalidState};
    try {
        stop_ = CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!stop_) return {Status::NativeFailure,GetLastError()};
        UserSecurity security(environment_.sid);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto& slot = slots_[i];
            slot.pipe = CreateNamedPipeW(environment_.pipe.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,static_cast<DWORD>(slots_.size()),MaximumControlBytes+4,MaximumControlBytes+4,3000,security.Attributes());
            if (slot.pipe == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError(); Stop(); Join();
                for (auto& allocated : slots_) if (allocated.pipe != INVALID_HANDLE_VALUE) { CloseHandle(allocated.pipe); allocated.pipe = INVALID_HANDLE_VALUE; }
                CloseHandle(stop_); stop_ = nullptr; return {Status::NativeFailure,error};
            }
        }
        started_ = true;
        for (auto& slot : slots_) slot.thread = std::thread(&ControlPipe::Listen,this,slot.pipe);
        return {};
    } catch (...) { Stop(); Join(); return {Status::AllocationFailure}; }
}
void ControlPipe::Stop() noexcept {
    if (stop_) SetEvent(stop_);
    for (const auto& slot : slots_) if (slot.pipe != INVALID_HANDLE_VALUE) CancelIoEx(slot.pipe,nullptr);
}
void ControlPipe::Join() noexcept { for (auto& slot : slots_) if (slot.thread.joinable()) slot.thread.join(); }
void ControlPipe::Listen(HANDLE pipe) noexcept {
    while (WaitForSingleObject(stop_,0) != WAIT_OBJECT_0) {
        Event event; if (!event.handle) return;
        OVERLAPPED operation{}; operation.hEvent = event.handle;
        bool connected = ConnectNamedPipe(pipe,&operation) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) {
                HANDLE waits[]{stop_,event.handle}; const auto waited = WaitForMultipleObjects(2,waits,FALSE,INFINITE);
                DWORD ignored{};
                if (waited == WAIT_OBJECT_0+1) connected = GetOverlappedResult(pipe,&operation,&ignored,FALSE) != FALSE;
                else { CancelIoEx(pipe,&operation); GetOverlappedResult(pipe,&operation,&ignored,TRUE); break; }
            }
        }
        if (!connected) { DisconnectNamedPipe(pipe); continue; }
        ULONG clientPid{},clientSession{};
        if (!GetNamedPipeClientProcessId(pipe,&clientPid) || !GetNamedPipeClientSessionId(pipe,&clientSession) || clientSession != environment_.session) {
            DisconnectNamedPipe(pipe); continue;
        }
        const auto connection = ++serial_; ++clients_;
        try {
            std::string request;
            while (ReadControlFrame(pipe,stop_,request)) {
                const auto response = handler_(request,connection,clientPid);
                if (!WriteControlFrame(pipe,stop_,response)) break;
            }
        } catch (...) { /* Isolate a malformed connection from other clients and input. */ }
        if (disconnected_) { try { disconnected_(connection); } catch (...) {} }
        --clients_; DisconnectNamedPipe(pipe);
    }
}
}
