// 用户级测试报告助手：KOPAW_Test / KOPMS_Test 及下游应用共用。
// 输出格式：每项一行 [PASS]/[FAIL]/[SKIP]，末尾汇总；退出码 = 失败数。
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace kop {
namespace sdk {

class TestReport {
public:
    explicit TestReport(std::string app_name) : app_(std::move(app_name)) {}

    void section(const std::string& name) {
        std::printf("\n== %s ==\n", name.c_str());
    }

    void pass(const std::string& what, const std::string& detail = "") {
        std::printf("  [PASS] %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
                    detail.c_str());
        ++passed_;
    }

    void fail(const std::string& what, const std::string& detail = "") {
        std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
                    detail.c_str());
        ++failed_;
        failures_.push_back(what);
    }

    void skip(const std::string& what, const std::string& reason) {
        std::printf("  [SKIP] %s — %s\n", what.c_str(), reason.c_str());
        ++skipped_;
    }

    // 条件式：check(cond, what, fail_detail) → 计 PASS/FAIL
    bool check(bool ok, const std::string& what, const std::string& detail = "") {
        if (ok) {
            pass(what, detail);
        } else {
            fail(what, detail);
        }
        return ok;
    }

    void summary() const {
        std::printf("\n== %s 汇总 ==\n", app_.c_str());
        std::printf("  PASS %d / FAIL %d / SKIP %d\n", passed_, failed_, skipped_);
        for (const auto& f : failures_) {
            std::printf("  失败项: %s\n", f.c_str());
        }
        std::printf("%s: %s\n", app_.c_str(), failed_ == 0 ? "通过" : "未通过");
    }

    int failures() const { return failed_; }
    int skipped() const { return skipped_; }
    int passed() const { return passed_; }

private:
    std::string app_;
    int passed_ = 0;
    int failed_ = 0;
    int skipped_ = 0;
    std::vector<std::string> failures_;
};

}  // namespace sdk
}  // namespace kop
