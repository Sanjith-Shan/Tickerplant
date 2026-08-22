#pragma once

#include "tick/affinity.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/types.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

// What the machine was, recorded next to what the machine did.
//
// A latency number without a machine attached is not a result, it is a rumour.
// p99 of 380 ns means one thing on an isolated, pinned, governor pinned Linux
// box with hyperthreading off, and something entirely different on a laptop
// with a browser open. Both numbers can be honestly measured and only one of
// them supports a claim. So every results file this project writes embeds the
// output of detect_box, and every table prints one_line above it.
//
// THE RULE THIS FILE IS BUILT AROUND
//
// Anything that cannot be determined comes out as the literal string "unknown".
// Never a default that looks plausible. A governor field reading "performance"
// because that is the common case, on a box where the file could not be read,
// is a fabricated experimental condition, and it is the kind of thing that
// turns a merely weak result into a dishonest one. "unknown" is a worse looking
// field and a better one. The same applies to the booleans, which default to
// false and are only set true when a real file said so, so "not isolated" is
// the reported state whenever isolation could not be confirmed.
//
// The optimisation level is the one thing the preprocessor genuinely cannot
// see. NDEBUG says assertions are off, which is correlated with a release build
// and is not the same statement as -O3. So build_type reports what CMake passed
// in through TICK_BUILD_TYPE if it was passed, and otherwise reports only what
// NDEBUG actually tells us, qualified so a reader is not misled.

namespace tick {

struct BoxInfo {
    std::string cpu_model  = "unknown";
    std::string os         = "unknown";
    std::string kernel     = "unknown";
    std::string compiler   = "unknown";
    std::string build_type = "unknown";
    int         cores      = 0;
    bool        pinned     = false;
    bool        isolated   = false;
    bool        nohz_full  = false;
    std::string governor   = "unknown";
    std::string nic        = "unknown";
    std::string notes;

    // The one minute load average at the moment the run started.
    //
    // This is here because of a lesson from a sibling project rather than
    // because it is interesting. A benchmark taken at a load average of fifty
    // on a twelve thread machine is not a benchmark, and the only thing worse
    // than taking one is taking one without knowing. Recording it makes the
    // question checkable afterwards instead of remembered.
    double load_1min  = -1.0;
    double load_5min  = -1.0;
    double load_15min = -1.0;

    // True when the machine was quiet enough for the numbers to mean anything,
    // which is a judgement and is therefore spelled out rather than implied.
    // One runnable thread per core is already generous.
    [[nodiscard]] bool quiet() const noexcept {
        return load_1min >= 0.0 && cores > 0 &&
               load_1min < static_cast<double>(cores) * 0.5;
    }

    // e.g. "Apple M3 Pro, macOS 26.5.1, Apple clang 17.0.0, -O3, not pinned"
    [[nodiscard]] std::string one_line() const {
        std::string s = cpu_model;
        s += ", "; s += os;
        s += ", "; s += compiler;
        s += ", "; s += build_type;
        s += ", "; s += (pinned ? "pinned" : "not pinned");
        if (isolated)  s += ", isolcpus";
        if (nohz_full) s += ", nohz_full";
        if (governor != "unknown") { s += ", governor "; s += governor; }
        if (load_1min >= 0.0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ", load %.2f", load_1min);
            s += buf;
            if (!quiet()) s += " (LOADED, timings here are not trustworthy)";
        }
        return s;
    }

    [[nodiscard]] std::string to_json() const {
        std::string s = "{";
        s += "\"cpu_model\":\"";  s += esc(cpu_model);  s += "\",";
        s += "\"os\":\"";         s += esc(os);         s += "\",";
        s += "\"kernel\":\"";     s += esc(kernel);     s += "\",";
        s += "\"compiler\":\"";   s += esc(compiler);   s += "\",";
        s += "\"build_type\":\""; s += esc(build_type); s += "\",";
        s += "\"cores\":";        s += std::to_string(cores); s += ",";
        s += "\"pinned\":";       s += (pinned    ? "true" : "false"); s += ",";
        s += "\"isolated\":";     s += (isolated  ? "true" : "false"); s += ",";
        s += "\"nohz_full\":";    s += (nohz_full ? "true" : "false"); s += ",";
        s += "\"governor\":\"";   s += esc(governor);   s += "\",";
        s += "\"nic\":\"";        s += esc(nic);        s += "\",";
        s += "\"notes\":\"";      s += esc(notes);      s += "\",";
        s += "\"load_1min\":";    s += std::to_string(load_1min);  s += ",";
        s += "\"load_5min\":";    s += std::to_string(load_5min);  s += ",";
        s += "\"load_15min\":";   s += std::to_string(load_15min); s += ",";
        s += "\"quiet\":";        s += (quiet() ? "true" : "false");
        s += "}";
        return s;
    }

private:
    static std::string esc(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
            }
        }
        return out;
    }
};

namespace box_detail {

inline std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Read a whole small file. Returns false if it is not there, which is the
// normal answer for a cpufreq path on a machine with no cpufreq driver, and is
// how a field ends up as "unknown".
inline bool read_file(const char* path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = trim(ss.str());
    return !out.empty();
}

// First line of /proc/cpuinfo matching a key, e.g. "model name".
inline bool proc_cpuinfo_field(const char* key, std::string& out) {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return false;
    std::string line;
    const std::string k(key);
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (trim(line.substr(0, colon)) != k) continue;
        out = trim(line.substr(colon + 1));
        return !out.empty();
    }
    return false;
}

#if defined(__APPLE__)
inline bool sysctl_string(const char* name, std::string& out) {
    std::size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return false;
    std::string buf(len, '\0');
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return false;
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    out = trim(buf);
    return !out.empty();
}

inline bool sysctl_int(const char* name, int& out) {
    int v = 0;
    std::size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return false;
    out = v;
    return true;
}
#endif

// Compile time identity of the compiler that produced this binary. This one is
// knowable exactly, so it is not allowed to be "unknown" on any supported
// toolchain.
inline std::string compiler_string() {
#if defined(__apple_build_version__)
    return std::string("Apple clang ") + __clang_version__;
#elif defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

// See the header comment. TICK_BUILD_TYPE is the only source that can actually
// name an optimisation level, because -O3 leaves no preprocessor trace.
inline std::string build_type_string() {
#if defined(TICK_BUILD_TYPE)
    return std::string(TICK_BUILD_TYPE);
#elif defined(NDEBUG)
    return "NDEBUG, optimisation level not reported by the build";
#else
    return "assertions on, optimisation level not reported by the build";
#endif
}

#if defined(__linux__)
// Look for a kernel boot parameter in /proc/cmdline. Matches the parameter as a
// whole token or as the head of a key=value token, so isolcpus=1-7 is found by
// asking for "isolcpus" and a substring of some unrelated parameter is not.
inline bool cmdline_has(const char* key, std::string* value = nullptr) {
    std::string cmdline;
    if (!read_file("/proc/cmdline", cmdline)) return false;
    std::istringstream ss(cmdline);
    std::string tok;
    const std::string k(key);
    while (ss >> tok) {
        if (tok == k) { if (value) value->clear(); return true; }
        if (tok.size() > k.size() && tok.compare(0, k.size(), k) == 0 &&
            tok[k.size()] == '=') {
            if (value) *value = tok.substr(k.size() + 1);
            return true;
        }
    }
    return false;
}

// The first network interface that is up and is not loopback, with its driver
// name where sysfs exposes one. This is a hint for the results file rather than
// a claim about which interface carried the feed, which only the benchmark
// runner knows, so it is a starting value a caller is free to overwrite.
inline std::string detect_nic() {
    std::ifstream dir("/proc/net/dev");
    if (!dir) return "unknown";
    std::string line;
    std::getline(dir, line);  // two header lines
    std::getline(dir, line);
    while (std::getline(dir, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = trim(line.substr(0, colon));
        if (name.empty() || name == "lo") continue;
        std::string state;
        if (!read_file(("/sys/class/net/" + name + "/operstate").c_str(), state)) continue;
        if (state != "up") continue;
        char drv[512] = {};
        const ssize_t n = ::readlink(("/sys/class/net/" + name + "/device/driver").c_str(),
                                     drv, sizeof(drv) - 1);
        if (n > 0) {
            std::string path(drv, static_cast<std::size_t>(n));
            const auto slash = path.find_last_of('/');
            return name + " (" + (slash == std::string::npos ? path
                                                             : path.substr(slash + 1)) + ")";
        }
        return name;
    }
    return "unknown";
}
#endif

} // namespace box_detail

// Fill in everything the platform will actually tell us, and leave the rest as
// "unknown". Cheap enough to call once at startup and not cheap enough to call
// in a loop, since it reads files.
// The one, five, and fifteen minute load averages. Portable across macOS and
// Linux through getloadavg, which both provide.
inline void fill_load(BoxInfo& b) noexcept {
    double la[3] = {-1.0, -1.0, -1.0};
    if (::getloadavg(la, 3) == 3) {
        b.load_1min  = la[0];
        b.load_5min  = la[1];
        b.load_15min = la[2];
    }
}

[[nodiscard]] inline BoxInfo detect_box() {
    BoxInfo b;
    b.compiler   = box_detail::compiler_string();
    b.build_type = box_detail::build_type_string();

    // hardware_concurrency can return zero when it cannot tell, and zero is the
    // correct "unknown" for an int field here.
    b.cores = static_cast<int>(std::thread::hardware_concurrency());

#if defined(__APPLE__)
    std::string s;
    if (box_detail::sysctl_string("machdep.cpu.brand_string", s)) b.cpu_model = s;

    if (box_detail::sysctl_string("kern.osproductversion", s)) b.os = "macOS " + s;
    if (box_detail::sysctl_string("kern.osrelease", s)) {
        std::string type;
        b.kernel = (box_detail::sysctl_string("kern.ostype", type) ? type + " " : "") + s;
    }

    int n = 0;
    if (box_detail::sysctl_int("hw.logicalcpu", n) && n > 0) b.cores = n;

    // Apple Silicon splits cores into performance and efficiency clusters and
    // the scheduler decides which one a thread lands on. That is worth writing
    // into the notes, because a measurement that migrated from a P core to an E
    // core did not just lose cache, it changed clock, and there is no way from
    // user space to know it happened or to stop it.
    int perf = 0, eff = 0;
    if (box_detail::sysctl_int("hw.perflevel0.logicalcpu", perf) &&
        box_detail::sysctl_int("hw.perflevel1.logicalcpu", eff)) {
        b.notes = std::to_string(perf) + " performance cores and " +
                  std::to_string(eff) + " efficiency cores, thread placement between them is not observable or controllable from user space";
    }

    // macOS has no scaling governor concept that a program can read, and no
    // pinning, so both stay at their honest defaults.
    b.governor  = "unknown";
    b.isolated  = false;
    b.nohz_full = false;
    b.nic       = "unknown";

#elif defined(__linux__)
    std::string s;
    if (box_detail::proc_cpuinfo_field("model name", s)) b.cpu_model = s;
    else if (box_detail::proc_cpuinfo_field("Model Name", s)) b.cpu_model = s;
    else if (box_detail::proc_cpuinfo_field("Processor", s)) b.cpu_model = s;  // some arm64 kernels
    // arm64 kernels commonly publish no model name in /proc/cpuinfo at all,
    // only implementer and part numbers. The device tree usually has something
    // readable, and where it does not the field stays unknown rather than being
    // assembled out of hex ids that nobody can check against a real machine.
    else if (box_detail::read_file("/sys/firmware/devicetree/base/model", s) && !s.empty()) {
        b.cpu_model = box_detail::trim(s);
    } else if (box_detail::read_file("/proc/device-tree/model", s) && !s.empty()) {
        b.cpu_model = box_detail::trim(s);
    }

    // Distribution name from os-release, which is a shell fragment, so only the
    // PRETTY_NAME line is parsed and the quotes are stripped.
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (f && std::getline(f, line)) {
            if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            if (!v.empty()) b.os = v;
            break;
        }
    }

    {
        std::string rel;
        if (box_detail::read_file("/proc/sys/kernel/osrelease", rel)) b.kernel = "Linux " + rel;
    }

    {
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        if (n > 0) b.cores = static_cast<int>(n);
    }

    // The governor is per CPU. cpu0 is reported because a box where cpu0 and
    // the benchmark core disagree is misconfigured in a way this field should
    // make visible rather than paper over by scanning for a preferred answer.
    {
        std::string g;
        if (box_detail::read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", g))
            b.governor = g;
    }

    b.isolated  = box_detail::cmdline_has("isolcpus");
    b.nohz_full = box_detail::cmdline_has("nohz_full");
    b.nic       = box_detail::detect_nic();

    {
        // Record the isolated and nohz ranges themselves, because "isolated
        // true" without saying which cores does not let a reader check that the
        // benchmark core was one of them.
        std::string iso, nohz;
        box_detail::cmdline_has("isolcpus", &iso);
        box_detail::cmdline_has("nohz_full", &nohz);
        if (!iso.empty())  { b.notes += "isolcpus=" + iso; }
        if (!nohz.empty()) { if (!b.notes.empty()) b.notes += " "; b.notes += "nohz_full=" + nohz; }
    }
#endif

    // pinned is left false here on purpose. detect_box describes the machine,
    // not the run. The benchmark sets this from the PinningReport it actually
    // got back, so the field reflects what happened rather than what was
    // configured. pinning_supported is a property of the platform and is folded
    // into the notes so a reader of the JSON can tell the two apart.
    if (!pinning_supported()) {
        if (!b.notes.empty()) b.notes += ". ";
        b.notes += "platform offers no per core pinning, any tail number from this box is unpinned";
    }

    fill_load(b);
    return b;
}

} // namespace tick
