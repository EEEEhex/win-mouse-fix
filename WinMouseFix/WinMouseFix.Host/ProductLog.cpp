#include "ProductLog.h"
#include "DiagnosticText.h"
#include "Report.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace wmf::host {
namespace {
std::uint32_t Crc(std::string_view bytes) {
    std::uint32_t crc = UINT32_MAX;
    for (const auto byte : bytes) { crc ^= static_cast<unsigned char>(byte); for (int i = 0; i < 8; ++i) crc = (crc>>1) ^ (0xedb88320u & (0u-(crc&1))); }
    return ~crc;
}
void U16(std::ostream& out,std::uint16_t value) { out.put(static_cast<char>(value)); out.put(static_cast<char>(value>>8)); }
void U32(std::ostream& out,std::uint32_t value) { U16(out,static_cast<std::uint16_t>(value)); U16(out,static_cast<std::uint16_t>(value>>16)); }
void Zip(const std::filesystem::path& path,const std::vector<std::pair<std::string,std::string>>& files) {
    std::ofstream out(path,std::ios::binary); out.exceptions(std::ios::badbit|std::ios::failbit);
    struct Entry { std::string name; std::uint32_t crc,size,offset; }; std::vector<Entry> entries;
    for (const auto& [name,bytes] : files) {
        Entry entry{name,Crc(bytes),static_cast<std::uint32_t>(bytes.size()),static_cast<std::uint32_t>(out.tellp())};
        U32(out,0x04034b50); U16(out,20); U16(out,0x0800); U16(out,0); U16(out,0); U16(out,0x21);
        U32(out,entry.crc); U32(out,entry.size); U32(out,entry.size); U16(out,static_cast<std::uint16_t>(name.size())); U16(out,0);
        out.write(name.data(),name.size()); out.write(bytes.data(),bytes.size()); entries.push_back(entry);
    }
    const auto start = static_cast<std::uint32_t>(out.tellp());
    for (const auto& e : entries) {
        U32(out,0x02014b50); U16(out,20); U16(out,20); U16(out,0x0800); U16(out,0); U16(out,0); U16(out,0x21);
        U32(out,e.crc); U32(out,e.size); U32(out,e.size); U16(out,static_cast<std::uint16_t>(e.name.size()));
        U16(out,0); U16(out,0); U16(out,0); U16(out,0); U32(out,0); U32(out,e.offset); out.write(e.name.data(),e.name.size());
    }
    const auto end = static_cast<std::uint32_t>(out.tellp());
    U32(out,0x06054b50); U16(out,0); U16(out,0); U16(out,static_cast<std::uint16_t>(entries.size())); U16(out,static_cast<std::uint16_t>(entries.size()));
    U32(out,end-start); U32(out,start); U16(out,0); out.flush();
}
}
ProductLog::ProductLog(std::filesystem::path directory,ProductRuntime& runtime,std::uint64_t frequency,bool simulation)
    : directory_(std::move(directory)),runtime_(runtime),frequency_(frequency),simulation_(simulation),metrics_(frequency),thread_(&ProductLog::Work,this) {}
ProductLog::~ProductLog() { stop_ = true; if (thread_.joinable()) thread_.join(); }
void ProductLog::Record(std::string event,std::string message,std::uint64_t revision) noexcept {
    try {
        const auto bound = [](std::string& value,std::size_t maximum) {
            if (value.size() <= maximum) return;
            while (maximum && (static_cast<unsigned char>(value[maximum])&0xc0) == 0x80) --maximum;
            value.resize(maximum);
        };
        bound(event,128); bound(message,4096);
        std::string line = "{\"utc\":"+Json(UtcNow())+",\"kind\":"+Json(event)+",\"message\":"+Json(message)+",\"revision\":"+std::to_string(revision)+"}";
        std::lock_guard lock(controlMutex_); if (control_.size() == 128) { ++lostControl_; return; } control_.push_back(std::move(line));
    } catch (...) { ++lostControl_; }
}
void ProductLog::DetailFor(std::uint64_t milliseconds) noexcept { detailUntil_ = GetTickCount64()+(std::min)(milliseconds,60000ull); }
void ProductLog::Write(std::string_view name,std::string_view line,std::uintmax_t limit) {
    std::lock_guard lock(filesMutex_); std::filesystem::create_directories(directory_);
    const auto path = directory_/std::string(name);
    if (std::filesystem::exists(path) && std::filesystem::file_size(path)+line.size()+1 > limit) {
        const auto numbered = [&](int i) { return directory_/(std::string(name)+"."+std::to_string(i)); };
        std::filesystem::remove(numbered(3));
        for (int i = 2; i >= 1; --i) if (std::filesystem::exists(numbered(i))) std::filesystem::rename(numbered(i),numbered(i+1));
        std::filesystem::rename(path,numbered(1));
    }
    std::ofstream out(path,std::ios::binary|std::ios::app); out.exceptions(std::ios::badbit|std::ios::failbit); out << line << '\n'; out.flush();
}
void ProductLog::Trace(const ProductTrace& t) {
    { std::lock_guard lock(metricsMutex_); metrics_.Observe(t); }
    const bool detailed = GetTickCount64() < detailUntil_;
    if (!detailed && t.result && (t.kind == ProductTraceKind::Input || t.kind == ProductTraceKind::Frame)) return;
    std::ostringstream text; text << "{\"kind\":" << static_cast<unsigned>(t.kind) << ",\"action\":" << static_cast<unsigned>(t.action)
        << ",\"revision\":" << t.input.revision << ",\"session\":" << t.input.session << ",\"result\":" << JsonResult(t.result);
    if (t.wheelLedger) text << ",\"wheelLedgerMm\":{\"acceptedX\":" << t.wheelAcceptedX << ",\"acceptedY\":" << t.wheelAcceptedY
        << ",\"discardedX\":" << t.wheelDiscardedX << ",\"discardedY\":" << t.wheelDiscardedY << ",\"emittedX\":" << t.wheelEmittedX << ",\"emittedY\":" << t.wheelEmittedY << '}';
    if (detailed) {
        text << ",\"earliestQpc\":" << t.input.firstQpc << ",\"latestQpc\":" << t.input.lastQpc << ",\"processQpc\":" << t.processStarted
            << ",\"inputKind\":" << static_cast<unsigned>(t.input.kind) << ",\"inputSource\":" << static_cast<unsigned>(t.input.source)
            << ",\"device\":" << t.input.device << ",\"button\":" << static_cast<unsigned>(t.input.button) << ",\"generation\":" << t.input.generation
            << ",\"samples\":" << t.input.samples << ",\"wheelX\":" << t.input.wheelX << ",\"wheelY\":" << t.input.wheelY
            << ",\"relativeX\":" << t.input.dx << ",\"relativeY\":" << t.input.dy
            << ",\"submitQpc\":" << t.submitStarted << ",\"returnQpc\":" << t.returned << ",\"phase\":" << static_cast<unsigned>(t.frame.phase)
            << ",\"sequence\":" << t.frame.sequence << ",\"count\":" << t.frame.count << ",\"boundary\":" << t.boundary << ",\"stationary\":" << t.stationary;
        text << ",\"cancelReason\":" << static_cast<unsigned>(t.input.reason);
        if (t.kind == ProductTraceKind::Frame || t.kind == ProductTraceKind::Cleanup) {
            text << ",\"contactsMm\":[";
            for (unsigned i = 0; i < t.frame.count && i < 5; ++i) { if (i) text << ','; const auto& point = t.frame.contacts[i]; text << "{\"id\":" << point.id << ",\"x\":" << point.xMm << ",\"y\":" << point.yMm << '}'; }
            text << ']';
        }
        text << "}"; Write("detail.jsonl",text.str(),2*1024*1024);
    } else if (t.kind != ProductTraceKind::Input && t.kind != ProductTraceKind::Frame && (t.input.session || t.kind == ProductTraceKind::Initialization)) {
        text << ",\"cancelReason\":" << static_cast<unsigned>(t.input.reason) << "}"; Write("resident.jsonl",text.str(),1024*1024);
    } else if (!t.result) { text << "}"; Write("resident.jsonl",text.str(),1024*1024); }
}
void ProductLog::Work() noexcept {
    bool done{}; while (!done) {
        bool work{};
        try {
            ProductTrace trace; bool any{}; while (runtime_.ReadTrace(trace)) { any = true; Trace(trace); }
            std::deque<std::string> messages; { std::lock_guard lock(controlMutex_); messages.swap(control_); }
            for (const auto& message : messages) Write("resident.jsonl",message,1024*1024);
            work = any || !messages.empty();
            done = stop_ && !any && messages.empty();
        } catch (...) { error_ = ERROR_WRITE_FAULT; if (stop_) done = true; }
        if (!done) Sleep(work ? 10 : 250);
    }
}
std::string ProductLog::Summary() {
    std::lock_guard lock(metricsMutex_); std::ostringstream json;
    json << "{\"simulation\":" << (simulation_ ? "true" : "false") << ",\"qpcFrequency\":" << frequency_ << ",\"lostTraceRecords\":" << runtime_.LostTraces()
        << ",\"lostControlRecords\":" << lostControl_.load() << ",\"logError\":" << Error() << ",\"timing\":" << metrics_.Json() << '}'; return json.str();
}
std::filesystem::path ProductLog::Export(std::string_view status,std::string_view configuration) {
    std::vector<std::pair<std::string,std::string>> files{{"status.json",std::string(status)},{"config.json",std::string(configuration)},{"timing.json",Summary()}};
    {
        std::lock_guard lock(filesMutex_);
        for (const auto base : {"resident.jsonl","detail.jsonl"}) for (int suffix = 0; suffix <= 3; ++suffix) {
            const auto name = std::string(base)+(suffix ? "."+std::to_string(suffix) : ""); const auto path = directory_/name;
            if (!std::filesystem::exists(path) || std::filesystem::file_size(path) > 2*1024*1024) continue;
            std::ifstream in(path,std::ios::binary); files.emplace_back(name,std::string(std::istreambuf_iterator<char>(in),{}));
        }
    }
    const auto output = directory_.parent_path()/L"diagnostics"; std::filesystem::create_directories(output);
    const auto path = output/(L"WinMouseFix-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".zip"); Zip(path,files); return path;
}
}
