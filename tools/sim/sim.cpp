#include "sim.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace casement::sim {

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            out += c;
            slashes = 0;
        }
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

/// This process's environment with `extra` set.
std::wstring environment(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::vector<std::wstring> vars;
    wchar_t* block = GetEnvironmentStringsW();
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) vars.emplace_back(p);
    FreeEnvironmentStringsW(block);
    for (const auto& [k, v] : extra) {
        std::wstring key = widen(k) + L"=";
        std::erase_if(vars, [&](const std::wstring& e) { return _wcsnicmp(e.c_str(), key.c_str(), key.size()) == 0; });
        vars.push_back(key + widen(v));
    }
    std::wstring out;
    for (const auto& v : vars) {
        out += v;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

}  // namespace

HostSurface::~HostSurface() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

std::uint32_t HostSurface::pixel(std::uint32_t x, std::uint32_t y) const {
    const std::uint8_t* p = view_ + std::size_t{y} * stride + std::size_t{x} * 4;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t{p[3]} << 24);
}

std::unique_ptr<Casement> Casement::spawn(const std::wstring& program, const std::vector<std::wstring>& args,
                                          bool echo_stderr, const std::vector<std::pair<std::string, std::string>>& env) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read, in_write, out_read, out_write, err_read, err_write;
    if (!CreatePipe(&in_read, &in_write, &sa, 0) || !CreatePipe(&out_read, &out_write, &sa, 0) ||
        !CreatePipe(&err_read, &err_write, &sa, 0))
        return nullptr;
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote(program);
    for (const auto& a : args) cmd += L" " + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    PROCESS_INFORMATION pi{};
    std::wstring envblock = environment(env);
    BOOL ok = CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, envblock.data(), nullptr, &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (!ok) {
        std::fprintf(stderr, "can't start the plugin (error %lu)\n", GetLastError());
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(err_read);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    std::unique_ptr<Casement> c(new Casement());
    c->process_ = pi.hProcess;
    c->stdin_ = in_write;
    c->stdout_ = out_read;
    c->stderr_ = err_read;
    c->out_thread_ = std::thread([p = c.get()] { p->read_stdout(); });
    c->err_thread_ = std::thread([p = c.get(), echo_stderr] { p->read_stderr(echo_stderr); });
    return c;
}

Casement::~Casement() {
    close_stdin();
    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 9);
        WaitForSingleObject(process_, 5000);
    }
    if (out_thread_.joinable()) out_thread_.join();
    if (err_thread_.joinable()) err_thread_.join();
    if (stdout_) CloseHandle(stdout_);
    if (stderr_) CloseHandle(stderr_);
    if (process_) CloseHandle(process_);
}

void Casement::read_stdout() {
    for (;;) {
        std::uint8_t len[4];
        DWORD got = 0, n = 0;
        while (got < 4) {
            if (!ReadFile(stdout_, len + got, 4 - got, &n, nullptr) || n == 0) goto done;
            got += n;
        }
        std::size_t size = len[0] | (len[1] << 8) | (len[2] << 16) | (std::size_t{len[3]} << 24);
        std::string body(size, '\0');
        std::size_t have = 0;
        while (have < size) {
            if (!ReadFile(stdout_, body.data() + have, static_cast<DWORD>(size - have), &n, nullptr) || n == 0)
                goto done;
            have += n;
        }
        json msg = json::parse(body, nullptr, false);
        std::lock_guard g(lock_);
        messages_.push_back(msg.is_discarded() ? json{{"type", "undecodable"}} : std::move(msg));
        ready_.notify_all();
    }
done:
    std::lock_guard g(lock_);
    closed_ = true;
    ready_.notify_all();
}

void Casement::read_stderr(bool echo) {
    std::string line;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(stderr_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (echo) std::fprintf(stderr, "plugin: %s\n", line.c_str());
                std::lock_guard g(lock_);
                stderr_lines_.push_back(line);
                line.clear();
            } else if (buf[i] != '\r') {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) {
        std::lock_guard g(lock_);
        stderr_lines_.push_back(line);
    }
}

bool Casement::send(const json& msg) {
    if (!stdin_) return false;
    std::string body = msg.dump();
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    std::string frame{static_cast<char>(n), static_cast<char>(n >> 8), static_cast<char>(n >> 16),
                      static_cast<char>(n >> 24)};
    frame += body;
    DWORD written = 0;
    return WriteFile(stdin_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
           written == frame.size();
}

std::optional<json> Casement::recv(milliseconds timeout) {
    std::unique_lock g(lock_);
    if (!ready_.wait_for(g, timeout, [&] { return !messages_.empty() || closed_; })) return std::nullopt;
    if (messages_.empty()) return std::nullopt;
    json m = std::move(messages_.front());
    messages_.pop_front();
    return m;
}

std::optional<json> Casement::expect_type(milliseconds timeout, std::string_view type) {
    return expect(timeout, [&](const json& m) -> std::optional<json> {
        if (m.value("type", "") == type) return m;
        return std::nullopt;
    });
}

std::optional<json> Casement::hello(int protocol, milliseconds timeout) {
    send(json{{"type", "hello"}, {"protocol", protocol}, {"app", "Casement test"}});
    return expect_type(timeout, "hello");
}

void Casement::connect(std::string_view address, std::string_view password, std::string_view username,
                       const json& extra) {
    json m{{"type", "connect"}, {"address", address}, {"username", username}, {"password", password},
           {"title", "Test"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) m[it.key()] = it.value();
    send(m);
}

void Casement::close_stdin() {
    if (stdin_) {
        CloseHandle(stdin_);
        stdin_ = nullptr;
    }
}

std::optional<DWORD> Casement::wait(milliseconds timeout) {
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return code;
}

std::vector<std::string> Casement::stderr_lines() {
    std::lock_guard g(lock_);
    return stderr_lines_;
}

std::pair<std::unique_ptr<HostSurface>, json> Casement::create_surface(std::uint32_t id, std::uint32_t width,
                                                                       std::uint32_t height, std::uint32_t pad) {
    std::uint32_t stride = width * 4 + pad;
    std::uint64_t size = std::uint64_t{stride} * height;
    std::unique_ptr<HostSurface> s(new HostSurface());
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                     static_cast<DWORD>(size), nullptr);
    if (!s->mapping_) return {nullptr, json()};
    s->view_ = static_cast<std::uint8_t*>(MapViewOfFile(s->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    HANDLE theirs = nullptr;
    if (!s->view_ ||
        !DuplicateHandle(GetCurrentProcess(), s->mapping_, process_, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return {nullptr, json()};
    json msg{{"type", "surface"},
             {"id", id},
             {"handle", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(theirs))},
             {"width", width},
             {"height", height},
             {"stride", stride},
             {"size", size}};
    return {std::move(s), msg};
}

bool write_ppm(const std::wstring& path, const std::uint8_t* bgrx, std::uint32_t width, std::uint32_t height,
               std::uint32_t stride) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::vector<std::uint8_t> row(std::size_t{width} * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* p = bgrx + std::size_t{y} * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            row[x * 3] = p[x * 4 + 2];
            row[x * 3 + 1] = p[x * 4 + 1];
            row[x * 3 + 2] = p[x * 4];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

}  // namespace casement::sim
