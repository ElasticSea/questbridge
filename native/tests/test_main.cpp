#include <cstring>
#include <exception>

#include "harness.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& c : qltest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = qltest::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            qltest::fail(c.name, 0, std::string("exception: ") + e.what());
        }
        std::cout << (qltest::failures() == before ? "ok   " : "FAIL ") << c.name << "\n";
        ++ran;
    }
    std::cout << ran << " tests, " << qltest::failures() << " failures\n";
    return qltest::failures() ? 1 : 0;
}
