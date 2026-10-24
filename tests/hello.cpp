#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main()
{
    std::vector<std::string> words{"geode", "olpc", "xo", "sugar"};
    std::sort(words.begin(), words.end());

    std::map<std::string, std::size_t> lengths;
    for (auto const &w : words)
        lengths[w] = w.size();

    auto p = std::make_shared<int>(42);
    try {
        throw std::runtime_error("exception caught");
    } catch (std::exception const &e) {
        std::cout << "hello from C++ (" << __cplusplus << "), " << e.what()
                  << ", *p = " << *p << '\n';
    }
    for (auto const &kv : lengths)
        std::cout << "  " << kv.first << ": " << kv.second << '\n';
    return 0;
}
