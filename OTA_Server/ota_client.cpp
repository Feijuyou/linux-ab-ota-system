// RDK X3 OTA Client (C++11, libcurl, nlohmann/json, RAUC 1.6)
// Build with the existing cross-compilation CMake configuration.
// By default this program downloads and verifies, but does NOT install/reboot.
// Explicitly use --install --reboot to request installation and reboot.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;
using json = nlohmann::json;

static const uint64_t MAX_BUNDLE_BYTES = 4ULL * 1024 * 1024 * 1024;
static const uint64_t DISK_RESERVE = 32ULL * 1024 * 1024;
static const size_t MAX_MANIFEST_BYTES = 64 * 1024;
static const char* EXPECTED_COMPATIBLE = "rdk-x3-tf-ab";

struct Manifest {
    string compatible;
    string version;
    string bundle;
    string sha256;
    uint64_t size = 0;
};

struct Options {
    string baseUrl = "http://127.0.0.1:18000";
    string versionFile = "/etc/ota-version";
    string workDir = "/var/tmp/ota-client";
    string raucBin;
    bool install = false;
    bool reboot = false;
};

struct CurlGlobal {
    bool ok;
    CurlGlobal() : ok(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
    ~CurlGlobal() { if (ok) curl_global_cleanup(); }
};

struct HttpBuffer {
    string text;
};

// This callback receives the small manifest JSON into memory.
static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    if (size != 0 && nmemb > numeric_limits<size_t>::max() / size)
        return 0;
    const size_t bytes = size * nmemb;
    HttpBuffer* buffer = static_cast<HttpBuffer*>(userdata);
    if (bytes > MAX_MANIFEST_BYTES - buffer->text.size())
        return 0;
    try {
        buffer->text.append(ptr, bytes);
    } catch (...) {
        return 0; // Never throw C++ exceptions across the C callback boundary.
    }
    return bytes;
}

static bool httpGetText(const string& url, string& response)
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    HttpBuffer buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || status != 200) {
        cerr << "Manifest GET failed: " << curl_easy_strerror(res)
             << ", HTTP " << status << '\n';
        return false;
    }
    response.swap(buffer.text);
    return true;
}

static bool isHexSha256(const string& value)
{
    if (value.size() != 64) return false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static string lowerAscii(string value)
{
    for (char& c : value) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

static bool validBundleName(const string& name)
{
    const string suffix = ".raucb";
    if (name.size() <= suffix.size() || name.size() > 128 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0 ||
        name.find("..") != string::npos) return false;

    if (!((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= '0' && name[0] <= '9'))) return false;

    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

static bool parseManifest(const string& text, Manifest& result)
{
    try {
        json j = json::parse(text);
        if (!j.is_object()) return false;
        for (const char* key : {"compatible", "version", "bundle", "sha256"}) {
            if (!j.contains(key) || !j.at(key).is_string()) {
                cerr << "Missing/invalid manifest field: " << key << '\n';
                return false;
            }
        }
        if (!j.contains("size") || !j.at("size").is_number_integer()) {
            cerr << "Missing/invalid manifest field: size\n";
            return false;
        }

        Manifest temp;
        temp.compatible = j.at("compatible").get<string>();
        temp.version = j.at("version").get<string>();
        temp.bundle = j.at("bundle").get<string>();
        temp.sha256 = lowerAscii(j.at("sha256").get<string>());

        // Handle JSON signed/unsigned integers without accepting negatives.
        if (j.at("size").is_number_unsigned()) {
            temp.size = j.at("size").get<uint64_t>();
        } else {
            int64_t signedSize = j.at("size").get<int64_t>();
            if (signedSize <= 0) return false;
            temp.size = static_cast<uint64_t>(signedSize);
        }

        if (temp.compatible != EXPECTED_COMPATIBLE ||
            temp.version.empty() || !validBundleName(temp.bundle) ||
            !isHexSha256(temp.sha256) ||
            temp.size == 0 || temp.size > MAX_BUNDLE_BYTES) {
            cerr << "Manifest incompatible or values invalid\n";
            return false;
        }
        result = temp;
        return true;
    } catch (const json::exception& e) {
        cerr << "Manifest JSON error: " << e.what() << '\n';
        return false;
    }
}

static bool readLocalVersion(const string& path, string& version)
{
    ifstream file(path.c_str());
    if (!file.is_open()) {
        cerr << "Failed to open version file: " << path << '\n';
        return false;
    }
    string temp;
    if (!getline(file, temp)) return false;
    if (!temp.empty() && temp.back() == '\r') temp.pop_back();
    if (temp.empty() || temp.size() > 100) return false;
    version = temp;
    return true;
}

static bool parseVersionNumbers(const string& text, array<uint64_t, 3>& parts)
{
    size_t pos = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
            return false;
        uint64_t num = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            uint64_t digit = static_cast<uint64_t>(text[pos] - '0');
            if (num > (numeric_limits<uint64_t>::max() - digit) / 10)
                return false;
            num = num * 10 + digit;
            ++pos;
        }
        parts[i] = num;
        if (i < 2) {
            if (pos >= text.size() || text[pos] != '.') return false;
            ++pos;
        }
    }
    return pos == text.size();
}

// Return -1 if local is older, 0 if same, +1 if local is newer.
static bool compareVersion(const string& local, const string& remote, int& comparison)
{
    array<uint64_t, 3> a{};
    array<uint64_t, 3> b{};
    if (!parseVersionNumbers(local, a) || !parseVersionNumbers(remote, b)) {
        cerr << "Invalid version format (expected X.Y.Z)\n";
        return false;
    }
    comparison = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (a[i] < b[i]) { comparison = -1; break; }
        if (a[i] > b[i]) { comparison = 1; break; }
    }
    return true;
}

static bool prepareWorkDir(const string& dir)
{
    if (dir.empty() || dir[0] != '/' || dir == "/" ||
        dir.find("/../") != string::npos ||
        dir.compare(dir.size() >= 3 ? dir.size() - 3 : 0, 3, "/..") == 0)
        return false;

    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        perror("mkdir work directory");
        return false;
    }
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        cerr << "Work directory must be private (0700), owned by current user, and not a symlink: "
             << dir << '\n';
        return false;
    }
    return true;
}

static bool enoughDiskSpace(const string& dir, uint64_t bytes)
{
    struct statvfs vfs;
    if (statvfs(dir.c_str(), &vfs) != 0) {
        perror("statvfs");
        return false;
    }
    const long double available =
        static_cast<long double>(vfs.f_bavail) * vfs.f_frsize;
    if (available < static_cast<long double>(bytes + DISK_RESERVE)) {
        cerr << "Insufficient free disk space for " << bytes << " bytes\n";
        return false;
    }
    return true;
}

struct DownloadSink {
    FILE* file;
    uint64_t written;
    uint64_t limit;
};

static size_t FileWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    if (size != 0 && nmemb > numeric_limits<size_t>::max() / size)
        return 0;
    const size_t bytes = size * nmemb;
    DownloadSink* sink = static_cast<DownloadSink*>(userdata);
    if (sink->written > sink->limit ||
        static_cast<uint64_t>(bytes) > sink->limit - sink->written)
        return 0;
    const size_t written = fwrite(ptr, 1, bytes, sink->file);
    sink->written += written;
    return written;
}

static int DownloadProgress(void* userdata, curl_off_t total, curl_off_t now,
                            curl_off_t, curl_off_t)
{
    int* last = static_cast<int*>(userdata);
    if (total > 0 && now >= 0) {
        int percent = static_cast<int>(100.0 * now / total);
        if (percent > 100) percent = 100;
        if (percent != *last &&
            (percent == 100 || percent / 5 != *last / 5)) {
            cerr << "\rDownloading: " << percent << "%" << flush;
            *last = percent;
        }
    }
    return 0;
}

static void cleanupDownload(const string& file, const string& dir)
{
    if (!file.empty()) unlink(file.c_str());
    if (!dir.empty()) rmdir(dir.c_str());
}

static bool downloadOnce(const string& url, const Manifest& manifest,
                         const string& workDir, string& path, string& jobDir)
{
    string pattern = workDir + "/job-XXXXXX";
    vector<char> dirbuf(pattern.begin(), pattern.end());
    dirbuf.push_back('\0');
    char* created = mkdtemp(dirbuf.data());
    if (!created) { perror("mkdtemp"); return false; }
    const string dir(created);
    const string part = dir + "/bundle.raucb.part";
    const string final = dir + "/bundle.raucb";

    int fd = open(part.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        perror("open download file");
        rmdir(dir.c_str());
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        perror("fdopen");
        close(fd);
        cleanupDownload(part, dir);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        cleanupDownload(part, dir);
        return false;
    }
    DownloadSink sink{file, 0, manifest.size};
    int lastPercent = -1;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FileWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(manifest.size));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &lastPercent);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    cerr << '\n';

    bool ioOK = fflush(file) == 0;
    if (ioOK) ioOK = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ioOK = false;

    if (res != CURLE_OK || status != 200 || !ioOK ||
        sink.written != manifest.size) {
        cerr << "Download failed: " << curl_easy_strerror(res)
             << ", HTTP=" << status << ", bytes=" << sink.written
             << "/" << manifest.size << '\n';
        cleanupDownload(part, dir);
        return false;
    }
    if (rename(part.c_str(), final.c_str()) != 0) {
        perror("rename");
        cleanupDownload(part, dir);
        return false;
    }
    path = final;
    jobDir = dir;
    cout << "Downloaded " << sink.written << " bytes to " << path << '\n';
    return true;
}

static bool downloadBundle(const string& url, const Manifest& manifest,
                           const string& workDir, string& path, string& jobDir)
{
    if (!enoughDiskSpace(workDir, manifest.size)) return false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        cout << "Download attempt " << attempt << "/3\n";
        if (downloadOnce(url, manifest, workDir, path, jobDir)) return true;
        if (attempt < 3) sleep(static_cast<unsigned>(attempt));
    }
    return false;
}

// Find a known absolute executable path; no shell is ever used.
static string findProgram(const vector<string>& candidates)
{
    for (const string& p : candidates) {
        if (!p.empty() && p[0] == '/' && access(p.c_str(), X_OK) == 0)
            return p;
    }
    return string();
}

static bool waitChild(pid_t pid)
{
    int status = 0;
    pid_t got;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) { perror("waitpid"); return false; }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFEXITED(status)) cerr << "Command failed: exit " << WEXITSTATUS(status) << '\n';
    else if (WIFSIGNALED(status)) cerr << "Command killed by signal " << WTERMSIG(status) << '\n';
    return false;
}

static bool runCommand(const string& program, const vector<string>& args)
{
    if (program.empty() || program[0] != '/') return false;
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return false; }
    if (pid == 0) {
        vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(program.c_str(), argv.data());
        perror("execv");
        _exit(127);
    }
    return waitChild(pid);
}

// Compute SHA-256 using the target's sha256sum utility, without a shell.
// This avoids introducing a second linked crypto library into the existing cross-build.
static bool sha256File(const string& path, string& digest)
{
    const string program = findProgram({"/usr/bin/sha256sum", "/bin/sha256sum"});
    if (program.empty()) {
        cerr << "sha256sum not found\n";
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); return false; }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fds[0]); close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execl(program.c_str(), "sha256sum", "--", path.c_str(),
              static_cast<char*>(nullptr));
        perror("exec sha256sum");
        _exit(127);
    }
    close(fds[1]);
    string output;
    char buffer[128];
    ssize_t count;
    while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) {
        if (output.size() < 512) {
            output.append(buffer, static_cast<size_t>(count));
        }
    }
    close(fds[0]);
    if (!waitChild(pid)) return false;
    if (output.size() < 65) return false;
    string hash = output.substr(0, 64);
    if (!isHexSha256(hash) || (output[64] != ' ' && output[64] != '\t'))
        return false;
    digest = lowerAscii(hash);
    return true;
}

static bool requestReboot()
{
    sync(); // Flush filesystem data before requesting an orderly reboot.
    const string systemctl = findProgram({"/usr/bin/systemctl", "/bin/systemctl"});
    if (!systemctl.empty()) {
        cout << "Requesting systemctl reboot...\n" << flush;
        if (runCommand(systemctl, {"reboot"})) return true;
        cerr << "systemctl reboot failed, trying reboot utility\n";
    }
    const string rebootProgram = findProgram({"/usr/sbin/reboot", "/sbin/reboot", "/usr/bin/reboot", "/bin/reboot"});
    if (rebootProgram.empty()) {
        cerr << "No supported reboot utility found\n";
        return false;
    }
    cout << "Requesting reboot...\n" << flush;
    return runCommand(rebootProgram, {});
}

static void printUsage(const char* executable)
{
    cout << "Usage: " << executable << " [options]\n"
         << "  --base-url URL        (default: http://127.0.0.1:18000)\n"
         << "  --version-file PATH   (default: /etc/ota-version)\n"
         << "  --work-dir PATH       (default: /var/tmp/ota-client)\n"
         << "  --rauc-bin PATH       (auto-detect by default)\n"
         << "  --install             Actually execute: rauc install BUNDLE\n"
         << "  --reboot              Request reboot after successful install\n"
         << "  --help                Show this help\n"
         << "By default: check, download, SHA-256 verify, rauc info only.\n";
}

static bool parseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--install") opt.install = true;
        else if (arg == "--reboot") opt.reboot = true;
        else if (arg == "--base-url" || arg == "--version-file" ||
                 arg == "--work-dir" || arg == "--rauc-bin") {
            if (i + 1 >= argc) return false;
            string value = argv[++i];
            if (arg == "--base-url") opt.baseUrl = value;
            else if (arg == "--version-file") opt.versionFile = value;
            else if (arg == "--work-dir") opt.workDir = value;
            else opt.raucBin = value;
        } else return false;
    }
    if (opt.reboot && !opt.install) {
        cerr << "--reboot requires --install\n";
        return false;
    }
    if (opt.baseUrl.compare(0, 7, "http://") != 0 &&
        opt.baseUrl.compare(0, 8, "https://") != 0) return false;
    if (opt.baseUrl.find_first_of("?# ") != string::npos) return false;
    while (!opt.baseUrl.empty() && opt.baseUrl.back() == '/')
        opt.baseUrl.pop_back();
    if (opt.baseUrl == "http:" || opt.baseUrl == "https:") return false;
    return true;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (string(argv[i]) == "--help") { printUsage(argv[0]); return 0; }
    }
    Options options;
    if (!parseArgs(argc, argv, options)) {
        printUsage(argv[0]);
        return 2;
    }
    if (options.install && geteuid() != 0) {
        cerr << "Installation/reboot must be run as root on this device\n";
        return 1;
    }

    CurlGlobal curlGlobal;
    if (!curlGlobal.ok) {
        cerr << "curl_global_init failed\n";
        return 1;
    }

    cout << "=== OTA Check ===\n";
    string response;
    if (!httpGetText(options.baseUrl + "/manifest.json", response)) return 1;
    Manifest manifest;
    if (!parseManifest(response, manifest)) return 1;

    string localVersion;
    if (!readLocalVersion(options.versionFile, localVersion)) return 1;
    cout << "Compatible: " << manifest.compatible << '\n'
         << "Local: " << localVersion << '\n'
         << "Remote: " << manifest.version << '\n'
         << "Bundle: " << manifest.bundle << '\n'
         << "Size: " << manifest.size << " bytes\n";

    int comparison = 0;
    if (!compareVersion(localVersion, manifest.version, comparison)) return 1;
    if (comparison == 0) { cout << "Already up to date\n"; return 0; }
    if (comparison > 0) { cout << "Local version is newer; refuse downgrade\n"; return 0; }

    if (!prepareWorkDir(options.workDir)) return 1;
    const string lockFile = options.workDir + "/.lock";
    int lockFD = open(lockFile.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFD < 0) { perror("open lock"); return 1; }
    if (flock(lockFD, LOCK_EX | LOCK_NB) != 0) {
        cerr << "Another OTA Client is running\n";
        close(lockFD);
        return 1;
    }

    const string bundleURL = options.baseUrl + "/" + manifest.bundle;
    string bundlePath, jobDir;
    cout << "New version available. Downloading from " << bundleURL << '\n';
    if (!downloadBundle(bundleURL, manifest, options.workDir, bundlePath, jobDir)) {
        close(lockFD);
        return 1;
    }

    string digest;
    if (!sha256File(bundlePath, digest) || digest != manifest.sha256) {
        cerr << "SHA-256 mismatch or hash operation failed\n";
        cerr << "Expected: " << manifest.sha256 << '\n'
             << "Actual:   " << (digest.empty() ? "(unavailable)" : digest) << '\n';
        cleanupDownload(bundlePath, jobDir);
        close(lockFD);
        return 1;
    }
    cout << "SHA-256 verified: " << digest << '\n';

    const string raucBin = options.raucBin.empty()
        ? findProgram({"/usr/bin/rauc", "/usr/local/bin/rauc", "/bin/rauc", "/usr/sbin/rauc"})
        : options.raucBin;
    if (raucBin.empty() || raucBin[0] != '/' || access(raucBin.c_str(), X_OK) != 0) {
        cerr << "RAUC executable not found; use --rauc-bin /absolute/path/to/rauc\n";
        cleanupDownload(bundlePath, jobDir);
        close(lockFD);
        return 1;
    }

    cout << "=== RAUC info (signature verification) ===\n";
    if (!runCommand(raucBin, {"info", bundlePath})) {
        cerr << "rauc info failed; installation blocked\n";
        cleanupDownload(bundlePath, jobDir);
        close(lockFD);
        return 1;
    }

    if (!options.install) {
        cout << "[SAFE MODE] Bundle downloaded and checked; NOT installed\n"
             << "Bundle retained at: " << bundlePath << '\n'
             << "Run with --install --reboot for the real update.\n";
        close(lockFD);
        return 0;
    }

    cout << "=== RAUC status before installation ===\n";
    if (!runCommand(raucBin, {"status"})) {
        cerr << "rauc status failed; refusing installation\n";
        cleanupDownload(bundlePath, jobDir);
        close(lockFD);
        return 1;
    }
    cout << "=== RAUC install (WRITES INACTIVE SLOT) ===\n" << flush;
    if (!runCommand(raucBin, {"install", bundlePath})) {
        cerr << "RAUC installation failed; NOT rebooting\n";
        // Keep the downloaded bundle for troubleshooting.
        cerr << "Bundle retained at: " << bundlePath << '\n';
        close(lockFD);
        return 1;
    }
    cout << "RAUC installation succeeded (new system not yet health-checked).\n";

    // RAUC has finished using this local copy; save space before reboot.
    cleanupDownload(bundlePath, jobDir);
    if (!options.reboot) {
        cout << "Reboot not requested; run reboot manually when ready.\n";
        close(lockFD);
        return 0;
    }
    cout << "Requesting reboot now; boot health confirmation is a separate step.\n" << flush;
    bool rebootRequested = requestReboot();
    close(lockFD);
    if (!rebootRequested) {
        cerr << "Reboot request failed; manually reboot after checking RAUC status.\n";
        return 1;
    }
    return 0;
}
