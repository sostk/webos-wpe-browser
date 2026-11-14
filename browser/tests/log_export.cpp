// SPDX-License-Identifier: GPL-3.0-or-later
// Logs saved for a bug report keep web addresses' host and path but never their query or fragment.
#include "log_export.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

static void same(const std::string &got, const std::string &want) {
    check(got == want, "\"" + got + "\" == \"" + want + "\"");
}

int main() {
    using browser::redact_urls;
    same(redact_urls("load https://www.youtube.com/tv#/watch?v=abc done"), "load https://www.youtube.com/tv?… done");
    same(redact_urls("GET https://lic.example/wv?token=SECRET&x=1"), "GET https://lic.example/wv?…");
    same(redact_urls("\"http://a.b/p/q\" and 'ws://c.d/s?id=9'"), "\"http://a.b/p/q\" and 'ws://c.d/s?…'");
    same(redact_urls("no url here?really#yes"), "no url here?really#yes");
    same(redact_urls("https://a.b/?q=1 then plain? text"), "https://a.b/?… then plain? text");
    same(redact_urls("(https://a.b/x?y), https://c.d"), "(https://a.b/x?…), https://c.d");
    same(redact_urls(""), "");

    char pattern[] = "/tmp/wpe-logexport-test-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    std::ofstream(dir + "/engine.log") << "first line\nopened https://bank.example/acct?session=XYZ\n";
    std::ofstream(dir + "/events.log") << std::string(5000, 'a') << "\nlast line\n";

    browser::LogExportRequest req;
    req.runtime_dir = dir;
    req.header = {"Wren 0.3.0", "TV: test"};
    req.max_bytes_per_log = 100;
    std::string err;
    const std::string out = browser::export_logs(req, dir, &err);
    check(!out.empty(), "file written: " + out + err);
    std::stringstream ss;
    ss << std::ifstream(out).rdbuf();
    const std::string text = ss.str();
    check(text.find("Wren 0.3.0\nTV: test\n") != std::string::npos, "header written");
    check(text.find("session=XYZ") == std::string::npos, "query removed");
    check(text.find("https://bank.example/acct?…") != std::string::npos, "host and path kept");
    check(text.find("engine.prev.log (") != std::string::npos && text.find(": not found") != std::string::npos,
          "missing log noted");
    check(text.find("only the end") != std::string::npos && text.find("last line") != std::string::npos &&
              text.find(std::string(200, 'a')) == std::string::npos,
          "long log cut to its end");
    check(browser::export_logs(req, dir + "/nope", &err).empty() && !err.empty(), "unwritable folder fails: " + err);

    std::system(("rm -rf '" + dir + "'").c_str());
    return failures == 0 ? 0 : 1;
}
