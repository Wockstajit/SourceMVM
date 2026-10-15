#include "../../AfxHookSource2/Filmmaker/Platform/JsonParser.h"
#include <clocale>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace Filmmaker;
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x "\n"; ++failed; } } while (0)
void Reject(const std::string& text) {
    JsonValue out; out.type = JsonValue::Type::String; out.str = "untouched";
    CHECK(!JsonParse(text, out));
    CHECK(out.type == JsonValue::Type::String && out.str == "untouched" && out.obj.empty() && out.arr.empty());
}
}
int main(int argc, char** argv) {
    for (const char* text : { "", " ", "-", "+1", ".5", "-.5", "01", "-01", "1.", "1e", "1e+", "1e-",
        "1.2.3", "1-2", "1+2", "--1", "e1", "NaN", "Infinity", "1e999", "1e-999", "0 trailing",
        "{}{}", "[] null", "true false", "nullx", "1 2", "{\"a\":1e}", "[1+2]", "[1,]", "{\"a\":1,}" }) Reject(text);
    Reject(std::string("{}\0junk", 7));
    for (const char* text : { "0", "-0", "1", "-2", "3.5", "1e2", "1E+2", "-1.5e-2", "1e308", "5e-324", " \r\n2.75\t" }) {
        JsonValue out;
        CHECK(JsonParse(text, out));
        CHECK(out.type == JsonValue::Type::Number && std::isfinite(out.number));
    }
    JsonValue root;
    CHECK(JsonParse(R"({"old":[1,2],"name":"before"})", root));
    CHECK(JsonParse(R"({"new":true})", root));
    CHECK(root.obj.size() == 1 && !root.Find("old") && root.Find("new") && root.Find("new")->AsBool());
    CHECK(JsonParse(R"("after")", root));
    CHECK(root.str == "after" && root.obj.empty());
    CHECK(JsonParse("null", root));
    CHECK(root.type == JsonValue::Type::Null && root.str.empty());
    const char* nested = R"({"escaped":"\uD83D\uDE00\n","nested":[null,false,{"v":-2.5e+1}]})";
    CHECK(JsonParse(nested, root));
    CHECK(root.Find("escaped") && root.Find("escaped")->str == "\xf0\x9f\x98\x80\n");
    CHECK(root.Find("nested") && root.Find("nested")->arr[2].Find("v")->number == -25);
    const std::string originalLocale = std::setlocale(LC_NUMERIC, nullptr);
    CHECK(std::setlocale(LC_NUMERIC, "German_Germany.1252") != nullptr);
    CHECK(JsonParse("1.25", root) && root.number == 1.25);
    Reject("1,25");
    CHECK(std::setlocale(LC_NUMERIC, originalLocale.c_str()) != nullptr);
    // Optional corpus paths exercise the exact parser linked into both game hooks.
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!file || !JsonParse(text, root)) { std::cerr << "CORPUS FAILED: " << argv[i] << '\n'; ++failed; }
    }
    std::cout << "JSON parser: " << failed << " failure(s)\n";
    return failed ? 1 : 0;
}
