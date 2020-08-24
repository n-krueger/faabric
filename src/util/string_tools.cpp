#include <faabric/util/string_tools.h>

#include <string>
#include <algorithm>

namespace faabric::util {

    bool isAllWhitespace(const std::string &input) {
        return std::all_of(input.begin(), input.end(), isspace);
    }

    bool startsWith(const std::string &input, const std::string &subStr) {
        if (subStr.empty()) {
            return false;
        }

        return input.rfind(subStr, 0) == 0;
    }

    bool endsWith(std::string const &value, std::string const &ending) {
        if (ending.empty()) {
            return false;
        } else if (ending.size() > value.size()) {
            return false;
        }
        return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
    }

    bool contains(const std::string &input, const std::string &subStr) {
        if (input.find(subStr) != std::string::npos) {
            return true;
        } else {
            return false;
        }
    }

    std::string removeSubstr(const std::string &input, const std::string &toErase) {
        std::string output = input;

        size_t pos = output.find(toErase);

        if (pos != std::string::npos) {
            output.erase(pos, toErase.length());
        }

        return output;
    }

    bool stringIsInt(const std::string &input) {
        return !input.empty() && input.find_first_not_of("0123456789") == std::string::npos;
    }
}
