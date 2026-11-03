#include "../WinMouseFix.Host/ControlPipe.h"
#include "../WinMouseFix.Host/ProductProtocol.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <vector>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::filesystem::path Repository() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(path).parent_path().parent_path().parent_path().parent_path();
}
bool Wait(const std::function<bool()>& p) {
    const auto until = GetTickCount64()+5000; while (!p()) { if (GetTickCount64() > until) return false; Sleep(1); } return true;
}
HANDLE Connect(const std::wstring& name) {
    for (unsigned i = 0; i < 20; ++i) {
        const auto pipe = CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        WaitNamedPipeW(name.c_str(),100);
    }
    return INVALID_HANDLE_VALUE;
}
void Pipes() {
    const auto environment = ProductEnvironment::Simulation(Repository());
    std::atomic<unsigned> disconnected{};
    ControlPipe server(environment,[](std::string_view request,std::uint64_t,DWORD) {
        if (request == "slow") Sleep(150); return "echo:"+std::string(request);
    },[&](std::uint64_t) { ++disconnected; });
    VERIFY(server.Start());
    ControlPipe duplicate(environment,[](std::string_view,std::uint64_t,DWORD) { return "bad"; }); VERIFY(!duplicate.Start());
    const auto stop = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    const auto a = Connect(environment.pipe),b = Connect(environment.pipe); VERIFY(a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE);
    std::string response;
    VERIFY(WriteControlFrame(a,stop,"hello") && ReadControlFrame(a,stop,response) && response == "echo:hello");
    VERIFY(WriteControlFrame(a,stop,"slow"));
    const auto began = GetTickCount64(); VERIFY(WriteControlFrame(b,stop,"fast") && ReadControlFrame(b,stop,response));
    VERIFY(response == "echo:fast" && GetTickCount64()-began < 120);
    VERIFY(ReadControlFrame(a,stop,response) && response == "echo:slow");
    VERIFY(!WriteControlFrame(a,stop,std::string(65537,'x')));
    VERIFY(WriteControlFrame(a,stop,"slow"));
    const auto timeout = ReadControlFrame(a,stop,response,10); VERIFY(timeout.nativeError == ERROR_TIMEOUT);
    CloseHandle(a); CloseHandle(b); VERIFY(Wait([&] { return disconnected >= 2; }));
    const auto invalid = Connect(environment.pipe); VERIFY(invalid != INVALID_HANDLE_VALUE);
    const auto event = CreateEventW(nullptr,TRUE,FALSE,nullptr); OVERLAPPED operation{}; operation.hEvent = event;
    std::uint32_t tooLarge = 65537; DWORD transferred{};
    const auto sent = WriteFile(invalid,&tooLarge,sizeof(tooLarge),&transferred,&operation);
    if (!sent && GetLastError() == ERROR_IO_PENDING) VERIFY(GetOverlappedResult(invalid,&operation,&transferred,TRUE));
    VERIFY(!ReadControlFrame(invalid,stop,response,1000)); CloseHandle(event); CloseHandle(invalid);
    const auto after = Connect(environment.pipe); VERIFY(after != INVALID_HANDLE_VALUE);
    VERIFY(WriteControlFrame(after,stop,"recovered") && ReadControlFrame(after,stop,response) && response == "echo:recovered");
    const auto closeStart = GetTickCount64(); server.Stop(); server.Join(); VERIFY(GetTickCount64()-closeStart < 1000);
    CloseHandle(after); CloseHandle(stop);
}
std::string Request(std::string id,std::string command = "update-config",std::string host = "run-A",unsigned protocol = 1) {
    return "{\"protocolVersion\":"+std::to_string(protocol)+",\"requestId\":\""+id+"\",\"hostRunId\":\""+host+
        "\",\"command\":\""+command+"\",\"expectedRevision\":1,\"payload\":{}}";
}
void Protocol() {
    std::atomic<unsigned> committed{},called{};
    ProductProtocol router("run-A",[&](const ProtocolRequest& request) {
        ++called; ProtocolReply reply;
        if (request.command == "update-config") {
            if (committed.load()) { reply.ok = false; reply.code = "revision-conflict"; }
            else { Sleep(25); ++committed; reply.revision = 2; }
        }
        return reply;
    });
    const auto request = Request("once"); std::array<std::string,12> replies;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < replies.size(); ++i) threads.emplace_back([&,i] { replies[i] = router.Handle(request,i+1); });
    for (auto& thread : threads) thread.join();
    VERIFY(committed == 1 && called == 1);
    for (const auto& reply : replies) VERIFY(reply == replies[0] && ParseJsonObject(reply).GetNamedBoolean(L"ok"));
    const auto reused = ParseJsonObject(router.Handle(Request("once","exit"),1)); VERIFY(!reused.GetNamedBoolean(L"ok"));
    for (const auto bad : {Request("old","get-state","run-old"),Request("protocol","get-state","run-A",2),Request(std::string(65,'x')),
        std::string("{bad}"),std::string("{\"protocolVersion\":1,\"requestId\":\"bad-utf8\",\"data\":\"")+static_cast<char>(0xff)+"\"}"}) {
        VERIFY(!ParseJsonObject(router.Handle(bad,1)).GetNamedBoolean(L"ok"));
    }
    VERIFY(called == 1);
    for (unsigned i = 0; i < 150; ++i) VERIFY(ParseJsonObject(router.Handle(Request("status-"+std::to_string(i),"get-state"),1)).GetNamedBoolean(L"ok"));
    const auto afterEviction = ParseJsonObject(router.Handle(request,1)); VERIFY(!afterEviction.GetNamedBoolean(L"ok") && committed == 1);
    std::string deep = "{\"protocolVersion\":1,\"requestId\":\"deep\",\"payload\":";
    for (unsigned i = 0; i < 20; ++i) deep += "["; deep += "0"; for (unsigned i = 0; i < 20; ++i) deep += "]"; deep += "}";
    VERIFY(!ParseJsonObject(router.Handle(deep,1)).GetNamedBoolean(L"ok"));
}
}
int RunControlPipeTests(int& total) { Pipes(); Protocol(); total += checks; return failures; }
