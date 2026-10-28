#include <cstring>
#include <exception>

#include "harness.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& c : qbtest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = qbtest::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            qbtest::fail(c.name, 0, std::string("exception: ") + e.what());
        }
        std::cout << (qbtest::failures() == before ? "ok   " : "FAIL ") << c.name << "\n";
        ++ran;
    }
    std::cout << ran << " tests, " << qbtest::failures() << " failures\n";
    return qbtest::failures() ? 1 : 0;
}
