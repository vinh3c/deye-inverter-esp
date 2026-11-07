#include "logger_form.h"
#include "html_template.h"
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::ifstream file(argv[1]);
    const std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    assert(!html.empty());
    const logger_web::Replacement replacements[] = {
        {"{{HOST}}", "logger.local"}, {"{{PORT}}", "502"},
        {"{{TOKEN}}", "0123456789abcdef0123456789abcdef"}, {"{{NOTICE}}", nullptr},
    };
    std::string rendered;
    const auto emit = [&](const char *data, size_t length) {
        assert(length > 0); // An empty chunk would prematurely end the HTTP body.
        rendered.append(data, length);
        return true;
    };
    assert(logger_web::stream_template(html.c_str(), replacements, 4, emit,
        [&]() { return emit("Saved", 5); }));
    assert(rendered.find("{{") == std::string::npos);
    assert(rendered.find("value=\"logger.local\"") != std::string::npos);
    assert(rendered.find("Saved") != std::string::npos);
    rendered.clear();
    assert(logger_web::stream_template("{{HOST}}{{PORT}}{{NOTICE}}", replacements, 4, emit, []() { return true; }));
    assert(rendered == "logger.local502");
    int writes = 0;
    assert(!logger_web::stream_template("x{{HOST}}y", replacements, 4,
        [&](const char *, size_t) { return ++writes < 2; }, []() { return true; }));
    assert(writes == 2);
    assert(!logger_web::stream_template("{{NOTICE}}", replacements, 4, emit, []() { return false; }));
    using logger_form::Result;
    constexpr char token[] = "0123456789abcdef0123456789abcdef";
    const std::string suffix = std::string("&token=") + token;
    logger_form::Values values{};
    for (const auto &body : {"host=logger.local&port=502", "port=65535&host=192.0.2.10",
                             "host=logger%2D1.local&port=%35%30%32", "host=a&port=1"}) {
        assert(logger_form::parse(body + suffix, token, values) == Result::Ok);
    }
    assert(values.port == 1 && std::string(values.host) == "a");
    assert(logger_form::parse("host=" + std::string(63, 'a') + "&port=502" + suffix, token, values) == Result::Ok);
    for (const auto &body : {"host=&port=502", "host=-logger&port=502", "host=logger.&port=502",
            "host=http%3A%2F%2Flogger&port=502", "host=logger%00evil&port=502",
            "host=logger%0D%0Areset&port=502", "host=logger+local&port=502", "host=%ff&port=502",
            "host=logger%&port=502", "host=logger%2&port=502", "host=logger%ZZ&port=502",
            "host=logger&port=0", "host=logger&port=-1", "host=logger&port=65536",
            "host=logger&port=4294967297", "host=logger&port=1e2", "host=logger&port=502&port=503",
            "host=logger&host=other&port=502", "host=logger&port=502&extra=x",
            "host=logger", "host=logger&port=", "host=logger&&port=502"}) {
        const auto before = values;
        assert(logger_form::parse(body + suffix, token, values) == Result::Invalid);
        assert(values.port == before.port && std::string(values.host) == before.host);
    }
    assert(logger_form::parse("host=" + std::string(64, 'a') + "&port=502" + suffix, token, values) == Result::Invalid);
    assert(logger_form::parse(std::string(257, 'x'), token, values) == Result::Invalid);
    assert(logger_form::parse("host=logger&port=502" + suffix + "&", token, values) == Result::Invalid);
    assert(logger_form::parse("host=logger&port=502", token, values) == Result::Invalid);
    assert(logger_form::parse("host=logger&port=502&token=wrong", token, values) == Result::Forbidden);
    assert(logger_form::parse("host=logger&port=502" + suffix, std::string(32, 'f'), values) == Result::Forbidden);
    assert(logger_form::parse("host=logger&port=502" + suffix + suffix, token, values) == Result::Invalid);
}
