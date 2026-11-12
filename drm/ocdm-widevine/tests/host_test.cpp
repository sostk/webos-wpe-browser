// Unit test for ocdm_widevine::Host (storage, clock, timers). Runs on the build machine.
#include "../src/host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

struct Client : widevine::ITimerClient {
    std::atomic<int> fired { 0 };
    void* last = nullptr;
    void onTimerExpired(void* context) override
    {
        last = context;
        ++fired;
    }
};

int main()
{
    char tmpl[] = "/tmp/host_test.XXXXXX";
    std::string root = mkdtemp(tmpl);
    std::string dir = root + "/nested/widevine";
    {
        ocdm_widevine::Host host(dir);

        CHECK(host.usable());
        struct stat st;
        CHECK(stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        CHECK((st.st_mode & 0777) == 0700);

        std::string binary("cert\0bytes\xff", 11);
        CHECK(host.write("cert.bin", binary));
        CHECK(host.exists("cert.bin"));
        CHECK(host.size("cert.bin") == 11);
        std::string back;
        CHECK(host.read("cert.bin", &back) && back == binary);
        CHECK(stat((dir + "/cert.bin").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);

        CHECK(host.write("cert.bin", "v2"));
        CHECK(host.read("cert.bin", &back) && back == "v2");

        CHECK(!host.exists("missing.lic"));
        CHECK(host.size("missing.lic") == -1);
        CHECK(!host.read("missing.lic", &back));
        CHECK(host.remove("missing.lic"));

        // Names that could leave the directory are refused.
        CHECK(!host.write("../escape", "x"));
        CHECK(!host.write("a/b", "x"));
        CHECK(!host.write(".hidden", "x"));
        CHECK(!host.write("", "x"));
        CHECK(access((root + "/nested/escape").c_str(), F_OK) != 0);

        // A leftover temporary file from an interrupted write is never listed.
        CHECK(host.write("ksid1.lic", "lic"));
        FILE* f = std::fopen((dir + "/usgtable.bin.tmp").c_str(), "w");
        std::fputs("partial", f);
        std::fclose(f);
        std::vector<std::string> names;
        CHECK(host.list(&names));
        CHECK(names.size() == 2 && names[0] == "cert.bin" && names[1] == "ksid1.lic");

        CHECK(host.remove("ksid1.lic"));
        CHECK(!host.exists("ksid1.lic"));
        CHECK(host.remove(""));
        CHECK(host.list(&names) && names.empty());

        // Clock is the real wall clock (milliseconds since 1970).
        long long diff = host.now() - static_cast<long long>(time(nullptr)) * 1000;
        CHECK(diff > -2000 && diff < 2000);

        // Timers fire once, in order, with their context; cancel stops pending ones for that client.
        Client a, b;
        int ctx = 42;
        host.setTimeout(30, &a, &ctx);
        host.setTimeout(10, &b, nullptr);
        host.setTimeout(200, &b, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(a.fired == 1 && a.last == &ctx);
        CHECK(b.fired == 1);
        host.cancel(&b);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(b.fired == 1);

        // With a CDM lock set, a timer callback waits while another thread holds that lock.
        std::recursive_mutex cdm_lock;
        host.set_cdm_lock(&cdm_lock);
        Client c;
        {
            std::lock_guard<std::recursive_mutex> held(cdm_lock);
            host.setTimeout(5, &c, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            CHECK(c.fired == 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(c.fired == 1);

        // A pending timer must not fire after the host is destroyed.
        host.setTimeout(100, &a, nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // A directory that cannot be created (parent not writable) is reported, not silently accepted.
    {
        std::string locked = root + "/locked";
        mkdir(locked.c_str(), 0500);
        if (geteuid() != 0) { // root ignores the permission bits
            ocdm_widevine::Host blocked(locked + "/widevine");
            CHECK(!blocked.usable());
            CHECK(!blocked.write("cert.bin", "x"));
        }
        chmod(locked.c_str(), 0700);
    }

    std::string cleanup = "rm -rf '" + root + "'";
    if (std::system(cleanup.c_str()) != 0)
        std::printf("note: could not remove %s\n", root.c_str());
    std::printf(failures ? "%d failure(s)\n" : "host_test: all checks passed\n", failures);
    return failures ? 1 : 0;
}
